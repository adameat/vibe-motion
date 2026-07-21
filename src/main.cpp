#include "vibe_motion/config.hpp"
#include "vibe_motion/hooks.hpp"
#include "vibe_motion/log.hpp"
#include "vibe_motion/runtime.hpp"

extern "C" {
#include <libavcodec/version.h>
#include <libavformat/version.h>
#include <libavutil/version.h>
}

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<vibe_motion::Application*> application{nullptr};

void handle_signal(int signal) {
    auto* current = application.load(std::memory_order_relaxed);
    if (current == nullptr) {
        return;
    }
    if (signal == SIGHUP) {
        current->request_reload();
    } else {
        current->request_stop();
    }
}

void usage(std::ostream& output) {
    output << "usage: vibe-motion [-n] [-c FILE] [--check-config] [--strict-config] "
              "[--dump-effective-config]\n"
              "       vibe-motion --version\n";
}

} // namespace

int main(int argc, char** argv) try {
    if (argc == 4 && std::string(argv[1]) == "--hook-supervisor") {
        std::size_t consumed = 0;
        const int socket = std::stoi(argv[2], &consumed);
        if (consumed != std::string(argv[2]).size() || socket < 0 || argv[3][0] == '\0') {
            throw std::runtime_error("invalid hook supervisor arguments");
        }
        if (const char* delay = std::getenv("VIBE_HOOK_SUPERVISOR_START_DELAY_MS")) {
            const int milliseconds = std::stoi(delay);
            if (milliseconds > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            }
        }
        return vibe_motion::run_hook_supervisor(socket, argv[3]);
    }

    std::filesystem::path config_path = "/etc/motion/motion.conf";
    bool foreground = false;
    bool check_config = false;
    bool dump_config = false;
    bool strict_config = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "-c" || argument == "--config") && index + 1 < argc) {
            config_path = argv[++index];
        } else if (argument == "-n" || argument == "--foreground") {
            foreground = true;
        } else if (argument == "--check-config") {
            check_config = true;
        } else if (argument == "--dump-effective-config") {
            dump_config = true;
        } else if (argument == "--strict-config") {
            strict_config = true;
        } else if (argument == "--version") {
            std::cout << "vibe-motion 0.1.0 (libavformat " << LIBAVFORMAT_VERSION_MAJOR
                      << ", libavcodec " << LIBAVCODEC_VERSION_MAJOR << ", libavutil "
                      << LIBAVUTIL_VERSION_MAJOR << ")\n";
            return 0;
        } else if (argument == "-h" || argument == "--help") {
            usage(std::cout);
            return 0;
        } else {
            throw std::runtime_error("unknown or incomplete argument: " + argument);
        }
    }

    auto config = vibe_motion::load_config(config_path, {true, strict_config});
    config.validate();
    for (const auto& warning : config.warnings) {
        vibe_motion::Logger::instance().write(vibe_motion::LogLevel::warning, warning);
    }
    if (dump_config) {
        std::cout << config.dump_effective(true);
    }
    if (check_config || dump_config) {
        std::cout << "configuration OK: " << config.cameras.size() << " camera(s)\n";
        return 0;
    }

    if (!foreground && config.global.daemon) {
        if (::daemon(0, 0) != 0) {
            throw std::runtime_error("failed to enter daemon mode");
        }
    }

    const auto configured_level = config.global.log_level >= 7   ? vibe_motion::LogLevel::debug
                                  : config.global.log_level >= 5 ? vibe_motion::LogLevel::info
                                  : config.global.log_level >= 3 ? vibe_motion::LogLevel::warning
                                                                 : vibe_motion::LogLevel::error;
    vibe_motion::Logger::instance().set_level(configured_level);
    if (!config.global.log_file.empty()) {
        vibe_motion::Logger::instance().set_file(config.global.log_file);
    }

    vibe_motion::Application app(config_path, std::move(config));
    application.store(&app, std::memory_order_relaxed);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP, handle_signal);
    const int result = app.run();
    application.store(nullptr, std::memory_order_relaxed);
    return result;
} catch (const std::exception& error) {
    std::cerr << "vibe-motion: " << error.what() << '\n';
    return 2;
}
