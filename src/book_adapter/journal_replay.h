// Replays v1 capture journals through a BookAdapter, inline: the same adapter
// the live book thread drives behind a ring, called straight from the journal
// reader with no ring and no thread (docs/investigations/
// 2026-09-21-issue-14-architecture.md). It is the profiling harness for the
// book code, so it reports parse and apply wall time separately from the time
// spent reading the files.
//
// What a journal means to the adapter:
//   - a kConnect record is a connect (OnConnect, with the file header's
//     connect_id and the record's reason text);
//   - a kWireMessage record is a frame;
//   - the connect that is open ends (OnDisconnect) when the next kConnect
//     record arrives and when the last file ends. A file boundary alone is not a
//     disconnect: a rotated continuation file (issue #5) carries no connect
//     record and must not stale a book.
//
// The files are one connection's journals in order, not a merge of several
// connections: an ordered list of files of one exchange is enough for a run
// that rotated or reconnected. Mixing exchanges in one call is an error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include "book_adapter/book_adapter.h"
#include "book_adapter/book_settings.h"
#include "order_book/instrument_scale.h"

namespace book_adapter {

/// What a journal cannot say for itself (message_sink.h): the instrument scale
/// and, for Kraken, the depth the capture was subscribed at. They apply to every
/// symbol on the connection, exactly as BookSettings does live.
struct ReplayOptions {
    order_book::InstrumentScale scale;
    std::size_t kraken_depth = kDefaultKrakenDepth;
    /// Record parse and apply wall time (BookAdapter::Config::measure_timing).
    /// On by default here, because measuring is what a replay is for.
    bool measure_timing = true;
};

/// What was read from one journal file.
struct ReplayFileReport {
    std::filesystem::path path;
    /// The header's exchange tag ("kraken", "deribit") and connect_id.
    std::string exchange;
    std::uint64_t connect_id = 0;
    /// Valid records read, of any type.
    std::uint64_t records = 0;
    /// Of those, the raw wire messages handed to the adapter.
    std::uint64_t wire_messages = 0;
    /// The reader stopped on a record it could not validate instead of at a
    /// clean end of file (a crash mid-write, or corruption). What came before
    /// it was replayed; what came after was not.
    bool stopped_early = false;
    std::string stop_reason;
};

struct ReplayReport {
    std::vector<ReplayFileReport> files;
    /// Raw wire messages handed to the adapter, over all files.
    std::uint64_t wire_messages = 0;
    /// The adapter's counters for the connection. Its parse_ns and apply_ns are
    /// zero unless ReplayOptions::measure_timing was set.
    ConnectionStats stats;
    bool timed = false;
    /// Wall time of the whole run: opening, reading, parsing and applying.
    std::uint64_t total_ns = 0;

    /// True if any file's reader stopped early. A replay that did is incomplete
    /// and callers must not treat its counts as the whole capture.
    [[nodiscard]] bool StoppedEarly() const;

    /// Total minus parse and apply: reading and decoding the files, plus the
    /// driver's own loop. Zero when not timed.
    [[nodiscard]] std::uint64_t ReadNs() const;
};

class JournalReplay {
  public:
    /// The adapter's only connection: a replay is one connection's journals.
    static constexpr ConnectionHandle kConnection = 0;

    explicit JournalReplay(const ReplayOptions& options) : options_(options) {}

    /// Replays `paths` in order through a fresh adapter. Fails, with the
    /// offending path in the message and no payload bytes, when the list is
    /// empty, a file cannot be opened, its exchange tag is not one the adapter
    /// knows, or the files disagree on the exchange. A file that stops early is
    /// NOT a failure: it is in the report (ReplayReport::StoppedEarly).
    std::expected<ReplayReport, std::string> Run(std::span<const std::filesystem::path> paths);

    /// The adapter of the last Run, for inspecting its books. Before any Run it
    /// has no connections.
    [[nodiscard]] const BookAdapter& Adapter() const {
        return adapter_;
    }

  private:
    ReplayOptions options_;
    BookAdapter adapter_;
};

/// Writes the report as text, one fact per line, for the journal_replay tool.
void PrintReport(std::ostream& out, const ReplayReport& report);

}  // namespace book_adapter
