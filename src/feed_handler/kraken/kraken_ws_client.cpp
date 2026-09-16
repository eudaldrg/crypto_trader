#include "feed_handler/kraken/kraken_ws_client.h"

#include <ixwebsocket/IXWebSocket.h>
#include <simdjson.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <span>
#include <utility>

#include "feed_handler/logging.h"
#include "feed_handler/message_sink.h"

namespace feed_handler::kraken {
namespace {

constexpr std::uint64_t kNanosPerMilli = 1'000'000;

std::span<const std::byte> bytes_of(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

/// Parser plus a reusable padded buffer. Kept thread-local rather than
/// per-call so classification does not allocate once the buffer has grown to
/// the largest message seen: simdjson needs SIMDJSON_PADDING bytes of slack
/// past the payload, and a fresh padded_string per message would mean a heap
/// round trip on every single inbound frame.
struct classifier_state {
    simdjson::ondemand::parser parser;
    std::string buffer;
};

classifier_state& classifier() {
    static thread_local classifier_state state;
    return state;
}

message_kind classify_channel(std::string_view channel, std::string_view type) {
    if (channel == "heartbeat") {
        return message_kind::heartbeat;
    }
    if (channel == "status") {
        return message_kind::status;
    }
    if (channel == "level3") {
        return type == "snapshot" ? message_kind::book_snapshot : message_kind::book_update;
    }
    return message_kind::unknown;
}

}  // namespace

message_classification classify_message(std::string_view json) {
    classifier_state& state = classifier();
    state.buffer.assign(json);
    state.buffer.append(simdjson::SIMDJSON_PADDING, '\0');
    const simdjson::padded_string_view view(state.buffer.data(), json.size(), state.buffer.size());

    simdjson::ondemand::document doc;
    if (state.parser.iterate(view).get(doc) != simdjson::SUCCESS) {
        return {.kind = message_kind::unknown, .detail = "unparseable message"};
    }
    simdjson::ondemand::object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS) {
        return {.kind = message_kind::unknown, .detail = "message is not a JSON object"};
    }

    // One pass over the top-level fields: Kraken does not guarantee key order,
    // and re-looking-up keys would make simdjson's on-demand cursor rewind.
    std::string method;
    std::string channel;
    std::string type;
    std::string error;
    bool has_method = false;
    bool success = false;
    bool has_success = false;

    for (auto field : root) {
        std::string_view key;
        if (field.unescaped_key().get(key) != simdjson::SUCCESS) {
            continue;
        }
        std::string_view text;
        if (key == "method") {
            has_method = true;
            if (field.value().get_string().get(text) == simdjson::SUCCESS) {
                method.assign(text);
            }
        } else if (key == "success") {
            bool flag = false;
            if (field.value().get_bool().get(flag) == simdjson::SUCCESS) {
                success = flag;
                has_success = true;
            }
        } else if (key == "error") {
            if (field.value().get_string().get(text) == simdjson::SUCCESS) {
                error.assign(text);
            }
        } else if (key == "channel") {
            if (field.value().get_string().get(text) == simdjson::SUCCESS) {
                channel.assign(text);
            }
        } else if (key == "type") {
            if (field.value().get_string().get(text) == simdjson::SUCCESS) {
                type.assign(text);
            }
        }
    }

    if (has_method || !error.empty()) {
        const bool failed = (has_success && !success) || !error.empty();
        if (!failed) {
            return {
                .kind = method == "subscribe" ? message_kind::subscribe_ack : message_kind::unknown,
                .detail = method};
        }
        return {.kind = method == "subscribe" ? message_kind::subscribe_error
                                              : message_kind::method_error,
                .detail = error.empty() ? method : error};
    }

    return {.kind = classify_channel(channel, type), .detail = type};
}

std::string build_subscribe_message(std::string_view symbol, std::string_view token) {
    // Hand-built rather than via a JSON writer: the payload is fixed shape and
    // both interpolated values are constrained (a literal symbol and Kraken's
    // own base64-ish token), so there is nothing here needing escaping.
    std::string message;
    message.reserve(160 + token.size());
    message += R"({"method":"subscribe","params":{"channel":"level3","symbol":[")";
    message += symbol;
    message += R"("],"snapshot":true,"token":")";
    message += token;
    message += R"("}})";
    return message;
}

