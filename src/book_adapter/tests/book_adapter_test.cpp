#include "book_adapter/book_adapter.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "book_adapter/book_settings.h"
#include "feed_handler/feed_event.h"
#include "feed_handler/message_sink.h"
#include "order_book/instrument_scale.h"
#include "order_book/kraken_checksum.h"
#include "order_book/kraken_l3_policy.h"
#include "order_book/kraken_l3_wire.h"
#include "order_book/types.h"

namespace book_adapter {
namespace {

using nlohmann::json;
using order_book::IntegrityIssue;
using order_book::Readiness;
using order_book::Side;

constexpr order_book::InstrumentScale kScale(/*price_decimals=*/1, /*quantity_decimals=*/8);
constexpr std::size_t kDepth = 10;

const BookSettings kKrakenSettings{
    .source = feed_handler::FrameSource::kKrakenJson,
    .scale = kScale,
    .kraken_depth = kDepth,
};

struct Order {
    std::string id;
    double price;
    double quantity;
};

// Builds Kraken level3 messages whose checksums are right, by replaying every
// message through a reference policy of its own. That is not an independent
// oracle for the checksum (kraken_capture_replay_test and the documented
// example are); these tests are about routing and lifecycle, and only need a
// message the book accepts to be told apart from one it rejects.
class Feed {
  public:
    std::string Snapshot(const std::string& symbol, const std::vector<Order>& bids,
                         const std::vector<Order>& asks) {
        return Message("snapshot", {Entry(symbol, "", bids, asks)});
    }

    std::string Add(const std::string& symbol, Side side, const Order& order) {
        return Message("update", {SideEntry(symbol, "add", side, order)});
    }

    std::string Delete(const std::string& symbol, Side side, const Order& order) {
        return Message("update", {SideEntry(symbol, "delete", side, order)});
    }

    // One frame carrying a bid add for each of several symbols, as one data[].
    std::string AddToEach(const std::vector<std::pair<std::string, Order>>& adds) {
        std::vector<json> entries;
        for (const auto& [symbol, order] : adds) {
            entries.push_back(SideEntry(symbol, "add", Side::kBid, order));
        }
        return Message("update", entries);
    }

    // The same update, with the checksum the book would reject.
    static std::string WithWrongChecksum(const std::string& message) {
        json parsed = json::parse(message);
        for (json& entry : parsed["data"]) {
            entry["checksum"] = entry["checksum"].get<std::uint32_t>() ^ 1U;
        }
        return parsed.dump();
    }

  private:
    json SideEntry(const std::string& symbol, const std::string& event, Side side,
                   const Order& order) {
        const std::vector<Order> none;
        const std::vector<Order> one{order};
        return side == Side::kBid ? Entry(symbol, event, one, none)
                                  : Entry(symbol, event, none, one);
    }

    json Entry(const std::string& symbol, const std::string& event, const std::vector<Order>& bids,
               const std::vector<Order>& asks) {
        json entry = {{"symbol", symbol},
                      {"checksum", 0},
                      {"timestamp", NextTimestamp()},
                      {"bids", Orders(bids, event)},
                      {"asks", Orders(asks, event)}};
        return entry;
    }

    json Orders(const std::vector<Order>& orders, const std::string& event) {
        json array = json::array();
        for (const Order& order : orders) {
            json entry = {{"order_id", order.id},
                          {"limit_price", order.price},
                          {"order_qty", order.quantity},
                          {"timestamp", NextTimestamp()}};
            if (!event.empty()) {
                entry["event"] = event;
            }
            array.push_back(entry);
        }
        return array;
    }

    // Fixed-width RFC3339 so the timestamps sort as strings, like Kraken's.
    std::string NextTimestamp() {
        const std::string digits = std::to_string(1000000000 + ++clock_);
        return "2026-09-21T10:00:00." + digits.substr(1) + "Z";
    }

