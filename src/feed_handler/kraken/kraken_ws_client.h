// Kraken level3 WebSocket client: the live-socket end of the feed handler.
//
// Per decisions/0004 this connection runs on IXWebSocket's own thread (the
// library owns the fd and does not expose it, so it cannot join an epoll
// group), journals every inbound message synchronously on that thread, and
// treats every (re)connect identically: fresh token, fresh subscribe, fresh
// snapshot, new journal incarnation.
//
// Reconnect/backoff is IXWebSocket's automatic reconnection rather than a
// hand-rolled loop -- the library already implements exponential backoff with
// jitter, which is exactly what exchanges/kraken.md asks for so token fetches
// do not hammer the REST rate limit. What this class adds on top is the
// application-level staleness watchdog decisions/0004 requires, since a
// half-open connection looks like silence, not an error.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "feed_handler/capture_session.h"
#include "feed_handler/kraken/kraken_endpoints.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/logging.h"
#include "feed_handler/staleness_watchdog.h"
#include "feed_handler/stop_signal.h"

namespace ix {
class WebSocket;
}  // namespace ix

namespace feed_handler::kraken {

/// What every frame this client captures is: Kraken WebSocket v2 JSON text.
///
/// Stamped onto each frame by this client rather than taken from the
/// CaptureSession's configuration, because the client is the only thing that
/// knows first-hand what shape the bytes it just received are in. A session
/// configured by the binary that owns it could be handed the wrong answer and
/// nothing would notice until an order book parsed JSON as tag=value.
inline constexpr FrameSource kWireSource = FrameSource::kRakenJson;

/// The bits of an inbound message this client cares about. Book content is
/// deliberately not parsed: v1 journals raw bytes and the order book that
/// would consume them does not exist yet (decisions/0004).
enum class MessageKind : std::uint8_t {
    kUnknown,
    /// A `"method"` response saying the subscribe succeeded.
    kSubscribeAck,
    /// A `"method"` response saying it failed -- the one case worth shouting
    /// about, because the connection stays open and simply never delivers data.
    kSubscribeError,
    /// Any other failed method response.
    kMethodError,
    kHeartbeat,
    kStatus,
    kBookSnapshot,
    kBookUpdate,
};

struct MessageClassification {
    MessageKind kind = MessageKind::kUnknown;
    /// Exchange-supplied error/status text, for the log line. Inbound only, so
    /// it can never contain our token.
    std::string detail;
    /// For a `kSubscribeAck`, the symbol it acknowledges. Kraken answers a
    /// multi-symbol subscribe with one ack per symbol, so this is what tells a
    /// fully subscribed connection from a partly subscribed one. Empty
    /// otherwise, and when the ack does not carry one. Has a default
    /// initializer so the designated-init sites that never set it stay valid
    /// under -Wmissing-designated-field-initializers.
    std::string symbol = {};
};

/// Minimal top-level classification of one inbound Kraken v2 message.
/// Single pass over the top-level object; nothing inside `data` is touched.
MessageClassification ClassifyMessage(std::string_view json);

/// Builds the level3 subscribe payload for `symbols` (exchanges/kraken.md).
///
/// The symbols are spliced in without JSON escaping, so they must already have
/// passed the config layer's symbol check (feed_handler/config): no quote,
/// backslash or control character can appear in one.
///
/// The result carries a live credential in-body: it must never be journaled
/// or logged. JournalWriter has no outbound path at all, which is what keeps
/// that structural rather than a rule to remember.
std::string BuildSubscribeMessage(std::span<const std::string> symbols, std::string_view token);

struct WsClientConfig {
    std::string url = std::string(kDefaultWsUrl);
    /// The connection's identity in log lines, so several connections in one
    /// process can be told apart. The config's `id`.
    std::string id = "kraken";
    /// WS v2 spells bitcoin "BTC", not REST's "XBT" (exchanges/kraken.md). All
    /// of them ride one subscribe on one socket, up to Kraken's cap of 200.
    std::vector<std::string> symbols = {"BTC/USD"};
    /// WebSocket-level ping. IXWebSocket defaults this to -1 (off), so it is
    /// set deliberately; it is the transport half of liveness detection, with
    /// the staleness watchdog below as the independent application half.
    int ping_interval_seconds = 20;
    /// No inbound traffic for this long forces a reconnect. Kraken sends
    /// heartbeats about once a second on an idle connection, so this is many
    /// missed heartbeats rather than a tight bound.
    std::uint64_t staleness_timeout_ns = 30ULL * 1'000'000'000ULL;
    /// How often the watchdog thread wakes to check.
    std::uint64_t watchdog_poll_ms = 500;
    /// IXWebSocket's own reconnect backoff bounds. Its default minimum is 1ms,
    /// which would retry the REST token call far too fast to stay inside
    /// Kraken's rate limits; both bounds are therefore set explicitly. The
    /// minimum doubles as this client's own floor on how often a connection
    /// may be set up, because the library's bounds only apply to *failed*
    /// connection attempts.
    std::uint32_t min_reconnect_wait_ms = 1'000;
    std::uint32_t max_reconnect_wait_ms = 30'000;
};

class WsClient {
  public:
    /// `rest` must outlive this client and be shared across reconnects: it
    /// owns the nonce high-water mark that keeps signed calls strictly
    /// increasing (exchanges/kraken.md), so a per-reconnect instance would
    /// reintroduce the nonce collision it exists to prevent.
    WsClient(RestClient& rest, Credentials creds, CaptureSession& session, WsClientConfig cfg = {});

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&&) = delete;
    WsClient& operator=(WsClient&&) = delete;
    ~WsClient();

