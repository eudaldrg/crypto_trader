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
#include "feed_handler/symbols.h"

namespace feed_handler::kraken {
namespace {

constexpr std::uint64_t kNanosPerMilli = 1'000'000;

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

/// Parser plus a reusable padded buffer. Kept thread-local rather than
/// per-call so classification does not allocate once the buffer has grown to
/// the largest message seen: simdjson needs SIMDJSON_PADDING bytes of slack
/// past the payload, and a fresh padded_string per message would mean a heap
/// round trip on every single inbound frame.
struct ClassifierState {
    simdjson::ondemand::parser parser;
    std::string buffer;
};

ClassifierState& Classifier() {
    static thread_local ClassifierState state;
    return state;
}

MessageKind ClassifyChannel(std::string_view channel, std::string_view type) {
    if (channel == "heartbeat") {
        return MessageKind::kHeartbeat;
    }
    if (channel == "status") {
        return MessageKind::kStatus;
    }
    if (channel == "level3") {
        return type == "snapshot" ? MessageKind::kBookSnapshot : MessageKind::kBookUpdate;
    }
    return MessageKind::kUnknown;
}

}  // namespace

MessageClassification ClassifyMessage(std::string_view json) {
    ClassifierState& state = Classifier();
    state.buffer.assign(json);
    state.buffer.append(simdjson::SIMDJSON_PADDING, '\0');
    const simdjson::padded_string_view view(state.buffer.data(), json.size(), state.buffer.size());

    simdjson::ondemand::document doc;
    if (state.parser.iterate(view).get(doc) != simdjson::SUCCESS) {
        return {.kind = MessageKind::kUnknown, .detail = "unparseable message"};
    }
    simdjson::ondemand::object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS) {
        return {.kind = MessageKind::kUnknown, .detail = "message is not a JSON object"};
    }

    // One pass over the top-level fields: Kraken does not guarantee key order,
    // and re-looking-up keys would make simdjson's on-demand cursor rewind.
    std::string method;
    std::string channel;
    std::string type;
    std::string error;
    std::string result_symbol;
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
        } else if (key == "result") {
            // Only a subscribe ack has a `result` object with a symbol; anything
            // else there is left unread, which on-demand simply skips.
            simdjson::ondemand::object result;
            if (field.value().get_object().get(result) == simdjson::SUCCESS &&
                result["symbol"].get_string().get(text) == simdjson::SUCCESS) {
                result_symbol.assign(text);
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
                .kind = method == "subscribe" ? MessageKind::kSubscribeAck : MessageKind::kUnknown,
                .detail = method,
                .symbol = std::move(result_symbol)};
        }
        return {.kind = method == "subscribe" ? MessageKind::kSubscribeError
                                              : MessageKind::kMethodError,
                .detail = error.empty() ? method : error};
    }

    return {.kind = ClassifyChannel(channel, type), .detail = type};
}

std::string BuildSubscribeMessage(std::span<const std::string> symbols, int depth,
                                  std::string_view token) {
    // Hand-built rather than via a JSON writer: the payload is fixed shape and
    // every interpolated value is constrained (config-validated symbols, an
    // integer depth and Kraken's own base64-ish token), so there is nothing
    // here needing escaping.
    std::string message;
    message.reserve(160 + token.size() + symbols.size() * 16);
    message += R"({"method":"subscribe","params":{"channel":"level3","symbol":[)";
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        message += index == 0 ? "\"" : ",\"";
        message += symbols[index];
        message += '"';
    }
    message += R"(],"depth":)";
    message += std::to_string(depth);
    message += R"(,"snapshot":true,"token":")";
    message += token;
    message += R"("}})";
    return message;
}

namespace {

/// "BTC/USD" for one symbol, "3 symbols" for several: the connection id already
/// says which entry this is, and a 200-symbol list would drown the log line.
std::string DescribeSymbols(const std::vector<std::string>& symbols) {
    return symbols.size() == 1 ? symbols.front() : std::to_string(symbols.size()) + " symbols";
}

}  // namespace

