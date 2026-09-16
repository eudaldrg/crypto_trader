// Minimal operational logging for the feed handler binaries: one timestamped
// line per lifecycle event on stderr.
//
// Deliberately tiny rather than a logging library: v1 only needs "what is this
// process doing right now" on a terminal, and nothing on the per-message hot
// path logs at all. Nothing here is ever handed a credential -- API keys, WS
// tokens and raw outbound payloads never reach a log line (exchanges/kraken.md).
#pragma once

#include <cstdint>
#include <string_view>

namespace feed_handler {

enum class LogLevel : std::uint8_t { kInfo, kWarn, kError };

/// Writes one line to stderr: "<UTC timestamp> <level> <message>". Serialized
/// with a mutex, so the WS thread and the watchdog thread cannot interleave
/// half-lines.
void LogMessage(LogLevel level, std::string_view message);

inline void LogInfo(std::string_view message) {
    LogMessage(LogLevel::kInfo, message);
}

inline void LogWarn(std::string_view message) {
    LogMessage(LogLevel::kWarn, message);
}

inline void LogError(std::string_view message) {
    LogMessage(LogLevel::kError, message);
}

}  // namespace feed_handler
