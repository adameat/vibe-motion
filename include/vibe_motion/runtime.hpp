#pragma once

#include "vibe_motion/config.hpp"

#include <atomic>
#include <filesystem>
#include <memory>

namespace vibe_motion {

class Application {
  public:
    Application(std::filesystem::path config_path, Config config);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();
    void request_stop() noexcept;
    void request_reload() noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibe_motion
