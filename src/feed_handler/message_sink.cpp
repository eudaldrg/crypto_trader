#include "feed_handler/message_sink.h"

#include <ctime>

namespace feed_handler {
namespace {

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;

std::uint64_t ClockNowNs(clockid_t clock_id) {
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

std::uint64_t MonotonicNowNs() {
    return ClockNowNs(CLOCK_MONOTONIC);
}

std::uint64_t RealtimeNowNs() {
    return ClockNowNs(CLOCK_REALTIME);
}

CaptureFrame CaptureStamper::Stamp(std::span<const std::byte> payload, FrameSource source) {
    ++sequence_;
    return CaptureFrame{
        .payload = payload,
        .capture_sequence = sequence_,
        .monotonic_ns = MonotonicNowNs(),
        .source = source,
    };
}

}  // namespace feed_handler
