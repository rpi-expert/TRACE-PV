#pragma once

#include <atomic>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>
#include <unordered_map>

namespace tracepv::reporting {

// Preserve historical diagnostics until the command-line options are parsed.
inline std::atomic<bool>& verbose_output_enabled() {
    static std::atomic<bool> enabled{true};
    return enabled;
}

class NullBuffer final : public std::streambuf {
protected:
    int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
};

inline std::ostream& debug_output() {
    if (verbose_output_enabled().load(std::memory_order_relaxed)) {
        return std::cerr;
    }
    // Each worker has its own formatting state; the sink never shares a put area.
    thread_local NullBuffer buffer;
    thread_local std::ostream sink(&buffer);
    return sink;
}

// Filter complete stdout lines only. stderr is deliberately never redirected:
// warnings, errors and their continuation lines must remain visible in all modes.
class SummaryOutputBuffer final : public std::streambuf {
public:
    explicit SummaryOutputBuffer(std::streambuf* destination) : destination_(destination) {}

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : pending_) {
            emit(entry.second);
        }
        destination_->pubsync();
    }

protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        const char value = traits_type::to_char_type(ch);
        return xsputn(&value, 1) == 1 ? ch : traits_type::eof();
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = pending_[std::this_thread::get_id()];
        for (std::streamsize i = 0; i < count; ++i) {
            state.line.push_back(data[i]);
            if (data[i] == '\n') {
                emit(state);
            }
        }
        return count;
    }

    int sync() override {
        std::lock_guard<std::mutex> lock(mutex_);
        // cerr is tied to cout and flushes it before every insertion. Retain
        // incomplete stdout lines until newline (or finish), so a split marker
        // remains recognizable even when another stream is flushed mid-line.
        return destination_->pubsync();
    }

private:
    struct PendingLine {
        std::string line;
        bool warning_continuation = false;
    };

    static bool starts_with(const std::string& text, const char* prefix) {
        return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
    }

    static bool is_gui_marker(const std::string& text) {
        // Both atomic progress records and legacy context are used by webui/server.py.
        return starts_with(text, "TRACEPV_") ||
               starts_with(text, "Mission Profile Iteration #") ||
               starts_with(text, "Starting Round ") ||
               (starts_with(text, "Round ") && text.find(" finished") != std::string::npos) ||
               starts_with(text, "Failed Component:") ||
               starts_with(text, "Total Mission Profile Iterations:") ||
               starts_with(text, "Total Cases per Iteration:") ||
               starts_with(text, "Execution Time (Wall Clock):") ||
               starts_with(text, "Internal temp range:") ||
               starts_with(text, "Internal RH range:") ||
               starts_with(text, "Simulation Complete") ||
               starts_with(text, "SIMULATION STOPPED:") ||
               starts_with(text, "DEGRADATION LIMIT REACHED") ||
               starts_with(text, "FAILURE DETECTED:");
    }

    void emit(PendingLine& state) {
        if (state.line.empty()) {
            return;
        }
        const auto first = state.line.find_first_not_of(" \t\r\n");
        const std::string text = first == std::string::npos ? "" : state.line.substr(first);
        const bool warning = text.find("Warning:") != std::string::npos ||
                             text.find("WARNING:") != std::string::npos ||
                             starts_with(text, "Error:") || starts_with(text, "ERROR:");
        const bool continuation = state.warning_continuation && first != 0;
        if (warning || continuation || is_gui_marker(text)) {
            destination_->sputn(state.line.data(), static_cast<std::streamsize>(state.line.size()));
        }
        state.warning_continuation = warning || continuation;
        state.line.clear();
    }

    std::streambuf* destination_;
    std::mutex mutex_;
    std::unordered_map<std::thread::id, PendingLine> pending_;
};

// Construct after option parsing and keep alive until all workers have joined.
// summary() explicitly bypasses the filter for selected exports/configuration.
class ConsoleOutput final {
public:
    explicit ConsoleOutput(bool verbose)
        : original_(std::cout.rdbuf()), summary_(original_), filter_(original_),
          previous_verbose_(verbose_output_enabled().exchange(verbose)), filtered_(!verbose) {
        std::cout.flush();
        if (filtered_) {
            std::cout.rdbuf(&filter_);
        }
    }

    ~ConsoleOutput() {
        if (filtered_) {
            std::cout.flush();
            filter_.finish();
            std::cout.rdbuf(original_);
        }
        summary_.flush();
        verbose_output_enabled().store(previous_verbose_);
    }

    ConsoleOutput(const ConsoleOutput&) = delete;
    ConsoleOutput& operator=(const ConsoleOutput&) = delete;
    std::ostream& summary() { return summary_; }
    std::ostream& debug() { return debug_output(); }

private:
    std::streambuf* original_;
    std::ostream summary_;
    SummaryOutputBuffer filter_;
    bool previous_verbose_;
    bool filtered_;
};

}  // namespace tracepv::reporting
