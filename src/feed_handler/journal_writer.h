// Append-only v1 capture journal writer -- the always-present `message_sink`
// every CaptureSession owns, and the only one that makes capture durable.
// See decisions/0004-feed-handler-architecture.md and journal_format.h for the
// on-disk layout.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "feed_handler/journal_format.h"
#include "feed_handler/message_sink.h"

namespace feed_handler {

/// Writes one append-only journal file for one (exchange,
/// connect_id) pair.
///
/// Deliberately a dumb sink: it journals whatever frame it is handed and never
/// originates traffic of its own. Keeping outbound requests out of the journal
/// is therefore just "don't call OnFrame for them" -- there is no outbound
/// path through this class to accidentally take (exchanges/kraken.md: the WS
/// subscribe payload carries a live token in-body).
///
/// Writes are buffered and NOT fsync'd per record (decisions/0004's
/// backpressure note): they happen synchronously on the connection's own
/// thread, so one instance belongs to exactly one thread and needs no locking.
class JournalWriter final : public MessageSink {
  public:
    struct Config {
        /// Short exchange tag, e.g. "kraken". Truncated to 16 bytes on disk.
        std::string exchange;
        /// Connect_id ordinal for this file, monotonically
        /// increasing within a process run.
        std::uint64_t connect_id = 0;
        /// Output buffer size. One flush per this many bytes rather than per
        /// record; a larger buffer trades crash-tail length for syscalls.
        std::size_t buffer_bytes = 1U << 20U;
    };

    /// Creates (truncating) `path` and writes the file header immediately, so
    /// an empty journal is still a valid, self-describing file.
    /// Throws std::runtime_error if the file cannot be opened or the header
    /// cannot be written -- this happens once at connection setup, never on
    /// the per-message path.
    JournalWriter(const std::filesystem::path& path, const Config& cfg);

    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    JournalWriter(JournalWriter&&) = delete;
    JournalWriter& operator=(JournalWriter&&) = delete;
    ~JournalWriter() override;

    /// Journals `frame` as a `wire_message` record. Inbound wire bytes only.
    void OnFrame(const CaptureFrame& frame) override;

    /// Journals the explicit "new connection, fresh snapshot
    /// follows" marker. `frame.payload` is a free-form reason string rather
    /// than wire data; the caller stamps it like any other frame so the marker
    /// takes its place in the same capture sequence.
    ///
    /// Deliberately its own concrete method rather than an override of
    /// message_sink::on_connect(): the marker is a record and needs a
    /// stamped frame, and the only thing entitled to allocate a capture
    /// sequence number is the CaptureSession's stamper. on_connect() is
    /// the notification other sinks get; this is the record. Hence this writer
    /// leaves that interface method at its inherited no-op.
    void WriteConnectMarker(const CaptureFrame& frame);

    void Flush();

    /// False once any write has failed or an out-of-order frame was rejected.
    /// Sticky: the file is considered unreliable from that point on.
    bool Good() const {
        return error_.empty();
    }

    /// Empty while good(). Never contains payload bytes.
    const std::string& Error() const {
        return error_;
    }

    std::uint64_t RecordsWritten() const {
        return records_written_;
    }

    /// Highest capture sequence number accepted so far; 0 before the first
    /// record.
    std::uint64_t LastSequence() const {
        return last_sequence_;
    }

  private:
    void WriteRecord(journal::RecordType type, const CaptureFrame& frame);
    void Fail(std::string reason);

    std::ofstream out_;
    std::vector<char> buffer_;
    std::string path_;
    std::string error_;
    std::uint64_t records_written_ = 0;
    std::uint64_t last_sequence_ = 0;
};

}  // namespace feed_handler
