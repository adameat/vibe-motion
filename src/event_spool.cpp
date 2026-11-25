#include "event_processing.hpp"

#include <array>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <sys/random.h>
#include <unistd.h>

namespace vibe_motion {
std::filesystem::path event_spool(const GlobalConfig& config) {
    return std::filesystem::absolute(config.event_spool_dir.empty()
                                         ? config.target_dir / ".vibe-events"
                                         : config.event_spool_dir);
}

std::string new_event_key() {
    std::array<unsigned char, 16> random{};
    if (::getrandom(random.data(), random.size(), 0) != static_cast<ssize_t>(random.size()))
        throw std::runtime_error("cannot generate event identifier");
    std::ostringstream out;
    for (auto byte : random)
        out << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

void queue_event_action(const std::filesystem::path& spool, const std::string& kind,
                        const std::string& key, const ExpansionContext& context,
                        std::chrono::system_clock::time_point when) {
    static std::atomic<unsigned long long> sequence{};
    static const auto instance = new_event_key();
    static const auto started = std::chrono::system_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << started << '-' << instance << '-' << std::setw(20) << std::setfill('0') << sequence++;
    const auto dir = spool / std::to_string(context.camera_id);
    std::filesystem::create_directories(dir);
    // Snapshots rotate on disk. Only the newest waiting snapshot is relevant.
    const auto dest = dir / (kind == "snapshot" ? "snapshot" : name.str() + ".event");
    const auto tmp = dir / (name.str() + ".tmp");
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(tmp);
    out << kind << ' ' << std::quoted(key) << ' ' << std::chrono::system_clock::to_time_t(when)
        << ' ' << context.changed_pixels << ' ' << context.noise_level << ' '
        << std::quoted(context.filename.empty()
                           ? std::string{}
                           : std::filesystem::absolute(context.filename).string())
        << '\n';
    out.close();
    std::filesystem::rename(tmp, dest);
}
} // namespace vibe_motion
