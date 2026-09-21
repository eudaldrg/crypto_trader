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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "feed_handler/capture_session.h"
#include "feed_handler/journal_format.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/tests/recording_sink.h"
#include "feed_handler/tests/test_support.h"

namespace {

using feed_handler::CaptureSession;
using feed_handler::FrameSource;
using feed_handler::kraken::BuildSubscribeMessage;
using feed_handler::kraken::ClassifyMessage;
using feed_handler::kraken::MessageKind;
using feed_handler::kraken::RestClient;
using feed_handler::kraken::WsClient;
using feed_handler::testing::RecordingSink;

std::span<const std::byte> BytesOf(std::string_view text) {
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

/// The shutdown-promptness test's clocks. The watchdog poll interval is what
/// stop() has to interrupt, the bound is far below it, and the library's own
/// reconnect bounds are turned right down so nothing but this client's own
/// condition variable can be responsible for the difference.
constexpr std::uint64_t kLongWatchdogPollMs = 10'000;
constexpr int kPromptStopMs = 2'000;
constexpr int kStopRaceAttempts = 10;

/// The overflow test's bounds: a two-event ring behind a journal thread that is
/// not running overflows within a handful of frames, and the loop gives up long
/// before a wrong build could make it expensive. Small payloads, so a hundred
/// times this is still a few kilobytes.
constexpr std::size_t kTinyJournalRing = 2;
constexpr std::size_t kOverflowLoopBound = 1'000;

/// Nothing listens on port 1, so IXWebSocket's connect is refused at once and
/// no Open event -- hence no signed REST token call -- can ever happen.
constexpr std::string_view kUnreachableUrl = "ws://127.0.0.1:1";

/// Never a real Kraken credential: nothing in these tests makes a REST call.
feed_handler::kraken::Credentials TestCredentials() {
    return feed_handler::kraken::Credentials{
        .api_key = "not-a-key",
        .api_secret_b64 = "bm90LWEtcmVhbC1zZWNyZXQ=",
    };
}

}  // namespace

// Tests at namespace scope, after the anonymous namespace closes: cppcheck
// cannot parse TEST macros that follow another definition inside one.

TEST(KrakenSubscribeMessage, MatchesTheShapeKrakenDocuments) {
    const std::vector<std::string> symbols = {"BTC/USD"};
    const std::string message = BuildSubscribeMessage(symbols, "fake-token-not-a-credential");
    EXPECT_EQ(message, R"({"method":"subscribe","params":{"channel":"level3","symbol":["BTC/USD"],)"
                       R"("snapshot":true,"token":"fake-token-not-a-credential"}})");
}

TEST(KrakenSubscribeMessage, AsksForASnapshotBecauseThatIsTheRecoveryMechanism) {
    // exchanges/kraken.md: there is no resume-from-sequence-number request, so
    // every (re)subscribe has to ask for a fresh snapshot.
    const std::vector<std::string> symbols = {"BTC/USD"};
    const std::string message = BuildSubscribeMessage(symbols, "fake-token");
    EXPECT_NE(message.find(R"("snapshot":true)"), std::string::npos);
    // WS v2 spells bitcoin BTC, not REST's XBT.
    EXPECT_NE(message.find(R"("BTC/USD")"), std::string::npos);
}

TEST(KrakenSubscribeMessage, ListsEverySymbolInOneSubscribeInConfigOrder) {
    // One socket, one subscribe: Kraken's `symbol` param is documented as an
    // array, so several symbols cost one message rather than one each.
    const std::vector<std::string> symbols = {"BTC/USD", "ETH/USD", "SOL/EUR"};
    EXPECT_EQ(BuildSubscribeMessage(symbols, "fake-token"),
              R"({"method":"subscribe","params":{"channel":"level3",)"
              R"("symbol":["BTC/USD","ETH/USD","SOL/EUR"],"snapshot":true,"token":"fake-token"}})");
}

TEST(KrakenMessageClassification, RecognizesASuccessfulSubscribeAck) {
    const auto classified = ClassifyMessage(kSubscribeAck);
    EXPECT_EQ(classified.kind, MessageKind::kSubscribeAck);
}

TEST(KrakenMessageClassification, ASubscribeAckNamesTheSymbolItAcknowledges) {
    // A multi-symbol subscribe is answered with one ack per symbol.
    EXPECT_EQ(ClassifyMessage(kSubscribeAck).symbol, "BTC/USD");
    EXPECT_TRUE(ClassifyMessage(kSubscribeError).symbol.empty());
    EXPECT_TRUE(ClassifyMessage(R"({"method":"subscribe","success":true})").symbol.empty());
}

TEST(KrakenMessageClassification, RecognizesARejectedSubscribeAndKeepsTheReason) {
    const auto classified = ClassifyMessage(kSubscribeError);
    EXPECT_EQ(classified.kind, MessageKind::kSubscribeError);
    EXPECT_EQ(classified.detail, "Authentication failed");
}

TEST(KrakenMessageClassification, SeparatesSnapshotsFromIncrementalUpdates) {
    EXPECT_EQ(ClassifyMessage(kSnapshot).kind, MessageKind::kBookSnapshot);
    EXPECT_EQ(ClassifyMessage(kUpdate).kind, MessageKind::kBookUpdate);
}

TEST(KrakenMessageClassification, RecognizesControlTraffic) {
    EXPECT_EQ(ClassifyMessage(kHeartbeat).kind, MessageKind::kHeartbeat);
    EXPECT_EQ(ClassifyMessage(kStatus).kind, MessageKind::kStatus);
}

TEST(KrakenMessageClassification, TreatsUnparseableInputAsUnknownRatherThanThrowing) {
    // Classification runs after the message has already been journaled, so its
    // only job on garbage is to not blow up the connection.
    EXPECT_EQ(ClassifyMessage("not json at all").kind, MessageKind::kUnknown);
    EXPECT_EQ(ClassifyMessage("").kind, MessageKind::kUnknown);
    EXPECT_EQ(ClassifyMessage("[1,2,3]").kind, MessageKind::kUnknown);
    EXPECT_EQ(ClassifyMessage(R"({"channel":"level3","type":)").kind, MessageKind::kUnknown);
}

TEST(KrakenMessageClassification, DoesNotDependOnTopLevelKeyOrder) {
    EXPECT_EQ(ClassifyMessage(R"({"success":false,"error":"Subscription failed",)"
                              R"("method":"subscribe"})")
                  .kind,
              MessageKind::kSubscribeError);
    EXPECT_EQ(ClassifyMessage(R"({"type":"update","channel":"level3","data":[]})").kind,
              MessageKind::kBookUpdate);
}

TEST(KrakenMessageClassification, ReportsANonSubscribeMethodFailureSeparately) {
    const auto classified =
        ClassifyMessage(R"({"error":"Unsupported method","method":"pong","success":false})");
    EXPECT_EQ(classified.kind, MessageKind::kMethodError);
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
    RestClient rest_;
};

TEST_F(KrakenCapture, StampsEveryCapturedFrameWithItsOwnWireShape) {
    // What an order-book sink fed by both exchanges branches on. The session is
    // deliberately opened as `unknown`, so a frame that says kraken_json can
    // only have been stamped by the client itself.
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    RecordingSink sink;
    session.AddSink(sink);
    ASSERT_TRUE(session.BeginConnect("test", FrameSource::kUnknown).has_value());

    WsClient client(rest_, TestCredentials(), session);
    client.HandleMessage(std::string(kHeartbeat));
    client.HandleMessage(std::string(kUpdate));

    const auto frames = sink.Frames();
    ASSERT_EQ(frames.size(), 2U);
    EXPECT_EQ(frames[0].source, FrameSource::kKrakenJson);
    EXPECT_EQ(frames[1].source, FrameSource::kKrakenJson);
    EXPECT_EQ(frames[1].payload, kUpdate);
    EXPECT_EQ(client.MessagesReceived(), 2U);
    EXPECT_FALSE(client.Fatal());
}

TEST_F(KrakenCapture, TreatsAJournalRingOverflowAsFatalToTheProcess) {
    // The bug this covers: before, a failed journal write was logged and
    // nothing else, so after a full disk the main loop's `!client.fatal()`
    // stayed true forever and the process looked healthy while capturing
    // nothing. The journal now runs on its own thread, so the failure arrives
    // through the session's fatal handler, not through a return value.
    //
    // The journal thread is left unstarted, so nothing drains the two-event ring
    // and it overflows for certain on any build, instead of depending on a slow
    // consumer.
    CaptureSession session({.directory = dir_,
                            .exchange = "kraken",
                            .journal_mode = feed_handler::JournalMode::kThreaded,
                            .journal_ring_events = kTinyJournalRing,
                            .journal_start = false});
    ASSERT_TRUE(session.BeginConnect("connected", FrameSource::kKrakenJson).has_value());

    WsClient client(rest_, TestCredentials(), session);
    ASSERT_FALSE(client.Fatal());
    std::size_t sent = 0;
    while (!client.Fatal() && sent < kOverflowLoopBound) {
        client.HandleMessage(std::string(kUpdate));
        ++sent;
    }
    ASSERT_LT(sent, kOverflowLoopBound) << "the journal ring never overflowed";
    EXPECT_TRUE(client.Fatal());
    EXPECT_NE(session.Error().find("overflow"), std::string_view::npos) << session.Error();

    // Once fatal, later messages are still counted and do not undo it. Closing
    // starts the journal thread, so the barrier completes and the session is
    // idle before the client that its handler points at goes away.
    client.HandleMessage(std::string(kHeartbeat));
    EXPECT_TRUE(client.Fatal());
    session.Close();
}

TEST_F(KrakenCapture, TreatsAFailedJournalWriteOnTheJournalThreadAsFatal) {
    // The other way a journal fails: the write itself, which happens on the
    // journal thread, so the latch is set from a thread the client does not own.
    // A payload over the format's limit is refused by the writer, the way a full
    // disk is: the record is not written and the file is unreliable from then on.
    // Close() is a barrier, so the failure has been reported by the time it
    // returns and nothing here waits on a clock.
    CaptureSession session({.directory = dir_,
                            .exchange = "kraken",
                            .journal_mode = feed_handler::JournalMode::kThreaded});
    ASSERT_TRUE(session.BeginConnect("connected", FrameSource::kKrakenJson).has_value());

    WsClient client(rest_, TestCredentials(), session);
    ASSERT_FALSE(client.Fatal());
    {
        const std::string oversized(feed_handler::journal::kMaxPayloadBytes + 1U, 'x');
        // Accepted by the ring: it is the writer that refuses it.
        ASSERT_TRUE(session.OnWireMessage(BytesOf(oversized), FrameSource::kKrakenJson));
    }
    session.Close();

    EXPECT_TRUE(client.Fatal());
    EXPECT_FALSE(session.Error().empty());
}

TEST_F(KrakenCapture, DoesNotEndTheProcessOverAMessageThatArrivedBeforeTheFirstConnect) {
    // The other half of the same branch, and the reason it is a branch: a
    // message arriving before handle_open() has opened a file is loud but
    // recoverable -- the next connect captures normally -- so it must not
    // be confused with a journal that has failed.
    CaptureSession session({.directory = dir_,
                            .exchange = "kraken",
                            .journal_mode = feed_handler::JournalMode::kThreaded});

    WsClient client(rest_, TestCredentials(), session);
    client.HandleMessage(std::string(kHeartbeat));

    EXPECT_FALSE(client.Fatal());
    EXPECT_EQ(session.TotalRecordsWritten(), 0U);
}

TEST_F(KrakenCapture, DisconnectsTheSinkWhenTheSocketCloses) {
    // Every way a Kraken socket is lost -- peer close, transport failure, the
    // watchdog's ForceReconnect, shutdown -- reaches HandleClose() as
    // IXWebSocket's Close event, so this is the one path to prove.
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    RecordingSink sink;
    session.AddSink(sink);
    ASSERT_TRUE(session.BeginConnect("connected", FrameSource::kKrakenJson).has_value());

    WsClient client(rest_, TestCredentials(), session);
    client.HandleMessage(std::string(kUpdate));
    EXPECT_TRUE(sink.Disconnects().empty());

    client.HandleClose(1006, "abnormal closure");
    EXPECT_EQ(sink.Disconnects(), (std::vector<std::uint64_t>{1}));
    const auto events = sink.Events();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[2], "disconnect 1");