    std::string Message(const std::string& type, std::vector<json> entries) {
        json message = {{"channel", "level3"}, {"type", type}, {"data", json::array()}};
        for (json& entry : entries) {
            // Parse it as the adapter will, apply it to the reference book of
            // that symbol, and write down the checksum that book computes.
            json probe = {{"channel", "level3"}, {"type", type}, {"data", json::array({entry})}};
            const order_book::KrakenL3Message parsed =
                order_book::ParseKrakenL3Messages(probe, kScale, order_book::HashOrderId).front();
            order_book::KrakenL3Policy& reference = ReferenceFor(parsed.symbol);
            const order_book::ChecksumMeta placeholder{0};
            if (parsed.is_snapshot) {
                reference.ApplySnapshot(parsed.Snapshot(), placeholder);
            } else {
                reference.ApplyBatch(std::span<const order_book::KrakenL3Update>(parsed.orders),
                                     placeholder);
            }
            entry["checksum"] = order_book::KrakenL3Checksum(reference.Book().GetBookView(
                static_cast<std::size_t>(order_book::kKrakenChecksumLevels)));
            message["data"].push_back(entry);
        }
        return message.dump();
    }

    order_book::KrakenL3Policy& ReferenceFor(const std::string& symbol) {
        return references_.try_emplace(symbol, kDepth).first->second;
    }

    std::map<std::string, order_book::KrakenL3Policy> references_;
    std::uint64_t clock_ = 0;
};

feed_handler::CaptureFrame FrameOf(const std::string& text, std::uint64_t sequence = 0) {
    return feed_handler::CaptureFrame{
        .payload = std::as_bytes(std::span<const char>(text)),
        .capture_sequence = sequence,
        .monotonic_ns = 0,
        .source = feed_handler::FrameSource::kUnknown,
    };
}

Order MakeOrder(const std::string& id, double price, double quantity = 1.0) {
    return Order{id, price, quantity};
}

// A connection with one Kraken book, ready to take frames.
struct Fixture {
    BookAdapter adapter;
    ConnectionHandle conn = adapter.AddConnection("kraken-1", kKrakenSettings);
    Feed feed;

    void Frame(const std::string& text) {
        adapter.OnFrame(conn, FrameOf(text));
    }

    [[nodiscard]] const KrakenBook& Book(std::string_view symbol) const {
        const KrakenBook* book = adapter.FindKrakenBook(conn, symbol);
        if (book == nullptr) {
            // gtest reports an exception out of a test body as that test failing.
            throw std::runtime_error("no book for " + std::string(symbol));
        }
        return *book;
    }
};

TEST(BookAdapter, TwoSymbolsOnOneConnectionStayIndependent) {
    Fixture fx;
    fx.adapter.OnConnect(fx.conn, 1, "test");
    fx.Frame(fx.feed.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)}));
    fx.Frame(fx.feed.Snapshot("ETH/USD", {MakeOrder("E1", 50.0)}, {MakeOrder("E2", 51.0)}));
    ASSERT_EQ(fx.adapter.BookCount(fx.conn), 2U);

    // A bid better than BTC's best moves only BTC's top of book.
    fx.Frame(fx.feed.Add("BTC/USD", Side::kBid, MakeOrder("B2", 100.5)));
    EXPECT_EQ(fx.Book("BTC/USD").Best(Side::kBid)->price, order_book::Price(1005));
    EXPECT_EQ(fx.Book("ETH/USD").Best(Side::kBid)->price, order_book::Price(500));
    EXPECT_EQ(fx.adapter.Stats(fx.conn).TotalIssues(), 0U);

    // One frame whose data[] holds an entry for each symbol is routed by entry.
    fx.Frame(fx.feed.AddToEach(
        {{"BTC/USD", MakeOrder("B3", 100.7)}, {"ETH/USD", MakeOrder("E3", 50.5)}}));
    EXPECT_EQ(fx.Book("BTC/USD").Best(Side::kBid)->price, order_book::Price(1007));
    EXPECT_EQ(fx.Book("ETH/USD").Best(Side::kBid)->price, order_book::Price(505));

    // A failed checksum on one symbol desyncs that symbol only.
    fx.Frame(Feed::WithWrongChecksum(fx.feed.Add("BTC/USD", Side::kBid, MakeOrder("B4", 100.9))));
    EXPECT_EQ(fx.Book("BTC/USD").GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(fx.Book("ETH/USD").GetReadiness(), Readiness::kReady);
    fx.Frame(fx.feed.Add("ETH/USD", Side::kBid, MakeOrder("E4", 50.7)));
    EXPECT_EQ(fx.Book("ETH/USD").Best(Side::kBid)->price, order_book::Price(507));

    const ConnectionStats& stats = fx.adapter.Stats(fx.conn);
    EXPECT_EQ(stats.snapshots, 2U);
    EXPECT_EQ(stats.updates, 5U);
    EXPECT_EQ(stats.IssueCount(IntegrityIssue::kChecksumMismatch), 1U);
    EXPECT_EQ(stats.TotalIssues(), 1U);
    EXPECT_EQ(stats.parse_errors, 0U);
}