ws_client::ws_client(rest_client& rest, credentials creds, capture_session& session,
                     ws_client_config cfg)
    : rest_(rest),
      creds_(std::move(creds)),
      session_(session),
      cfg_(std::move(cfg)),
      ws_(std::make_unique<ix::WebSocket>()),
      watchdog_(cfg_.staleness_timeout_ns) {
    ws_->setUrl(cfg_.url);
    // Automatic reconnection is on by default, but its default bounds
    // (1ms minimum) would retry the signed REST token call far faster than
    // Kraken's rate limits allow, so both ends are set explicitly.
    ws_->enableAutomaticReconnection();
    ws_->setMinWaitBetweenReconnectionRetries(cfg_.min_reconnect_wait_ms);
    ws_->setMaxWaitBetweenReconnectionRetries(cfg_.max_reconnect_wait_ms);
    ws_->setPingInterval(cfg_.ping_interval_seconds);

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
        switch (message->type) {
            case ix::WebSocketMessageType::Open:
                handle_open();
                break;
            case ix::WebSocketMessageType::Message:
                handle_message(message->str);
                break;
            case ix::WebSocketMessageType::Close:
                // Flush and close the incarnation's file here rather than
                // waiting for the next connect: the reconnect may take a
                // while, and a closed file is a complete, readable one.
                watchdog_.disarm();
                session_.close();
                log_warn("websocket closed (code " + std::to_string(message->closeInfo.code) +
                         "): " + message->closeInfo.reason);
                break;
            case ix::WebSocketMessageType::Error:
                watchdog_.disarm();
                log_error("websocket error: " + message->errorInfo.reason + " (retry " +
                          std::to_string(message->errorInfo.retries) + ")");
                break;
            case ix::WebSocketMessageType::Ping:
            case ix::WebSocketMessageType::Pong:
                // Transport-level frames: proof of life for the watchdog, but
                // not Kraken protocol messages, so they stay out of the
                // journal, which holds exchange wire messages verbatim.
                watchdog_.note_activity(monotonic_now_ns());
                break;
            case ix::WebSocketMessageType::Fragment:
                watchdog_.note_activity(monotonic_now_ns());
                break;
        }
    });
}

ws_client::~ws_client() {
    stop();
}

void ws_client::start() {
    started_.store(true, std::memory_order_release);
    log_info("connecting to " + cfg_.url + " for " + cfg_.symbol);
    watchdog_thread_ = std::thread([this] { run_watchdog(); });
    ws_->start();
}

