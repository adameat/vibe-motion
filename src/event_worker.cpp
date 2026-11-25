#include "event_store.hpp"
#include "vibe_motion/hooks.hpp"
#include "vibe_motion/log.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <curl/curl.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace vibe_motion::processing {
using namespace std::chrono_literals;
std::atomic<bool> stopping{false};

Database::Database(const GlobalConfig& config) {
    if (!connection_)
        throw std::bad_alloc();
    if (!std::ifstream(config.event_db_options_file))
        throw std::runtime_error("cannot read event_db_options_file");
    auto* db = connection_.get();
    unsigned timeout = 10;
    mysql_options(db, MYSQL_READ_DEFAULT_FILE, config.event_db_options_file.c_str());
    mysql_options(db, MYSQL_READ_DEFAULT_GROUP, "vibe-motion");
    mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    if (!mysql_real_connect(db, nullptr, nullptr, nullptr, config.event_database.c_str(), 0,
                            nullptr, 0) ||
        mysql_set_character_set(db, "utf8mb4"))
        throw std::runtime_error("event database connection failed (" +
                                 std::to_string(mysql_errno(db)) + ")");
}

std::vector<Row> Database::query(std::string_view sql, std::initializer_list<std::string> values) {
    std::string statement;
    auto it = values.begin();
    for (char ch : sql) {
        if (ch != '?') {
            statement += ch;
            continue;
        }
        if (it == values.end())
            throw std::logic_error("missing query value");
        constexpr std::string_view hex = "0123456789abcdef";
        statement += "CONVERT(X'";
        for (char c : *it++) {
            auto b = static_cast<unsigned char>(c);
            statement += hex[b >> 4];
            statement += hex[b & 15];
        }
        statement += "' USING utf8mb4)";
    }
    if (it != values.end())
        throw std::logic_error("unused query value");
    auto* db = connection_.get();
    if (mysql_real_query(db, statement.data(), statement.size()))
        throw std::runtime_error("event database query failed (" + std::to_string(mysql_errno(db)) +
                                 ")");
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(mysql_store_result(db),
                                                                    mysql_free_result);
    std::vector<Row> rows;
    if (!result && mysql_field_count(db))
        throw std::runtime_error("event database result unavailable");
    if (result) {
        auto fields = mysql_fetch_fields(result.get());
        while (auto row = mysql_fetch_row(result.get())) {
            auto lengths = mysql_fetch_lengths(result.get());
            auto& values_row = rows.emplace_back();
            for (unsigned i = 0; i < mysql_num_fields(result.get()); ++i)
                values_row[fields[i].name] =
                    row[i] ? std::string(row[i], lengths[i]) : std::string{};
        }
    }
    return rows;
}
Row Database::one(std::string_view sql, std::initializer_list<std::string> values) {
    auto rows = query(sql, values);
    return rows.empty() ? Row{} : std::move(rows.front());
}

Path media_path(const Path& root, const Path& path) {
    if (path.empty() || !path.is_absolute())
        throw std::runtime_error("invalid event media path");
    auto resolved = std::filesystem::weakly_canonical(path);
    auto relative = resolved.lexically_relative(root);
    if (relative.empty() || relative == "." || *relative.begin() == "..")
        throw std::runtime_error("event media path outside target_dir");
    return path;
}
Path derivative(Path path, const std::string& suffix) {
    path.replace_extension();
    return path.string() + suffix;
}

void render(const Path& input, const Path& output, const std::vector<std::string>& options) {
    // Keep partial encodes invisible to the web frontend and Telegram.
    const auto temporary =
        derivative(output, ".partial-" + std::to_string(getpid()) + output.extension().string());
    std::vector<std::string> args{"ffmpeg", "-nostdin", "-loglevel",   "error",
                                  "-y",     "-i",       input.string()};
    args.insert(args.end(), options.begin(), options.end());
    args.push_back(temporary.string());
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid{};
    if (posix_spawnp(&pid, "ffmpeg", nullptr, nullptr, argv.data(), environ))
        throw std::runtime_error("cannot start ffmpeg");
    const auto deadline = std::chrono::steady_clock::now() + 2h;
    int status{};
    while (true) {
        auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid)
            break;
        if (waited < 0 && errno != EINTR)
            throw std::runtime_error("cannot wait for ffmpeg");
        if (stopping || std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !std::filesystem::exists(temporary) ||
        !std::filesystem::file_size(temporary)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("ffmpeg did not finish " + output.filename().string());
    }
    std::filesystem::rename(temporary, output);
}