    // A second Close for the same connect (the library reporting the shutdown
    // after the watchdog already closed it) must not announce it again.
    client.HandleClose(1000, "normal closure");
    EXPECT_EQ(sink.Disconnects().size(), 1U);
}

TEST_F(KrakenCapture, DoesNotDisconnectTheSinkForASocketThatNeverGotAConnect) {
    // A Close before HandleOpen reached BeginConnect (token fetch failed, say):
    // the sink was never told about a connect, so it is not told about its end.
    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    RecordingSink sink;
    session.AddSink(sink);

    WsClient client(rest_, TestCredentials(), session);
    client.HandleClose(1006, "abnormal closure");
    EXPECT_TRUE(sink.Disconnects().empty());
    EXPECT_TRUE(sink.Events().empty());
}

TEST_F(KrakenCapture, RequestStopReturnsWithoutJoiningAndJoinThenCompletes) {
    feed_handler::kraken::WsClientConfig cfg;
    cfg.url = std::string(kUnreachableUrl);
    cfg.watchdog_poll_ms = kLongWatchdogPollMs;
    cfg.min_reconnect_wait_ms = 10;
    cfg.max_reconnect_wait_ms = 50;

    CaptureSession session({.directory = dir_, .exchange = "kraken"});
    WsClient client(rest_, TestCredentials(), session, cfg);
    client.Start();

    const auto before = std::chrono::steady_clock::now();
    client.RequestStop();
    const auto request_ms = feed_handler::test_support::ElapsedSince(before).count();
    EXPECT_LT(request_ms, kPromptStopMs) << "RequestStop() should not wait for the threads";

    client.Join();
    const auto joined_ms = feed_handler::test_support::ElapsedSince(before).count();
    EXPECT_LT(joined_ms, kPromptStopMs) << "the watchdog waited out its poll interval";

    // Every later call is harmless, and so is the destructor after them.
    client.RequestStop();
    client.Join();
    client.Stop();
}

