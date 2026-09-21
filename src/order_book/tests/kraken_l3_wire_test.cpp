#include "order_book/kraken_l3_wire.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

// Every existing single-entry test goes through this: the one message of a
// one-entry data[].
KrakenL3Message ParseOne(const json& message, const InstrumentScale& scale = kScale,
                         const OrderIdMapper& to_order_id = HashOrderId) {
    std::vector<KrakenL3Message> parsed = ParseKrakenL3Messages(message, scale, to_order_id);
    EXPECT_EQ(parsed.size(), 1U);
    return parsed.empty() ? KrakenL3Message{} : std::move(parsed.front());
}

TEST(KrakenL3Wire, ParsesSnapshotEntriesAsAddsBidsFirst) {
    const KrakenL3Message parsed = ParseOne(Snapshot());

    EXPECT_TRUE(parsed.is_snapshot);
    EXPECT_EQ(parsed.symbol, "BTC/USD");
    EXPECT_EQ(parsed.meta.checksum, 111U);
    ASSERT_EQ(parsed.orders.size(), 2U);
    EXPECT_EQ(parsed.orders[0].update.side, Side::kBid);
    EXPECT_EQ(parsed.orders[0].update.event, OrderEventKind::kAdded);
    EXPECT_EQ(parsed.orders[1].update.side, Side::kAsk);
    EXPECT_EQ(parsed.orders[1].update.event, OrderEventKind::kAdded);
    EXPECT_EQ(parsed.Snapshot().orders.size(), 2U);
}

TEST(KrakenL3Wire, AppliesTheInstrumentScaleToPricesAndQuantities) {
    const KrakenL3Message parsed = ParseOne(Snapshot());

    ASSERT_EQ(parsed.orders.size(), 2U);
    EXPECT_EQ(parsed.orders[0].update.price, Price(1005));
    EXPECT_EQ(parsed.orders[0].update.quantity, Quantity(50000000));
    EXPECT_EQ(parsed.orders[1].update.price, Price(1010));
    EXPECT_EQ(parsed.orders[1].update.quantity, Quantity(225000000));

    const KrakenL3Message whole_units = ParseOne(Snapshot(), InstrumentScale(0, 0));
    ASSERT_EQ(whole_units.orders.size(), 2U);
    EXPECT_EQ(whole_units.orders[1].update.price, Price(101));
}

TEST(KrakenL3Wire, ParsesUpdateEventKindsAndTimestamps) {
    const KrakenL3Message parsed = ParseOne(Update());

    EXPECT_FALSE(parsed.is_snapshot);
    EXPECT_EQ(parsed.symbol, "BTC/USD");
    EXPECT_EQ(parsed.meta.checksum, 222U);
    ASSERT_EQ(parsed.orders.size(), 3U);
    EXPECT_EQ(parsed.orders[0].update.event, OrderEventKind::kDeleted);
    EXPECT_EQ(parsed.orders[1].update.event, OrderEventKind::kModified);
    EXPECT_EQ(parsed.orders[2].update.event, OrderEventKind::kAdded);
    EXPECT_EQ(parsed.orders[2].timestamp, "2026-09-15T23:41:47.461426258Z");
}

TEST(KrakenL3Wire, MapsOrderIdsThroughTheSuppliedMapper) {
    const OrderIdMapper first_letter = [](const std::string& raw) {
        return OrderId(static_cast<std::uint64_t>(raw.front()));
    };

    const KrakenL3Message parsed = ParseOne(Snapshot(), kScale, first_letter);

    ASSERT_EQ(parsed.orders.size(), 2U);
    EXPECT_EQ(parsed.orders[0].update.order_id, OrderId('B'));
    EXPECT_EQ(parsed.orders[1].update.order_id, OrderId('A'));
}

TEST(KrakenL3Wire, ParsesEveryDataEntryWithItsOwnSymbolAndChecksum) {
    json message = Update();
    json eth_entry = Update().at("data").at(0);
    eth_entry["symbol"] = "ETH/USD";
    eth_entry["checksum"] = 333;
    eth_entry["asks"] = json::array();
    eth_entry["bids"] = json::parse(R"([{"event":"add","order_id":"E-1","limit_price":2500.5,
        "order_qty":3.0,"timestamp":"2026-09-15T23:41:47.461426259Z"}])");
    message["data"].push_back(eth_entry);

    const std::vector<KrakenL3Message> parsed = ParseKrakenL3Messages(message, kScale, HashOrderId);

    ASSERT_EQ(parsed.size(), 2U);
    EXPECT_EQ(parsed[0].symbol, "BTC/USD");
    EXPECT_EQ(parsed[0].meta.checksum, 222U);
    EXPECT_FALSE(parsed[0].is_snapshot);
    EXPECT_EQ(parsed[0].orders.size(), 3U);
    EXPECT_EQ(parsed[1].symbol, "ETH/USD");
    EXPECT_EQ(parsed[1].meta.checksum, 333U);
    EXPECT_FALSE(parsed[1].is_snapshot);
    ASSERT_EQ(parsed[1].orders.size(), 1U);
    EXPECT_EQ(parsed[1].orders[0].update.side, Side::kBid);
    EXPECT_EQ(parsed[1].orders[0].update.price, Price(25005));
}

TEST(KrakenL3Wire, ADataEntryWithNoOrdersStillYieldsAMessage) {
    json message = Update();
    message["data"][0]["asks"] = json::array();

    const KrakenL3Message parsed = ParseOne(message);

    EXPECT_TRUE(parsed.orders.empty());
    EXPECT_EQ(parsed.meta.checksum, 222U);
}

TEST(KrakenL3Wire, AnEmptyDataArrayYieldsNoMessages) {
    json message = Update();
    message["data"] = json::array();

    EXPECT_TRUE(ParseKrakenL3Messages(message, kScale, HashOrderId).empty());
}

TEST(KrakenL3Wire, IgnoresMessagesThatAreNotLevel3SnapshotsOrUpdates) {
    EXPECT_TRUE(
        ParseKrakenL3Messages(json::parse(R"({"channel":"heartbeat"})"), kScale, HashOrderId)
            .empty());
    EXPECT_TRUE(
        ParseKrakenL3Messages(
            json::parse(R"({"method":"subscribe","success":true,"result":{"channel":"level3"}})"),
            kScale, HashOrderId)
            .empty());
    EXPECT_TRUE(
        ParseKrakenL3Messages(
            json::parse(R"({"channel":"status","type":"update","data":[{"system":"online"}]})"),
            kScale, HashOrderId)
            .empty());
}

TEST(KrakenL3Wire, RejectsAnUnknownEventKind) {
    json message = Update();
    message["data"][0]["asks"][0]["event"] = "teleport";

    EXPECT_THROW((void)ParseKrakenL3Messages(message, kScale, HashOrderId), std::runtime_error);
}

TEST(KrakenL3Wire, RejectsAMalformedSecondDataEntry) {
    json message = Update();
    json broken = Update().at("data").at(0);
    broken.erase("symbol");
    message["data"].push_back(broken);

    EXPECT_THROW((void)ParseKrakenL3Messages(message, kScale, HashOrderId), json::exception);
}

TEST(KrakenL3Wire, RejectsADataEntryWithoutAChecksum) {
    json message = Snapshot();
    message["data"][0].erase("checksum");

    EXPECT_THROW((void)ParseKrakenL3Messages(message, kScale, HashOrderId), json::exception);
}

}  // namespace
}  // namespace order_book
