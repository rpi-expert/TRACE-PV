#include "reporting/console_output.h"

#include <cassert>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

int main() {
    std::ostringstream output;
    std::ostringstream errors;
    auto* old_output = std::cout.rdbuf(output.rdbuf());
    auto* old_errors = std::cerr.rdbuf(errors.rdbuf());
    {
        tracepv::reporting::ConsoleOutput console(false);
        std::cout << "GPU Configuration:\n  Available GPUs: 2\n";
        std::cout << "TRACEPV_PROGRESS iteration=" << 1 << std::flush;
        std::cerr << "Warning: diagnostic between marker fragments\n  Keep this detail\n";
        std::cout << " round=1/2 pass=1 processed=10 total=10\n";
        std::cout << "Mission Profile Iteration #1\nStarting Round 1/2\nRound 1/2 finished\n";
        std::cout << "Mission Profile Iteration #1 Complete\n";
        std::cout << "  Internal temp range: [20, 30] C\n  Internal RH range: [20, 70] %\n";
        std::cout << "  Execution Time (Wall Clock): 1.0 s\nFailed Component: none\n";
        std::cout << "Total Mission Profile Iterations: 1\nTotal Cases per Iteration: 10\nSimulation Complete\n";
        std::cout << "Warning: stdout warning\n  Important continuation\nAnother detail\n";
        tracepv::reporting::debug_output() << "DEBUG: hidden\n  Hidden continuation\n";
        console.debug() << std::setprecision(1) << 3.14159 << '\n';
        console.summary() << "Selected lifetime export\n";
        std::cerr << "ERROR: visible\n  Error continuation\n";
        std::cout << "TRACEPV_TRAILING no_newline=1";
    }
    const std::string quiet = output.str();
    assert(quiet.find("GPU Configuration") == std::string::npos);
    assert(quiet.find("Available GPUs") == std::string::npos);
    assert(quiet.find("Another detail") == std::string::npos);
    assert(quiet.find("TRACEPV_PROGRESS iteration=1 round=1/2 pass=1 processed=10 total=10\n") != std::string::npos);
    assert(quiet.find("Mission Profile Iteration #1 Complete") != std::string::npos);
    assert(quiet.find("Starting Round 1/2") != std::string::npos);
    assert(quiet.find("Round 1/2 finished") != std::string::npos);
    assert(quiet.find("Internal temp range") != std::string::npos);
    assert(quiet.find("Internal RH range") != std::string::npos);
    assert(quiet.find("Execution Time (Wall Clock)") != std::string::npos);
    assert(quiet.find("Selected lifetime export") != std::string::npos);
    assert(quiet.find("Important continuation") != std::string::npos);
    assert(quiet.find("TRACEPV_TRAILING no_newline=1") != std::string::npos);
    assert(errors.str().find("DEBUG:") == std::string::npos);
    assert(errors.str().find("Hidden continuation") == std::string::npos);
    assert(errors.str().find("Keep this detail") != std::string::npos);
    assert(errors.str().find("ERROR: visible\n  Error continuation") != std::string::npos);
    {
        tracepv::reporting::ConsoleOutput console(true);
        std::cout << "Verbose details\n";
        console.debug() << "DEBUG: visible\n  Visible continuation\n";
    }
    assert(output.str().find("Verbose details") != std::string::npos);
    assert(errors.str().find("DEBUG: visible\n  Visible continuation") != std::string::npos);

    // Partial lines from different workers never share filtering/continuation state.
    {
        tracepv::reporting::ConsoleOutput console(false);
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([worker]() {
                for (int sample = 0; sample < 20; ++sample) {
                    std::cout << "TRACEPV_WORKER id=" << worker << " sample=" << sample << '\n';
                    tracepv::reporting::debug_output() << std::fixed << std::setprecision(worker) << 2.5 << '\n';
                }
            });
        }
        for (auto& worker : workers) worker.join();
    }
    const std::string threaded = output.str();
    for (int worker = 0; worker < 4; ++worker) {
        for (int sample = 0; sample < 20; ++sample) {
            assert(threaded.find("TRACEPV_WORKER id=" + std::to_string(worker) +
                                 " sample=" + std::to_string(sample) + "\n") != std::string::npos);
        }
    }
    assert(tracepv::reporting::verbose_output_enabled().load());
    std::cout.rdbuf(old_output);
    std::cerr.rdbuf(old_errors);
}