void record_file(Database& db, const std::string& id, const std::string& type, const Path& path,
                 const std::string& status) {
    db.query("INSERT INTO files(event_id,type,path,status) VALUES(?,?,?,?) ON DUPLICATE KEY UPDATE "
             "status=?",
             {id, type, path.string(), status, status});
}

namespace {
std::string hostname() {
    char name[256]{};
    if (gethostname(name, sizeof(name) - 1))
        throw std::runtime_error("gethostname failed");
    return std::string(name).substr(0, std::string_view(name).find('.'));
}

void ingest(Database& db, const CameraConfig& config, const Path& record, const Path& root) {
    std::ifstream in(record);
    std::string kind, key, file;
    std::time_t timestamp{};
    int changed{}, noise{};
    if (!(in >> kind >> std::quoted(key) >> timestamp >> changed >> noise >> std::quoted(file)))
        throw std::runtime_error("unreadable event record " + record.filename().string());
    const auto camera = std::to_string(config.camera_id), when = std::to_string(timestamp);
    Path path;
    if (!file.empty())
        path = media_path(root, file);
    const auto host = hostname();
    if (db.one("SELECT id FROM cams WHERE id=?", {camera}).empty())
        throw std::runtime_error("camera missing from cams");
    db.query("START TRANSACTION");
    if (kind == "snapshot") {
        if (!std::filesystem::exists(path)) {
            db.query("COMMIT");
            return;
        }
        const auto small = derivative(path, "s.jpg");
        render(path, small,
               {"-vf", "scale=" + std::to_string(config.event_snapshot_width) + ":-2", "-frames:v",
                "1"});
        auto tmp = small.parent_path() / ".lastsnaps.tmp";
        std::filesystem::remove(tmp);
        std::filesystem::create_hard_link(small, tmp);
        std::filesystem::rename(tmp, small.parent_path() / "lastsnaps.jpg");
        db.query("UPDATE cams SET snapshot=?,snapshot_small=?,last_update=from_unixtime(?),owner=? "
                 "WHERE id=?",
                 {file, small.string(), when, host, camera});
        db.query("UPDATE events SET files_time=from_unixtime(?), chat_time=NULL WHERE cam_id=? AND "
                 "cam_event_id=? AND end_time IS NULL",
                 {when, camera, key});
    } else if (kind.starts_with("timelapse-")) {
        if (kind == "timelapse-start")
            db.query("INSERT INTO timelapses(cam_id,start_time,path,status) SELECT "
                     "?,from_unixtime(?),?,'writing' WHERE NOT EXISTS (SELECT 1 FROM timelapses "
                     "WHERE cam_id=? AND path=?)",
                     {camera, when, file, camera, file});
        else
            db.query("UPDATE timelapses SET status=?,end_time=from_unixtime(?) WHERE cam_id=? AND "
                     "path=?",
                     {kind == "timelapse-end" ? "ok" : "failed", when, camera, file});
    } else {
        auto event = db.one(
            "SELECT id FROM events WHERE cam_id=? AND cam_event_id=? ORDER BY id DESC LIMIT 1",
            {camera, key});
        auto id = event["id"];
        if (kind == "event-start") {
            if (id.empty()) {
                db.query("INSERT INTO "
                         "events(cam_id,cam_event_id,event_type,start_time,owner,changed,noise) "
                         "VALUES(?,?,'motion',from_unixtime(?),?,?,?)",
                         {camera, key, when, host, std::to_string(changed), std::to_string(noise)});
                id = db.one("SELECT LAST_INSERT_ID() id").at("id");
            }
            db.query("UPDATE cams SET last_cam_event_id=?,last_event_id=? WHERE id=?",
                     {key, id, camera});
        } else {
            if (id.empty())
                throw std::runtime_error("missing event for " + kind);
            if (kind == "event-end") {
                if (!db.one("SELECT 1 FROM events WHERE id=? AND end_time=from_unixtime(?)",
                            {id, when})
                         .empty()) {
                    db.query("COMMIT");
                    return;
                }
                db.query("UPDATE events SET end_time=from_unixtime(?), chat_time=NULL WHERE id=?",
                         {when, id});
            } else if (kind == "event-picture") {
                if (!db.one("SELECT 1 FROM files WHERE event_id=? AND path=? AND "
                            "type='motion.picture'",
                            {id, file})
                         .empty()) {
                    db.query("COMMIT");
                    return;
                }
                record_file(db, id, "motion.picture", path, "ok");
                const auto thumb = derivative(path, "-thumb.jpg");
                if (std::filesystem::exists(path)) {
                    try {
                        render(path, thumb,
                               {"-vf",
                                "scale=" + std::to_string(config.event_thumbnail_width) + ":-2",
                                "-frames:v", "1"});
                        record_file(db, id, "media.thumb", thumb, "ok");
                    } catch (const std::runtime_error& error) {
                        if (stopping)
                            throw;
                        // A malformed JPEG must not strand the movie-end record.
                        Logger::instance().write(LogLevel::warning,
                                                 "event thumbnail: ", error.what());
                        record_file(db, id, "media.thumb", thumb, "failed");
                    }
                }
            } else if (kind == "movie-start" || kind == "movie-end" || kind == "movie-failed") {
                const auto status = kind == "movie-start" ? "writing"
                                    : kind == "movie-end" ? "ok"
                                                          : "failed";
                if (!db.one("SELECT 1 FROM files WHERE event_id=? AND path=? AND status=?",
                            {id, file, status})
                         .empty()) {
                    db.query("COMMIT");
                    return;
                }
                record_file(db, id, "motion.movie", path, status);
            } else
                throw std::runtime_error("unknown event action");
            db.query("INSERT INTO traces(event_id,time,message) VALUES(?,from_unixtime(?),?)",
                     {id, when, kind});
            db.query("UPDATE events SET files_time=from_unixtime(?),chat_time=NULL WHERE id=?",
                     {when, id});
        }
    }
    db.query("COMMIT");
}

bool consume(Database& db, const CameraConfig& camera, const Path& spool, const Path& root) {
    const auto dir = spool / std::to_string(camera.camera_id);
    if (!std::filesystem::exists(dir))
        return false;
    std::vector<Path> records;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".event")
            records.push_back(entry.path());
    std::sort(records.begin(), records.end());
    if (records.size() > 32)
        records.resize(32);
    const auto claimed = dir / "snapshot.pending";
    if (!std::filesystem::exists(claimed) && std::filesystem::exists(dir / "snapshot"))
        std::filesystem::rename(dir / "snapshot", claimed);
    if (std::filesystem::exists(claimed))
        records.push_back(claimed);
    for (const auto& record : records) {
        if (stopping)
            break;
        if (record == claimed && std::filesystem::exists(dir / "snapshot")) {
            std::filesystem::remove(claimed);
            continue;
        }
        ingest(db, camera, record, root);
        std::filesystem::remove(record);
    }
    return !records.empty();
}

