#include "event_store.hpp"
#include "vibe_motion/log.hpp"

#include <algorithm>
#include <curl/curl.h>
#include <fstream>
#include <iomanip>
#include <json-c/json.h>
#include <sstream>

namespace vibe_motion::processing {
namespace {
using Params = std::map<std::string, std::string>;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

std::string json(const Params& params) {
    Json object(json_object_new_object(), json_object_put);
    if (!object)
        throw std::bad_alloc();
    for (const auto& [key, value] : params)
        json_object_object_add(object.get(), key.c_str(), json_object_new_string(value.c_str()));
    return json_object_to_json_string_ext(object.get(), JSON_C_TO_STRING_PLAIN);
}

size_t receive(char* data, size_t size, size_t count, void* user) {
    auto& buffer = *static_cast<std::string*>(user);
    auto bytes = size * count;
    if (bytes > 1024 * 1024 - buffer.size())
        return 0;
    buffer.append(data, bytes);
    return bytes;
}
int progress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return stopping ? 1 : 0;
}

struct Reply {
    long status{};
    bool ok{};
    std::string message;
    int retry_after{};
    std::string description;
};

Reply request(Database& db, const CameraConfig& camera, const Row& settings, const std::string& id,
              const std::string& method, Params params, const Path& file = {},
              const char* attachment = nullptr) {
    std::string token = settings.at("bot_id");
    if (!camera.telegram_token_file.empty()) {
        std::ifstream in(camera.telegram_token_file);
        if (!(in >> token))
            throw std::runtime_error("cannot read telegram_token_file");
    }
    if (token.starts_with("bot"))
        token.erase(0, 3);
    if (token.empty() || token.find_first_not_of(
                             "0123456789:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") !=
                             std::string::npos)
        throw std::runtime_error("invalid Telegram bot token");
    params["chat_id"] =
        camera.telegram_chat_id.empty() ? settings.at("chat_id") : camera.telegram_chat_id;
    if (params["chat_id"].empty())
        throw std::runtime_error("missing Telegram chat ID");
    const auto url = camera.telegram_api_url + "/bot" + token + "/" + method;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::runtime_error("cannot initialize Telegram HTTP client");
    auto* handle = curl.get();
    std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(curl_mime_init(handle),
                                                               curl_mime_free);
    if (!mime)
        throw std::bad_alloc();
    for (const auto& [key, value] : params) {
        auto* part = curl_mime_addpart(mime.get());
        if (!part || curl_mime_name(part, key.c_str()) ||
            curl_mime_data(part, value.data(), value.size()))
            throw std::runtime_error("cannot create Telegram form");
    }
    if (attachment) {
        auto* part = curl_mime_addpart(mime.get());
        if (!part || curl_mime_name(part, attachment) || curl_mime_filedata(part, file.c_str()))
            throw std::runtime_error("cannot attach event media");
    }
    std::string body;
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_MIMEPOST, mime.get());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, attachment ? 120L : 30L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress);
    if (curl_easy_perform(handle) != CURLE_OK)
        throw std::runtime_error("Telegram " + method + " transport error");
    Reply reply;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &reply.status);
    Json response(json_tokener_parse(body.c_str()), json_object_put);
    auto field = [&](const char* key) { return json_object_object_get(response.get(), key); };
    reply.ok = reply.status == 200 && json_object_get_boolean(field("ok"));
    if (auto* value = json_object_object_get(field("result"), "message_id"))
        reply.message = json_object_get_string(value);
    if (auto* value = field("description"))
        reply.description = json_object_get_string(value);
    reply.retry_after =
        json_object_get_int(json_object_object_get(field("parameters"), "retry_after"));
    db.query("INSERT INTO responses(time,event_id,command,parameters,status_code,reason,content) "
             "VALUES(now(),?,?,?,?,?,?)",
             {id, method, json(params), std::to_string(reply.status), reply.ok ? "OK" : "error",
              response ? body : json({{"error", "invalid JSON response"}})});
    return reply;
}

