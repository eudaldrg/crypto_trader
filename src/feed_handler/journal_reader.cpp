#include "feed_handler/journal_reader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>

namespace feed_handler {
namespace {

/// Reads exactly `out.size()` bytes. Returns false on a short read, which for
/// a journal means a truncated tail rather than an I/O error.
bool read_exact(std::ifstream& stream, std::span<std::byte> out) {
    if (out.empty()) {
        return true;
    }
    stream.read(std::bit_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return stream.gcount() == static_cast<std::streamsize>(out.size());
}

}  // namespace

std::expected<journal_reader, std::string> journal_reader::open(const std::filesystem::path& path) {
    journal_reader reader;
    reader.in_.open(path, std::ios::binary | std::ios::in);
    if (!reader.in_.is_open()) {
        return std::unexpected("journal_reader: cannot open " + path.string());
    }

    std::array<std::byte, journal::kFileHeaderSize> raw{};
    if (!read_exact(reader.in_, raw)) {
        return std::unexpected("journal_reader: file shorter than the journal header");
    }
    std::span<const std::byte> view(raw);

    if (!std::ranges::equal(view.first(journal::kMagic.size()), journal::kMagic)) {
        return std::unexpected("journal_reader: bad magic, not a capture journal");
    }
    const std::uint32_t stored_crc = journal::load_le<std::uint32_t>(view.subspan(60));
    if (stored_crc != journal::crc32_of(view.first(60))) {
        return std::unexpected("journal_reader: header checksum mismatch");
    }
    const auto version = journal::load_le<std::uint16_t>(view.subspan(8));
    if (version != journal::kFormatVersion) {
        return std::unexpected("journal_reader: unsupported format version " +
                               std::to_string(version));
    }
    const auto header_size = journal::load_le<std::uint16_t>(view.subspan(10));
    if (header_size != journal::kFileHeaderSize) {
        return std::unexpected("journal_reader: unexpected header size " +
                               std::to_string(header_size));
    }

    reader.header_.format_version = version;
    reader.header_.realtime_ns = journal::load_le<std::uint64_t>(view.subspan(16));
    reader.header_.monotonic_ns = journal::load_le<std::uint64_t>(view.subspan(24));
    reader.header_.incarnation = journal::load_le<std::uint64_t>(view.subspan(48));

    const std::span<const std::byte> name = view.subspan(32, journal::kExchangeFieldSize);
    for (const std::byte letter : name) {
        if (letter == std::byte{0}) {
            break;
        }
        reader.header_.exchange.push_back(static_cast<char>(letter));
    }

    return reader;
}

std::optional<journal_record> journal_reader::next() {
    if (finished_) {
        return std::nullopt;
    }

    std::array<std::byte, journal::kRecordHeaderSize> raw{};
    if (!read_exact(in_, raw)) {
        // A clean EOF lands exactly on a record boundary; anything else is a
        // torn record header from a crash mid-write.
        if (in_.gcount() != 0) {
            stop("truncated record header");
        } else {
            finished_ = true;
        }
        return std::nullopt;
    }
    std::span<const std::byte> view(raw);

    const auto type_byte = std::to_integer<std::uint8_t>(view[0]);
    const auto payload_length = journal::load_le<std::uint32_t>(view.subspan(4));
    if (payload_length > journal::kMaxPayloadBytes) {
        stop("record length exceeds kMaxPayloadBytes");
        return std::nullopt;
    }

    payload_.resize(payload_length);
    if (!read_exact(in_, payload_)) {
        stop("truncated record payload");
        return std::nullopt;
    }

    std::array<std::byte, journal::kRecordTrailerSize> trailer{};
    if (!read_exact(in_, trailer)) {
        stop("truncated record checksum");
        return std::nullopt;
    }

    const std::uint32_t expected = journal::crc32_update(journal::crc32_of(view), payload_);
    if (journal::load_le<std::uint32_t>(trailer) != expected) {
        stop("record checksum mismatch");
        return std::nullopt;
    }
    // Checked after the CRC so a corrupt type byte is reported as corruption
    // rather than as an unknown-but-intact future record type.
    if (!journal::is_known_record_type(type_byte)) {
        stop("unknown record type");
        return std::nullopt;
    }

    ++records_read_;
    return journal_record{
        .type = static_cast<journal::record_type>(type_byte),
        .capture_sequence = journal::load_le<std::uint64_t>(view.subspan(8)),
        .monotonic_ns = journal::load_le<std::uint64_t>(view.subspan(16)),
        .payload = payload_,
    };
}

void journal_reader::stop(std::string reason) {
    finished_ = true;
    stopped_early_ = true;
    if (stop_reason_.empty()) {
        stop_reason_ = std::move(reason);
    }
}

}  // namespace feed_handler
