// A message_sink test double: records what a second sink actually sees.
//
// Shared by the capture_session tests and by both exchange clients' tests,
// because "what does a frame look like by the time a downstream sink gets it"
// is the same question in all three, and the answer (the frame identity, the
// incarnation notification) is exactly the seam the order book will plug into.
//
// Thread safe on purpose: the client tests register one of these with a
// capture_session that a client's own connection thread then drives, so the
// test thread reads what the connection thread wrote.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "feed_handler/message_sink.h"

namespace feed_handler::testing {

/// One incarnation notification, as seen by a sink.
struct recorded_incarnation {
    std::uint64_t incarnation = 0;
    std::string reason;
};

/// One frame, copied out. The payload is copied deliberately: capture_frame is
/// a non-owning view valid only for the duration of the call (message_sink.h),
/// so a sink that wants to look at it later is required to copy -- which is
/// also what this double exists to demonstrate.
struct recorded_frame {
    std::string payload;
    std::uint64_t capture_sequence = 0;
    std::uint64_t monotonic_ns = 0;
    frame_source source = frame_source::unknown;
};

/// An empty span may carry a null pointer, which std::string(ptr, 0) is not
/// allowed to be handed, so the empty case is spelled out rather than assumed.
inline std::string copy_of(std::span<const std::byte> payload) {
    return payload.empty()
               ? std::string{}
               : std::string(std::bit_cast<const char*>(payload.data()), payload.size());
}

class recording_sink final : public message_sink {
  public:
    void on_frame(const capture_frame& frame) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        frames_.push_back(recorded_frame{
            .payload = copy_of(frame.payload),
            .capture_sequence = frame.capture_sequence,
            .monotonic_ns = frame.monotonic_ns,
            .source = frame.source,
        });
    }

    void on_incarnation(std::uint64_t incarnation, std::string_view reason) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        incarnations_.push_back(recorded_incarnation{
            .incarnation = incarnation,
            .reason = std::string(reason),
        });
    }

    std::vector<recorded_frame> frames() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }

    std::vector<recorded_incarnation> incarnations() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return incarnations_;
    }

    std::size_t frame_count() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<recorded_frame> frames_;
    std::vector<recorded_incarnation> incarnations_;
};

}  // namespace feed_handler::testing
