#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "book_adapter/book_adapter.h"
#include "book_adapter/book_settings.h"
#include "book_adapter/deribit_fix_book_wire.h"
#include "book_adapter/journal_replay.h"
#include "feed_handler/fix/fix_message.h"
#include "feed_handler/journal_writer.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/test_support.h"
#include "order_book/instrument_scale.h"
#include "order_book/l2_policy.h"
#include "order_book/types.h"

namespace book_adapter {
namespace {

namespace fix = feed_handler::fix;
using order_book::IntegrityIssue;
using order_book::Price;
using order_book::Quantity;
using order_book::Readiness;
using order_book::Side;

// Deribit's BTC-PERPETUAL ticks at 0.5 and trades in whole contracts, so one
// price decimal and no quantity decimals represent it exactly.
constexpr order_book::InstrumentScale kScale(/*price_decimals=*/1, /*quantity_decimals=*/0);

const BookSettings kDeribitSettings{
    .source = feed_handler::FrameSource::kDeribitFix,
    .scale = kScale,
};

constexpr std::string_view kBtc = "BTC-PERPETUAL";
constexpr std::string_view kEth = "ETH-PERPETUAL";

// The Deribit tags a book has no use for but a real message carries, so the
// group reader has to skip past them (exchanges/deribit.md).
constexpr int kContractMultiplier = 231;
constexpr int kTag746 = 746;
constexpr int kDeribitTag100087 = 100087;
constexpr int kDeribitTag100090 = 100090;
constexpr int kDeribitTag100092 = 100092;
constexpr int kDeribitTag100093 = 100093;

// MDEntryType and MDUpdateAction values, spelled as the wire spells them.
constexpr std::string_view kBid = "0";
constexpr std::string_view kOffer = "1";
constexpr std::string_view kNew = "0";
constexpr std::string_view kChange = "1";
constexpr std::string_view kDelete = "2";

/// One MD entry. An empty member is left off the wire.
struct Entry {
    std::string_view update_action;  // 279, 35=X only
    std::string_view entry_type;     // 269
    std::string_view price;          // 270
    std::string_view size;           // 271
};

// A snapshot entry: no MDUpdateAction, as on the wire.
Entry Level(std::string_view side, std::string_view price, std::string_view size) {
    return Entry{.update_action = "", .entry_type = side, .price = price, .size = size};
}

Entry Act(std::string_view action, std::string_view side, std::string_view price,
          std::string_view size) {
    return Entry{.update_action = action, .entry_type = side, .price = price, .size = size};
}

void Append(std::vector<fix::Field>& fields, int tag_number, std::string_view value) {
    if (!value.empty()) {
        fields.push_back({.tag = tag_number, .value = std::string(value)});
    }
}

fix::SessionHeader HeaderFor(std::string_view msg_type, std::uint64_t seq_num) {
    return fix::SessionHeader{
        .msg_type = msg_type,
        .sender_comp_id = "DERIBITSERVER",
        .target_comp_id = "CLIENT",
        .msg_seq_num = seq_num,
        .sending_time = "20260921-10:00:00.000",
    };
}

// A 35=W or 35=X for `symbol`, built with the project's own FIX builder.
// `declared_count` overrides NoMDEntries(268), which is how a group that ends
// before its declared count is made.
enum class Kind : std::uint8_t { kSnapshot, kIncremental };

std::string MarketData(Kind kind, std::string_view symbol, const std::vector<Entry>& entries,
                       std::optional<std::size_t> declared_count = std::nullopt) {
    const bool is_snapshot = kind == Kind::kSnapshot;
    std::vector<fix::Field> body;
    Append(body, fix::tag::kSymbol, symbol);
    Append(body, kContractMultiplier, "10");
    Append(body, kTag746, "0");
    Append(body, kDeribitTag100087, "1");
    Append(body, kDeribitTag100090, "2");
    if (is_snapshot) {
        Append(body, kDeribitTag100092, "3");
        Append(body, kDeribitTag100093, "4");
    }
    Append(body, fix::tag::kMdReqId, "req-1");
    Append(body, fix::tag::kNoMdEntries, std::to_string(declared_count.value_or(entries.size())));
    for (const Entry& entry : entries) {
        Append(body, fix::tag::kMdUpdateAction, entry.update_action);
        Append(body, fix::tag::kMdEntryType, entry.entry_type);
        Append(body, fix::tag::kMdEntryPx, entry.price);
        Append(body, fix::tag::kMdEntrySize, entry.size);
        Append(body, fix::tag::kMdEntryDate, "20260921");
    }
    return fix::BuildMessage(HeaderFor(is_snapshot ? "W" : "X", 2), body);
}

std::string Snapshot(std::string_view symbol, const std::vector<Entry>& entries) {
    return MarketData(Kind::kSnapshot, symbol, entries);
}

std::string Incremental(std::string_view symbol, const std::vector<Entry>& entries) {
    return MarketData(Kind::kIncremental, symbol, entries);
}

// A session message with no market data in it.
std::string Session(std::string_view msg_type, std::vector<fix::Field> body = {}) {
    return fix::BuildMessage(HeaderFor(msg_type, 1), body);
}

// The book both sides of BTC-PERPETUAL open with in most tests:
//   bids  64000.0 x100, 63999.5 x200, 63999.0 x300
//   asks  64000.5 x50,  64001.0 x75
std::string OpeningSnapshot(std::string_view symbol = kBtc) {
    return Snapshot(symbol, {Level(kBid, "64000.0", "100"), Level(kBid, "63999.5", "200"),
                             Level(kBid, "63999.0", "300"), Level(kOffer, "64000.5", "50"),
                             Level(kOffer, "64001.0", "75")});
}

std::span<const std::byte> BytesOf(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

feed_handler::CaptureFrame FrameOf(const std::string& text) {
    return feed_handler::CaptureFrame{
        .payload = BytesOf(text),
        .capture_sequence = 0,
        .monotonic_ns = 0,
        .source = feed_handler::FrameSource::kUnknown,
    };
}

// A Deribit connection ready to take frames, with the books' top of book as the
// thing the tests read back.
struct Fixture {
    BookAdapter adapter;
    ConnectionHandle conn = adapter.AddConnection("deribit-1", kDeribitSettings);

    Fixture() {
        adapter.OnConnect(conn, 1, "test");
    }

    void Frame(const std::string& text) {
        adapter.OnFrame(conn, FrameOf(text));
    }

    [[nodiscard]] const DeribitBook* Book(std::string_view symbol) const {
        return adapter.FindDeribitBook(conn, symbol);
    }

    [[nodiscard]] const ConnectionStats& Stats() const {
        return adapter.Stats(conn);
    }
};

// The price of `book`'s best level on `side`, in the scale's decimals (a plain
// double: 64000.5 is exact in binary), or nullopt for an empty side.
std::optional<double> BestPrice(const DeribitBook* book, Side side) {
    if (book == nullptr) {
        return std::nullopt;
    }
    const auto best = book->Best(side);
    if (!best) {
        return std::nullopt;
    }
    return kScale.FromPrice(best->price);
}

std::optional<std::int64_t> BestLots(const DeribitBook* book, Side side) {
    if (book == nullptr) {
        return std::nullopt;
    }
    const auto best = book->Best(side);
    if (!best) {
        return std::nullopt;
    }
    return best->quantity.Lots();
}

// ---------------------------------------------------------------------------
// The wire parser, without a book.
// ---------------------------------------------------------------------------

TEST(DeribitFixBook, SnapshotParsesIntoScaledLevelsOfBothSides) {
    const std::string raw = OpeningSnapshot();
    const auto parsed = ParseDeribitFixMessage(BytesOf(raw), kScale);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().what;

    EXPECT_EQ(parsed->kind, FixBookMessageKind::kSnapshot);
    EXPECT_EQ(parsed->symbol, "BTC-PERPETUAL");
    EXPECT_TRUE(parsed->updates.empty());
    const auto& levels = parsed->snapshot.levels;
    ASSERT_EQ(levels.size(), 5U);
    EXPECT_EQ(levels[0].side, Side::kBid);
    EXPECT_EQ(levels[0].price, Price(640000));
    EXPECT_EQ(levels[0].quantity, Quantity(100));
    EXPECT_EQ(levels[0].operation, order_book::L2Operation::kNew);
    EXPECT_EQ(levels[2].price, Price(639990));
    EXPECT_EQ(levels[3].side, Side::kAsk);
    EXPECT_EQ(levels[3].price, Price(640005));
    EXPECT_EQ(levels[3].quantity, Quantity(50));
    EXPECT_EQ(levels[4].price, Price(640010));
}

TEST(DeribitFixBook, IncrementalParsesEachActionAndKeepsWireOrder) {
    const std::string raw =
        Incremental(kBtc, {Act(kNew, kBid, "64000.5", "10"), Act(kChange, kOffer, "64001.0", "80"),
                           Act(kDelete, kBid, "63999.0", "0")});
    const auto parsed = ParseDeribitFixMessage(BytesOf(raw), kScale);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().what;

    EXPECT_EQ(parsed->kind, FixBookMessageKind::kUpdate);
    EXPECT_EQ(parsed->symbol, "BTC-PERPETUAL");
    ASSERT_EQ(parsed->updates.size(), 3U);
    EXPECT_EQ(parsed->updates[0].operation, order_book::L2Operation::kNew);
    EXPECT_EQ(parsed->updates[0].side, Side::kBid);
    EXPECT_EQ(parsed->updates[0].price, Price(640005));
    EXPECT_EQ(parsed->updates[0].quantity, Quantity(10));
    EXPECT_EQ(parsed->updates[1].operation, order_book::L2Operation::kChange);
    EXPECT_EQ(parsed->updates[1].side, Side::kAsk);
    EXPECT_EQ(parsed->updates[1].quantity, Quantity(80));
    EXPECT_EQ(parsed->updates[2].operation, order_book::L2Operation::kDelete);
    EXPECT_EQ(parsed->updates[2].price, Price(639990));
}

TEST(DeribitFixBook, DeleteMayOmitItsSizeButNewAndChangeMayNot) {
    const std::string delete_without_size = Incremental(kBtc, {Act(kDelete, kBid, "63999.0", "")});
    const auto deleted = ParseDeribitFixMessage(BytesOf(delete_without_size), kScale);
    ASSERT_TRUE(deleted.has_value()) << deleted.error().what;
    ASSERT_EQ(deleted->updates.size(), 1U);
    EXPECT_EQ(deleted->updates[0].quantity, Quantity(0));

    for (const std::string_view action : {kNew, kChange}) {
        const std::string no_size = Incremental(kBtc, {Act(action, kBid, "63999.0", "")});
        const auto parsed = ParseDeribitFixMessage(BytesOf(no_size), kScale);
        ASSERT_FALSE(parsed.has_value()) << "action " << action;
        EXPECT_EQ(parsed.error().symbol, "BTC-PERPETUAL");
    }
}

TEST(DeribitFixBook, SessionMessagesYieldNothing) {
    const std::vector<std::string> messages = {
        Session("A", {{.tag = fix::tag::kHeartBtInt, .value = "30"}}),  Session("0"),
        Session("1", {{.tag = fix::tag::kTestReqId, .value = "ping"}}), Session("5"),
        Session("3", {{.tag = fix::tag::kText, .value = "rejected"}}),  Session("j"),
        Session("Y", {{.tag = fix::tag::kMdReqId, .value = "req-1"}}),
    };
    for (const std::string& raw : messages) {
        const auto parsed = ParseDeribitFixMessage(BytesOf(raw), kScale);
        ASSERT_TRUE(parsed.has_value()) << parsed.error().what;
        EXPECT_EQ(parsed->kind, FixBookMessageKind::kIgnored);
        EXPECT_TRUE(parsed->snapshot.levels.empty());
        EXPECT_TRUE(parsed->updates.empty());
    }
}

TEST(DeribitFixBook, MalformedMessagesAreErrorsAndNameTheSymbolWhenTheyCan) {
    // Shorter than the group claims.
    const std::string truncated = MarketData(Kind::kSnapshot, kBtc, {Level(kBid, "1.0", "1")}, 3);
    const auto short_group = ParseDeribitFixMessage(BytesOf(truncated), kScale);
    ASSERT_FALSE(short_group.has_value());
    EXPECT_EQ(short_group.error().symbol, "BTC-PERPETUAL");
    EXPECT_NE(short_group.error().what.find("268"), std::string::npos) << short_group.error().what;

    // Neither a bid nor an offer (2 is a trade in FIX).
    const std::string trade = Snapshot(kBtc, {Level("2", "64000.0", "1")});
    EXPECT_FALSE(ParseDeribitFixMessage(BytesOf(trade), kScale).has_value());

    // A price that is not a number, and one with junk after the digits.
    for (const std::string_view price : {"abc", "64000.0x", "nan", "inf"}) {
        const std::string bad = Snapshot(kBtc, {Level(kBid, price, "1")});
        EXPECT_FALSE(ParseDeribitFixMessage(BytesOf(bad), kScale).has_value()) << price;
    }

    // A negative size.
    const std::string negative = Snapshot(kBtc, {Level(kBid, "64000.0", "-1")});
    EXPECT_FALSE(ParseDeribitFixMessage(BytesOf(negative), kScale).has_value());

    // An update action FIX does not define for this feed.
    const std::string bad_action = Incremental(kBtc, {Act("7", kBid, "64000.0", "1")});
    EXPECT_FALSE(ParseDeribitFixMessage(BytesOf(bad_action), kScale).has_value());

    // A 35=X whose entries carry no MDUpdateAction: the 35=W shape read as a
    // 35=X is not tolerated (exchanges/deribit.md).
    const std::string snapshot_shaped =
        MarketData(Kind::kIncremental, kBtc, {Level(kBid, "64000.0", "1")});
    EXPECT_FALSE(ParseDeribitFixMessage(BytesOf(snapshot_shaped), kScale).has_value());

    // No symbol: nobody to blame.
    const std::string no_symbol = MarketData(Kind::kSnapshot, "", {Level(kBid, "64000.0", "1")});
    const auto missing = ParseDeribitFixMessage(BytesOf(no_symbol), kScale);
    ASSERT_FALSE(missing.has_value());
    EXPECT_TRUE(missing.error().symbol.empty());

    // A failed CheckSum, and something that is not FIX at all.
    std::string corrupt = OpeningSnapshot();
    const std::size_t digit = corrupt.find("64000.0");
    ASSERT_NE(digit, std::string::npos);
    corrupt[digit] = '7';
    const auto bad_envelope = ParseDeribitFixMessage(BytesOf(corrupt), kScale);
    ASSERT_FALSE(bad_envelope.has_value());
    EXPECT_TRUE(bad_envelope.error().symbol.empty());
    EXPECT_FALSE(ParseDeribitFixMessage(BytesOf("not fix at all"), kScale).has_value());
}

TEST(DeribitFixBook, ErrorTextNeverCarriesFieldValues) {
    const std::string bad = Snapshot(kBtc, {Level(kBid, "SECRET-PRICE", "1")});
    const auto parsed = ParseDeribitFixMessage(BytesOf(bad), kScale);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().what.find("SECRET"), std::string::npos) << parsed.error().what;
}

// ---------------------------------------------------------------------------
// Books driven through the adapter.
// ---------------------------------------------------------------------------

TEST(DeribitFixBook, SnapshotThenIncrementalsGiveExactLevelsAndTopOfBook) {
    Fixture env;
    env.Frame(OpeningSnapshot());

    const DeribitBook* book = env.Book(kBtc);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(BestPrice(book, Side::kBid), 64000.0);
    EXPECT_EQ(BestLots(book, Side::kBid), 100);
    EXPECT_EQ(BestPrice(book, Side::kAsk), 64000.5);
    EXPECT_EQ(BestLots(book, Side::kAsk), 50);

    // One message, three entries, applied as one batch: a new best bid inside
    // the spread, the best ask's size changed, and a new ask behind the others.
    env.Frame(
        Incremental(kBtc, {Act(kNew, kBid, "64000.2", "10"), Act(kChange, kOffer, "64000.5", "60"),
                           Act(kNew, kOffer, "64001.5", "5")}));
    EXPECT_EQ(BestPrice(book, Side::kBid), 64000.2);
    EXPECT_EQ(BestLots(book, Side::kBid), 10);
    EXPECT_EQ(BestPrice(book, Side::kAsk), 64000.5);
    EXPECT_EQ(BestLots(book, Side::kAsk), 60);

    // The asks behind the top, read by removing what is in front of them: each
    // level keeps exactly the quantity it was given.
    env.Frame(Incremental(kBtc, {Act(kDelete, kOffer, "64000.5", "0")}));
    EXPECT_EQ(BestPrice(book, Side::kAsk), 64001.0);
    EXPECT_EQ(BestLots(book, Side::kAsk), 75);
    env.Frame(Incremental(kBtc, {Act(kDelete, kOffer, "64001.0", "0")}));
    EXPECT_EQ(BestPrice(book, Side::kAsk), 64001.5);
    EXPECT_EQ(BestLots(book, Side::kAsk), 5);

    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(env.Stats().TotalIssues(), 0U);
    EXPECT_EQ(env.Stats().snapshots, 1U);
    EXPECT_EQ(env.Stats().updates, 3U);
}

TEST(DeribitFixBook, ABatchThatCrossesTheBookIsAnIntegrityIssue) {
    Fixture env;
    env.Frame(OpeningSnapshot());
    const DeribitBook* book = env.Book(kBtc);
    ASSERT_NE(book, nullptr);

    // A bid at the best ask's price.
    env.Frame(Incremental(kBtc, {Act(kNew, kBid, "64000.5", "10")}));

    EXPECT_EQ(book->GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(env.Stats().IssueCount(IntegrityIssue::kCrossedBook), 1U);
    EXPECT_EQ(env.Stats().TotalIssues(), 1U);
}

TEST(DeribitFixBook, ChangeReplacesTheQuantityAndDeleteRemovesTheLevel) {
    Fixture env;
    env.Frame(OpeningSnapshot());
    const DeribitBook* book = env.Book(kBtc);
    ASSERT_NE(book, nullptr);

    env.Frame(Incremental(kBtc, {Act(kChange, kBid, "64000.0", "150")}));
    EXPECT_EQ(BestPrice(book, Side::kBid), 64000.0);
    EXPECT_EQ(BestLots(book, Side::kBid), 150);

    // Delete the best bid: the next level, still with its snapshot quantity,
    // becomes the top. The size a delete carries is not what removes the level.
    env.Frame(Incremental(kBtc, {Act(kDelete, kBid, "64000.0", "0")}));
    EXPECT_EQ(BestPrice(book, Side::kBid), 63999.5);
    EXPECT_EQ(BestLots(book, Side::kBid), 200);

    // And the next, with no size on the wire at all.
    env.Frame(Incremental(kBtc, {Act(kDelete, kBid, "63999.5", "")}));
    EXPECT_EQ(BestPrice(book, Side::kBid), 63999.0);
    EXPECT_EQ(BestLots(book, Side::kBid), 300);

    // The asks were never touched.
    EXPECT_EQ(BestPrice(book, Side::kAsk), 64000.5);
    EXPECT_EQ(BestLots(book, Side::kAsk), 50);

    // Delete the last bid: the side is empty.
    env.Frame(Incremental(kBtc, {Act(kDelete, kBid, "63999.0", "0")}));
    EXPECT_EQ(BestPrice(book, Side::kBid), std::nullopt);
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(env.Stats().TotalIssues(), 0U);
    EXPECT_EQ(env.Stats().snapshots, 1U);
    EXPECT_EQ(env.Stats().updates, 4U);
}

TEST(DeribitFixBook, DeletedLevelCanComeBackWithNew) {
    Fixture env;
    env.Frame(OpeningSnapshot());
    const DeribitBook* book = env.Book(kBtc);
    ASSERT_NE(book, nullptr);

    env.Frame(Incremental(kBtc, {Act(kDelete, kBid, "64000.0", "0")}));
    env.Frame(Incremental(kBtc, {Act(kNew, kBid, "64000.0", "7")}));
    EXPECT_EQ(BestPrice(book, Side::kBid), 64000.0);
    EXPECT_EQ(BestLots(book, Side::kBid), 7);
    EXPECT_EQ(env.Stats().TotalIssues(), 0U);
}

TEST(DeribitFixBook, ChangeOnAnUnknownLevelIsAnIntegrityIssue) {
    Fixture env;
    env.Frame(OpeningSnapshot());
    const DeribitBook* book = env.Book(kBtc);
    ASSERT_NE(book, nullptr);

    env.Frame(Incremental(kBtc, {Act(kChange, kBid, "10.0", "5")}));

    EXPECT_EQ(env.Stats().IssueCount(IntegrityIssue::kUnknownLevel), 1U);
    EXPECT_EQ(env.Stats().TotalIssues(), 1U);
    EXPECT_EQ(book->GetReadiness(), Readiness::kDesynced);

    // Desynced: what follows is counted, not applied, until a snapshot.
    env.Frame(Incremental(kBtc, {Act(kNew, kBid, "64000.2", "1")}));
    EXPECT_EQ(env.Stats().updates_while_desynced, 1U);
    EXPECT_EQ(BestPrice(book, Side::kBid), 64000.0);

    env.Frame(OpeningSnapshot());
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
}

TEST(DeribitFixBook, ASecondSymbolHasItsOwnBook) {
    Fixture env;
    env.Frame(OpeningSnapshot(kBtc));
    env.Frame(Snapshot(kEth, {Level(kBid, "3000.0", "20"), Level(kOffer, "3000.5", "30")}));
    ASSERT_EQ(env.adapter.BookCount(env.conn), 2U);

    env.Frame(Incremental(kEth, {Act(kNew, kBid, "3000.2", "5")}));
    const DeribitBook* btc = env.Book(kBtc);
    const DeribitBook* eth = env.Book(kEth);
    ASSERT_NE(btc, nullptr);
    ASSERT_NE(eth, nullptr);
    EXPECT_EQ(BestPrice(eth, Side::kBid), 3000.2);
    EXPECT_EQ(BestPrice(btc, Side::kBid), 64000.0);

    // A broken message for one symbol desyncs that symbol only.
    env.Frame(Incremental(kBtc, {Act(kChange, kBid, "10.0", "5")}));
    EXPECT_EQ(btc->GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(eth->GetReadiness(), Readiness::kReady);
    env.Frame(Incremental(kEth, {Act(kNew, kBid, "3000.3", "5")}));
    EXPECT_EQ(BestPrice(eth, Side::kBid), 3000.3);

    EXPECT_EQ(env.Stats().snapshots, 2U);
    EXPECT_EQ(env.Stats().updates, 3U);
    EXPECT_EQ(env.Stats().IssueCount(IntegrityIssue::kUnknownLevel), 1U);
}

TEST(DeribitFixBook, SessionMessagesAreIgnoredAndDoNotTouchTheBooks) {
    Fixture env;
    env.Frame(Session("A", {{.tag = fix::tag::kHeartBtInt, .value = "30"}}));
    env.Frame(Session("0"));
    EXPECT_EQ(env.adapter.BookCount(env.conn), 0U);
    env.Frame(OpeningSnapshot());
    env.Frame(Session("1", {{.tag = fix::tag::kTestReqId, .value = "ping"}}));
    env.Frame(Session("3", {{.tag = fix::tag::kText, .value = "rejected"}}));
    env.Frame(Session("j"));
    env.Frame(Session("Y", {{.tag = fix::tag::kMdReqId, .value = "req-1"}}));
    env.Frame(Session("5"));

    const ConnectionStats& stats = env.Stats();
    EXPECT_EQ(stats.frames, 8U);
    EXPECT_EQ(stats.snapshots, 1U);
    EXPECT_EQ(stats.updates, 0U);
    EXPECT_EQ(stats.parse_errors, 0U);
    EXPECT_EQ(stats.frames_unsupported, 0U);
    EXPECT_EQ(stats.TotalIssues(), 0U);
    const DeribitBook* book = env.Book(kBtc);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(BestPrice(book, Side::kBid), 64000.0);
}

TEST(DeribitFixBook, ATruncatedGroupDesyncsThatSymbolsBook) {
    Fixture env;
    env.Frame(OpeningSnapshot(kBtc));
    env.Frame(Snapshot(kEth, {Level(kBid, "3000.0", "20"), Level(kOffer, "3000.5", "30")}));

    // The entry that is there is a valid delete, but the message says two. It
    // must not be applied half way: the book is desynced instead.
    env.Frame(MarketData(Kind::kIncremental, kBtc, {Act(kDelete, kBid, "64000.0", "0")},
                         /*declared_count=*/2));

    const ConnectionStats& stats = env.Stats();
    EXPECT_EQ(stats.parse_errors, 1U);
    EXPECT_EQ(stats.updates, 0U);
    const DeribitBook* btc = env.Book(kBtc);
    const DeribitBook* eth = env.Book(kEth);
    ASSERT_NE(btc, nullptr);
    ASSERT_NE(eth, nullptr);
    EXPECT_EQ(btc->GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(eth->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(BestPrice(btc, Side::kBid), 64000.0) << "the partial entry was not applied";

    // Later incrementals are counted while desynced, and a snapshot recovers it.
    env.Frame(Incremental(kBtc, {Act(kNew, kBid, "64000.2", "1")}));
    EXPECT_EQ(stats.updates_while_desynced, 1U);
    env.Frame(OpeningSnapshot(kBtc));
    EXPECT_EQ(btc->GetReadiness(), Readiness::kReady);
}

TEST(DeribitFixBook, AMalformedEntryDesyncsThatSymbolsBook) {
    Fixture env;
    env.Frame(OpeningSnapshot());
    const DeribitBook* book = env.Book(kBtc);
    ASSERT_NE(book, nullptr);

    env.Frame(Incremental(kBtc, {Act(kNew, kBid, "63990.0", "1"), Act(kNew, kBid, "oops", "1")}));

    EXPECT_EQ(env.Stats().parse_errors, 1U);
    EXPECT_EQ(book->GetReadiness(), Readiness::kDesynced);
    // Neither entry was applied, the valid one included.
    env.Frame(OpeningSnapshot());
    EXPECT_EQ(book->GetReadiness(), Readiness::kReady);
}

TEST(DeribitFixBook, AMessageWithNoSymbolOrABadEnvelopeDesyncsEveryBook) {
    Fixture env;
    env.Frame(OpeningSnapshot(kBtc));
    env.Frame(Snapshot(kEth, {Level(kBid, "3000.0", "20"), Level(kOffer, "3000.5", "30")}));

    std::string corrupt = Incremental(kEth, {Act(kNew, kBid, "3000.2", "5")});
    const std::size_t digit = corrupt.find("3000.2");
    ASSERT_NE(digit, std::string::npos);
    corrupt[digit] = '9';
    env.Frame(corrupt);

    EXPECT_EQ(env.Stats().parse_errors, 1U);
    EXPECT_EQ(env.Book(kBtc)->GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(env.Book(kEth)->GetReadiness(), Readiness::kDesynced);
}

TEST(DeribitFixBook, IncrementalBeforeAnySnapshotIsCountedNotApplied) {
    Fixture env;
    env.Frame(Incremental(kBtc, {Act(kNew, kBid, "64000.0", "1")}));

    EXPECT_EQ(env.Stats().updates, 1U);
    EXPECT_EQ(env.Stats().updates_before_snapshot, 1U);
    EXPECT_EQ(env.adapter.BookCount(env.conn), 0U);
    EXPECT_EQ(env.Stats().parse_errors, 0U);
}

TEST(DeribitFixBook, ConnectResetsTheBooksAndDisconnectMakesFramesStale) {
    Fixture env;
    env.Frame(OpeningSnapshot());
    ASSERT_EQ(env.adapter.BookCount(env.conn), 1U);

    env.adapter.OnDisconnect(env.conn, 1);
    env.Frame(Incremental(kBtc, {Act(kNew, kBid, "63990.0", "1")}));
    EXPECT_EQ(env.Stats().frames_ignored_stale, 1U);
    EXPECT_EQ(env.Stats().updates, 0U);
    EXPECT_EQ(env.adapter.BookCount(env.conn), 1U) << "kept for inspection";

    env.adapter.OnConnect(env.conn, 2, "reconnect");
    EXPECT_EQ(env.adapter.BookCount(env.conn), 0U);
    EXPECT_EQ(env.Book(kBtc), nullptr);
    env.Frame(OpeningSnapshot());
    ASSERT_NE(env.Book(kBtc), nullptr);
    EXPECT_EQ(env.Book(kBtc)->GetReadiness(), Readiness::kReady);
}

TEST(DeribitFixBook, DroppedFramesDesyncTheBooks) {
    Fixture env;
    env.Frame(OpeningSnapshot());
    env.adapter.OnFramesDropped(env.conn, 3);
    ASSERT_NE(env.Book(kBtc), nullptr);
    EXPECT_EQ(env.Book(kBtc)->GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(env.Stats().drops, 3U);
}

TEST(DeribitFixBook, TheConnectionsScaleAppliesToEverySymbol) {
    // Two price decimals and two quantity decimals: 0.05 ticks, 0.25 lots.
    BookAdapter adapter;
    const ConnectionHandle conn =
        adapter.AddConnection("fine", BookSettings{.source = feed_handler::FrameSource::kDeribitFix,
                                                   .scale = order_book::InstrumentScale(2, 2)});
    adapter.OnConnect(conn, 1, "test");
    const std::string raw =
        Snapshot(kBtc, {Level(kBid, "100.05", "0.25"), Level(kOffer, "100.10", "1.5")});
    adapter.OnFrame(conn, FrameOf(raw));

    const DeribitBook* book = adapter.FindDeribitBook(conn, kBtc);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->Best(Side::kBid), (order_book::BookEntry{Price(10005), Quantity(25)}));
    EXPECT_EQ(book->Best(Side::kAsk), (order_book::BookEntry{Price(10010), Quantity(150)}));
}

// ---------------------------------------------------------------------------
// A synthetic journal, replayed.
// ---------------------------------------------------------------------------

class ScopedDir {
  public:
    ScopedDir() : path_(feed_handler::test_support::UniqueTestDir("deribit_fix_book_test")) {
        std::filesystem::create_directories(path_);
    }
    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;
    ScopedDir(ScopedDir&&) = delete;
    ScopedDir& operator=(ScopedDir&&) = delete;
    ~ScopedDir() {
        std::filesystem::remove_all(path_);
    }

    [[nodiscard]] const std::filesystem::path& Path() const {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

// The frames of the synthetic journal, in order (15 of them):
//   the session's Logon, two snapshots, incrementals that apply cleanly, a
//   heartbeat, one incremental cut short (BTC desyncs), one more BTC
//   incremental (counted while desynced), an ETH one that applies, a Logout.
std::vector<std::string> SyntheticFrames() {
    return {
        Session("A", {{.tag = fix::tag::kHeartBtInt, .value = "30"}}),
        OpeningSnapshot(kBtc),
        Snapshot(kEth, {Level(kBid, "3000.0", "20"), Level(kOffer, "3000.5", "30")}),
        Incremental(kBtc, {Act(kNew, kBid, "63998.5", "10")}),
        Incremental(kBtc,
                    {Act(kChange, kBid, "64000.0", "120"), Act(kNew, kOffer, "64002.0", "9")}),
        Incremental(kEth, {Act(kNew, kBid, "2999.5", "4")}),
        Incremental(kBtc, {Act(kDelete, kBid, "63999.0", "0")}),
        Session("0"),
        Incremental(kEth, {Act(kChange, kOffer, "3000.5", "35")}),
        Incremental(kBtc, {Act(kDelete, kOffer, "64001.0", "")}),
        Incremental(kBtc, {Act(kNew, kBid, "63997.0", "1")}),
        MarketData(Kind::kIncremental, kBtc, {Act(kNew, kBid, "63996.0", "1")},
                   /*declared_count=*/3),
        Incremental(kBtc, {Act(kNew, kBid, "63995.0", "1")}),
        Incremental(kEth, {Act(kNew, kBid, "2999.0", "1")}),
        Session("5"),
    };
}

// Writes `frames` as a Deribit journal with the real writer, after a connect
// marker, and replays it. Fails the calling test, and returns an empty report,
// if the replay itself fails.
ReplayReport ReplayJournalOf(const std::vector<std::string>& frames, JournalReplay& replay) {
    const ScopedDir dir;
    const std::filesystem::path path = dir.Path() / "deribit.journal";
    {
        feed_handler::CaptureStamper stamper;
        feed_handler::JournalWriter writer(path, {.exchange = "deribit", .connect_id = 1});
        writer.WriteConnectMarker(stamper.Stamp(BytesOf("test")));
        for (const std::string& frame : frames) {
            writer.OnFrame(stamper.Stamp(BytesOf(frame)));
        }
        writer.Flush();
    }
    const std::vector<std::filesystem::path> paths{path};
    auto report = replay.Run(paths);
    if (!report) {
        ADD_FAILURE() << report.error();
        return ReplayReport{};
    }
    return *report;
}

TEST(DeribitFixBook, SyntheticJournalReplaysToExactCounts) {
    const std::vector<std::string> frames = SyntheticFrames();
    ASSERT_EQ(frames.size(), 15U);
    JournalReplay replay(ReplayOptions{.scale = kScale});
    const ReplayReport report = ReplayJournalOf(frames, replay);

    EXPECT_FALSE(report.StoppedEarly());
    ASSERT_EQ(report.files.size(), 1U);
    EXPECT_EQ(report.files[0].exchange, "deribit");
    EXPECT_EQ(report.wire_messages, frames.size());

    const ConnectionStats& stats = report.stats;
    EXPECT_EQ(stats.frames, 15U);
    EXPECT_EQ(stats.snapshots, 2U);
    // Seven that apply before the cut-short message, then BTC's 63995.0 (counted
    // while desynced) and ETH's 2999.0 (applies). The cut-short message is a
    // parse error, not an update.
    EXPECT_EQ(stats.updates, 9U);
    EXPECT_EQ(stats.updates_while_desynced, 1U);
    EXPECT_EQ(stats.updates_before_snapshot, 0U);
    EXPECT_EQ(stats.parse_errors, 1U);
    EXPECT_EQ(stats.apply_errors, 0U);
    EXPECT_EQ(stats.frames_unsupported, 0U);
    EXPECT_EQ(stats.TotalIssues(), 0U);
    EXPECT_EQ(stats.connects, 1U);
    EXPECT_EQ(stats.disconnects, 1U);
}

TEST(DeribitFixBook, SyntheticJournalLeavesTheExpectedBooks) {
    JournalReplay replay(ReplayOptions{.scale = kScale});
    const ReplayReport report = ReplayJournalOf(SyntheticFrames(), replay);
    ASSERT_EQ(report.wire_messages, 15U);

    const BookAdapter& adapter = replay.Adapter();
    EXPECT_EQ(adapter.BookCount(JournalReplay::kConnection), 2U);
    const DeribitBook* btc = adapter.FindDeribitBook(JournalReplay::kConnection, kBtc);
    const DeribitBook* eth = adapter.FindDeribitBook(JournalReplay::kConnection, kEth);
    ASSERT_NE(btc, nullptr);
    ASSERT_NE(eth, nullptr);
    EXPECT_EQ(btc->GetReadiness(), Readiness::kDesynced);
    EXPECT_EQ(eth->GetReadiness(), Readiness::kReady);
    EXPECT_EQ(BestPrice(eth, Side::kBid), 3000.0);
    EXPECT_EQ(BestPrice(eth, Side::kAsk), 3000.5);
    EXPECT_EQ(BestLots(eth, Side::kAsk), 35);
    // BTC's book stopped following the feed at the cut-short message, so its top
    // is what the batches before it left: bid 64000.0 changed to 120, and ask
    // 64000.5 untouched (64001.0 was deleted, 64002.0 added behind it).
    EXPECT_EQ(BestPrice(btc, Side::kBid), 64000.0);
    EXPECT_EQ(BestLots(btc, Side::kBid), 120);
    EXPECT_EQ(BestPrice(btc, Side::kAsk), 64000.5);
}

}  // namespace
}  // namespace book_adapter
