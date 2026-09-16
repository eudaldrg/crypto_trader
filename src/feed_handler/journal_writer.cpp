#include "feed_handler/journal_writer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <utility>

namespace feed_handler {

JournalWriter::JournalWriter(const std::filesystem::path& path, const Config& cfg)
    : buffer_(std::max<std::size_t>(cfg.buffer_bytes, 1)), path_(path.string()) {
    // pubsetbuf only has an effect before the stream is opened, hence the
    // deliberate open-after-construct dance.
    out_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    out_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
        throw std::runtime_error("JournalWriter: cannot open " + path_);
    }

    std::array<std::byte, journal::kFileHeaderSize> header{};
    std::span<std::byte> view(header);
    std::ranges::copy(journal::kMagic, view.begin());
    journal::StoreLe<std::uint16_t>(view.subspan(8), journal::kFormatVersion);
    journal::StoreLe<std::uint16_t>(view.subspan(10),
                                    static_cast<std::uint16_t>(journal::kFileHeaderSize));
    journal::StoreLe<std::uint32_t>(view.subspan(12), 0);
    // The realtime/monotonic pair must be sampled as close together as
    // possible: it is the only anchor tying per-record monotonic readings back
    // to wall clock for this incarnation.
    journal::StoreLe<std::uint64_t>(view.subspan(16), RealtimeNowNs());
    journal::StoreLe<std::uint64_t>(view.subspan(24), MonotonicNowNs());

    const std::size_t name_bytes = std::min(cfg.exchange.size(), journal::kExchangeFieldSize);
    for (std::size_t index = 0; index < name_bytes; ++index) {
        view[32 + index] = static_cast<std::byte>(cfg.exchange[index]);
    }
    journal::StoreLe<std::uint64_t>(view.subspan(48), cfg.incarnation);
    journal::StoreLe<std::uint32_t>(view.subspan(56), 0);
    journal::StoreLe<std::uint32_t>(view.subspan(60), journal::Crc32Of(view.first(60)));

    out_.write(std::bit_cast<const char*>(view.data()), static_cast<std::streamsize>(view.size()));
    if (!out_) {
        throw std::runtime_error("JournalWriter: cannot write header to " + path_);
    }
}

JournalWriter::~JournalWriter() {
    // Best effort: a destructor must not throw, and a failure here is already
    // reflected by good()/error() for anything that cares.
    out_.flush();
}

void JournalWriter::OnFrame(const CaptureFrame& frame) {
    WriteRecord(journal::RecordType::kWireMessage, frame);
}

void JournalWriter::WriteIncarnationMarker(const CaptureFrame& frame) {
    WriteRecord(journal::RecordType::kConnectionIncarnation, frame);
}

void JournalWriter::WriteRecord(journal::RecordType type, const CaptureFrame& frame) {
    if (!Good()) {
        return;
    }
    if (frame.capture_sequence <= last_sequence_) {
        // Writing an out-of-order record would silently break the ordering
        // guarantee Replay mode is built on, so refuse rather than corrupt.
        Fail("out-of-order capture sequence");
        return;
    }
    if (frame.payload.size() > journal::kMaxPayloadBytes) {
        Fail("payload exceeds kMaxPayloadBytes");
        return;
    }

    std::array<std::byte, journal::kRecordHeaderSize> header{};
    std::span<std::byte> view(header);
    view[0] = static_cast<std::byte>(type);
    view[1] = std::byte{0};
    journal::StoreLe<std::uint16_t>(view.subspan(2), 0);
    journal::StoreLe<std::uint32_t>(view.subspan(4),
                                    static_cast<std::uint32_t>(frame.payload.size()));
    journal::StoreLe<std::uint64_t>(view.subspan(8), frame.capture_sequence);
    journal::StoreLe<std::uint64_t>(view.subspan(16), frame.monotonic_ns);

    const std::uint32_t crc = journal::Crc32Update(journal::Crc32Of(view), frame.payload);
    std::array<std::byte, journal::kRecordTrailerSize> trailer{};
    journal::StoreLe<std::uint32_t>(trailer, crc);

    out_.write(std::bit_cast<const char*>(view.data()), static_cast<std::streamsize>(view.size()));
    if (!frame.payload.empty()) {
        out_.write(std::bit_cast<const char*>(frame.payload.data()),
                   static_cast<std::streamsize>(frame.payload.size()));
    }
    out_.write(std::bit_cast<const char*>(trailer.data()),
               static_cast<std::streamsize>(trailer.size()));
    if (!out_) {
        Fail("write failed");
        return;
    }

    last_sequence_ = frame.capture_sequence;
    ++records_written_;
}

void JournalWriter::Flush() {
    out_.flush();
    if (!out_ && Good()) {
        Fail("flush failed");
    }
}

void JournalWriter::Fail(std::string reason) {
    if (error_.empty()) {
        error_ = std::move(reason);
    }
}

}  // namespace feed_handler