TEST_F(KrakenCapture, StopsPromptlyWhileTheWatchdogIsWaitingOutItsPollInterval) {
    // A notify issued without the StopSignal's mutex held can land in the window
    // between the watchdog evaluating the predicate and its wait actually
    // registering, and the wakeup is then delivered to nobody -- shutdown
    // sleeps out the rest of the wait instead. A race cannot be hit on demand,
    // so what is asserted is the property the fix guarantees: stop() is bounded
    // well below the wait it interrupts, every time.
    //
    // This covers the notify in stop() only. Kraken's other two wakeup sites
    // (a journal failure in handle_open/handle_message unblocking the watchdog)
    // are reachable from a live connection, which IXWebSocket owns end to end --
    // there is no loopback harness for them the way there is for Deribit.
    feed_handler::kraken::WsClientConfig cfg;
    cfg.url = std::string(kUnreachableUrl);
    // Ten seconds of watchdog sleep is what stop() must cut short; the
    // library's own retry bounds are kept tiny so its reconnect backoff cannot
    // be mistaken for this client's.
    cfg.watchdog_poll_ms = kLongWatchdogPollMs;
    cfg.min_reconnect_wait_ms = 10;
    cfg.max_reconnect_wait_ms = 50;

    for (int attempt = 0; attempt < kStopRaceAttempts; ++attempt) {
        CaptureSession session({.directory = dir_, .exchange = "kraken"});
        WsClient client(rest_, TestCredentials(), session, cfg);
        // start() launches the watchdog thread, which goes straight into its
        // first poll wait -- stopping right behind it is the race worth
        // bounding.
        client.Start();

        const auto before = std::chrono::steady_clock::now();
        client.Stop();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count();
        ASSERT_LT(elapsed_ms, kPromptStopMs)
            << "stop() took " << elapsed_ms << "ms on attempt " << attempt
            << ": the watchdog waited out its " << kLongWatchdogPollMs << "ms poll interval";
    }
}
