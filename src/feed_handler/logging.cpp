#include "feed_handler/logging.h"

#include <array>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>

#include "feed_handler/message_sink.h"

namespace feed_handler {
namespace {

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;
constexpr std::uint64_t kNanosPerMilli = 1'000'000;

std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}

std::string_view LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::kWarn:
            return "WARN ";
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kInfo:
            break;
    }
    return "INFO ";
}

/// "2026-09-16T21:30:00.123Z". UTC on purpose: capture files and logs get read
/// next to exchange-side timestamps, which are UTC.
std::string UtcTimestamp() {
    const std::uint64_t now_ns = RealtimeNowNs();
    const auto seconds = static_cast<std::time_t>(now_ns / kNanosPerSecond);
    const auto millis = static_cast<unsigned>((now_ns % kNanosPerSecond) / kNanosPerMilli);

    std::tm utc{};
    ::gmtime_r(&seconds, &utc);

    std::array<char, 32> formatted{};
    const std::size_t date_length =
        std::strftime(formatted.data(), formatted.size(), "%Y-%m-%dT%H:%M:%S", &utc);

    std::array<char, 8> fraction{};
    std::snprintf(fraction.data(), fraction.size(), ".%03uZ", millis);
    return std::string(formatted.data(), date_length) + fraction.data();
}

}  // namespace

void LogMessage(LogLevel level, std::string_view message) {
    const std::string prefix = UtcTimestamp();
    const std::lock_guard<std::mutex> guard(LogMutex());
    std::clog << prefix << ' ' << LevelTag(level) << ' ' << message << '\n';
}

}  // namespace feed_handler
