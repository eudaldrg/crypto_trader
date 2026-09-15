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
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "feed_handler/capture_session.h"
#include "feed_handler/kraken/kraken_rest_client.h"
#include "feed_handler/staleness_watchdog.h"

namespace ix {
class WebSocket;
}  // namespace ix

namespace feed_handler::kraken {

/// The bits of an inbound message this client cares about. Book content is
/// deliberately not parsed: v1 journals raw bytes and the order book that
/// would consume them does not exist yet (decisions/0004).
enum class message_kind : std::uint8_t {
    unknown,
    /// A `"method"` response saying the subscribe succeeded.
    subscribe_ack,
    /// A `"method"` response saying it failed -- the one case worth shouting
    /// about, because the connection stays open and simply never delivers data.
    subscribe_error,
    /// Any other failed method response.
    method_error,
    heartbeat,
    status,
    book_snapshot,
    book_update,
};

struct message_classification {
    message_kind kind = message_kind::unknown;
    /// Exchange-supplied error/status text, for the log line. Inbound only, so
    /// it can never contain our token.
    std::string detail;
};

/// Minimal top-level classification of one inbound Kraken v2 message.
/// Single pass over the top-level object; nothing inside `data` is touched.
message_classification classify_message(std::string_view json);

/// Builds the level3 subscribe payload (exchanges/kraken.md).
///
/// The result carries a live credential in-body: it must never be journaled
/// or logged. journal_writer has no outbound path at all, which is what keeps
/// that structural rather than a rule to remember.
std::string build_subscribe_message(std::string_view symbol, std::string_view token);

struct ws_client_config {
    std::string url = "wss://ws-l3.kraken.com/v2";
    /// WS v2 spells bitcoin "BTC", not REST's "XBT" (exchanges/kraken.md).
    std::string symbol = "BTC/USD";
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

class ws_client {
  public:
    /// `rest` must outlive this client and be shared across reconnects: it
    /// owns the nonce high-water mark that keeps signed calls strictly
    /// increasing (exchanges/kraken.md), so a per-reconnect instance would
    /// reintroduce the nonce collision it exists to prevent.
    ws_client(rest_client& rest, credentials creds, capture_session& session,
              ws_client_config cfg = {});

    ws_client(const ws_client&) = delete;
    ws_client& operator=(const ws_client&) = delete;
    ws_client(ws_client&&) = delete;
    ws_client& operator=(ws_client&&) = delete;
    ~ws_client();

    /// Starts the IXWebSocket thread and the watchdog thread. Returns
    /// immediately; everything after this happens on those threads.
    void start();

    /// Permanent shutdown: stops the watchdog, closes the socket and joins
    /// IXWebSocket's thread. Idempotent.
    void stop();

    /// True when capture cannot continue (the journal file could not be
    /// opened). The owning process should shut down rather than stay connected
    /// while dropping data on the floor.
    bool fatal() const {
        return fatal_.load(std::memory_order_acquire);
    }

    std::uint64_t messages_received() const {
        return messages_received_.load(std::memory_order_relaxed);
    }

    std::uint64_t forced_reconnects() const {
        return forced_reconnects_.load(std::memory_order_relaxed);
    }

  private:
    void handle_open();
    void handle_message(const std::string& payload);
    void run_watchdog();
    /// Closes the current connection so IXWebSocket's automatic reconnection
    /// re-establishes it. Deliberately close(), not stop(): stop() joins the
    /// library's thread and ends reconnection for good.
    void force_reconnect(std::string_view reason);
    /// Sleeps, interruptibly, after a failed connection setup so a persistent
    /// failure cannot spin on Kraken's REST endpoint.
    void back_off_after_setup_failure();
    /// Floors how often a connection can be set up, since every setup costs a
    /// signed REST token call. Returns true if shutdown was requested while
    /// waiting, in which case the caller must abandon the setup.
    bool throttle_connection_setup();
    /// Returns true if shutdown was requested while waiting.
    bool wait_for_stop(std::uint64_t millis);

    rest_client& rest_;
    credentials creds_;
    capture_session& session_;
    ws_client_config cfg_;
    std::unique_ptr<ix::WebSocket> ws_;
    staleness_watchdog watchdog_;

    std::thread watchdog_thread_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> fatal_{false};
    std::atomic<bool> started_{false};
    std::atomic<std::uint64_t> messages_received_{0};
    std::atomic<std::uint64_t> forced_reconnects_{0};
    /// Both touched only on the WebSocket thread.
    unsigned consecutive_setup_failures_ = 0;
    std::uint64_t last_setup_ns_ = 0;
};

}  // namespace feed_handler::kraken
