// Turns a connection's frames into order books: the code the replay driver runs
// inline and the live book thread runs behind a ring
// (docs/investigations/2026-09-21-issue-14-architecture.md).
//
// It is the sink-shaped consumer of a connection: frames, connect, disconnect,
// plus one notification a sink cannot get inline, "the ring in front of you
// dropped N frames". Everything is single-threaded. There are deliberately no
// threads, rings or locks in here, so the same object serves a replay (called
// straight from a journal reader) and the live book thread (called from a ring
// pop loop), and behaves identically in both.
//
// One book per (connection, symbol), held by unique_ptr and created lazily on
// that symbol's first snapshot. A connection is identified by the handle
// AddConnection returned; a book by its connection plus its symbol.
//
// Failure policy: a book bug must never take a capture down. Nothing here
// throws or blocks. A malformed payload, an exception out of a book, a dropped
// frame or a failed integrity check is counted, logged and leaves the affected
// books desynced until the next natural reconnect (a new connect_id) or a fresh
// snapshot. Nothing asks the socket to reconnect.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "book_adapter/book_settings.h"
#include "feed_handler/feed_event.h"
#include "feed_handler/logging.h"
#include "feed_handler/message_sink.h"
#include "order_book/book_view.h"
#include "order_book/engine.h"
#include "order_book/kraken_l3_policy.h"
#include "order_book/l3_policy.h"
#include "order_book/types.h"

namespace book_adapter {

/// How many IntegrityIssue kinds there are. ToString below is a switch with no
/// default, so a new enumerator fails the build there; bump this with it.
inline constexpr std::size_t kIntegrityIssueKinds =
    static_cast<std::size_t>(order_book::IntegrityIssue::kUnknownLevel) + 1;

/// "gap", "checksum_mismatch", ...: the label logs and reports use.
std::string_view ToString(order_book::IntegrityIssue issue);

/// Everything counted for one connection, cumulative over the adapter's life
/// (a reconnect does not reset it).
///
/// `snapshots` and `updates` count every snapshot / update entry the parser
/// produced, so a message whose data[] holds two symbols counts twice. Of the
/// updates, some are not applied: `updates_before_snapshot` (no book for that
/// symbol yet) and `updates_while_desynced` (the book is not Ready). The rest
/// were applied.
struct ConnectionStats {
    /// Every frame handed to OnFrame, including ones then ignored or unparsable.
    std::uint64_t frames = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t updates = 0;
    std::uint64_t updates_before_snapshot = 0;
    std::uint64_t updates_while_desynced = 0;
    /// Frames that arrived between a disconnect and the next connect.
    std::uint64_t frames_ignored_stale = 0;
    /// Frames of a source the adapter has no book for.
    std::uint64_t frames_unsupported = 0;
    /// A payload that could not be parsed. Its books are desynced.
    std::uint64_t parse_errors = 0;
    /// An exception out of a book. That book is desynced.
    std::uint64_t apply_errors = 0;
    /// Frames the ring in front of the adapter dropped, from OnFramesDropped.
    std::uint64_t drops = 0;
    std::uint64_t connects = 0;
    std::uint64_t disconnects = 0;
    std::array<std::uint64_t, kIntegrityIssueKinds> issues{};
    /// Only with Config::measure_timing: wall time spent parsing, and applying
    /// to books, over all frames. Zero otherwise.
    std::uint64_t parse_ns = 0;
    std::uint64_t apply_ns = 0;

    [[nodiscard]] std::uint64_t IssueCount(order_book::IntegrityIssue issue) const {
        return issues[static_cast<std::size_t>(issue)];
    }

    [[nodiscard]] std::uint64_t TotalIssues() const {
        std::uint64_t total = 0;
        for (const std::uint64_t count : issues) {
            total += count;
        }
        return total;
    }
};

/// One symbol's Kraken level3 book plus the adapter's own desync flag. The
/// engine cannot be told "you are stale" (it has no Reset(), and holds its
/// listener by reference, so it is neither assignable nor movable), so a book
/// the adapter knows to be untrustworthy is flagged here and reads as Desynced
/// until its next snapshot. Not copyable or movable: the engine points at the
/// listener inside this object.
class KrakenBook {
  public:
    KrakenBook(std::string symbol, std::size_t depth, ConnectionStats& stats,
               const feed_handler::TaggedLog& log);
    KrakenBook(const KrakenBook&) = delete;
    KrakenBook& operator=(const KrakenBook&) = delete;
    KrakenBook(KrakenBook&&) = delete;
    KrakenBook& operator=(KrakenBook&&) = delete;
    ~KrakenBook() = default;

    [[nodiscard]] const std::string& Symbol() const {
        return symbol_;
    }

    [[nodiscard]] order_book::Readiness GetReadiness() const {
        return desynced_ ? order_book::Readiness::kDesynced : book_.GetReadiness();
    }

    [[nodiscard]] bool IsReady() const {
        return GetReadiness() == order_book::Readiness::kReady;
    }

    [[nodiscard]] std::optional<order_book::BookEntry> Best(order_book::Side side) const {
        return book_.Best(side);
    }

    /// The underlying policy, for queries (Policy().Book() is the L3 book).
    [[nodiscard]] const order_book::KrakenL3Policy& Policy() const {
        return book_.Policy();
    }

