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

std::mutex& log_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::string_view level_tag(log_level level) {
    switch (level) {
        case log_level::warn:
            return "WARN ";
        case log_level::error:
            return "ERROR";
        case log_level::info:
            break;
    }
    return "INFO ";
}

/// "2026-09-16T21:30:00.123Z". UTC on purpose: capture files and logs get read
/// next to exchange-side timestamps, which are UTC.
std::string utc_timestamp() {
    const std::uint64_t now_ns = realtime_now_ns();
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

void log_message(log_level level, std::string_view message) {
    const std::string prefix = utc_timestamp();
    const std::lock_guard<std::mutex> guard(log_mutex());
    std::clog << prefix << ' ' << level_tag(level) << ' ' << message << '\n';
}

}  // namespace feed_handler