TEST(BookAdapter, IntegrityIssuesAreCountedByKindAndNeverThrow) {
    Fixture fx;
    fx.adapter.OnConnect(fx.conn, 1, "test");
    fx.Frame(fx.feed.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)}));

    // Deleting an order the book never saw: the first issue found is the one
    // reported, though the checksum no longer matches either.
    fx.Frame(fx.feed.Delete("BTC/USD", Side::kBid, MakeOrder("NOPE", 100.0)));

    const ConnectionStats& stats = fx.adapter.Stats(fx.conn);
    EXPECT_EQ(stats.IssueCount(IntegrityIssue::kUnknownOrder), 1U);
    EXPECT_EQ(stats.IssueCount(IntegrityIssue::kChecksumMismatch), 0U);
    EXPECT_EQ(stats.TotalIssues(), 1U);
    EXPECT_EQ(fx.Book("BTC/USD").GetReadiness(), Readiness::kDesynced);
}

TEST(BookAdapter, ConnectResetsThatConnectionsBooksOnly) {
    BookAdapter adapter;
    Feed feed_a;
    Feed feed_b;
    const ConnectionHandle a = adapter.AddConnection("a", kKrakenSettings);
    const ConnectionHandle b = adapter.AddConnection("b", kKrakenSettings);
    adapter.OnConnect(a, 1, "start");
    adapter.OnConnect(b, 1, "start");
    adapter.OnFrame(
        a, FrameOf(feed_a.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)})));
    adapter.OnFrame(
        b, FrameOf(feed_b.Snapshot("BTC/USD", {MakeOrder("B1", 200.0)}, {MakeOrder("A1", 201.0)})));
    ASSERT_EQ(adapter.BookCount(a), 1U);

    adapter.OnConnect(a, 2, "reconnect");

    EXPECT_EQ(adapter.ConnectId(a), 2U);
    EXPECT_EQ(adapter.BookCount(a), 0U);
    EXPECT_EQ(adapter.FindKrakenBook(a, "BTC/USD"), nullptr);
    ASSERT_NE(adapter.FindKrakenBook(b, "BTC/USD"), nullptr);
    EXPECT_EQ(adapter.FindKrakenBook(b, "BTC/USD")->Best(Side::kBid)->price,
              order_book::Price(2000));
    EXPECT_EQ(adapter.ConnectId(b), 1U);

    // The new connection's snapshot builds a fresh book from nothing.
    Feed fresh;
    adapter.OnFrame(
        a, FrameOf(fresh.Snapshot("BTC/USD", {MakeOrder("X1", 300.0)}, {MakeOrder("Y1", 301.0)})));
    const KrakenBook* rebuilt = adapter.FindKrakenBook(a, "BTC/USD");
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_EQ(rebuilt->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(rebuilt->Best(Side::kBid)->price, order_book::Price(3000));
    EXPECT_EQ(adapter.Stats(a).connects, 2U);
}

TEST(BookAdapter, DisconnectMarksTheConnectionStaleUntilTheNextConnect) {
    Fixture fx;
    fx.adapter.OnConnect(fx.conn, 1, "test");
    fx.Frame(fx.feed.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)}));

    fx.adapter.OnDisconnect(fx.conn, 1);

    EXPECT_TRUE(fx.adapter.IsStale(fx.conn));
    // The books are kept, for inspection.
    EXPECT_EQ(fx.adapter.BookCount(fx.conn), 1U);
    fx.Frame(fx.feed.Add("BTC/USD", Side::kBid, MakeOrder("B2", 100.5)));
    EXPECT_EQ(fx.Book("BTC/USD").Best(Side::kBid)->price, order_book::Price(1000));
    const ConnectionStats& stats = fx.adapter.Stats(fx.conn);
    EXPECT_EQ(stats.frames, 2U);
    EXPECT_EQ(stats.frames_ignored_stale, 1U);
    EXPECT_EQ(stats.updates, 0U);
    EXPECT_EQ(stats.disconnects, 1U);

    fx.adapter.OnConnect(fx.conn, 2, "reconnect");

    EXPECT_FALSE(fx.adapter.IsStale(fx.conn));
    EXPECT_EQ(fx.adapter.BookCount(fx.conn), 0U);
    Feed fresh;
    fx.Frame(fresh.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)}));
    EXPECT_EQ(fx.Book("BTC/USD").GetReadiness(), Readiness::kReady);
    EXPECT_EQ(stats.frames_ignored_stale, 1U);
}