bool convert(Database& db, const CameraConfig& camera, const Path& root) {
    const auto cam = std::to_string(camera.camera_id);
    // Older queued jobs contain shell commands. Reconstruct argv from the
    // recorded source and camera profile for both old and new work.
    auto row = db.one("SELECT e.id,f.path,c.video_options FROM events e JOIN files f ON "
                      "f.event_id=e.id JOIN cams c ON c.id=e.cam_id WHERE e.cam_id=? AND "
                      "e.end_time IS NOT NULL AND f.type='motion.movie' AND f.status='ok' AND NOT "
                      "EXISTS(SELECT 1 FROM files v WHERE v.event_id=e.id AND v.type='media.video' "
                      "AND v.status IN ('ok','failed')) ORDER BY e.id LIMIT 1",
                      {cam});
    if (row.empty())
        return false;
    const auto id = row.at("id");
    AdvisoryLock media_lock(db, "motion.convert_event." + id);
    if (!media_lock ||
        db.one("SELECT id FROM events WHERE id=? AND NOT EXISTS(SELECT 1 FROM files WHERE "
               "event_id=? AND type='media.video' AND status IN ('ok','failed'))",
               {id, id})
            .empty())
        return false;
    auto path = media_path(root, row.at("path")), output = derivative(path, "-video.mp4");
    auto profile =
        camera.event_video_options.empty() ? row.at("video_options") : camera.event_video_options;
    auto options = profile.empty() ? std::vector<std::string>{"-c:v", "libx264", "-an"}
                                   : parse_hook_command(profile);
    std::string status = "ok";
    try {
        render(path, output, options);
    } catch (const std::exception& error) {
        if (stopping)
            return false;
        Logger::instance().write(LogLevel::error, "event ", id, ": ", error.what());
        status = "failed";
    }
    db.query("START TRANSACTION");
    record_file(db, id, "media.video", output, status);
    db.query(
        "DELETE FROM jobs WHERE sender=? AND touch_event_id=? AND touch_file_type='media.video'",
        {hostname(), id});
    db.query("UPDATE events SET media_time=now(),chat_time=NULL WHERE id=?", {id});
    db.query("COMMIT");
    return true;
}

