#include "feed_handler/spsc_ring.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "feed_handler/feed_event.h"

namespace {

using feed_handler::ConnectEvent;
using feed_handler::DisconnectEvent;
using feed_handler::FeedEvent;
using feed_handler::FrameEvent;
using feed_handler::FrameSource;
using feed_handler::MakeFrameEvent;
using feed_handler::SpscRing;

constexpr std::size_t kSmallCapacity = 4;
// Far more than any small ring holds: a loop bounded by this that does not see
// a full ring has failed.
constexpr std::uint64_t kMoreThanAnyRingHolds = 1'000'000;
constexpr std::uint64_t kThreadedEvents = 20'000;
constexpr std::uint64_t kControlEvery = 97;

// Fills the ring until Push refuses; returns how many it took.
std::uint64_t FillUntilFull(SpscRing<std::uint64_t>& ring) {
    std::uint64_t pushed = 0;
    while (pushed < kMoreThanAnyRingHolds && ring.Push(std::uint64_t{pushed})) {
        ++pushed;
    }
    return pushed;
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

TEST(SpscRing, PopsInTheOrderPushed) {
    SpscRing<std::uint64_t> ring(kSmallCapacity);
    for (std::uint64_t i = 0; i < kSmallCapacity; ++i) {
        ASSERT_TRUE(ring.Push(std::uint64_t{i}));
    }
    for (std::uint64_t i = 0; i < kSmallCapacity; ++i) {
        std::uint64_t out = 0;
        ASSERT_TRUE(ring.Pop(out));
        EXPECT_EQ(out, i);
    }
    std::uint64_t out = 0;
    EXPECT_FALSE(ring.Pop(out));
    EXPECT_EQ(ring.Dropped(), 0U);
}

TEST(SpscRing, AFullRingRefusesAndCountsEachDrop) {
    SpscRing<std::uint64_t> ring(kSmallCapacity);
    const std::uint64_t held = FillUntilFull(ring);
    ASSERT_GE(held, kSmallCapacity);
    ASSERT_LT(held, kMoreThanAnyRingHolds);
    // FillUntilFull's last Push is the refused one.
    EXPECT_EQ(ring.Dropped(), 1U);

    EXPECT_FALSE(ring.Push(std::uint64_t{held}));
    EXPECT_FALSE(ring.Push(std::uint64_t{held}));
    EXPECT_EQ(ring.Dropped(), 3U);

    // What was accepted is intact and in order; the refused values are not there.
    for (std::uint64_t i = 0; i < held; ++i) {
        std::uint64_t out = 0;
        ASSERT_TRUE(ring.Pop(out));
        EXPECT_EQ(out, i);
    }
    std::uint64_t out = 0;
    EXPECT_FALSE(ring.Pop(out));
}

TEST(SpscRing, APopMakesRoomForAnotherPush) {
    SpscRing<std::uint64_t> ring(kSmallCapacity);
    const std::uint64_t held = FillUntilFull(ring);
    std::uint64_t out = 0;
    ASSERT_TRUE(ring.Pop(out));
    EXPECT_TRUE(ring.Push(std::uint64_t{held}));
}

TEST(SpscRing, ARefusedPushLeavesTheValueWithTheCaller) {
    SpscRing<FeedEvent> ring(kSmallCapacity);
    std::uint64_t seq = 0;
    while (ring.Push(FeedEvent{FrameEvent{.capture_sequence = seq, .payload = Bytes("x")}})) {
        ++seq;
        ASSERT_LT(seq, kMoreThanAnyRingHolds);
    }
    FeedEvent refused = FrameEvent{.capture_sequence = seq, .payload = Bytes("kept")};
    ASSERT_FALSE(ring.Push(std::move(refused)));
    // Not moved from: the caller can retry or log it.
    EXPECT_EQ(std::get<FrameEvent>(refused).payload, Bytes("kept"));
}

TEST(SpscRing, ControlEventsAreNeverDroppedEvenWhenTheRingIsFull) {
    SpscRing<std::uint64_t> ring(kSmallCapacity);
    const std::uint64_t held = FillUntilFull(ring);
    const std::uint64_t drops_before = ring.Dropped();

    constexpr std::uint64_t kControls = 50;
    for (std::uint64_t i = 0; i < kControls; ++i) {
        ring.PushControl(std::uint64_t{held + i});
    }
    EXPECT_EQ(ring.Dropped(), drops_before);

    for (std::uint64_t i = 0; i < held + kControls; ++i) {
        std::uint64_t out = 0;
        ASSERT_TRUE(ring.Pop(out));
        EXPECT_EQ(out, i);
    }
    std::uint64_t out = 0;
    EXPECT_FALSE(ring.Pop(out));
}

TEST(SpscRing, MakeFrameEventCopiesTheStampAndThePayload) {
    std::vector<std::byte> wire = Bytes("8=FIX.4.4");
    const feed_handler::CaptureFrame frame{
        .payload = wire,
        .capture_sequence = 7,
        .monotonic_ns = 123'456,
        .source = FrameSource::kDeribitFix,
    };

    const FeedEvent event = MakeFrameEvent(frame);
    // The producer may reuse its buffer the moment OnFrame returns.
    wire.assign(wire.size(), std::byte{0});

    const auto* frame_event = std::get_if<FrameEvent>(&event);
    ASSERT_NE(frame_event, nullptr);
    EXPECT_EQ(frame_event->capture_sequence, 7U);
    EXPECT_EQ(frame_event->monotonic_ns, 123'456U);
    EXPECT_EQ(frame_event->source, FrameSource::kDeribitFix);
    EXPECT_EQ(frame_event->payload, Bytes("8=FIX.4.4"));

    const feed_handler::CaptureFrame view = frame_event->View();
    EXPECT_EQ(view.capture_sequence, 7U);
    EXPECT_EQ(view.source, FrameSource::kDeribitFix);
    EXPECT_EQ(view.payload.size(), frame_event->payload.size());
    EXPECT_EQ(view.payload.data(), frame_event->payload.data());
}

// One producer, one consumer, no sleeps: the ring's own ordering is the only
// thing that lines the two up, so this is what TSan gets to look at. The
// producer retries a refused Push (it is a test of order, not of loss), so every
// refusal must show up in Dropped().
TEST(SpscRing, ProducerAndConsumerThreadsSeeEveryEventInOrder) {
    SpscRing<FeedEvent> ring(kSmallCapacity);
    std::uint64_t refused = 0;

    std::thread producer([&ring, &refused] {
        std::uint64_t connect_id = 0;
        for (std::uint64_t seq = 1; seq <= kThreadedEvents; ++seq) {
            if (seq % kControlEvery == 0) {
                ++connect_id;
                ring.PushControl(FeedEvent{ConnectEvent{.connect_id = connect_id, .reason = "r"}});
            }
            FeedEvent event =
                FrameEvent{.capture_sequence = seq, .payload = Bytes(std::to_string(seq))};
            while (!ring.Push(std::move(event))) {
                ++refused;
                std::this_thread::yield();
            }
        }
        ring.PushControl(FeedEvent{DisconnectEvent{.connect_id = connect_id}});
    });

    std::uint64_t next_frame = 1;
    std::uint64_t next_connect = 1;
    bool disconnected = false;
    while (!disconnected) {
        FeedEvent event;
        if (!ring.Pop(event)) {
            std::this_thread::yield();
            continue;
        }
        if (const auto* frame = std::get_if<FrameEvent>(&event)) {
            ASSERT_EQ(frame->capture_sequence, next_frame);
            ASSERT_EQ(frame->payload, Bytes(std::to_string(next_frame)));
            ++next_frame;
        } else if (const auto* connect = std::get_if<ConnectEvent>(&event)) {
            ASSERT_EQ(connect->connect_id, next_connect);
            // A Connect is pushed just before the frame with this sequence.
            ASSERT_EQ(next_frame, next_connect * kControlEvery);
            ++next_connect;
        } else {
            ASSERT_EQ(std::get<DisconnectEvent>(event).connect_id, next_connect - 1);
            disconnected = true;
        }
    }
    producer.join();

    EXPECT_EQ(next_frame, kThreadedEvents + 1);
    EXPECT_EQ(ring.Dropped(), refused);
}

}  // namespace
