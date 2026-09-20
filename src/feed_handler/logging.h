// Minimal operational logging for the feed handler binaries: one timestamped
// line per lifecycle event on stderr.
//
// Deliberately tiny rather than a logging library: v1 only needs "what is this
// process doing right now" on a terminal, and nothing on the per-message hot
// path logs at all. Nothing here is ever handed a credential -- API keys, WS
// tokens and raw outbound payloads never reach a log line (exchanges/kraken.md).
#pragma once

#include <cstdint>
#include <string>
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

/// The same three levels with every line prefixed `[tag] `, so several
/// connections in one process can be told apart. A connection holds one for its
/// whole life; the prefix is built once.
class TaggedLog {
  public:
    explicit TaggedLog(std::string_view tag) : prefix_("[" + std::string(tag) + "] ") {}

    void Info(std::string_view message) const {
        Write(LogLevel::kInfo, message);
    }

    void Warn(std::string_view message) const {
        Write(LogLevel::kWarn, message);
    }

    void Error(std::string_view message) const {
        Write(LogLevel::kError, message);
    }

  private:
    void Write(LogLevel level, std::string_view message) const {
        std::string line;
        line.reserve(prefix_.size() + message.size());
        line += prefix_;
        line += message;
        LogMessage(level, line);
    }

    std::string prefix_;
};

}  // namespace feed_handler
