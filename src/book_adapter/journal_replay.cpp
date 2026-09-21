#include "book_adapter/journal_replay.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "book_adapter/book_adapter.h"
#include "book_adapter/book_settings.h"
#include "feed_handler/journal_format.h"
#include "feed_handler/journal_reader.h"
#include "feed_handler/message_sink.h"
#include "order_book/types.h"

namespace book_adapter {

namespace {

// The exchange tags the capture writes into a journal header
// (config::ToString(Exchange)). Nothing else has a parser behind it.
std::optional<feed_handler::FrameSource> SourceForExchange(std::string_view exchange) {
    if (exchange == "kraken") {
        return feed_handler::FrameSource::kKrakenJson;
    }
    if (exchange == "deribit") {
        return feed_handler::FrameSource::kDeribitFix;
    }
    return std::nullopt;
}

// The connect record's reason, which is UTF-8 text rather than wire data.
std::string AsText(std::span<const std::byte> bytes) {
    std::string text(bytes.size(), '\0');
    std::ranges::transform(bytes, text.begin(),
                           [](std::byte byte) { return static_cast<char>(byte); });
    return text;
}

std::string Milliseconds(std::uint64_t nanoseconds) {
    return std::format("{:.3f} ms", static_cast<double>(nanoseconds) / 1e6);
}

// "12.3 ns/frame", or nothing to divide by.
std::string PerUnit(std::uint64_t nanoseconds, std::uint64_t count, std::string_view unit) {
    if (count == 0) {
        return "";
    }
    return std::format(" ({:.1f} ns/{})",
                       static_cast<double>(nanoseconds) / static_cast<double>(count), unit);
}

}  // namespace

bool ReplayReport::StoppedEarly() const {
    return std::ranges::any_of(files,
                               [](const ReplayFileReport& file) { return file.stopped_early; });
}

std::uint64_t ReplayReport::ReadNs() const {
    if (!timed) {
        return 0;
    }
    const std::uint64_t accounted = stats.parse_ns + stats.apply_ns;
    return total_ns > accounted ? total_ns - accounted : 0;
}

std::expected<ReplayReport, std::string> JournalReplay::Run(
    std::span<const std::filesystem::path> paths) {
    if (paths.empty()) {
        return std::unexpected("no journal files given");
    }

    const std::uint64_t start_ns = feed_handler::MonotonicNowNs();
    adapter_ = BookAdapter(BookAdapter::Config{.measure_timing = options_.measure_timing});
    ReplayReport report;
    report.timed = options_.measure_timing;

    std::string exchange;
    // The connect_id the adapter was last told about and has not yet been told
    // ended; a journal that was cut off ends its connect with the replay.
    std::optional<std::uint64_t> open_connect_id;

    for (const std::filesystem::path& path : paths) {
        auto reader = feed_handler::JournalReader::Open(path);
        if (!reader) {
            return std::unexpected(path.string() + ": " + reader.error());
        }
        const feed_handler::JournalFileHeader& header = reader->Header();
        const std::optional<feed_handler::FrameSource> source = SourceForExchange(header.exchange);
        if (!source) {
            return std::unexpected(path.string() + ": no book for exchange tag '" +
                                   header.exchange + "'");
        }
        if (exchange.empty()) {
            exchange = header.exchange;
            // A fresh adapter numbers its connections from 0, which is kConnection.
            [[maybe_unused]] const ConnectionHandle handle = adapter_.AddConnection(
                exchange, BookSettings{.source = *source,
                                       .scale = options_.scale,
                                       .kraken_depth = options_.kraken_depth});
            assert(handle == kConnection);
        } else if (header.exchange != exchange) {
            return std::unexpected(path.string() + ": exchange '" + header.exchange +
                                   "' differs from '" + exchange +
                                   "', journals of one connection only");
        }

        ReplayFileReport file;
        file.path = path;
        file.exchange = header.exchange;
        file.connect_id = header.connect_id;
        while (const std::optional<feed_handler::JournalRecord> record = reader->Next()) {
            ++file.records;
            switch (record->type) {
                case feed_handler::journal::RecordType::kConnect:
                    if (open_connect_id) {
                        adapter_.OnDisconnect(kConnection, *open_connect_id);
                    }
                    adapter_.OnConnect(kConnection, header.connect_id, AsText(record->payload));
                    open_connect_id = header.connect_id;
                    break;
                case feed_handler::journal::RecordType::kWireMessage:
                    ++file.wire_messages;
                    adapter_.OnFrame(kConnection, feed_handler::CaptureFrame{
                                                      .payload = record->payload,
                                                      .capture_sequence = record->capture_sequence,
                                                      .monotonic_ns = record->monotonic_ns,
                                                      .source = *source,
                                                  });
                    break;
            }
        }
        file.stopped_early = reader->StoppedEarly();
        file.stop_reason = std::string(reader->StopReason());
        report.wire_messages += file.wire_messages;
        report.files.push_back(std::move(file));
    }

    if (open_connect_id) {
        adapter_.OnDisconnect(kConnection, *open_connect_id);
    }
    report.stats = adapter_.Stats(kConnection);
    report.total_ns = feed_handler::MonotonicNowNs() - start_ns;
    return report;
}

void PrintReport(std::ostream& out, const ReplayReport& report) {
    const ConnectionStats& stats = report.stats;
    out << "files: " << report.files.size() << "\n";
    for (const ReplayFileReport& file : report.files) {
        out << "  " << file.path.string() << ": " << file.exchange << " connect_id "
            << file.connect_id << ", " << file.records << " records, " << file.wire_messages
            << " wire messages\n";
        if (file.stopped_early) {
            out << "  STOPPED EARLY: " << file.stop_reason
                << " (the rest of this file was not replayed)\n";
        }
    }
    out << "wire messages: " << report.wire_messages << "\n";
    out << "connects: " << stats.connects << ", disconnects: " << stats.disconnects << "\n";
    out << "snapshots: " << stats.snapshots << "\n";
    const std::uint64_t applied =
        stats.updates - stats.updates_before_snapshot - stats.updates_while_desynced;
    out << "updates: " << stats.updates << " (applied " << applied << ", before a snapshot "
        << stats.updates_before_snapshot << ", while desynced " << stats.updates_while_desynced
        << ")\n";
    out << "integrity issues: " << stats.TotalIssues();
    for (std::size_t kind = 0; kind < kIntegrityIssueKinds; ++kind) {
        const auto issue = static_cast<order_book::IntegrityIssue>(kind);
        out << (kind == 0 ? " (" : ", ") << ToString(issue) << " " << stats.IssueCount(issue);
    }
    out << ")\n";
    out << "parse errors: " << stats.parse_errors << ", apply errors: " << stats.apply_errors
        << ", unsupported frames: " << stats.frames_unsupported
        << ", ignored while stale: " << stats.frames_ignored_stale << "\n";
    if (report.timed) {
        out << "parse time: " << Milliseconds(stats.parse_ns)
            << PerUnit(stats.parse_ns, stats.frames, "frame") << "\n";
        out << "apply time: " << Milliseconds(stats.apply_ns)
            << PerUnit(stats.apply_ns, stats.snapshots + stats.updates, "entry") << "\n";
        out << "read and other: " << Milliseconds(report.ReadNs()) << "\n";
    } else {
        out << "parse and apply time: not measured\n";
    }
    out << "total time: " << Milliseconds(report.total_ns) << "\n";
}

}  // namespace book_adapter
