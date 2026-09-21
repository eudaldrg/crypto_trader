#include "feed_handler/capture_session.h"

#include <array>
#include <bit>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <system_error>

namespace feed_handler {
namespace {

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

std::string JournalFileName(std::string_view prefix, std::uint64_t connect_id,
                            std::uint64_t realtime_ns) {
    const auto seconds = static_cast<std::time_t>(realtime_ns / kNanosPerSecond);
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    std::array<char, 24> stamp{};
    const std::size_t length = std::strftime(stamp.data(), stamp.size(), "%Y%m%dT%H%M%SZ", &utc);

    std::array<char, 24> ordinal{};
    std::snprintf(ordinal.data(), ordinal.size(), "%06llu",
                  static_cast<unsigned long long>(connect_id));

    return std::string(prefix) + "-" + ordinal.data() + "-" + std::string(stamp.data(), length) +
           ".journal";
}

std::expected<std::filesystem::path, std::string> CaptureSession::BeginConnect(
    std::string_view reason, FrameSource source) {
    Close();
    ++connect_id_;
    // Capture sequence numbers are per-connect (journal_writer.h), so the
    // stamper restarts with the file rather than carrying over.
    stamper_ = CaptureStamper{};

    std::error_code ec;
    std::filesystem::create_directories(cfg_.directory, ec);
    if (ec && !std::filesystem::is_directory(cfg_.directory)) {
        return std::unexpected("cannot create journal directory " + cfg_.directory.string() + ": " +
                               ec.message());
    }

    const std::filesystem::path path =
        cfg_.directory /
        JournalFileName(cfg_.file_prefix.empty() ? cfg_.exchange : cfg_.file_prefix, connect_id_,
                        RealtimeNowNs());
    try {
        writer_ = std::make_unique<JournalWriter>(path, JournalWriter::Config{
                                                            .exchange = cfg_.exchange,
                                                            .connect_id = connect_id_,
                                                        });
    } catch (const std::runtime_error& error) {
        return std::unexpected(std::string(error.what()));
    }

    current_path_ = path;
    // The marker goes to the journal writer through its own concrete
    // write_connect_marker(), not through the message_sink interface: it is
    // a *record*, so it needs a stamped frame and a place in this connect's
    // capture sequence, and this stamper is the only thing that may hand those
    // out. A sink-interface version would need a second sequence source, which
    // is precisely what the single stamper exists to prevent. The other sinks
    // get the notification form below, which needs neither.
    writer_->WriteConnectMarker(stamper_.Stamp(BytesOf(reason), source));
    if (!writer_->Good()) {
        return std::unexpected("cannot write connect marker: " + writer_->Error());
    }

    // Only once the connect is actually usable: every caller treats a
    // failure above as fatal to capture, and telling a sink to reset its state
    // for a connect that never starts would be worse than not telling it.
    for (MessageSink* sink : sinks_) {
        sink->OnConnect(connect_id_, reason);
    }
    return path;
}

bool CaptureSession::OnWireMessage(std::span<const std::byte> payload, FrameSource source) {
    if (writer_ == nullptr) {
        return false;
    }
    const CaptureFrame frame = stamper_.Stamp(payload, source);
    // Journal first, sinks second -- the same discipline both exchange clients
    // follow one level up ("journal first, classify second"), for the same
    // reason: nothing a downstream sink does may decide whether the record gets
    // written. The extra sinks still see a frame whose journal write failed,
    // though: what failed is the disk, not the data, and an order book silently
    // missing a message would be a second fault on top of the first.
    writer_->OnFrame(frame);
    const bool journaled = writer_->Good();
    for (MessageSink* sink : sinks_) {
        sink->OnFrame(frame);
    }
    return journaled;
}

void CaptureSession::Close() {
    if (writer_ == nullptr) {
        return;
    }
    closed_records_ += writer_->RecordsWritten();
    writer_->Flush();
    writer_.reset();
    current_path_.clear();
}

}  // namespace feed_handler
