#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "order_book/engine.h"
#include "order_book/instrument_scale.h"
#include "order_book/l2_policy.h"
#include "order_book/types.h"

namespace order_book {
namespace {

using nlohmann::json;

// BTC-PERPETUAL as captured: tick size 0.5, so one price decimal,
// and quantity in whole USD notional, so none. These are the capture
// instrument's settings, confirmed against the capture, not assumptions the
// book makes -- a live book takes them from its instrument reference data.
constexpr InstrumentScale kCaptureScale(/*price_decimals=*/1, /*quantity_decimals=*/0);

L2Operation ToOperation(const std::string& op) {
    if (op == "new") {
        return L2Operation::kNew;
    }
    if (op == "change") {
        return L2Operation::kChange;
    }
    if (op == "delete") {
        return L2Operation::kDelete;
    }
    throw std::runtime_error("unknown Deribit book operation: " + op);
}

void AppendEntries(std::vector<L2Update>& out, const json& data, Side side, const char* key) {
    for (const auto& entry : data.at(key)) {
        const std::string op = entry.at(0).get<std::string>();
        const double price = entry.at(1).get<double>();
        const double quantity = entry.at(2).get<double>();
        out.push_back(L2Update{side, kCaptureScale.ToPrice(price),
                               kCaptureScale.ToQuantity(quantity), ToOperation(op)});
    }
}

struct RecordingListener {
    int gap_count = 0;
    int crossed_count = 0;
    int unknown_level_count = 0;

    void OnTopOfBookChanged(Side, std::optional<BookEntry>) {}
    void OnIntegrityCheckFailed(IntegrityIssue issue) {
        switch (issue) {
            case IntegrityIssue::kGap:
                ++gap_count;
                break;
            case IntegrityIssue::kCrossedBook:
                ++crossed_count;
                break;
            case IntegrityIssue::kUnknownLevel:
                ++unknown_level_count;
                break;
            default:
                break;
        }
    }
};

using L2Book = OrderBook<L2Policy, RecordingListener>;

// Replays a real ~2-minute Deribit `book.BTC-PERPETUAL.raw` session
// (captured via experiments/deribit_ws_book_probe.py) through
// the golden L2 book. This is what settled decisions/0001's previously
// open assumption about change_id/prev_change_id semantics: the capture
// showed zero gaps across 2069 consecutive messages, and it also caught
// a real bug -- the original L2Update design assumed a zero-quantity
// wire encoding for deletes, but Deribit actually sends an explicit
// New/Change/Delete operation per entry (fixed in l2_policy.h before this
// test was written).
TEST(DeribitCaptureReplay, RealSessionAppliesWithNoIntegrityIssues) {
    const std::filesystem::path capture_path =
        std::filesystem::path(TEST_DATA_DIR) / "deribit_book_capture.json";
    std::ifstream in(capture_path);
    ASSERT_TRUE(in.is_open()) << "missing capture fixture: " << capture_path;
    json capture;
    in >> capture;

    RecordingListener listener;
    L2Book book(listener);

    bool got_snapshot = false;
    int change_message_count = 0;

    for (const json& entry : capture.at("journal")) {
        if (entry.at("direction") != "received") {
            continue;
        }
        const json& message = entry.at("message");
        if (!message.contains("params") || !message.at("params").contains("data")) {
            continue;
        }
        const json& data = message.at("params").at("data");
        const std::string type = data.at("type").get<std::string>();
        const ChangeId change_id(data.at("change_id").get<std::int64_t>());

        if (type == "snapshot") {
            std::vector<L2Update> levels;
            AppendEntries(levels, data, Side::kBid, "bids");
            AppendEntries(levels, data, Side::kAsk, "asks");
            book.ApplySnapshot(L2Snapshot{.levels = std::move(levels), .change_id = change_id});
            got_snapshot = true;
        } else if (type == "change") {
            ASSERT_TRUE(got_snapshot) << "change message arrived before a snapshot";
            std::vector<L2Update> updates;
            AppendEntries(updates, data, Side::kBid, "bids");
            AppendEntries(updates, data, Side::kAsk, "asks");
            const ChangeId prev_change_id(data.at("prev_change_id").get<std::int64_t>());
            book.ApplyBatch(std::span<const L2Update>(updates),
                            ChangeIdMeta{change_id, prev_change_id});
            ++change_message_count;
        }
    }

    ASSERT_TRUE(got_snapshot);
    // Exact count, not a lower bound -- see kraken_capture_replay_test.cpp's
    // equivalent assertion for why.
    EXPECT_EQ(change_message_count, 2069);
    EXPECT_EQ(listener.gap_count, 0);
    EXPECT_EQ(listener.crossed_count, 0);
    EXPECT_EQ(listener.unknown_level_count, 0);
    EXPECT_EQ(book.GetReadiness(), Readiness::kReady);
}

}  // namespace
}  // namespace order_book
