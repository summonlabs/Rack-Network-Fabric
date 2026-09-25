// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/core/log.hpp"

#include <cstdio>
#include <utility>

namespace rnf {

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kTrace: return "trace";
        case LogLevel::kDebug: return "debug";
        case LogLevel::kInfo: return "info";
        case LogLevel::kWarn: return "warn";
        case LogLevel::kError: return "error";
        case LogLevel::kOff: return "off";
    }
    return "unknown";
}

bool parse_log_level(std::string_view text, LogLevel& out) noexcept {
    if (text == "trace") { out = LogLevel::kTrace; return true; }
    if (text == "debug") { out = LogLevel::kDebug; return true; }
    if (text == "info") { out = LogLevel::kInfo; return true; }
    if (text == "warn") { out = LogLevel::kWarn; return true; }
    if (text == "error") { out = LogLevel::kError; return true; }
    if (text == "off") { out = LogLevel::kOff; return true; }
    return false;
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_sink(Sink sink) {
    const std::lock_guard<std::mutex> guard(sink_mutex_);
    sink_ = std::move(sink);
}

void Logger::log(LogLevel level, std::string_view message) {
    if (level < level_.load(std::memory_order_relaxed)) {
        return;
    }
    Sink local;
    {
        const std::lock_guard<std::mutex> guard(sink_mutex_);
        local = sink_;
    }
    // The sink is copied out and invoked outside the lock so that a sink which
    // itself logs cannot deadlock on sink_mutex_.
    if (local) {
        local(level, message);
    } else {
        std::string line(to_string(level));
        line += ": ";
        line.append(message.data(), message.size());
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }
}

void log_message(LogLevel level, std::string_view message) {
    Logger::instance().log(level, message);
}

}  // namespace rnf
