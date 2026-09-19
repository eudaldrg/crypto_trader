#include "order_book/kraken_l3_wire.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "order_book/instrument_scale.h"
#include "order_book/types.h"

namespace order_book {
namespace {

using nlohmann::json;

constexpr InstrumentScale kScale(1, 8);

// Shapes as documented in exchanges/kraken.md (snapshot entries carry no
// "event"; update entries do, and each carries its own timestamp).
json Snapshot() {
    return json::parse(R"({"channel":"level3","type":"snapshot","data":[{
        "checksum":111,"symbol":"BTC/USD","timestamp":"2026-09-15T23:41:00.000000000Z",
        "bids":[{"order_id":"B-1","limit_price":100.5,"order_qty":0.5,
                 "timestamp":"2026-09-15T23:40:00.000000000Z"}],
        "asks":[{"order_id":"A-1","limit_price":101.0,"order_qty":2.25,
                 "timestamp":"2026-09-15T23:40:01.000000000Z"}]}]})");
}

json Update() {
    return json::parse(R"({"channel":"level3","type":"update","data":[{
        "checksum":222,"symbol":"BTC/USD","timestamp":"2026-09-15T23:41:47.461426256Z",
        "bids":[],
        "asks":[{"event":"delete","order_id":"A-1","limit_price":101.0,"order_qty":2.25,
                 "timestamp":"2026-09-15T23:41:47.461426256Z"},
                {"event":"modify","order_id":"A-2","limit_price":102.0,"order_qty":1.0,
                 "timestamp":"2026-09-15T23:41:47.461426257Z"},
                {"event":"add","order_id":"A-3","limit_price":103.0,"order_qty":0.1,
                 "timestamp":"2026-09-15T23:41:47.461426258Z"}]}]})");
}

TEST(KrakenL3Wire, ParsesSnapshotEntriesAsAddsBidsFirst) {
    const auto parsed = ParseKrakenL3Message(Snapshot(), kScale, HashOrderId);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->is_snapshot);
    EXPECT_EQ(parsed->meta.checksum, 111U);
    ASSERT_EQ(parsed->orders.size(), 2U);
    EXPECT_EQ(parsed->orders[0].update.side, Side::kBid);
    EXPECT_EQ(parsed->orders[0].update.event, OrderEventKind::kAdded);
    EXPECT_EQ(parsed->orders[1].update.side, Side::kAsk);
    EXPECT_EQ(parsed->orders[1].update.event, OrderEventKind::kAdded);
    EXPECT_EQ(parsed->Snapshot().orders.size(), 2U);
}

TEST(KrakenL3Wire, AppliesTheInstrumentScaleToPricesAndQuantities) {
    const auto parsed = ParseKrakenL3Message(Snapshot(), kScale, HashOrderId);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->orders[0].update.price, Price(1005));
    EXPECT_EQ(parsed->orders[0].update.quantity, Quantity(50000000));
    EXPECT_EQ(parsed->orders[1].update.price, Price(1010));
    EXPECT_EQ(parsed->orders[1].update.quantity, Quantity(225000000));

    const auto whole_units = ParseKrakenL3Message(Snapshot(), InstrumentScale(0, 0), HashOrderId);
    ASSERT_TRUE(whole_units.has_value());
    EXPECT_EQ(whole_units->orders[1].update.price, Price(101));
}

TEST(KrakenL3Wire, ParsesUpdateEventKindsAndTimestamps) {
    const auto parsed = ParseKrakenL3Message(Update(), kScale, HashOrderId);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->is_snapshot);
    EXPECT_EQ(parsed->meta.checksum, 222U);
    ASSERT_EQ(parsed->orders.size(), 3U);
    EXPECT_EQ(parsed->orders[0].update.event, OrderEventKind::kDeleted);
    EXPECT_EQ(parsed->orders[1].update.event, OrderEventKind::kModified);
    EXPECT_EQ(parsed->orders[2].update.event, OrderEventKind::kAdded);
    EXPECT_EQ(parsed->orders[2].timestamp, "2026-09-15T23:41:47.461426258Z");
}

TEST(KrakenL3Wire, MapsOrderIdsThroughTheSuppliedMapper) {
    const OrderIdMapper first_letter = [](const std::string& raw) {
        return OrderId(static_cast<std::uint64_t>(raw.front()));
    };

    const auto parsed = ParseKrakenL3Message(Snapshot(), kScale, first_letter);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->orders[0].update.order_id, OrderId('B'));
    EXPECT_EQ(parsed->orders[1].update.order_id, OrderId('A'));
}

TEST(KrakenL3Wire, IgnoresMessagesThatAreNotLevel3SnapshotsOrUpdates) {
    EXPECT_FALSE(
        ParseKrakenL3Message(json::parse(R"({"channel":"heartbeat"})"), kScale, HashOrderId));
    EXPECT_FALSE(ParseKrakenL3Message(
        json::parse(R"({"method":"subscribe","success":true,"result":{"channel":"level3"}})"),
        kScale, HashOrderId));
    EXPECT_FALSE(ParseKrakenL3Message(
        json::parse(R"({"channel":"status","type":"update","data":[{"system":"online"}]})"), kScale,
        HashOrderId));
}

TEST(KrakenL3Wire, RejectsAnUnknownEventKind) {
    json message = Update();
    message["data"][0]["asks"][0]["event"] = "teleport";

    EXPECT_THROW((void)ParseKrakenL3Message(message, kScale, HashOrderId), std::runtime_error);
}

}  // namespace
}  // namespace order_book
