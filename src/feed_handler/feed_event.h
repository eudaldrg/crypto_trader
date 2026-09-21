// What crosses a thread boundary between a connection thread and a consumer
// thread (the book thread; see docs/investigations/2026-09-21-issue-14-
// architecture.md): the three things a MessageSink is told, as owned values.
//
// MessageSink hands a sink a NON-OWNING CaptureFrame that is only valid for the
// duration of the call (message_sink.h), which is exactly what a queue cannot
// use. FeedEvent is the owning counterpart: one alternative per MessageSink
// callback, so a consumer on the far side of a ring sees the same sequence of
// frame / connect / disconnect the sink would have seen inline.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "feed_handler/message_sink.h"

namespace feed_handler {

/// A CaptureFrame whose payload bytes are owned, so it can outlive the
/// on_frame call it was made in.
struct FrameEvent {
    std::uint64_t capture_sequence = 0;
    std::uint64_t monotonic_ns = 0;
    FrameSource source = FrameSource::kUnknown;
    std::vector<std::byte> payload;

    /// A non-owning CaptureFrame over this event's bytes, for code written
    /// against the sink interface. Valid while this event is.
    CaptureFrame View() const {
        return CaptureFrame{
            .payload = std::span<const std::byte>(payload),
            .capture_sequence = capture_sequence,
            .monotonic_ns = monotonic_ns,
            .source = source,
        };
    }
};

/// MessageSink::OnConnect: a connection was (re)established.
struct ConnectEvent {
    std::uint64_t connect_id = 0;
    std::string reason;
};

/// MessageSink::OnDisconnect: the connection `connect_id` ended.
struct DisconnectEvent {
    std::uint64_t connect_id = 0;
};

using FeedEvent = std::variant<FrameEvent, ConnectEvent, DisconnectEvent>;

/// Copies `frame` into an owned FrameEvent. This is the one place a frame's
/// payload is copied on its way to another thread; a per-frame heap allocation
/// on the connection thread, accepted for v1 and measured by the replay driver.
/// A ring with its own event type (the journal's) calls this directly.
inline FrameEvent CopyFrame(const CaptureFrame& frame) {
    return FrameEvent{
        .capture_sequence = frame.capture_sequence,
        .monotonic_ns = frame.monotonic_ns,
        .source = frame.source,
        .payload = std::vector<std::byte>(frame.payload.begin(), frame.payload.end()),
    };
}

/// CopyFrame as a FeedEvent, for the rings that carry the sink-shaped events.
inline FeedEvent MakeFrameEvent(const CaptureFrame& frame) {
    return CopyFrame(frame);
}

}  // namespace feed_handler
