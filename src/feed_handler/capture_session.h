// Per-connection capture bookkeeping: which connect_id we are on, which
// journal file it writes to, and the capture sequence numbering inside it.
//
// decisions/0004 makes the journal one file per (exchange,
// connect_id) and makes the reconnect an explicit record rather
// than something inferred from message content. That is three pieces of state
// that have to move together on every (re)connect -- new file, incremented
// connect_id, reset sequence numbering -- so they live in one place rather
// than being open-coded in the WebSocket callback, where they would be
// untestable without a live socket.
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "feed_handler/journal_thread.h"
#include "feed_handler/journal_writer.h"
#include "feed_handler/logging.h"
#include "feed_handler/message_sink.h"
#include "feed_handler/stop_signal.h"

namespace feed_handler {

/// `<prefix>-<connect_id>-<UTC timestamp>.journal`, e.g.
/// "kraken-000003-20260916T213000Z.journal". The connect_id comes first after
/// the prefix so a directory listing sorts by connection order, and the
/// timestamp keeps files from separate process runs (which both start
/// counting connect_ids at 1) from colliding.
std::string JournalFileName(std::string_view prefix, std::uint64_t connect_id,
                            std::uint64_t realtime_ns);

/// Where the journal file is written.
enum class JournalMode : std::uint8_t {
    /// On the connection's own thread, synchronously, inside OnWireMessage. The
    /// default: deterministic, which is what unit tests want.
    kInline,
    /// On a dedicated journal thread behind an SPSC ring (journal_thread.h): the
    /// connection thread stamps a frame, copies it into the ring and returns, and
    /// never touches the disk. Failures become asynchronous (see SetFatalHandler)
    /// and Close() becomes a barrier.
    kThreaded,
};

/// Owns one journal file at a time and fans every captured frame out to it
/// plus any additional registered sinks. Not thread safe: it belongs to the
/// connection's own thread and every sink registered with it is called on that
/// same thread. The exceptions are what a journal thread reports back
/// (SetFatalHandler, Error(), RecordsWritten()) and ConnectId(), which are safe from
/// any thread.
///
/// The journal writer is deliberately not one of the registered sinks: it is
/// the always-present one that makes capture durable, it is the only sink
/// whose failure the return value of on_wire_message() reports, and it is the
/// only one that needs the stamped CaptureFrame for the connect marker
/// (see begin_connect). Additional sinks are strictly downstream of it.
class CaptureSession {
  public:
    struct Config {
        std::filesystem::path directory = "journal";
        /// Short exchange tag, written into every journal file's header and,
        /// unless `file_prefix` says otherwise, used as the file name prefix.
        std::string exchange = "kraken";
        /// Journal file name prefix; empty means `exchange`. A process running
        /// several connections to one exchange gives each its own, so their
        /// files cannot collide: every session counts connect_ids from 1, and a
        /// same-second start would share a name.
        std::string file_prefix = {};
        JournalMode journal_mode = JournalMode::kInline;
        /// kThreaded only: how many events the journal ring holds before an
        /// overflow, which is fatal to the capture. Size it for seconds of traffic.
        std::size_t journal_ring_events = 1U << 16U;
        /// kThreaded only. False leaves the journal thread unstarted, so nothing
        /// drains its ring: how a test makes the ring overflow for certain, without
        /// racing the consumer. Close() and destruction start it and drain.
        bool journal_start = true;
    };

    /// Called with a reason when the journal fails in a way that ends the
    /// capture: a write that failed (from the journal thread) or a journal ring
    /// that overflowed (from the connection thread). Fires at most once per
    /// session and must be thread safe. kThreaded only: inline mode has no other
    /// thread to report from and says the same thing through OnWireMessage()'s
    /// return value and Error().
    using FatalHandler = std::function<void(std::string_view reason)>;

    explicit CaptureSession(Config cfg);

    // A threaded journal holds a pointer back to this session.
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;
    CaptureSession(CaptureSession&&) = delete;
    CaptureSession& operator=(CaptureSession&&) = delete;

    /// Drains a threaded journal and joins its thread. Announces nothing: a
    /// connect ends with Close(), not with destruction.
    ~CaptureSession();

    /// Installs the handler for a fatal journal failure (see FatalHandler). Safe
    /// to call at any time, but a failure that happens before it is installed is
    /// not replayed to it: install it before the first BeginConnect().
    void SetFatalHandler(FatalHandler handler);

    /// Wires this session's fatal handler to log the reason and latch
    /// `stop_signal` fatal: every capture client does exactly this, so it is
    /// one call instead of each hand-writing the same handler. `stop_signal`
    /// and `log` must outlive this session, or the caller must clear the
    /// handler with SetFatalHandler({}) first, the same lifetime rule as
    /// passing a lambda directly.
    void RouteFatalToStopSignal(StopSignal& stop_signal, const TaggedLog& log);

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