void scan_external(Database& db, const CameraConfig& camera, const Path& root,
                   const Row& settings) {
    auto now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::ostringstream day;
    day << std::put_time(&local, "%Y/%m/%d");
    auto cam = std::to_string(camera.camera_id);
    auto base = settings.at("file_path").empty() ? root : Path(settings.at("file_path"));
    auto dir = media_path(root, base / "ext" / ("cam" + cam) / day.str());
    if (!std::filesystem::is_directory(dir))
        return;
    std::string latest;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (stopping || !entry.is_regular_file() || entry.is_symlink())
            continue;
        auto file = entry.path();
        auto name = file.stem().string();
        auto first = name.find('_'),
             second = first == std::string::npos ? first : name.find('_', first + 1);
        if (second == std::string::npos)
            continue;
        auto stamp = name.substr(second + 1, 14);
        if (stamp.size() != 14 || stamp.find_first_not_of("0123456789") != std::string::npos)
            continue;
        latest = std::max(latest, stamp);
        auto event = db.one("SELECT id FROM events WHERE cam_id=? AND start_time BETWEEN "
                            "STR_TO_DATE(?,'%Y%m%d%H%i%s')-INTERVAL 10 MINUTE AND "
                            "STR_TO_DATE(?,'%Y%m%d%H%i%s')+INTERVAL 25 SECOND AND "
                            "end_time>=STR_TO_DATE(?,'%Y%m%d%H%i%s')-INTERVAL 25 SECOND ORDER BY "
                            "start_time DESC LIMIT 1",
                            {cam, stamp, stamp, stamp});
        if (event.empty())
            continue;
        auto label = "cam" + (cam.size() == 1 ? "0" + cam : cam);
        auto dest =
            media_path(root, base / "events" / label / day.str().substr(0, 7) /
                                 (label + "-" + stamp + "-ext" + file.extension().string()));
        std::filesystem::create_directories(dest.parent_path());
        // Retain the source until the database acknowledges the destination.
        if (!std::filesystem::exists(dest)) {
            const auto tmp = dest.string() + ".partial";
            std::filesystem::copy_file(file, tmp,
                                       std::filesystem::copy_options::overwrite_existing);
            std::filesystem::rename(tmp, dest);
        } else {
            if (std::filesystem::file_size(file) != std::filesystem::file_size(dest))
                continue;
            std::ifstream left(file, std::ios::binary), right(dest, std::ios::binary);
            if (!left || !right ||
                !std::equal(std::istreambuf_iterator<char>(left), {},
                            std::istreambuf_iterator<char>(right), {}))
                continue;
        }
        record_file(db, event.at("id"),
                    file.extension() == ".jpg" ? "external.picture" : "external.movie", dest, "ok");
        std::filesystem::remove(file);
    }
    if (!latest.empty())
        db.query("UPDATE cams SET last_external=STR_TO_DATE(?,'%Y%m%d%H%i%s')-INTERVAL 120 MINUTE "
                 "WHERE id=?",
                 {latest, cam});
}
} // namespace
} // namespace vibe_motion::processing

