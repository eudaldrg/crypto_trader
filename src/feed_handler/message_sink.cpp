#include "feed_handler/message_sink.h"

#include <ctime>

namespace feed_handler {
namespace {

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;

std::uint64_t clock_now_ns(clockid_t clock_id) {
    timespec now{};
    // clock_gettime cannot fail for CLOCK_MONOTONIC/CLOCK_REALTIME on a sane
    // kernel; treating a failure as 0 keeps this noexcept-shaped rather than
    // adding an error path no caller could act on.
    if (::clock_gettime(clock_id, &now) != 0) {
        return 0;
    }
    return (static_cast<std::uint64_t>(now.tv_sec) * kNanosPerSecond) +
           static_cast<std::uint64_t>(now.tv_nsec);
}

}  // namespace

std::uint64_t monotonic_now_ns() {
    return clock_now_ns(CLOCK_MONOTONIC);
}

std::uint64_t realtime_now_ns() {
    return clock_now_ns(CLOCK_REALTIME);
}

capture_frame capture_stamper::stamp(std::span<const std::byte> payload, frame_source source) {
    ++sequence_;
    return capture_frame{
        .payload = payload,
        .capture_sequence = sequence_,
        .monotonic_ns = monotonic_now_ns(),
        .source = source,
    };
}

}  // namespace feed_handler