    /// Closes the previous connect's file, opens the next one and writes
    /// the connect marker as its first record, so a reader never has to
    /// guess where a reconnect happened. The file is opened and its header
    /// written synchronously in both modes (rare, off the hot path), so an
    /// unusable directory is still reported here; in kThreaded mode the marker
    /// is queued and written by the journal thread, and a failure to write it
    /// surfaces through the fatal handler instead. Capture sequence numbers restart at 1
    /// because they are per-connect (journal_writer.h). Every registered
    /// sink is then told via message_sink::on_connect(), which is how a
    /// stateful sink learns it must reset.
    ///
    /// `reason` is journaled verbatim as the marker payload and passed to the
    /// sinks unchanged: free-form text, never anything carrying a credential.
    /// `source` is what every frame of this connect will be stamped with.
    std::expected<std::filesystem::path, std::string> BeginConnect(std::string_view reason,
                                                                   FrameSource source);

    /// Journals one inbound wire message and hands it to every registered sink.
    /// Returns false if there is no open connect (nothing to write into) or
    /// the journal write failed -- the return value is about durability only,
    /// never about what another sink did with the frame. In kThreaded mode
    /// "journal write failed" is what is known so far: a ring overflow, or a
    /// write failure the journal thread has already reported (Error() says
    /// which); the disk is never touched here.
    bool OnWireMessage(std::span<const std::byte> payload, FrameSource source);

    /// Flushes and closes the current file and, if a connect was open, tells
    /// every registered sink via message_sink::OnDisconnect() once the file is
    /// complete. In kThreaded mode this is a BARRIER: it returns only after the
    /// journal thread has written everything queued, flushed and closed the file,
    /// so the file is complete and readable on return and OnDisconnect keeps the
    /// same "after the file is closed" guarantee. Safe to call twice: the second call finds nothing
    /// open and delivers nothing. This is the one place a connect ends, so it is also the only
    /// place OnDisconnect comes from: BeginConnect() reaches it through Close() (so the previous
    /// connect's OnDisconnect precedes the next OnConnect) and each client calls it whenever its
    /// socket is lost.
    void Close();

    /// Safe from any thread: a connection's Summary() reads it while the client
    /// thread may be inside BeginConnect().
    std::uint64_t ConnectId() const {
        return connect_id_.load(std::memory_order_relaxed);
    }

    /// Records written into the current file, including its connect
    /// marker; 0 when no file is open. In kThreaded mode this is what the journal
    /// thread has written so far, so it lags OnWireMessage by whatever is still
    /// queued; it is exact after Close().
    std::uint64_t RecordsWritten() const {
        if (journal_ != nullptr) {
            return journal_->RecordsWritten();
        }
        return writer_ == nullptr ? 0 : writer_->RecordsWritten();
    }

    /// Total records written across every connect this session opened.
    std::uint64_t TotalRecordsWritten() const {
        return closed_records_ + RecordsWritten();
    }

    const std::filesystem::path& CurrentPath() const {
        return current_path_;
    }

    /// Empty while healthy; a sticky failure otherwise. In kThreaded mode it
    /// covers a journal ring overflow too and stays set for the life of the
    /// session (the journal thread is finished once it failed), where inline mode
    /// forgets it with the file it belonged to.
    std::string_view Error() const {
        if (journal_ != nullptr) {
            return journal_->Error();
        }
        return writer_ == nullptr ? std::string_view{} : std::string_view(writer_->Error());
    }

  private:
    void NotifyFatal(std::string_view reason);

    Config cfg_;
    /// kInline only.
    std::unique_ptr<JournalWriter> writer_;
    /// Non-owning, in registration order. Expected to hold one or two entries,
    /// so a vector walk is the whole dispatch cost.
    std::vector<MessageSink*> sinks_;
    std::filesystem::path current_path_;
    CaptureStamper stamper_;
    std::atomic<std::uint64_t> connect_id_{0};
    /// True from the moment the sinks were told OnConnect(connect_id_) until
    /// OnDisconnect(connect_id_) has been delivered. Not the same as
    /// `writer_ != nullptr`: a BeginConnect that fails after opening the file
    /// (the marker write) leaves a writer but never announced the connect, so
    /// closing it must not announce a disconnect for it.
    bool announced_ = false;
    std::uint64_t closed_records_ = 0;
    /// kThreaded only: true from a successful BeginConnect until Close() has
    /// waited for the journal thread to close that file. The threaded counterpart
    /// of `writer_ != nullptr`.
    bool journal_open_ = false;

    std::mutex fatal_mutex_;
    FatalHandler fatal_handler_;
    /// kThreaded only. Declared last: its thread calls NotifyFatal, so it has to be
    /// joined (destroyed) before the handler and mutex it uses are.
    std::unique_ptr<JournalThread> journal_;
};

}  // namespace feed_handler