    /// Replaces the book's state; clears a desync the adapter had set. An
    /// integrity failure is counted and logged through the listener.
    void ApplySnapshot(const order_book::L3Snapshot& snapshot,
                       const order_book::ChecksumMeta& meta);

    /// Applies one message's updates as a unit. A defined no-op unless Ready.
    void ApplyBatch(std::span<const order_book::KrakenL3Update> updates,
                    const order_book::ChecksumMeta& meta);

    /// The adapter no longer trusts this book (a dropped frame, a parse or
    /// apply failure). Stays desynced until the next ApplySnapshot.
    void MarkDesynced() {
        desynced_ = true;
    }

  private:
    // Counts and logs an integrity issue the engine reports. Never throws.
    class Listener {
      public:
        Listener(const std::string& symbol, ConnectionStats& stats,
                 const feed_handler::TaggedLog& log)
            : symbol_(&symbol), stats_(&stats), log_(&log) {}

        void OnTopOfBookChanged(order_book::Side /*side*/,
                                std::optional<order_book::BookEntry> /*best*/) {}
        void OnIntegrityCheckFailed(order_book::IntegrityIssue issue);

      private:
        const std::string* symbol_;
        ConnectionStats* stats_;
        const feed_handler::TaggedLog* log_;
    };

    std::string symbol_;
    Listener listener_;
    order_book::OrderBook<order_book::KrakenL3Policy, Listener> book_;
    bool desynced_ = false;
};

/// Handle to a registered connection, returned by AddConnection.
using ConnectionHandle = std::size_t;

class BookAdapter {
  public:
    struct Config {
        /// Record parse_ns and apply_ns. Off by default: three clock reads per
        /// frame are noise next to a ~100 ns/update budget when profiling that
        /// budget, and a live run has no use for them.
        bool measure_timing = false;
    };

    BookAdapter() = default;
    explicit BookAdapter(Config config) : config_(config) {}

    /// Registers a connection. `name` labels its log lines (the config's
    /// connection id). The returned handle is valid for this adapter's life.
    ConnectionHandle AddConnection(std::string name, const BookSettings& settings);

    /// The connection was (re)established: forget every book, they are rebuilt
    /// from the snapshots that follow, and start accepting frames again.
    void OnConnect(ConnectionHandle handle, std::uint64_t connect_id, std::string_view reason);

    /// The connection ended. The books are kept for inspection but frames are
    /// ignored until the next OnConnect: what they hold is stale.
    void OnDisconnect(ConnectionHandle handle, std::uint64_t connect_id);

    /// One wire message. Parsed and applied here, on the caller's thread.
    void OnFrame(ConnectionHandle handle, const feed_handler::CaptureFrame& frame);

    /// `count` frames of this connection were dropped before reaching the
    /// adapter (a full ring), so its books no longer follow the feed.
    void OnFramesDropped(ConnectionHandle handle, std::uint64_t count);

    /// OnFrame / OnConnect / OnDisconnect for an owned event, the shape a ring
    /// carries.
    void OnEvent(ConnectionHandle handle, const feed_handler::FeedEvent& event);

    [[nodiscard]] std::size_t ConnectionCount() const {
        return connections_.size();
    }
    [[nodiscard]] const std::string& Name(ConnectionHandle handle) const {
        return At(handle).name;
    }
    [[nodiscard]] const ConnectionStats& Stats(ConnectionHandle handle) const {
        return At(handle).stats;
    }
    /// The connect_id last announced by OnConnect, 0 before the first.
    [[nodiscard]] std::uint64_t ConnectId(ConnectionHandle handle) const {
        return At(handle).connect_id;
    }
    /// True between an OnDisconnect and the next OnConnect.
    [[nodiscard]] bool IsStale(ConnectionHandle handle) const {
        return At(handle).stale;
    }
    /// How many symbols have a book on this connection.
    [[nodiscard]] std::size_t BookCount(ConnectionHandle handle) const {
        return At(handle).books.size();
    }
    /// The Kraken book for `symbol` on this connection, or nullptr before that
    /// symbol's first snapshot (and after a connect, until the next one).
    [[nodiscard]] const KrakenBook* FindKrakenBook(ConnectionHandle handle,
                                                   std::string_view symbol) const;

  private:
    struct SymbolHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view symbol) const noexcept {
            return std::hash<std::string_view>{}(symbol);
        }
    };

    struct Connection {
        Connection(std::string connection_name, const BookSettings& connection_settings)
            : name(std::move(connection_name)), settings(connection_settings), log(name) {}

        std::string name;
        BookSettings settings;
        feed_handler::TaggedLog log;
        ConnectionStats stats;
        std::uint64_t connect_id = 0;
        bool stale = false;
        // Only the first drop notification after a connect is logged: a ring
        // that stays full would otherwise log once per pop.
        bool drop_logged = false;
        std::unordered_map<std::string, std::unique_ptr<KrakenBook>, SymbolHash, std::equal_to<>>
            books;
    };

    [[nodiscard]] Connection& At(ConnectionHandle handle);
    [[nodiscard]] const Connection& At(ConnectionHandle handle) const;

    void OnKrakenFrame(Connection& conn, const feed_handler::CaptureFrame& frame);
    void MarkAllDesynced(Connection& conn);

    Config config_;
    // unique_ptr so a Connection never moves: its books point at its stats and log.
    std::vector<std::unique_ptr<Connection>> connections_;
};

}  // namespace book_adapter