    /// Starts the IXWebSocket thread and the watchdog thread. Returns
    /// immediately; everything after this happens on those threads.
    void Start();

    /// Asks the client to stop, and returns without waiting for anything.
    /// Wakes the watchdog and asks IXWebSocket to close with automatic
    /// reconnection off, so its thread winds down on its own. Idempotent. A
    /// process with several connections requests every stop first and only then
    /// joins, so shutdown costs one wind-down, not one per connection.
    void RequestStop();

    /// Waits for the client to finish stopping: requests the stop first if
    /// nobody has, joins the watchdog thread and IXWebSocket's thread. Idempotent.
    void Join();

    /// Permanent shutdown: RequestStop() then Join(). Idempotent.
    void Stop();

    /// Handles one inbound Kraken wire message: journal it, then classify it.
    /// Normally called by IXWebSocket's callback on its own thread.
    ///
    /// Public so the capture paths that matter -- the frame identity it stamps,
    /// and what it does when the journal write fails -- are testable without a
    /// live socket, in the same spirit as RestClient::parse_asset_pairs. It is
    /// not a send path: nothing outbound goes through here.
    void HandleMessage(const std::string& payload);

    /// True when capture cannot continue: a journal file could not be opened,
    /// or a write into an open one failed. The owning process should shut down
    /// rather than stay connected while dropping data on the floor.
    bool Fatal() const {
        return stop_signal_.Fatal();
    }

    std::uint64_t MessagesReceived() const {
        return messages_received_.load(std::memory_order_relaxed);
    }

    std::uint64_t ForcedReconnects() const {
        return forced_reconnects_.load(std::memory_order_relaxed);
    }

  private:
    void HandleOpen();
    void RunWatchdog();
    /// Closes the current connection so IXWebSocket's automatic reconnection
    /// re-establishes it. Deliberately close(), not stop(): stop() joins the
    /// library's thread and ends reconnection for good.
    void ForceReconnect(std::string_view reason);
    /// Sleeps, interruptibly, after a failed connection setup so a persistent
    /// failure cannot spin on Kraken's REST endpoint.
    void BackOffAfterSetupFailure();
    /// Floors how often a connection can be set up, since every setup costs a
    /// signed REST token call. Returns true if shutdown was requested (or
    /// capture failed) while waiting, in which case the caller must abandon the
    /// setup.
    bool ThrottleConnectionSetup();

    RestClient& rest_;
    Credentials creds_;
    CaptureSession& session_;
    WsClientConfig cfg_;
    /// Every line tagged with `cfg_.id`.
    TaggedLog log_;
    std::unique_ptr<ix::WebSocket> ws_;
    StalenessWatchdog watchdog_;

    std::thread watchdog_thread_;
    /// The stop request and the fatal latch, with the wakeup every timed wait in
    /// this client goes through (stop_signal.h).
    StopSignal stop_signal_;
    std::atomic<bool> started_{false};
    /// Set by the first Join(), so a second one (or the destructor) is a no-op.
    std::atomic<bool> joined_{false};
    std::atomic<std::uint64_t> messages_received_{0};
    std::atomic<std::uint64_t> forced_reconnects_{0};
    /// All touched only on the WebSocket thread.
    unsigned consecutive_setup_failures_ = 0;
    std::uint64_t last_setup_ns_ = 0;
    /// Subscribe acks seen on the current connection, against
    /// `cfg_.symbols.size()` expected. Reset on every (re)connect.
    std::size_t subscribe_acks_ = 0;
};

}  // namespace feed_handler::kraken
