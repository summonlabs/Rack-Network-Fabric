// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal structured logger. Writes to a caller supplied sink; produces no
// network traffic of any kind (the runtime never transmits telemetry).

#ifndef RNF_CORE_LOG_HPP
#define RNF_CORE_LOG_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace rnf {

enum class LogLevel : std::uint8_t { kTrace = 0, kDebug, kInfo, kWarn, kError, kOff };

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

/// Parse "trace", "debug", "info", "warn", "error", "off".
[[nodiscard]] bool parse_log_level(std::string_view text, LogLevel& out) noexcept;

/// Process-wide sink. The default sink writes one line per record to stderr.
/// Installing a sink is not thread safe with concurrent logging; install it
/// once during start-up.
class Logger {
public:
    using Sink = std::function<void(LogLevel, std::string_view)>;

    static Logger& instance();

    void set_level(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }
    [[nodiscard]] LogLevel level() const noexcept { return level_.load(std::memory_order_relaxed); }
    void set_sink(Sink sink);
    void log(LogLevel level, std::string_view message);

private:
    Logger() = default;
    std::atomic<LogLevel> level_{LogLevel::kInfo};
    std::mutex sink_mutex_;
    Sink sink_;
};

void log_message(LogLevel level, std::string_view message);

#define RNF_LOG(level, message) ::rnf::log_message((level), (message))

}  // namespace rnf

#endif  // RNF_CORE_LOG_HPP
