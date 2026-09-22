// Sequential reader for the v1 capture journal written by JournalWriter.
// See journal_format.h for the byte layout.
//
// Full Replay mode (pacing, cross-connect manifest ordering) is a future
// ADR; this reader is the lower layer both that and any offline journal
// inspection will sit on, and it is what makes the writer round-trip testable.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "feed_handler/journal_format.h"

namespace feed_handler {

/// The once-per-file metadata from the journal header.
struct JournalFileHeader {
    std::uint16_t format_version = 0;
    /// CLOCK_REALTIME and CLOCK_MONOTONIC sampled at the same instant when the
    /// file was created; together they convert any record's monotonic_ns to
    /// wall clock.
    std::uint64_t realtime_ns = 0;
    std::uint64_t monotonic_ns = 0;
    std::string exchange;
    std::uint64_t connect_id = 0;
};

/// One decoded record. `payload` views the reader's internal buffer and is
/// only valid until the next next() call -- the same non-owning contract
/// CaptureFrame uses, so a replay source can hand it straight to a
/// message_sink without copying.
struct JournalRecord {
    journal::RecordType type = journal::RecordType::kWireMessage;
    std::uint64_t capture_sequence = 0;
    std::uint64_t monotonic_ns = 0;
    std::span<const std::byte> payload;
};

class JournalReader {
  public:
    /// Opens `path` and validates the file header. The error string is
    /// human-readable and never contains file contents.
    static std::expected<JournalReader, std::string> Open(const std::filesystem::path& path);

    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;
    JournalReader(JournalReader&&) = default;
    JournalReader& operator=(JournalReader&&) = default;
    ~JournalReader() = default;

    const JournalFileHeader& Header() const {
        return header_;
    }

    /// Returns the next valid record, or nullopt at end of valid data.
    ///
    /// A truncated or corrupt tail -- the normal outcome of a crash mid-write
    /// -- is NOT an error: the reader stops cleanly at the first record it
    /// cannot fully validate and reports it via stopped_early()/stop_reason(),
    /// per decisions/0004.
    std::optional<JournalRecord> Next();

    /// True if next() stopped on an invalid record rather than clean EOF.
    bool StoppedEarly() const {
        return stopped_early_;
    }

    /// Why reading stopped; empty until it does.
    std::string_view StopReason() const {
        return stop_reason_;
    }

    std::uint64_t RecordsRead() const {
        return records_read_;
    }

  private:
    JournalReader() = default;

    void Stop(std::string reason);

    std::ifstream in_;
    JournalFileHeader header_;
    std::vector<std::byte> payload_;
    std::string stop_reason_;
    std::uint64_t records_read_ = 0;
    bool stopped_early_ = false;
    bool finished_ = false;
};

}  // namespace feed_handler
