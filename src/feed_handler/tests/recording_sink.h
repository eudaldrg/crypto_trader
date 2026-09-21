// A message_sink test double: records what a second sink actually sees.
//
// Shared by the CaptureSession tests and by both exchange clients' tests,
// because "what does a frame look like by the time a downstream sink gets it"
// is the same question in all three, and the answer (the frame identity, the
// connect notification) is exactly the seam the order book will plug into.
//
// Thread safe on purpose: the client tests register one of these with a
// CaptureSession that a client's own connection thread then drives, so the
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

/// One connect notification, as seen by a sink.
struct RecordedConnect {
    std::uint64_t connect_id = 0;
    std::string reason;
};

/// One frame, copied out. The payload is copied deliberately: CaptureFrame is
/// a non-owning view valid only for the duration of the call (message_sink.h),
/// so a sink that wants to look at it later is required to copy -- which is
/// also what this double exists to demonstrate.
struct RecordedFrame {
    std::string payload;
    std::uint64_t capture_sequence = 0;
    std::uint64_t monotonic_ns = 0;
    FrameSource source = FrameSource::kUnknown;
};

/// An empty span may carry a null pointer, which std::string(ptr, 0) is not
/// allowed to be handed, so the empty case is spelled out rather than assumed.
inline std::string CopyOf(std::span<const std::byte> payload) {
    return payload.empty()
               ? std::string{}
               : std::string(std::bit_cast<const char*>(payload.data()), payload.size());
}

class RecordingSink final : public MessageSink {
  public:
    void OnFrame(const CaptureFrame& frame) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back("frame " + std::to_string(frame.capture_sequence));
        frames_.push_back(RecordedFrame{
            .payload = CopyOf(frame.payload),
            .capture_sequence = frame.capture_sequence,
            .monotonic_ns = frame.monotonic_ns,
            .source = frame.source,
        });
    }

    void OnConnect(std::uint64_t connect_id, std::string_view reason) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back("connect " + std::to_string(connect_id));
        connects_.push_back(RecordedConnect{
            .connect_id = connect_id,
            .reason = std::string(reason),
        });
    }

    std::vector<RecordedFrame> Frames() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }

    void OnDisconnect(std::uint64_t connect_id) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back("disconnect " + std::to_string(connect_id));
        disconnects_.push_back(connect_id);
    }

    std::vector<RecordedConnect> Connects() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return connects_;
    }

    /// The connect_id of every disconnect, in delivery order.
    std::vector<std::uint64_t> Disconnects() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return disconnects_;
    }

    /// Every event in delivery order across the three kinds ("connect 1",
    /// "frame 2", "disconnect 1"), for tests about ordering between them, which
    /// the per-kind accessors cannot express.
    std::vector<std::string> Events() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    std::size_t FrameCount() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<RecordedFrame> frames_;
    std::vector<RecordedConnect> connects_;
    std::vector<std::uint64_t> disconnects_;
    std::vector<std::string> events_;
};

}  // namespace feed_handler::testing
