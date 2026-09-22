#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>

#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
#include "order_book/engine.h"
#include "order_book/instrument_scale.h"
#include "order_book/kraken_l3_policy.h"
#include "order_book/kraken_l3_wire.h"
#include "order_book/types.h"

namespace order_book {
namespace {

// Settings of the captured session, not of the book: the fixture is BTC/USD,
// whose reference data (the feed handler's LogInstrumentReference output for
// the capture: tick_size=0.1 price_decimals=1 lot_decimals=8) gives these
// decimals, and it was subscribed at Kraken's default depth, because
// kraken_ws_client.cpp sends no depth parameter. The depth has to match the
// subscription the capture was taken with -- Kraken sends no delete for levels
// falling out of it, so a different value here would desync the checksum. A
// live book takes both from its own instrument reference data and subscription.
constexpr InstrumentScale kCaptureScale(/*price_decimals=*/1, /*quantity_decimals=*/8);
constexpr std::size_t kCaptureSubscribedDepth = 10;

struct RecordingListener {
    int unknown_order_count = 0;
    int crossed_count = 0;
    int checksum_mismatch_count = 0;

    void OnTopOfBookChanged(Side, std::optional<BookEntry>) {}
    void OnIntegrityCheckFailed(IntegrityIssue issue) {
        switch (issue) {
            case IntegrityIssue::kUnknownOrder:
                ++unknown_order_count;
                break;
            case IntegrityIssue::kCrossedBook:
                ++crossed_count;
                break;
            case IntegrityIssue::kChecksumMismatch:
                ++checksum_mismatch_count;
                break;
            default:
                break;
        }
    }
};

using KrakenBook = OrderBook<KrakenL3Policy, RecordingListener>;

// Replays a real ~2-minute Kraken level3 BTC/USD session (captured by the
// real kraken_feed_handler binary -- not a Python probe -- via its native
// v1 journal format) through the Kraken L3 book, verifying every
// message's checksum along the way, including the snapshot's. 6496 wire
// messages, 6497 journal records (the extra one is the kConnect
// marker), zero forced reconnects at capture time; 6362 of them are level3
// messages that carry a checksum (1 snapshot + 6361 updates).
TEST(KrakenCaptureReplay, RealSessionAppliesWithNoIntegrityIssues) {
    const std::filesystem::path capture_path =
        std::filesystem::path(TEST_DATA_DIR) / "kraken_l3_capture.journal";
    auto reader = feed_handler::JournalReader::Open(capture_path);
    ASSERT_TRUE(reader.has_value()) << "missing/invalid capture fixture: " << capture_path << " ("
                                    << (reader.has_value() ? "" : reader.error()) << ")";

    RecordingListener listener;
    KrakenBook book(listener, KrakenL3Policy(kCaptureSubscribedDepth));

    bool got_snapshot = false;
    int update_message_count = 0;

    while (const std::optional<feed_handler::JournalRecord> record = reader->Next()) {
        if (record->type != feed_handler::journal::RecordType::kWireMessage) {
            continue;
        }
        // The capture is a single-symbol connection, so every message is for
        // the one book; a message for another symbol would fail the checksum.
        for (const KrakenL3Message& message :
             ParseKrakenL3Messages(ParseWirePayload(record->payload), kCaptureScale, HashOrderId)) {
            EXPECT_EQ(message.symbol, "BTC/USD");
            if (message.is_snapshot) {
                book.ApplySnapshot(message.Snapshot(), message.meta);
                got_snapshot = true;
            } else {
                ASSERT_TRUE(got_snapshot) << "update message arrived before a snapshot";
                book.ApplyBatch(std::span<const KrakenL3Update>(message.orders), message.meta);
                ++update_message_count;
            }
        }
    }

    EXPECT_FALSE(reader->StoppedEarly()) << reader->StopReason();
    ASSERT_TRUE(got_snapshot);
    // Exact count, not a lower bound: a regression that silently drops or
    // duplicates messages (e.g. a parsing change that skips a type) would
    // pass EXPECT_GT(..., 0) just as easily as the correct run does.
    EXPECT_EQ(update_message_count, 6361);
    EXPECT_EQ(listener.unknown_order_count, 0);
    EXPECT_EQ(listener.crossed_count, 0);
    EXPECT_EQ(listener.checksum_mismatch_count, 0);
    EXPECT_EQ(book.GetReadiness(), Readiness::kReady);
}

}  // namespace
}  // namespace order_book
