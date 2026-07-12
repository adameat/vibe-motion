#include "vibe_motion/hooks.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace vibe_motion;
using namespace std::chrono_literals;

int main() {
    const auto parsed = parse_hook_command("/bin/tool one \"two words\" 'three words'");
    assert(parsed.size() == 4);
    assert(parsed[2] == "two words" && parsed[3] == "three words");
    assert(expand_hook_tokens("file=%f %% %x", {{"f", "a b"}}) == "file=a b % %x");

    const auto output =
        std::filesystem::temp_directory_path() / ("vibe-motion-hook-" + std::to_string(::getpid()));
    HookExecutor executor({1, 4, 5s, 100ms, "motion"});
    const std::string shell = "cat /proc/$PPID/comm > '" + output.string() + "'";
    assert(executor.submit(std::vector<std::string>{"/bin/sh", "-c", shell}));
    assert(executor.wait_idle(5s));
    std::ifstream input(output);
    std::string parent_name;
    input >> parent_name;
    std::filesystem::remove(output);
    assert(parent_name == "motion");
    executor.stop();

    std::cout << "hook tests passed\n";
}