TEST(BookAdapter, DroppedFramesDesyncTheConnectionsBooksUntilASnapshot) {
    BookAdapter adapter;
    Feed feed;
    Feed other_feed;
    const ConnectionHandle conn = adapter.AddConnection("a", kKrakenSettings);
    const ConnectionHandle other = adapter.AddConnection("b", kKrakenSettings);
    adapter.OnConnect(conn, 1, "start");
    adapter.OnConnect(other, 1, "start");
    adapter.OnFrame(conn, FrameOf(feed.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)},
                                                {MakeOrder("A1", 101.0)})));
    adapter.OnFrame(
        conn, FrameOf(feed.Snapshot("ETH/USD", {MakeOrder("E1", 50.0)}, {MakeOrder("E2", 51.0)})));
    adapter.OnFrame(other, FrameOf(other_feed.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)},
                                                       {MakeOrder("A1", 101.0)})));

    adapter.OnFramesDropped(conn, 3);
    adapter.OnFramesDropped(conn, 0);

    EXPECT_EQ(adapter.Stats(conn).drops, 3U);
    EXPECT_EQ(adapter.FindKrakenBook(conn, "BTC/USD")->GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(adapter.FindKrakenBook(conn, "ETH/USD")->GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(adapter.FindKrakenBook(other, "BTC/USD")->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(adapter.Stats(other).drops, 0U);

    // Updates that follow are counted, not applied.
    adapter.OnFrame(conn, FrameOf(feed.Add("BTC/USD", Side::kBid, MakeOrder("B2", 100.5))));
    EXPECT_EQ(adapter.Stats(conn).updates_while_desynced, 1U);
    EXPECT_EQ(adapter.FindKrakenBook(conn, "BTC/USD")->Best(Side::kBid)->price,
              order_book::Price(1000));
    EXPECT_EQ(adapter.Stats(conn).TotalIssues(), 0U);

    // A fresh snapshot is a new truth and recovers the book.
    Feed resnap;
    adapter.OnFrame(conn, FrameOf(resnap.Snapshot("BTC/USD", {MakeOrder("B9", 110.0)},
                                                  {MakeOrder("A9", 111.0)})));
    EXPECT_EQ(adapter.FindKrakenBook(conn, "BTC/USD")->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(adapter.FindKrakenBook(conn, "BTC/USD")->Best(Side::kBid)->price,
              order_book::Price(1100));
    EXPECT_EQ(adapter.FindKrakenBook(conn, "ETH/USD")->GetReadiness(), Readiness::kDesynced);
}

TEST(BookAdapter, MalformedPayloadIsCountedAndDesyncsWithoutThrowing) {
    Fixture fx;
    fx.adapter.OnConnect(fx.conn, 1, "test");
    fx.Frame(fx.feed.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)}));

    fx.Frame("this is not json");
    EXPECT_EQ(fx.adapter.Stats(fx.conn).parse_errors, 1U);
    EXPECT_EQ(fx.Book("BTC/USD").GetReadiness(), Readiness::kDesynced);

    // Valid JSON that is a level3 message missing its fields fails to parse too.
    Feed fresh;
    fx.Frame(fresh.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)}));
    ASSERT_EQ(fx.Book("BTC/USD").GetReadiness(), Readiness::kReady);
    fx.Frame(R"({"channel":"level3","type":"update","data":[{"symbol":"BTC/USD"}]})");
    fx.Frame("");

    const ConnectionStats& stats = fx.adapter.Stats(fx.conn);
    EXPECT_EQ(stats.parse_errors, 3U);
    EXPECT_EQ(stats.frames, 5U);
    EXPECT_EQ(fx.Book("BTC/USD").GetReadiness(), Readiness::kDesynced);
}