std::string caption(const Row& event, const CameraConfig& camera, const Row& settings,
                    const Path& video) {
    using namespace std::chrono;
    auto start = sys_seconds{seconds{std::stoll(event.at("start_epoch"))}};
    const auto zone = camera.event_timezone.empty() ? settings.at("tz") : camera.event_timezone;
    const auto* tz = zone.empty() ? current_zone() : locate_zone(zone);
    auto local = tz->to_local(start), today = tz->to_local(floor<seconds>(system_clock::now()));
    auto day = floor<days>(local);
    auto elapsed = std::max<int64_t>(0, std::stoll(event.at("duration")));
    bool live = event.at("end_time").empty();
    std::ostringstream out;
    if (live)
        out << "🔴 *LIVE*\n";
    out << "#cam" << camera.camera_id << '\n';
    if (day != floor<days>(today))
        out << year_month_day{day} << ' ';
    out << hh_mm_ss{local - day} << " / " << elapsed / 60 << ':' << std::setfill('0')
        << std::setw(2) << elapsed % 60;
    if (!video.empty()) {
        double bytes = static_cast<double>(std::filesystem::file_size(video));
        for (const auto* unit : {"B", "KB", "MB", "GB", "TB"}) {
            if (bytes < 1024 || std::string_view(unit) == "TB") {
                out << " / " << std::fixed << std::setprecision(1) << bytes << unit;
                break;
            }
            bytes /= 1024;
        }
    }
    out << " / " << event.at("changed") << " / " << event.at("noise");
    if (live)
        out << ' ' << event.at("id");
    else if (!camera.event_link_prefix.empty())
        out << '\n' << camera.event_link_prefix << event.at("id");
    return out.str();
}

// Telegram's retry_after applies across cameras sharing a bot. The sender
// lane observes the longest requested pause before attempting another upload.
auto next_request = std::chrono::steady_clock::time_point{};
void check_reply(const Reply& reply) {
    if (reply.status == 429)
        next_request =
            std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, reply.retry_after));
}
} // namespace

bool deliver(Database& db, const CameraConfig& camera, const Path& root, int limit) {
    if (std::chrono::steady_clock::now() < next_request)
        return false;
    auto cam = std::to_string(camera.camera_id);
    const auto settings = db.one("SELECT * FROM cams WHERE id=?", {cam});
    if (settings.empty())
        throw std::runtime_error("camera missing from cams");
    // Pending replacements survive a restart between sendVideo and deleteMessage.
    auto pending =
        db.one("SELECT t.event_id id,t.message FROM traces t JOIN events e ON e.id=t.event_id "
               "WHERE e.cam_id=? AND t.message LIKE 'telegram-delete %' ORDER BY t.time LIMIT 1",
               {cam});
    if (!pending.empty()) {
        AdvisoryLock lock(db, "motion.send_event." + pending.at("id"));
        if (!lock)
            return false;
        auto reply = request(db, camera, settings, pending.at("id"), "deleteMessage",
                             {{"message_id", pending.at("message").substr(16)}});
        check_reply(reply);
        if (reply.ok || reply.status == 400 || reply.status == 403)
            db.query("DELETE FROM traces WHERE event_id=? AND message=?",
                     {pending.at("id"), pending.at("message")});
        return reply.ok;
    }
    if (limit > 0) {
        auto count = db.one("SELECT COUNT(*) n FROM responses WHERE time>=now()-INTERVAL 1 MINUTE");
        if (std::stoi(count.at("n")) >= limit)
            return false;
    }
    auto candidate = db.one(
        "SELECT e.id FROM events e JOIN cams c ON c.id=e.cam_id WHERE e.cam_id=? AND (e.end_time "
        "IS NOT NULL OR (c.last_event_id=e.id AND c.snapshot_small IS NOT NULL)) AND (e.chat_time "
        "IS NULL OR e.chat_time<e.files_time OR e.chat_time<e.end_time OR "
        "e.chat_time<e.media_time) AND NOT EXISTS(SELECT 1 FROM jobs j WHERE "
        "j.touch_event_id=e.id) ORDER BY (e.end_time IS NULL) DESC,e.id LIMIT 1",
        {cam});
    if (candidate.empty())
        return false;
    auto id = candidate.at("id");
    AdvisoryLock lock(db, "motion.send_event." + id);
    if (!lock)
        return false;
    auto event = db.one("SELECT *,CAST(UNIX_TIMESTAMP(start_time) AS SIGNED) "
                        "start_epoch,TIMESTAMPDIFF(SECOND,start_time,COALESCE(end_time,now())) "
                        "duration,now() send_time FROM events WHERE id=?",
                        {id});
    if (event.empty())
        return false;
    const bool live = event.at("end_time").empty();
    auto old = event.at("message_id");
    Path photo, video;
    if (live) {
        if (!settings.at("snapshot_small").empty())
            photo = media_path(root, settings.at("snapshot_small"));
    } else {
        for (auto& row :
             db.query("SELECT path,type FROM files WHERE event_id=? AND status='ok'", {id})) {
            if (camera.event_compression && row.at("type") == "media.video")
                video = media_path(root, row.at("path"));
            if (row.at("type") == "motion.picture")
                photo = media_path(root, row.at("path"));
        }
    }
    if (!video.empty() && !std::filesystem::is_regular_file(video)) {
        db.query("UPDATE files SET status='converting' WHERE event_id=? AND type='media.video'",
                 {id});
        return false;
    }
    const auto acknowledge = [&] {
        db.query("UPDATE events SET chat_time=? WHERE id=? AND COALESCE(files_time,'')=? AND "
                 "COALESCE(media_time,'')=? AND COALESCE(end_time,'')=?",
                 {event.at("send_time"), id, event.at("files_time"), event.at("media_time"),
                  event.at("end_time")});
    };
    if (video.empty() && (photo.empty() || !std::filesystem::is_regular_file(photo))) {
        acknowledge();
        return false;
    }
    bool replace = old.empty() || !video.empty();
    if (live && !replace) {
        const auto chat =
            camera.telegram_chat_id.empty() ? settings.at("chat_id") : camera.telegram_chat_id;
        replace = !db.one("SELECT 1 FROM responses WHERE command='sendVideo' AND status_code='200' "
                          "AND JSON_UNQUOTE(JSON_EXTRACT(parameters,'$.chat_id'))=? AND id>(SELECT "
                          "COALESCE(MAX(id),0) FROM responses WHERE event_id=? AND "
                          "command='sendPhoto' AND status_code='200') LIMIT 1",
                          {chat, id})
                       .empty();
    }
    const auto type = video.empty() ? "photo" : "video";
    auto text = caption(event, camera, settings, video);
    Params params;
    if (replace)
        params = {{"caption", text}, {"parse_mode", "Markdown"}};
    else
        params = {{"message_id", old},
                  {"media", json({{"type", type},
                                  {"media", std::string("attach://") + type},
                                  {"caption", text},
                                  {"parse_mode", "Markdown"}})}};
    auto reply = request(db, camera, settings, id,
                         replace ? (video.empty() ? "sendPhoto" : "sendVideo") : "editMessageMedia",
                         params, video.empty() ? photo : video, type);
    check_reply(reply);
    if (reply.ok && !reply.message.empty()) {
        db.query("START TRANSACTION");
        db.query("UPDATE events SET message_id=? WHERE id=?", {reply.message, id});
        if (replace && !old.empty() && old != reply.message)
            db.query("INSERT INTO traces(event_id,time,message) VALUES(?,now(),?)",
                     {id, "telegram-delete " + old});
        acknowledge();
        db.query("COMMIT");
        return true;
    }
    if (reply.status == 400 && !replace &&
        reply.description.find("message to edit not found") != std::string::npos)
        db.query("UPDATE events SET message_id=NULL WHERE id=?", {id});
    else if (reply.status >= 400 && reply.status < 500 && reply.status != 429)
        acknowledge();
    return false;
}