WsClient::WsClient(RestClient& rest, Credentials creds, CaptureSession& session, WsClientConfig cfg)
    : rest_(rest),
      creds_(std::move(creds)),
      session_(session),
      cfg_(std::move(cfg)),
      log_(cfg_.id),
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

    // Before the socket can deliver anything: the journal thread reports a write
    // failure, and the connection thread a ring overflow, through this.
    session_.RouteFatalToStopSignal(stop_signal_, log_);

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
        switch (message->type) {
            case ix::WebSocketMessageType::Open:
                HandleOpen();
                break;
            case ix::WebSocketMessageType::Message:
                HandleMessage(message->str);
                break;
            case ix::WebSocketMessageType::Close:
                HandleClose(message->closeInfo.code, message->closeInfo.reason);
                break;
            case ix::WebSocketMessageType::Error:
                // Not a socket loss: IXWebSocket raises Error only for a failed
                // connection attempt (handshake or refused connect), before any
                // Open, so no connect is open here and there is nothing to
                // close or announce. A socket that was up is always reported
                // as Close, which is where the disconnect comes from. Closing
                // the session here would be wrong, not just redundant: were an
                // Error ever raised on a live socket, it would end the journal
                // while frames kept arriving.
                watchdog_.Disarm();
                log_.Error("websocket error: " + message->errorInfo.reason + " (retry " +
                           std::to_string(message->errorInfo.retries) + ")");
                break;
            case ix::WebSocketMessageType::Ping:
            case ix::WebSocketMessageType::Pong:
                // Transport-level frames: proof of life for the watchdog, but
                // not Kraken protocol messages, so they stay out of the
                // journal, which holds exchange wire messages verbatim.
                watchdog_.NoteActivity(MonotonicNowNs());
                break;
            case ix::WebSocketMessageType::Fragment:
                watchdog_.NoteActivity(MonotonicNowNs());
                break;
        }
    });
}

WsClient::~WsClient() {
    Stop();
    // The handler points at this object; the session outlives it.
    session_.SetFatalHandler({});
}

void WsClient::Start() {
    started_.store(true, std::memory_order_release);
    log_.Info("connecting to " + cfg_.url + " for " + DescribeSymbols(cfg_.symbols));
    watchdog_thread_ = std::thread([this] { RunWatchdog(); });
    ws_->start();
}

void WsClient::RequestStop() {
    // Joining stays in Join(): the threads it waits for need the signal's mutex
    // to notice the request.
    if (!stop_signal_.RequestStop()) {
        return;
    }
    if (started_.load(std::memory_order_acquire)) {
        // Reconnection off first, or the library's thread would treat this close
        // like the watchdog's ForceReconnect() and set the connection up again.
        // With it off, close() only sends the close frame and the thread ends
        // its run loop once the handshake is done, so Join()'s stop() has
        // little left to wait for.
        ws_->disableAutomaticReconnection();
        ws_->close();
    }
}