TEST(BookAdapter, UpdateBeforeSnapshotIsCountedNotApplied) {
    Fixture fx;
    fx.adapter.OnConnect(fx.conn, 1, "test");

    fx.Frame(fx.feed.Add("BTC/USD", Side::kBid, MakeOrder("B1", 100.0)));

    const ConnectionStats& stats = fx.adapter.Stats(fx.conn);
    EXPECT_EQ(stats.updates, 1U);
    EXPECT_EQ(stats.updates_before_snapshot, 1U);
    EXPECT_EQ(fx.adapter.BookCount(fx.conn), 0U);
    EXPECT_EQ(fx.adapter.FindKrakenBook(fx.conn, "BTC/USD"), nullptr);
    EXPECT_EQ(stats.TotalIssues(), 0U);
}

TEST(BookAdapter, FramesThatCarryNoBookDataAreOnlyCounted) {
    Fixture fx;
    fx.adapter.OnConnect(fx.conn, 1, "test");

    fx.Frame(R"({"channel":"heartbeat"})");
    fx.Frame(R"({"channel":"status","type":"update","data":[{"system":"online"}]})");

    const ConnectionStats& stats = fx.adapter.Stats(fx.conn);
    EXPECT_EQ(stats.frames, 2U);
    EXPECT_EQ(stats.snapshots + stats.updates + stats.parse_errors, 0U);
}

TEST(BookAdapter, FramesOfASourceWithNoBookAreCountedAsUnsupported) {
    BookAdapter adapter;
    // kUnknown is the one source the adapter has no book for: a journal does not
    // record it, so a caller that never said what the connection carries gets it.
    const ConnectionHandle conn = adapter.AddConnection(
        "unknown", BookSettings{.source = feed_handler::FrameSource::kUnknown, .scale = kScale});
    adapter.OnConnect(conn, 1, "test");

    adapter.OnFrame(conn, FrameOf("some payload"));

    EXPECT_EQ(adapter.Stats(conn).frames, 1U);
    EXPECT_EQ(adapter.Stats(conn).frames_unsupported, 1U);
    EXPECT_EQ(adapter.BookCount(conn), 0U);
}

TEST(BookAdapter, FeedEventsDriveTheSameStateAsDirectCalls) {
    Fixture fx;
    Feed feed;
    const std::string snapshot =
        feed.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)});
    const feed_handler::CaptureFrame frame = FrameOf(snapshot, 7);

    fx.adapter.OnEvent(fx.conn, feed_handler::ConnectEvent{.connect_id = 4, .reason = "test"});
    fx.adapter.OnEvent(fx.conn, feed_handler::MakeFrameEvent(frame));
    EXPECT_EQ(fx.adapter.ConnectId(fx.conn), 4U);
    EXPECT_EQ(fx.Book("BTC/USD").GetReadiness(), Readiness::kReady);

    fx.adapter.OnEvent(fx.conn, feed_handler::DisconnectEvent{.connect_id = 4});
    EXPECT_TRUE(fx.adapter.IsStale(fx.conn));
}

TEST(BookAdapter, TimingIsRecordedOnlyWhenAskedFor) {
    Feed feed;
    const std::string snapshot =
        feed.Snapshot("BTC/USD", {MakeOrder("B1", 100.0)}, {MakeOrder("A1", 101.0)});

    BookAdapter untimed;
    const ConnectionHandle plain = untimed.AddConnection("plain", kKrakenSettings);
    untimed.OnConnect(plain, 1, "test");
    untimed.OnFrame(plain, FrameOf(snapshot));
    EXPECT_EQ(untimed.Stats(plain).parse_ns, 0U);
    EXPECT_EQ(untimed.Stats(plain).apply_ns, 0U);

    BookAdapter timed(BookAdapter::Config{.measure_timing = true});
    const ConnectionHandle measured = timed.AddConnection("measured", kKrakenSettings);
    timed.OnConnect(measured, 1, "test");
    timed.OnFrame(measured, FrameOf(snapshot));
    EXPECT_GT(timed.Stats(measured).parse_ns, 0U);
    EXPECT_GT(timed.Stats(measured).apply_ns, 0U);
    EXPECT_EQ(timed.Stats(measured).snapshots, 1U);
}

}  // namespace
}  // namespace book_adapter
