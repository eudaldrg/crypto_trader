#include "feed_handler/journal_writer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <utility>

namespace feed_handler {

journal_writer::journal_writer(const std::filesystem::path& path, const config& cfg)
    : buffer_(std::max<std::size_t>(cfg.buffer_bytes, 1)), path_(path.string()) {
    // pubsetbuf only has an effect before the stream is opened, hence the
    // deliberate open-after-construct dance.
    out_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    out_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
        throw std::runtime_error("journal_writer: cannot open " + path_);
    }

    std::array<std::byte, journal::kFileHeaderSize> header{};
    std::span<std::byte> view(header);
    std::ranges::copy(journal::kMagic, view.begin());
    journal::store_le<std::uint16_t>(view.subspan(8), journal::kFormatVersion);
    journal::store_le<std::uint16_t>(view.subspan(10),
                                     static_cast<std::uint16_t>(journal::kFileHeaderSize));
    journal::store_le<std::uint32_t>(view.subspan(12), 0);
    // The realtime/monotonic pair must be sampled as close together as
    // possible: it is the only anchor tying per-record monotonic readings back
    // to wall clock for this incarnation.
    journal::store_le<std::uint64_t>(view.subspan(16), realtime_now_ns());
    journal::store_le<std::uint64_t>(view.subspan(24), monotonic_now_ns());

    const std::size_t name_bytes = std::min(cfg.exchange.size(), journal::kExchangeFieldSize);
    for (std::size_t index = 0; index < name_bytes; ++index) {
        view[32 + index] = static_cast<std::byte>(cfg.exchange[index]);
    }
    journal::store_le<std::uint64_t>(view.subspan(48), cfg.incarnation);
    journal::store_le<std::uint32_t>(view.subspan(56), 0);
    journal::store_le<std::uint32_t>(view.subspan(60), journal::crc32_of(view.first(60)));

    out_.write(std::bit_cast<const char*>(view.data()), static_cast<std::streamsize>(view.size()));
    if (!out_) {
        throw std::runtime_error("journal_writer: cannot write header to " + path_);
    }
}

journal_writer::~journal_writer() {
    // Best effort: a destructor must not throw, and a failure here is already
    // reflected by good()/error() for anything that cares.
    out_.flush();
}

void journal_writer::on_frame(const capture_frame& frame) {
    write_record(journal::record_type::wire_message, frame);
}

void journal_writer::write_incarnation_marker(const capture_frame& frame) {
    write_record(journal::record_type::connection_incarnation, frame);
}

void journal_writer::write_record(journal::record_type type, const capture_frame& frame) {
    if (!good()) {
        return;
    }
    if (frame.capture_sequence <= last_sequence_) {
        // Writing an out-of-order record would silently break the ordering
        // guarantee Replay mode is built on, so refuse rather than corrupt.
        fail("out-of-order capture sequence");
        return;
    }
    if (frame.payload.size() > journal::kMaxPayloadBytes) {
        fail("payload exceeds kMaxPayloadBytes");
        return;
    }

    std::array<std::byte, journal::kRecordHeaderSize> header{};
    std::span<std::byte> view(header);
    view[0] = static_cast<std::byte>(type);
    view[1] = std::byte{0};
    journal::store_le<std::uint16_t>(view.subspan(2), 0);
    journal::store_le<std::uint32_t>(view.subspan(4),
                                     static_cast<std::uint32_t>(frame.payload.size()));
    journal::store_le<std::uint64_t>(view.subspan(8), frame.capture_sequence);
    journal::store_le<std::uint64_t>(view.subspan(16), frame.monotonic_ns);

    const std::uint32_t crc = journal::crc32_update(journal::crc32_of(view), frame.payload);
    std::array<std::byte, journal::kRecordTrailerSize> trailer{};
    journal::store_le<std::uint32_t>(trailer, crc);

    out_.write(std::bit_cast<const char*>(view.data()), static_cast<std::streamsize>(view.size()));
    if (!frame.payload.empty()) {
        out_.write(std::bit_cast<const char*>(frame.payload.data()),
                   static_cast<std::streamsize>(frame.payload.size()));
    }
    out_.write(std::bit_cast<const char*>(trailer.data()),
               static_cast<std::streamsize>(trailer.size()));
    if (!out_) {
        fail("write failed");
        return;
    }

    last_sequence_ = frame.capture_sequence;
    ++records_written_;
}

void journal_writer::flush() {
    out_.flush();
    if (!out_ && good()) {
        fail("flush failed");
    }
}

void journal_writer::fail(std::string reason) {
    if (error_.empty()) {
        error_ = std::move(reason);
    }
}

}  // namespace feed_handler