void expire_event(Database& db, const CameraConfig& camera, const Path& root, const Row& settings) {
    const auto days = camera.event_retention_days < 0 ? settings.at("max_event_days")
                                                      : std::to_string(camera.event_retention_days);
    if (days.empty() || std::stoi(days) <= 0)
        return;
    const auto cam = std::to_string(camera.camera_id);
    const std::string expired =
        "SELECT e.id,e.message_id FROM events e WHERE e.cam_id=? AND "
        "e.start_time<now()-INTERVAL ? DAY AND e.end_time IS NOT NULL AND "
        "NOT EXISTS(SELECT 1 FROM files f WHERE f.event_id=e.id AND f.status='writing')";
    auto event = db.one(expired + " ORDER BY e.start_time LIMIT 1", {cam, days});
    if (event.empty())
        return;
    auto id = event.at("id");
    AdvisoryLock lock(db, "motion.send_event." + id);
    if (!lock)
        return;
    AdvisoryLock media_lock(db, "motion.convert_event." + id);
    if (!media_lock)
        return;
    event = db.one(expired + " AND e.id=?", {cam, days, id});
    if (event.empty())
        return;
    if (camera.event_telegram) {
        auto pending = db.query("SELECT SUBSTRING(message,17) message_id FROM traces WHERE "
                                "event_id=? AND message LIKE 'telegram-delete %'",
                                {id});
        pending.push_back(event);
        for (const auto& previous : pending) {
            if (previous.at("message_id").empty())
                continue;
            auto reply = request(db, camera, settings, id, "deleteMessage",
                                 {{"message_id", previous.at("message_id")}});
            if (!reply.ok && reply.status != 400 && reply.status != 403)
                return;
        }
    }
    for (auto& row : db.query("SELECT path FROM files WHERE event_id=?", {id})) {
        const auto path = media_path(root, row.at("path"));
        if (!std::filesystem::is_directory(path))
            std::filesystem::remove(path);
    }
    db.query("START TRANSACTION");
    db.query("DELETE FROM jobs WHERE touch_event_id=?", {id});
    db.query("DELETE FROM traces WHERE event_id=?", {id});
    db.query("DELETE FROM responses WHERE event_id=?", {id});
    db.query("DELETE FROM files WHERE event_id=?", {id});
    db.query("DELETE FROM events WHERE id=?", {id});
    db.query("COMMIT");
}
} // namespace vibe_motion::processing
