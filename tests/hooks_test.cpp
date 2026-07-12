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
    const auto second_output = output.string() + "-second";
    std::ifstream original_name_input("/proc/self/comm");
    std::string original_process_name;
    original_name_input >> original_process_name;
    HookExecutor executor({1, 4, 5s, 100ms, "motion"});
    HookResult first_result;
    const std::string shell = "printf '%s ' \"$PPID\" > '" + output.string() +
                              "'; cat /proc/$PPID/comm >> '" + output.string() + "'";
    assert(executor.submit(std::vector<std::string>{"/bin/sh", "-c", shell},
                           [&](const HookResult& result) { first_result = result; }));
    assert(executor.wait_idle(5s));
    const std::string second_shell = "printf '%s ' \"$PPID\" > '" + second_output +
                                     "'; cat /proc/$PPID/comm >> '" + second_output + "'";
    assert(executor.submit(std::vector<std::string>{"/bin/sh", "-c", second_shell}));
    assert(executor.wait_idle(5s));
    std::ifstream input(output);
    std::ifstream second_input(second_output);
    pid_t parent_pid = -1;
    pid_t second_parent_pid = -1;
    std::string parent_name;
    std::string second_parent_name;
    input >> parent_pid >> parent_name;
    second_input >> second_parent_pid >> second_parent_name;
    std::filesystem::remove(output);
    std::filesystem::remove(second_output);
    assert(first_result.exit_code == 0 && first_result.exec_errno == 0);
    assert(parent_pid > 0 && parent_pid == second_parent_pid);
    assert(parent_name == "motion" && second_parent_name == "motion");
    executor.stop();
    std::ifstream final_name_input("/proc/self/comm");
    std::string final_process_name;
    final_name_input >> final_process_name;
    assert(!original_process_name.empty() && final_process_name == original_process_name);

    std::cout << "hook tests passed\n";
}
