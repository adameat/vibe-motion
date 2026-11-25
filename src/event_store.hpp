#pragma once
#include "event_processing.hpp"
#include <atomic>
#include <memory>
#include <mysql.h>
#include <string_view>

namespace vibe_motion::processing {
using Row = std::map<std::string, std::string>;
using Path = std::filesystem::path;
extern std::atomic<bool> stopping;

// Bind string values as MySQL hex string literals. The SQL templates below are
// internal constants, with one '?' per value and no question marks in literals.
class Database {
  public:
    explicit Database(const GlobalConfig& config);
    std::vector<Row> query(std::string_view sql, std::initializer_list<std::string> values = {});
    Row one(std::string_view sql, std::initializer_list<std::string> values = {});

  private:
    std::unique_ptr<MYSQL, decltype(&mysql_close)> connection_{mysql_init(nullptr), mysql_close};
};
class AdvisoryLock {
  public:
    AdvisoryLock(Database& db, std::string name)
        : db_(db), name_(std::move(name)),
          held_(db_.one("SELECT GET_LOCK(?,0) ok", {name_}).at("ok") == "1") {}
    ~AdvisoryLock() {
        if (held_)
            try {
                db_.query("SELECT RELEASE_LOCK(?)", {name_});
            } catch (...) {
            }
    }
    AdvisoryLock(const AdvisoryLock&) = delete;
    AdvisoryLock& operator=(const AdvisoryLock&) = delete;
    explicit operator bool() const {
        return held_;
    }

  private:
    Database& db_;
    std::string name_;
    bool held_;
};

Path media_path(const Path& root, const Path& path);
Path derivative(Path source, const std::string& suffix);
void render(const Path& input, const Path& output, const std::vector<std::string>& options);
void record_file(Database& db, const std::string& id, const std::string& type, const Path& path,
                 const std::string& status);
bool deliver(Database& db, const CameraConfig& camera, const Path& root, int limit);
void expire_event(Database& db, const CameraConfig& camera, const Path& root, const Row& settings);
} // namespace vibe_motion::processing
