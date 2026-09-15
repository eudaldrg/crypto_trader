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

std::span<const std::byte> bytes_of(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

std::string journal_file_name(std::string_view exchange, std::uint64_t incarnation,
                              std::uint64_t realtime_ns) {
    const auto seconds = static_cast<std::time_t>(realtime_ns / kNanosPerSecond);
    std::tm utc{};
    ::gmtime_r(&seconds, &utc);
    std::array<char, 24> stamp{};
    const std::size_t length = std::strftime(stamp.data(), stamp.size(), "%Y%m%dT%H%M%SZ", &utc);

    std::array<char, 24> ordinal{};
    std::snprintf(ordinal.data(), ordinal.size(), "%06llu",
                  static_cast<unsigned long long>(incarnation));

    return std::string(exchange) + "-" + ordinal.data() + "-" + std::string(stamp.data(), length) +
           ".journal";
}

std::expected<std::filesystem::path, std::string> capture_session::begin_incarnation(
    std::string_view reason) {
    close();
    ++incarnation_;
    // Capture sequence numbers are per-incarnation (journal_writer.h), so the
    // stamper restarts with the file rather than carrying over.
    stamper_ = capture_stamper{};

    std::error_code ec;
    std::filesystem::create_directories(cfg_.directory, ec);
    if (ec && !std::filesystem::is_directory(cfg_.directory)) {
        return std::unexpected("cannot create journal directory " + cfg_.directory.string() + ": " +
                               ec.message());
    }

    const std::filesystem::path path =
        cfg_.directory / journal_file_name(cfg_.exchange, incarnation_, realtime_now_ns());
    try {
        writer_ = std::make_unique<journal_writer>(path, journal_writer::config{
                                                             .exchange = cfg_.exchange,
                                                             .incarnation = incarnation_,
                                                         });
    } catch (const std::runtime_error& error) {
        return std::unexpected(std::string(error.what()));
    }

    current_path_ = path;
    writer_->write_incarnation_marker(stamper_.stamp(bytes_of(reason)));
    if (!writer_->good()) {
        return std::unexpected("cannot write incarnation marker: " + writer_->error());
    }
    return path;
}

bool capture_session::on_wire_message(std::span<const std::byte> payload) {
    if (writer_ == nullptr) {
        return false;
    }
    writer_->on_frame(stamper_.stamp(payload));
    return writer_->good();
}

void capture_session::close() {
    if (writer_ == nullptr) {
        return;
    }
    closed_records_ += writer_->records_written();
    writer_->flush();
    writer_.reset();
    current_path_.clear();
}

}  // namespace feed_handler
