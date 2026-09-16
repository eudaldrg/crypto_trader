// The socket-independent halves of the Kraken WS client: the outbound
// subscribe payload, the minimal inbound classification that exists only so a
// rejected subscribe is noticed rather than looking like a quiet connection,
// and the capture half of handle_message() -- what it stamps onto a frame and
// what it does when the journal write fails.
//
// The capture tests drive handle_message() directly rather than over a socket.
// IXWebSocket owns its own thread and fd (decisions/0004), and a real
// connection would also need a signed REST token call, so a live-socket
// harness like the Deribit one is not available here; handle_message() is the
// entry point that callback would reach anyway.
//
// The JSON fixtures below are real messages captured from
// wss://ws-l3.kraken.com/v2 (2026-09-16), with the token removed from the
// outbound sample -- see exchanges/kraken.md.
#include "feed_handler/kraken/kraken_ws_client.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <bit>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "feed_handler/capture_session.h"
#include "feed_handler/journal_format.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/recording_sink.h"

namespace {

using feed_handler::capture_session;
using feed_handler::frame_source;
using feed_handler::kraken::build_subscribe_message;
using feed_handler::kraken::classify_message;
using feed_handler::kraken::message_kind;
using feed_handler::kraken::rest_client;
using feed_handler::kraken::ws_client;
using feed_handler::testing::recording_sink;

std::span<const std::byte> bytes_of(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

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

/// Never a real Kraken credential: nothing in these tests makes a REST call.
feed_handler::kraken::credentials test_credentials() {
    return feed_handler::kraken::credentials{
        .api_key = "not-a-key",
        .api_secret_b64 = "bm90LWEtcmVhbC1zZWNyZXQ=",
    };
}

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

/// The capture half of handle_message(), driven directly: the client is
/// constructed but never start()ed, so no thread, no socket and no REST call
/// exists for the duration of these tests.
class KrakenCapture : public ::testing::Test {
  protected:
    void SetUp() override {
        // Test name and pid, for the same reason the Deribit loopback fixture
        // uses them: two concurrent runs of this binary must not share a
        // journal directory.
        dir_ = std::filesystem::temp_directory_path() /
               ("kraken_ws_capture_" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
                std::to_string(::getpid()));
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    std::filesystem::path dir_;
    rest_client rest_;
};

TEST_F(KrakenCapture, StampsEveryCapturedFrameWithItsOwnWireShape) {
    // What an order-book sink fed by both exchanges branches on. The session is
    // deliberately opened as `unknown`, so a frame that says kraken_json can
    // only have been stamped by the client itself.
    capture_session session({.directory = dir_, .exchange = "kraken"});
    recording_sink sink;
    session.add_sink(sink);
    ASSERT_TRUE(session.begin_incarnation("test", frame_source::unknown).has_value());

    ws_client client(rest_, test_credentials(), session);
    client.handle_message(std::string(kHeartbeat));
    client.handle_message(std::string(kUpdate));

    const auto frames = sink.frames();
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames[0].source, frame_source::kraken_json);
    EXPECT_EQ(frames[1].source, frame_source::kraken_json);
    EXPECT_EQ(frames[1].payload, kUpdate);
    EXPECT_EQ(client.messages_received(), 2U);
    EXPECT_FALSE(client.fatal());
}

TEST_F(KrakenCapture, TreatsAFailedJournalWriteAsFatalToTheProcess) {
    // The bug this covers: before, a failed journal write was logged and
    // nothing else, so after a full disk the main loop's `!client.fatal()`
    // stayed true forever and the process looked healthy while capturing
    // nothing. Deribit's journal_message already ended the session here.
    capture_session session({.directory = dir_, .exchange = "kraken"});
    ASSERT_TRUE(session.begin_incarnation("connected", frame_source::kraken_json).has_value());

    // Latches the writer's sticky error the way a full disk would: the record
    // is refused and the file is unreliable from that point on. Every later
    // write into this session now fails the same way, which is the property
    // that makes a journal failure worth ending the process over.
    const std::string oversized(feed_handler::journal::kMaxPayloadBytes + 1U, 'x');
    ASSERT_FALSE(session.on_wire_message(bytes_of(oversized), frame_source::kraken_json));
    ASSERT_FALSE(session.error().empty());

    ws_client client(rest_, test_credentials(), session);
    ASSERT_FALSE(client.fatal());
    client.handle_message(std::string(kHeartbeat));
    EXPECT_TRUE(client.fatal());
}

TEST_F(KrakenCapture, DoesNotEndTheProcessOverAMessageThatArrivedBeforeTheFirstIncarnation) {
    // The other half of the same branch, and the reason it is a branch: a
    // message arriving before handle_open() has opened a file is loud but
    // recoverable -- the next incarnation captures normally -- so it must not
    // be confused with a journal that has failed.
    capture_session session({.directory = dir_, .exchange = "kraken"});

    ws_client client(rest_, test_credentials(), session);
    client.handle_message(std::string(kHeartbeat));

    EXPECT_FALSE(client.fatal());
    EXPECT_EQ(session.total_records_written(), 0U);
}
