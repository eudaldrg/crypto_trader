#include "book_adapter/book_adapter.h"

#include <cassert>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "feed_handler/logging.h"
#include "order_book/kraken_l3_wire.h"

namespace book_adapter {

namespace {

// A stream of malformed frames must not become a stream of log lines: the first
// few are logged, the rest are only counted.
constexpr std::uint64_t kMaxLoggedErrors = 10;

std::string ErrorText(const char* what, const std::exception& error, std::uint64_t count) {
    std::string text = std::string(what) + ": " + error.what();
    if (count == kMaxLoggedErrors) {
        text += " (further errors of this kind are counted, not logged)";
    }
    return text;
}

}  // namespace

std::string_view ToString(order_book::IntegrityIssue issue) {
    switch (issue) {
        case order_book::IntegrityIssue::kGap:
            return "gap";
        case order_book::IntegrityIssue::kChecksumMismatch:
            return "checksum_mismatch";
        case order_book::IntegrityIssue::kCrossedBook:
            return "crossed_book";
        case order_book::IntegrityIssue::kUnknownOrder:
            return "unknown_order";
        case order_book::IntegrityIssue::kUnknownLevel:
            return "unknown_level";
    }
    return "unknown";
}

void BookListener::OnIntegrityCheckFailed(order_book::IntegrityIssue issue) {
    ++stats_->issues[static_cast<std::size_t>(issue)];
    log_->Warn("book " + *symbol_ + ": integrity issue " + std::string(ToString(issue)) +
               ", book desynced until its next snapshot");
}

KrakenBook::KrakenBook(std::string symbol, std::size_t depth, ConnectionStats& stats,
                       const feed_handler::TaggedLog& log)
    : symbol_(std::move(symbol)),
      listener_(symbol_, stats, log),
      book_(listener_, order_book::KrakenL3Policy(depth)) {}

void KrakenBook::ApplySnapshot(const order_book::L3Snapshot& snapshot,
                               const order_book::ChecksumMeta& meta) {
    desynced_ = false;
    book_.ApplySnapshot(snapshot, meta);
}

void KrakenBook::ApplyBatch(std::span<const order_book::KrakenL3Update> updates,
                            const order_book::ChecksumMeta& meta) {
    book_.ApplyBatch(updates, meta);
}

DeribitBook::DeribitBook(std::string symbol, ConnectionStats& stats,
                         const feed_handler::TaggedLog& log)
    : symbol_(std::move(symbol)), listener_(symbol_, stats, log), book_(listener_) {}

void DeribitBook::ApplySnapshot(const order_book::UnsequencedL2Snapshot& snapshot) {
    desynced_ = false;
    book_.ApplySnapshot(snapshot);
}

void DeribitBook::ApplyBatch(std::span<const order_book::L2Update> updates) {
    book_.ApplyBatch(updates);
}

ConnectionHandle BookAdapter::AddConnection(std::string name, const BookSettings& settings) {
    connections_.push_back(std::make_unique<Connection>(std::move(name), settings));
    return connections_.size() - 1;
}

BookAdapter::Connection& BookAdapter::At(ConnectionHandle handle) {
    assert(handle < connections_.size());
    return *connections_[handle];
}

const BookAdapter::Connection& BookAdapter::At(ConnectionHandle handle) const {
    assert(handle < connections_.size());
    return *connections_[handle];
}

const KrakenBook* BookAdapter::FindKrakenBook(ConnectionHandle handle,
                                              std::string_view symbol) const {
    const Connection& conn = At(handle);
    const auto found = conn.books.find(symbol);
    return found == conn.books.end() ? nullptr : found->second.get();
}

const DeribitBook* BookAdapter::FindDeribitBook(ConnectionHandle handle,
                                                std::string_view symbol) const {
    const Connection& conn = At(handle);
    const auto found = conn.deribit_books.find(symbol);
    return found == conn.deribit_books.end() ? nullptr : found->second.get();
}

void BookAdapter::OnConnect(ConnectionHandle handle, std::uint64_t connect_id,
                            std::string_view reason) {
    Connection& conn = At(handle);
    // The engine cannot be reset in place, so a connection's books are erased
    // and rebuilt lazily from the snapshots the new connection sends.
    conn.books.clear();
    conn.deribit_books.clear();
    conn.connect_id = connect_id;
    conn.stale = false;
    conn.drop_logged = false;
    ++conn.stats.connects;
    conn.log.Info("connect_id " + std::to_string(connect_id) + " started (" + std::string(reason) +
                  "), books reset");
}

void BookAdapter::OnDisconnect(ConnectionHandle handle, std::uint64_t connect_id) {
    Connection& conn = At(handle);
    conn.stale = true;
    ++conn.stats.disconnects;
    conn.log.Info("connect_id " + std::to_string(connect_id) +
                  " ended, books stale until the next connect");
}

void BookAdapter::OnFramesDropped(ConnectionHandle handle, std::uint64_t count) {
    if (count == 0) {
        return;
    }
    Connection& conn = At(handle);
    conn.stats.drops += count;
    MarkAllDesynced(conn);
    if (!conn.drop_logged) {
        conn.drop_logged = true;
        conn.log.Warn("dropped " + std::to_string(count) +
                      " frames before the books; books desynced until the next connect");
    }
}

void BookAdapter::OnEvent(ConnectionHandle handle, const feed_handler::FeedEvent& event) {
    struct Visitor {
        BookAdapter& adapter;
        ConnectionHandle handle;

        void operator()(const feed_handler::FrameEvent& frame) const {
            adapter.OnFrame(handle, frame.View());
        }
        void operator()(const feed_handler::ConnectEvent& connect) const {
            adapter.OnConnect(handle, connect.connect_id, connect.reason);
        }
        void operator()(const feed_handler::DisconnectEvent& disconnect) const {
            adapter.OnDisconnect(handle, disconnect.connect_id);
        }
    };
    std::visit(Visitor{*this, handle}, event);
}

void BookAdapter::OnFrame(ConnectionHandle handle, const feed_handler::CaptureFrame& frame) {
    Connection& conn = At(handle);
    ++conn.stats.frames;
    if (conn.stale) {
        ++conn.stats.frames_ignored_stale;
        return;
    }
    switch (conn.settings.source) {
        case feed_handler::FrameSource::kKrakenJson:
            OnKrakenFrame(conn, frame);
            return;
        case feed_handler::FrameSource::kDeribitFix:
            OnDeribitFixFrame(conn, frame);
            return;
        case feed_handler::FrameSource::kUnknown:
            ++conn.stats.frames_unsupported;
            return;
    }
}

void BookAdapter::MarkAllDesynced(Connection& conn) {
    for (auto& [symbol, book] : conn.books) {
        book->MarkDesynced();
    }
    for (auto& [symbol, book] : conn.deribit_books) {
        book->MarkDesynced();
    }
}

void BookAdapter::OnKrakenFrame(Connection& conn, const feed_handler::CaptureFrame& frame) {
    ConnectionStats& stats = conn.stats;
    const bool timed = config_.measure_timing;
    const std::uint64_t parse_start = timed ? feed_handler::MonotonicNowNs() : 0;

    // One function-local static rather than a member: it is stateless, and
    // keeping the mapper's type out of the header keeps the JSON library out of
    // every includer.
    static const order_book::OrderIdMapper to_order_id = order_book::HashOrderId;
    std::vector<order_book::KrakenL3Message> messages;
    try {
        messages = order_book::ParseKrakenL3Messages(order_book::ParseWirePayload(frame.payload),
                                                     conn.settings.scale, to_order_id);
    } catch (const std::exception& error) {
        // Which symbol the payload was for is unknown, and it may have carried
        // updates the books now miss, so none of them can be trusted.
        ++stats.parse_errors;
        if (stats.parse_errors <= kMaxLoggedErrors) {
            conn.log.Error(ErrorText("unparsable frame", error, stats.parse_errors));
        }
        MarkAllDesynced(conn);
        if (timed) {
            stats.parse_ns += feed_handler::MonotonicNowNs() - parse_start;
        }
        return;
    }

    const std::uint64_t apply_start = timed ? feed_handler::MonotonicNowNs() : 0;
    if (timed) {
        stats.parse_ns += apply_start - parse_start;
    }

    for (const order_book::KrakenL3Message& message : messages) {
        try {
            const auto found = conn.books.find(message.symbol);
            if (message.is_snapshot) {
                ++stats.snapshots;
                KrakenBook* book = nullptr;
                if (found == conn.books.end()) {
                    book = conn.books
                               .emplace(
                                   message.symbol,
                                   std::make_unique<KrakenBook>(
                                       message.symbol, conn.settings.kraken_depth, stats, conn.log))
                               .first->second.get();
                } else {
                    book = found->second.get();
                }
                book->ApplySnapshot(message.Snapshot(), message.meta);
                continue;
            }
            ++stats.updates;
            if (found == conn.books.end()) {
                ++stats.updates_before_snapshot;
            } else if (!found->second->IsReady()) {
                ++stats.updates_while_desynced;
            } else {
                found->second->ApplyBatch(
                    std::span<const order_book::KrakenL3Update>(message.orders), message.meta);
            }
        } catch (const std::exception& error) {
            ++stats.apply_errors;
            if (stats.apply_errors <= kMaxLoggedErrors) {
                conn.log.Error(
                    ErrorText("book failed to apply a message", error, stats.apply_errors));
            }
            const auto broken = conn.books.find(message.symbol);
            if (broken != conn.books.end()) {
                broken->second->MarkDesynced();
            }
        }
    }
    if (timed) {
        stats.apply_ns += feed_handler::MonotonicNowNs() - apply_start;
    }
}

void BookAdapter::OnDeribitFixFrame(Connection& conn, const feed_handler::CaptureFrame& frame) {
    ConnectionStats& stats = conn.stats;
    const bool timed = config_.measure_timing;
    const std::uint64_t parse_start = timed ? feed_handler::MonotonicNowNs() : 0;

    const auto parsed = ParseDeribitFixMessage(frame.payload, conn.settings.scale);
    const std::uint64_t apply_start = timed ? feed_handler::MonotonicNowNs() : 0;
    if (timed) {
        stats.parse_ns += apply_start - parse_start;
    }

    if (!parsed) {
        OnDeribitParseError(conn, parsed.error());
        return;
    }
    ApplyDeribitMessage(conn, *parsed);
    if (timed) {
        stats.apply_ns += feed_handler::MonotonicNowNs() - apply_start;
    }
}

void BookAdapter::OnDeribitParseError(Connection& conn, const DeribitFixParseError& error) {
    ConnectionStats& stats = conn.stats;
    ++stats.parse_errors;
    if (stats.parse_errors <= kMaxLoggedErrors) {
        std::string text = "unusable FIX message: " + error.what;
        if (stats.parse_errors == kMaxLoggedErrors) {
            text += " (further errors of this kind are counted, not logged)";
        }
        conn.log.Error(text);
    }
    // Without the symbol, the message may have been for any of the books. With
    // it, only that one has missed something.
    if (error.symbol.empty()) {
        MarkAllDesynced(conn);
        return;
    }
    const auto known = conn.deribit_books.find(error.symbol);
    if (known != conn.deribit_books.end()) {
        known->second->MarkDesynced();
    }
}

void BookAdapter::ApplyDeribitMessage(Connection& conn, const DeribitFixBookMessage& message) {
    if (message.kind == FixBookMessageKind::kIgnored) {
        return;
    }
    try {
        if (message.kind == FixBookMessageKind::kSnapshot) {
            ApplyDeribitSnapshot(conn, message);
        } else {
            ApplyDeribitUpdate(conn, message);
        }
    } catch (const std::exception& error) {
        ConnectionStats& stats = conn.stats;
        ++stats.apply_errors;
        if (stats.apply_errors <= kMaxLoggedErrors) {
            conn.log.Error(ErrorText("book failed to apply a message", error, stats.apply_errors));
        }
        const auto broken = conn.deribit_books.find(message.symbol);
        if (broken != conn.deribit_books.end()) {
            broken->second->MarkDesynced();
        }
    }
}

void BookAdapter::ApplyDeribitSnapshot(Connection& conn, const DeribitFixBookMessage& message) {
    ++conn.stats.snapshots;
    auto found = conn.deribit_books.find(message.symbol);
    if (found == conn.deribit_books.end()) {
        found = conn.deribit_books
                    .emplace(message.symbol,
                             std::make_unique<DeribitBook>(message.symbol, conn.stats, conn.log))
                    .first;
    }
    found->second->ApplySnapshot(message.snapshot);
}

void BookAdapter::ApplyDeribitUpdate(Connection& conn, const DeribitFixBookMessage& message) {
    ++conn.stats.updates;
    const auto found = conn.deribit_books.find(message.symbol);
    if (found == conn.deribit_books.end()) {
        ++conn.stats.updates_before_snapshot;
    } else if (!found->second->IsReady()) {
        ++conn.stats.updates_while_desynced;
    } else {
        found->second->ApplyBatch(std::span<const order_book::L2Update>(message.updates));
    }
}

}  // namespace book_adapter