namespace vibe_motion {
int run_event_worker(const Config& config) {
    using namespace processing;
    using namespace std::chrono_literals;
    const auto spool = event_spool(config.global),
               root = std::filesystem::weakly_canonical(
                   std::filesystem::absolute(config.global.target_dir));
    std::map<int, CameraConfig> cameras;
    for (auto& c : config.cameras)
        if (c.event_processing)
            cameras.emplace(c.camera_id, c);
    for (int value : config.global.event_worker_camera_ids) {
        if (!cameras.contains(value)) {
            auto extra = config.global.camera_defaults;
            extra.camera_id = value;
            cameras.emplace(value, std::move(extra));
        }
    }
    if (cameras.empty())
        throw ConfigError("event worker has no enabled cameras");
    std::filesystem::create_directories(spool);
    const int lock = open((spool / "worker.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0660);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
        throw std::runtime_error("cannot lock event spool");
    if (mysql_library_init(0, nullptr, nullptr) ||
        curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("cannot initialize event worker libraries");
    stopping = false;
    std::signal(SIGTERM, [](int) { stopping = true; });
    std::signal(SIGINT, [](int) { stopping = true; });
    // Each lane owns its connection. A slow upload or encode cannot delay
    // recording or prevent another camera from queuing a closed movie.
    auto loop = [&](const char* name, int idle, auto work) {
        std::unique_ptr<Database> db;
        std::map<int, std::chrono::steady_clock::time_point> retry;
        while (!stopping) {
            bool busy = false;
            for (const auto& [id, camera] : cameras) {
                if (stopping || std::chrono::steady_clock::now() < retry[id])
                    continue;
                try {
                    if (!db)
                        db = std::make_unique<Database>(config.global);
                    busy = work(*db, camera) || busy;
                } catch (const std::exception& error) {
                    Logger::instance().write(LogLevel::error, name, " camera ", id, ": ",
                                             error.what());
                    db.reset();
                    retry[id] = std::chrono::steady_clock::now() + 10s;
                }
            }
            for (int i = 0; i < (busy ? 1 : idle * 10) && !stopping; ++i)
                std::this_thread::sleep_for(100ms);
        }
    };
    std::thread intake([&] {
        loop("event queue", 1,
             [&](Database& db, const CameraConfig& c) { return consume(db, c, spool, root); });
    });
    std::thread encoder([&] {
        loop("event video", 2, [&](Database& db, const CameraConfig& c) {
            return c.event_compression && convert(db, c, root);
        });
    });
    std::thread sender([&] {
        loop("telegram", 2, [&](Database& db, const CameraConfig& c) {
            return c.event_telegram &&
                   deliver(db, c, root, config.global.telegram_updates_per_minute);
        });
    });
    loop("event maintenance", 60, [&](Database& db, const CameraConfig& c) {
        auto settings = db.one("SELECT * FROM cams WHERE id=?", {std::to_string(c.camera_id)});
        if (settings.empty())
            throw std::runtime_error("camera missing from cams");
        scan_external(db, c, root, settings);
        expire_event(db, c, root, settings);
        return false;
    });
    intake.join();
    encoder.join();
    sender.join();
    mysql_library_end();
    curl_global_cleanup();
    close(lock);
    return 0;
}
} // namespace vibe_motion