void WsClient::Join() {
    RequestStop();
    if (joined_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
    if (started_.load(std::memory_order_acquire)) {
        // stop(), not close(): this is the one place shutdown is meant to be
        // permanent, so joining IXWebSocket's thread is exactly what we want.
        ws_->stop();
    }
}

void WsClient::Stop() {
    RequestStop();
    Join();
}

void WsClient::HandleOpen() {
    // Runs on IXWebSocket's own thread, for the initial connect and for every
    // automatic reconnect alike -- there is no special first-connect path,
    // because Kraken's recovery mechanism is simply "resubscribe and take a
    // fresh snapshot" (exchanges/kraken.md).
    //
    // No inbound message can be dispatched while this callback is running
    // (same thread), so the subscribe cannot race the journal file that is
    // opened a few lines below it.
    if (ThrottleConnectionSetup()) {
        return;
    }
    watchdog_.NoteActivity(MonotonicNowNs());
    subscribe_acks_ = 0;
    log_.Info("connected");

    const auto token = rest_.FetchWebsocketsToken(creds_);
    if (!token) {
        log_.Error("token fetch failed: " + token.error());
        BackOffAfterSetupFailure();
        ForceReconnect("token fetch failed");
        return;
    }

    // Outbound only: never stamped, never journaled. The token lives in the
    // message body, so journaling this would archive a live credential
    // (decisions/0004).
    const auto sent = ws_->send(BuildSubscribeMessage(cfg_.symbols, cfg_.depth, token->token));
    if (!sent.success) {
        log_.Error("failed to send level3 subscribe");
        BackOffAfterSetupFailure();
        ForceReconnect("subscribe send failed");
        return;
    }

    const auto path = session_.BeginConnect(
        "kraken level3 " + JoinSymbols(cfg_.symbols) + " connected to " + cfg_.url, kWireSource);
    if (!path) {
        // Staying connected while unable to capture would silently throw away
        // the data this process exists to collect.
        log_.Error("cannot start capture: " + path.error());
        stop_signal_.LatchFatal();
        return;
    }

    consecutive_setup_failures_ = 0;
    log_.Info("connect_id " + std::to_string(session_.ConnectId()) + " started, journaling to " +
              path->string());
}

void WsClient::HandleClose(std::uint16_t code, const std::string& reason) {
    // Flush and close the connect's file here rather than waiting for the next
    // connect: the reconnect may take a while, and a closed file is a complete,
    // readable one. Closing the session is also what tells its sinks the
    // connection is gone, so a book goes stale now, not at the next connect.
    // A no-op when nothing is open, e.g. the socket closed before HandleOpen
    // got as far as BeginConnect.
    watchdog_.Disarm();
    session_.Close();
    log_.Warn("websocket closed (code " + std::to_string(code) + "): " + reason);
}

void WsClient::HandleMessage(const std::string& payload) {
    watchdog_.NoteActivity(MonotonicNowNs());
    messages_received_.fetch_add(1, std::memory_order_relaxed);

    // Journal first, classify second: a message this build does not understand
    // must still reach the file verbatim (decisions/0004, journal everything
    // inbound), and classification must never be able to cost a record.
    if (!session_.OnWireMessage(BytesOf(payload), kWireSource)) {
        const std::string_view reason = session_.Error();
        if (reason.empty()) {
            // No open connect yet -- a message that arrived between the
            // socket opening and handle_open() finishing. Loud, but the next
            // connect fixes it, so it is not a reason to end the process.
            log_.Error("dropped a message: no journal file open");
        }
        // Otherwise the journal has failed (a full disk, a ring overflow), which
        // never heals. It is not decided here: the session already reported it
        // through the fatal handler routed to stop_signal_ in the constructor
        // (CaptureSession::RouteFatalToStopSignal), which is what latches the
        // fatal and logs the reason. The journal is on its own thread, so this
        // return value can no longer be the one place a write failure is
        // noticed.
    }

    const MessageClassification classified = ClassifyMessage(payload);
    switch (classified.kind) {
        case MessageKind::kSubscribeAck:
            ++subscribe_acks_;
            log_.Info(
                "subscribed to level3 " +
                (classified.symbol.empty() ? DescribeSymbols(cfg_.symbols) : classified.symbol) +
                " (" + std::to_string(subscribe_acks_) + "/" + std::to_string(cfg_.symbols.size()) +
                ")");
            break;
        case MessageKind::kSubscribeError:
            // The connection stays open after a rejected subscribe and simply
            // never delivers data, so this has to be loud.
            log_.Error("kraken rejected the level3 subscribe: " + classified.detail);
            break;
        case MessageKind::kMethodError:
            log_.Error("kraken method error: " + classified.detail);
            break;
        case MessageKind::kBookSnapshot:
            log_.Info("received level3 snapshot");
            break;
        case MessageKind::kUnknown:
        case MessageKind::kHeartbeat:
        case MessageKind::kStatus:
        case MessageKind::kBookUpdate:
            break;
    }
}

void WsClient::RunWatchdog() {
    // Ends on a capture failure as well as on shutdown: once capture is dead
    // the process is on its way out, and forcing further reconnects would only
    // spend Kraken's REST rate limit on connections nothing can journal.
    while (!stop_signal_.Stopping()) {
        if (stop_signal_.WaitFor(std::chrono::milliseconds(cfg_.watchdog_poll_ms))) {
            return;
        }
        if (!watchdog_.IsStale(MonotonicNowNs())) {
            continue;
        }
        forced_reconnects_.fetch_add(1, std::memory_order_relaxed);
        ForceReconnect("no inbound message in " +
                       std::to_string(cfg_.staleness_timeout_ns / kNanosPerMilli) + "ms");
    }
}

bool WsClient::ThrottleConnectionSetup() {
    const std::uint64_t now = MonotonicNowNs();
    const std::uint64_t elapsed_ms =
        last_setup_ns_ == 0 ? cfg_.min_reconnect_wait_ms : (now - last_setup_ns_) / kNanosPerMilli;
    if (elapsed_ms < cfg_.min_reconnect_wait_ms) {
        // Confirmed live on 2026-09-16: IXWebSocket only sleeps between
        // *failed* connection attempts, so a connection that succeeds and is
        // then torn down (by the staleness watchdog, or by the exchange)
        // re-establishes instantly -- its reconnect bounds do not apply. Each
        // of those needs a fresh signed REST token, so the floor on how often
        // one connection can be set up has to live here.
        if (stop_signal_.WaitFor(
                std::chrono::milliseconds(cfg_.min_reconnect_wait_ms - elapsed_ms))) {
            return true;
        }
    }
    last_setup_ns_ = MonotonicNowNs();
    return false;
}

void WsClient::ForceReconnect(std::string_view reason) {
    // Disarm first: the watchdog must not fire again on the silence between
    // this close and the next connection's first message.
    watchdog_.Disarm();
    log_.Warn("forcing reconnect: " + std::string(reason));
    ws_->close();
}

void WsClient::BackOffAfterSetupFailure() {
    ++consecutive_setup_failures_;
    const std::uint64_t shift = std::min<std::uint64_t>(consecutive_setup_failures_ - 1, 5);
    const std::uint64_t delay_ms =
        std::min<std::uint64_t>(cfg_.max_reconnect_wait_ms, cfg_.min_reconnect_wait_ms << shift);
    // IXWebSocket only backs off when the *connection* fails; a connection
    // that succeeds and then fails at the token/subscribe step would otherwise
    // reconnect immediately and retry the signed REST call in a tight loop.
    log_.Warn("waiting " + std::to_string(delay_ms) + "ms before the next connection attempt");
    stop_signal_.WaitFor(std::chrono::milliseconds(delay_ms));
}

}  // namespace feed_handler::kraken
