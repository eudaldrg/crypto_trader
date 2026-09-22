// The connection-thread half of the book path: a MessageSink that copies what a
// CaptureSession hands it into a ring and returns, so no parsing and no book work
// ever runs on the connection's own thread (docs/investigations/
// 2026-09-21-issue-14-architecture.md). The other half is BookService, which
// pops the ring on the book thread.
//
// Frames are lossy, control events are not. A frame goes in with a non-blocking
// Push: a full ring drops it and counts the drop, because blocking here would
// stall the socket and Kraken drops slow consumers. Connect and Disconnect use
// PushControl, which grows the ring instead of failing, because losing a Connect
// leaves a book built from two different snapshots.
//
// Every entry carries the ring's drop count at the moment it was pushed. That is
// what tells the consumer WHERE in the stream frames went missing: comparing the
// live counter after a pop cannot, since the producer can be a whole ring ahead
// of the consumer, so a drop that happened after a Connect could be seen (and its
// desync wiped by the Connect's book reset) before the Connect was popped.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "feed_handler/feed_event.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/spsc_ring.h"

namespace book_adapter {

/// What the ring carries: the event, and how many frames the ring had refused
/// when it was pushed (all of which precede it in the stream).
struct RingEntry {
    feed_handler::FeedEvent event;
    std::uint64_t dropped_before = 0;
};

using BookRing = feed_handler::SpscRing<RingEntry>;

/// Registered on a CaptureSession (CaptureConnection::AddSink); every call runs
/// on that connection's thread, which is the ring's one producer.
class RingSink final : public feed_handler::MessageSink {
  public:
    explicit RingSink(BookRing& ring) : ring_(&ring) {}

    /// Copies the frame (its payload is only valid for this call, message_sink.h)
    /// and pushes it without blocking. A full ring drops it; the drop is counted
    /// by the ring and reaches the book through the next entry's `dropped_before`.
    void OnFrame(const feed_handler::CaptureFrame& frame) override {
        (void)ring_->Push(Stamped(feed_handler::MakeFrameEvent(frame)));
    }

    /// Never dropped.
    void OnConnect(std::uint64_t connect_id, std::string_view reason) override {
        ring_->PushControl(Stamped(feed_handler::ConnectEvent{
            .connect_id = connect_id,
            .reason = std::string(reason),
        }));
    }

    /// Never dropped.
    void OnDisconnect(std::uint64_t connect_id) override {
        ring_->PushControl(Stamped(feed_handler::DisconnectEvent{.connect_id = connect_id}));
    }

  private:
    RingEntry Stamped(feed_handler::FeedEvent event) const {
        return RingEntry{.event = std::move(event), .dropped_before = ring_->Dropped()};
    }

    BookRing* ring_;
};

}  // namespace book_adapter
