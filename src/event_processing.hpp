#pragma once
#include "vibe_motion/config.hpp"

namespace vibe_motion {

// The capture process only publishes completed filesystem records. A separate
// instance of the same binary owns the database, encoder and network requests.
std::filesystem::path event_spool(const GlobalConfig& config);
std::string new_event_key();
void queue_event_action(const std::filesystem::path& spool, const std::string& kind,
                        const std::string& key, const ExpansionContext& context,
                        std::chrono::system_clock::time_point when);
int run_event_worker(const Config& config);
} // namespace vibe_motion
