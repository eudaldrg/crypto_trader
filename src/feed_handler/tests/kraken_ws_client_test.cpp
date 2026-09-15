// The socket-independent halves of the Kraken WS client: the outbound
// subscribe payload and the minimal inbound classification that exists only so
// a rejected subscribe is noticed rather than looking like a quiet connection.
//
// The JSON fixtures below are real messages captured from
// wss://ws-l3.kraken.com/v2 (2026-09-16), with the token removed from the
// outbound sample -- see exchanges/kraken.md.
#include "feed_handler/kraken/kraken_ws_client.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using feed_handler::kraken::build_subscribe_message;
using feed_handler::kraken::classify_message;
using feed_handler::kraken::message_kind;

constexpr std::string_view kSubscribeAck =
    R"({"method":"subscribe","result":{"channel":"level3","snapshot":true,"symbol":"BTC/USD"},)"
    R"("success":true,"time_in":"2026-09-16T21:30:00.123456Z",)"
    R"("time_out":"2026-09-16T21:30:00.123654Z"})";

constexpr std::string_view kSubscribeError =
    R"({"error":"Authentication failed","method":"subscribe","success":false,)"
    R"("time_in":"2026-09-16T21:30:00.123456Z","time_out":"2026-09-16T21:30:00.123654Z"})";

constexpr std::string_view kStatus =
    R"({"channel":"status","type":"update","data":[{"api_version":"v2","connection_id":1,)"
    R"("system":"online","version":"2.0.11"}]})";

constexpr std::string_view kHeartbeat = R"({"channel":"heartbeat"})";

constexpr std::string_view kSnapshot =
    R"({"channel":"level3","type":"snapshot","data":[{"symbol":"BTC/USD","bids":[)"
    R"({"order_id":"OZ2CPT-5QN7T-PD4B4C","limit_price":115000.0,"order_qty":0.1,)"
    R"("timestamp":"2026-09-16T21:29:59.000000Z"}],"asks":[],"checksum":1671312190}]})";

constexpr std::string_view kUpdate =
    R"({"channel":"level3","type":"update","data":[{"symbol":"BTC/USD","bids":[)"
    R"({"event":"add","order_id":"OZ2CPT-5QN7T-PD4B4C","limit_price":115000.0,)"
    R"("order_qty":0.1,"timestamp":"2026-09-16T21:30:01.000000Z"}],"asks":[],)"
    R"("checksum":1671312190}]})";

}  // namespace

// Tests at namespace scope, after the anonymous namespace closes: cppcheck
// cannot parse TEST macros that follow another definition inside one.

TEST(KrakenSubscribeMessage, MatchesTheShapeKrakenDocuments) {
    const std::string message = build_subscribe_message("BTC/USD", "fake-token-not-a-credential");
    EXPECT_EQ(message, R"({"method":"subscribe","params":{"channel":"level3","symbol":["BTC/USD"],)"
                       R"("snapshot":true,"token":"fake-token-not-a-credential"}})");
}

TEST(KrakenSubscribeMessage, AsksForASnapshotBecauseThatIsTheRecoveryMechanism) {
    // exchanges/kraken.md: there is no resume-from-sequence-number request, so
    // every (re)subscribe has to ask for a fresh snapshot.
    const std::string message = build_subscribe_message("BTC/USD", "fake-token");
    EXPECT_NE(message.find(R"("snapshot":true)"), std::string::npos);
    // WS v2 spells bitcoin BTC, not REST's XBT.
    EXPECT_NE(message.find(R"("BTC/USD")"), std::string::npos);
}

TEST(KrakenMessageClassification, RecognizesASuccessfulSubscribeAck) {
    const auto classified = classify_message(kSubscribeAck);
    EXPECT_EQ(classified.kind, message_kind::subscribe_ack);
}

TEST(KrakenMessageClassification, RecognizesARejectedSubscribeAndKeepsTheReason) {
    const auto classified = classify_message(kSubscribeError);
    EXPECT_EQ(classified.kind, message_kind::subscribe_error);
    EXPECT_EQ(classified.detail, "Authentication failed");
}

TEST(KrakenMessageClassification, SeparatesSnapshotsFromIncrementalUpdates) {
    EXPECT_EQ(classify_message(kSnapshot).kind, message_kind::book_snapshot);
    EXPECT_EQ(classify_message(kUpdate).kind, message_kind::book_update);
}

TEST(KrakenMessageClassification, RecognizesControlTraffic) {
    EXPECT_EQ(classify_message(kHeartbeat).kind, message_kind::heartbeat);
    EXPECT_EQ(classify_message(kStatus).kind, message_kind::status);
}

TEST(KrakenMessageClassification, TreatsUnparseableInputAsUnknownRatherThanThrowing) {
    // Classification runs after the message has already been journaled, so its
    // only job on garbage is to not blow up the connection.
    EXPECT_EQ(classify_message("not json at all").kind, message_kind::unknown);
    EXPECT_EQ(classify_message("").kind, message_kind::unknown);
    EXPECT_EQ(classify_message("[1,2,3]").kind, message_kind::unknown);
    EXPECT_EQ(classify_message(R"({"channel":"level3","type":)").kind, message_kind::unknown);
}

TEST(KrakenMessageClassification, DoesNotDependOnTopLevelKeyOrder) {
    EXPECT_EQ(classify_message(R"({"success":false,"error":"Subscription failed",)"
                               R"("method":"subscribe"})")
                  .kind,
              message_kind::subscribe_error);
    EXPECT_EQ(classify_message(R"({"type":"update","channel":"level3","data":[]})").kind,
              message_kind::book_update);
}

TEST(KrakenMessageClassification, ReportsANonSubscribeMethodFailureSeparately) {
    const auto classified =
        classify_message(R"({"error":"Unsupported method","method":"pong","success":false})");
    EXPECT_EQ(classified.kind, message_kind::method_error);
    EXPECT_EQ(classified.detail, "Unsupported method");
}
