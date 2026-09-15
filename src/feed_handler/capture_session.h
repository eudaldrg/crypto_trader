// Per-connection capture bookkeeping: which incarnation we are on, which
// journal file it writes to, and the capture sequence numbering inside it.
//
// decisions/0004 makes the journal one file per (exchange,
// connection-incarnation) and makes the reconnect an explicit record rather
// than something inferred from message content. That is three pieces of state
// that have to move together on every (re)connect -- new file, incremented
// incarnation, reset sequence numbering -- so they live in one place rather
// than being open-coded in the WebSocket callback, where they would be
// untestable without a live socket.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "feed_handler/journal_writer.h"
#include "feed_handler/message_sink.h"

namespace feed_handler {

/// `<exchange>-<incarnation>-<UTC timestamp>.journal`, e.g.
/// "kraken-000003-20260916T213000Z.journal". The incarnation comes first after
/// the exchange so a directory listing sorts by connection order, and the
/// timestamp keeps files from separate process runs (which both start
/// counting incarnations at 1) from colliding.
std::string journal_file_name(std::string_view exchange, std::uint64_t incarnation,
                              std::uint64_t realtime_ns);

/// Owns one journal file at a time. Not thread safe: it belongs to the
/// connection's own thread, like the journal_writer it wraps.
class capture_session {
  public:
    struct config {
        std::filesystem::path directory = "journal";
        /// Short exchange tag; also the journal file name prefix.
        std::string exchange = "kraken";
    };

    explicit capture_session(config cfg) : cfg_(std::move(cfg)) {}

    /// Closes the previous incarnation's file, opens the next one and writes
    /// the incarnation marker as its first record, so a reader never has to
    /// guess where a reconnect happened. Capture sequence numbers restart at 1
    /// because they are per-incarnation (journal_writer.h).
    ///
    /// `reason` is journaled verbatim as the marker payload: free-form text,
    /// never anything carrying a credential.
    std::expected<std::filesystem::path, std::string> begin_incarnation(std::string_view reason);

    /// Journals one inbound wire message. Returns false if there is no open
    /// incarnation (nothing to write into) or the write failed.
    bool on_wire_message(std::span<const std::byte> payload);

    /// Flushes and closes the current file. Safe to call twice.
    void close();

    std::uint64_t incarnation() const {
        return incarnation_;
    }

    /// Records written into the current file, including its incarnation
    /// marker; 0 when no file is open.
    std::uint64_t records_written() const {
        return writer_ == nullptr ? 0 : writer_->records_written();
    }

    /// Total records written across every incarnation this session opened.
    std::uint64_t total_records_written() const {
        return closed_records_ + records_written();
    }

    const std::filesystem::path& current_path() const {
        return current_path_;
    }

    /// Empty while healthy; a sticky writer failure otherwise.
    std::string_view error() const {
        return writer_ == nullptr ? std::string_view{} : std::string_view(writer_->error());
    }

  private:
    config cfg_;
    std::unique_ptr<journal_writer> writer_;
    std::filesystem::path current_path_;
    capture_stamper stamper_;
    std::uint64_t incarnation_ = 0;
    std::uint64_t closed_records_ = 0;
};

}  // namespace feed_handler
