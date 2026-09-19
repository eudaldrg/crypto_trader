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
#include <vector>

#include "feed_handler/journal_writer.h"
#include "feed_handler/message_sink.h"

namespace feed_handler {

/// `<prefix>-<incarnation>-<UTC timestamp>.journal`, e.g.
/// "kraken-000003-20260916T213000Z.journal". The incarnation comes first after
/// the prefix so a directory listing sorts by connection order, and the
/// timestamp keeps files from separate process runs (which both start
/// counting incarnations at 1) from colliding.
std::string JournalFileName(std::string_view prefix, std::uint64_t incarnation,
                            std::uint64_t realtime_ns);

/// Owns one journal file at a time and fans every captured frame out to it
/// plus any additional registered sinks. Not thread safe: it belongs to the
/// connection's own thread, like the JournalWriter it wraps, and every sink
/// registered with it is called on that same thread.
///
/// The journal writer is deliberately not one of the registered sinks: it is
/// the always-present one that makes capture durable, it is the only sink
/// whose failure the return value of on_wire_message() reports, and it is the
/// only one that needs the stamped CaptureFrame for the incarnation marker
/// (see begin_incarnation). Additional sinks are strictly downstream of it.
class CaptureSession {
  public:
    struct Config {
        std::filesystem::path directory = "journal";
        /// Short exchange tag, written into every journal file's header and,
        /// unless `file_prefix` says otherwise, used as the file name prefix.
        std::string exchange = "kraken";
    };

    /// `file_prefix` names the journal files instead of `exchange` when it is
    /// not empty. A process running several connections to one exchange gives
    /// each its own, so their files cannot collide: every session counts
    /// incarnations from 1, and a same-second start would share a name. It is
    /// a parameter rather than a Config field because Config is built with
    /// designated initializers, and -Wmissing-designated-field-initializers
    /// would make every existing site spell out the new field.
    explicit CaptureSession(Config cfg, std::string file_prefix = {})
        : cfg_(std::move(cfg)),
          file_prefix_(file_prefix.empty() ? cfg_.exchange : std::move(file_prefix)) {}

    /// Registers an additional sink to receive every frame this session
    /// captures, after the journal writer has taken it. NON-OWNING: `sink` must
    /// outlive this session.
    ///
    /// Deliberately the whole of the fan-out machinery for now. Cross-thread
    /// delivery (the SPSC-ring fan-in seam in decisions/0004) is future work
    /// and does not change this call -- it changes what a sink does inside
    /// on_frame, which is exactly what the frame-ownership contract in
    /// message_sink.h was written to make possible.
    void AddSink(MessageSink& sink) {
        sinks_.push_back(&sink);
    }

    /// Closes the previous incarnation's file, opens the next one and writes
    /// the incarnation marker as its first record, so a reader never has to
    /// guess where a reconnect happened. Capture sequence numbers restart at 1
    /// because they are per-incarnation (journal_writer.h). Every registered
    /// sink is then told via message_sink::on_incarnation(), which is how a
    /// stateful sink learns it must reset.
    ///
    /// `reason` is journaled verbatim as the marker payload and passed to the
    /// sinks unchanged: free-form text, never anything carrying a credential.
    /// `source` is what every frame of this incarnation will be stamped with.
    std::expected<std::filesystem::path, std::string> BeginIncarnation(std::string_view reason,
                                                                       FrameSource source);

    /// Journals one inbound wire message and hands it to every registered sink.
    /// Returns false if there is no open incarnation (nothing to write into) or
    /// the journal write failed -- the return value is about durability only,
    /// never about what another sink did with the frame.
    bool OnWireMessage(std::span<const std::byte> payload, FrameSource source);

    /// Flushes and closes the current file. Safe to call twice.
    void Close();

    std::uint64_t Incarnation() const {
        return incarnation_;
    }

    /// Records written into the current file, including its incarnation
    /// marker; 0 when no file is open.
    std::uint64_t RecordsWritten() const {
        return writer_ == nullptr ? 0 : writer_->RecordsWritten();
    }

    /// Total records written across every incarnation this session opened.
    std::uint64_t TotalRecordsWritten() const {
        return closed_records_ + RecordsWritten();
    }

    const std::filesystem::path& CurrentPath() const {
        return current_path_;
    }

    /// Empty while healthy; a sticky writer failure otherwise.
    std::string_view Error() const {
        return writer_ == nullptr ? std::string_view{} : std::string_view(writer_->Error());
    }

  private:
    Config cfg_;
    std::string file_prefix_;
    std::unique_ptr<JournalWriter> writer_;
    /// Non-owning, in registration order. Expected to hold one or two entries,
    /// so a vector walk is the whole dispatch cost.
    std::vector<MessageSink*> sinks_;
    std::filesystem::path current_path_;
    CaptureStamper stamper_;
    std::uint64_t incarnation_ = 0;
    std::uint64_t closed_records_ = 0;
};

}  // namespace feed_handler