void ws_client::stop() {
    if (stopping_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    stop_cv_.notify_all();
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
    if (started_.load(std::memory_order_acquire)) {
        // stop(), not close(): this is the one place shutdown is meant to be
        // permanent, so joining IXWebSocket's thread is exactly what we want.
        ws_->stop();
    }
}

void ws_client::handle_open() {
    // Runs on IXWebSocket's own thread, for the initial connect and for every
    // automatic reconnect alike -- there is no special first-connect path,
    // because Kraken's recovery mechanism is simply "resubscribe and take a
    // fresh snapshot" (exchanges/kraken.md).
    //
    // No inbound message can be dispatched while this callback is running
    // (same thread), so the subscribe cannot race the journal file that is
    // opened a few lines below it.
    if (throttle_connection_setup()) {
        return;
    }
    watchdog_.note_activity(monotonic_now_ns());
    log_info("connected");

    const auto token = rest_.fetch_websockets_token(creds_);
    if (!token) {
        log_error("token fetch failed: " + token.error());
        back_off_after_setup_failure();
        force_reconnect("token fetch failed");
        return;
    }

    // Outbound only: never stamped, never journaled. The token lives in the
    // message body, so journaling this would archive a live credential
    // (decisions/0004).
    const auto sent = ws_->send(build_subscribe_message(cfg_.symbol, token->token));
    if (!sent.success) {
        log_error("failed to send level3 subscribe");
        back_off_after_setup_failure();
        force_reconnect("subscribe send failed");
        return;
    }

    const auto path = session_.begin_incarnation(
        "kraken level3 " + cfg_.symbol + " connected to " + cfg_.url, kWireSource);
    if (!path) {
        // Staying connected while unable to capture would silently throw away
        // the data this process exists to collect.
        log_error("cannot start capture: " + path.error());
        fatal_.store(true, std::memory_order_release);
        stop_cv_.notify_all();
        return;
    }

    consecutive_setup_failures_ = 0;
    log_info("incarnation " + std::to_string(session_.incarnation()) + " started, journaling to " +
             path->string());
}

void ws_client::handle_message(const std::string& payload) {
    watchdog_.note_activity(monotonic_now_ns());
    messages_received_.fetch_add(1, std::memory_order_relaxed);

    // Journal first, classify second: a message this build does not understand
    // must still reach the file verbatim (decisions/0004, journal everything
    // inbound), and classification must never be able to cost a record.
    if (!session_.on_wire_message(bytes_of(payload), kWireSource)) {
        const std::string_view reason = session_.error();
        log_error(reason.empty() ? std::string("dropped a message: no journal file open")
                                 : "journal write failed: " + std::string(reason));
    }

    const message_classification classified = classify_message(payload);
    switch (classified.kind) {
        case message_kind::subscribe_ack:
            log_info("subscribed to level3 " + cfg_.symbol);
            break;
        case message_kind::subscribe_error:
            // The connection stays open after a rejected subscribe and simply
            // never delivers data, so this has to be loud.
            log_error("kraken rejected the level3 subscribe: " + classified.detail);
            break;
        case message_kind::method_error:
            log_error("kraken method error: " + classified.detail);
            break;
        case message_kind::book_snapshot:
            log_info("received level3 snapshot");
            break;
        case message_kind::unknown:
        case message_kind::heartbeat:
        case message_kind::status:
        case message_kind::book_update:
            break;
    }
}

void ws_client::run_watchdog() {
    while (!stopping_.load(std::memory_order_acquire)) {
        if (wait_for_stop(cfg_.watchdog_poll_ms)) {
            return;
        }
        if (!watchdog_.is_stale(monotonic_now_ns())) {
            continue;
        }
        forced_reconnects_.fetch_add(1, std::memory_order_relaxed);
        force_reconnect("no inbound message in " +
                        std::to_string(cfg_.staleness_timeout_ns / kNanosPerMilli) + "ms");
    }
}

bool ws_client::throttle_connection_setup() {
    const std::uint64_t now = monotonic_now_ns();
    const std::uint64_t elapsed_ms =
        last_setup_ns_ == 0 ? cfg_.min_reconnect_wait_ms : (now - last_setup_ns_) / kNanosPerMilli;
    if (elapsed_ms < cfg_.min_reconnect_wait_ms) {
        // Confirmed live on 2026-09-16: IXWebSocket only sleeps between
        // *failed* connection attempts, so a connection that succeeds and is
        // then torn down (by the staleness watchdog, or by the exchange)
        // re-establishes instantly -- its reconnect bounds do not apply. Each
        // of those needs a fresh signed REST token, so the floor on how often
        // one connection can be set up has to live here.
        if (wait_for_stop(cfg_.min_reconnect_wait_ms - elapsed_ms)) {
            return true;
        }
    }
    last_setup_ns_ = monotonic_now_ns();
    return false;
}

void ws_client::force_reconnect(std::string_view reason) {
    // Disarm first: the watchdog must not fire again on the silence between
    // this close and the next connection's first message.
    watchdog_.disarm();
    log_warn("forcing reconnect: " + std::string(reason));
    ws_->close();
}

void ws_client::back_off_after_setup_failure() {
    ++consecutive_setup_failures_;
    const std::uint64_t shift = std::min<std::uint64_t>(consecutive_setup_failures_ - 1, 5);
    const std::uint64_t delay_ms =
        std::min<std::uint64_t>(cfg_.max_reconnect_wait_ms, cfg_.min_reconnect_wait_ms << shift);
    // IXWebSocket only backs off when the *connection* fails; a connection
    // that succeeds and then fails at the token/subscribe step would otherwise
    // reconnect immediately and retry the signed REST call in a tight loop.
    log_warn("waiting " + std::to_string(delay_ms) + "ms before the next connection attempt");
    wait_for_stop(delay_ms);
}

bool ws_client::wait_for_stop(std::uint64_t millis) {
    std::unique_lock<std::mutex> lock(stop_mutex_);
    return stop_cv_.wait_for(lock, std::chrono::milliseconds(millis),
                             [this] { return stopping_.load(std::memory_order_acquire); });
}

}  // namespace feed_handler::kraken
