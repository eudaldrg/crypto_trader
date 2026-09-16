#include "feed_handler/deribit/deribit_fix_session.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <bit>
#include <utility>
#include <vector>

namespace feed_handler::deribit {
namespace {

constexpr std::size_t kSha256Bytes = 32;

/// base64 and SHA-256 are deliberately re-implemented here instead of reaching
/// into feed_handler::kraken::base64_encode. A Deribit translation unit that
/// has to include a Kraken header to log on is exactly the kind of accidental
/// coupling this second backend exists to catch (decisions/0004: Kraken-first,
/// Deribit-second, to prove the seams are not Kraken-shaped). If a third user
/// appears, promote these to a shared feed_handler/crypto helper rather than
/// letting one exchange depend on another.
std::string base64_encode(std::span<const std::byte> data) {
    if (data.empty()) {
        return {};
    }
    // EVP_EncodeBlock writes 4 characters per 3 input bytes plus a NUL.
    std::vector<unsigned char> encoded((((data.size() + 2) / 3) * 4) + 1);
    const int written =
        EVP_EncodeBlock(encoded.data(), std::bit_cast<const unsigned char*>(data.data()),
                        static_cast<int>(data.size()));
    if (written <= 0) {
        return {};
    }
    return {std::bit_cast<const char*>(encoded.data()), static_cast<std::size_t>(written)};
}

std::array<std::byte, kSha256Bytes> sha256(std::span<const std::byte> data) {
    std::array<std::byte, kSha256Bytes> digest{};
    unsigned int produced = 0;
    if (EVP_Digest(data.data(), data.size(), std::bit_cast<unsigned char*>(digest.data()),
                   &produced, EVP_sha256(), nullptr) != 1 ||
        produced != digest.size()) {
        return {};
    }
    return digest;
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

std::string_view to_string(sequence_status status) {
    switch (status) {
        case sequence_status::gap:
            return "gap";
        case sequence_status::duplicate:
            return "duplicate";
        case sequence_status::possible_duplicate:
            return "possible-duplicate";
        case sequence_status::malformed:
            return "malformed";
        case sequence_status::in_sequence:
            break;
    }
    return "in-sequence";
}

std::string inbound_check::describe() const {
    std::string line(to_string(status));
    line += ": expected MsgSeqNum ";
    line += std::to_string(expected);
    line += ", received ";
    line += std::to_string(received);
    if (missing != 0) {
        line += " (";
        line += std::to_string(missing);
        line += " missing)";
    }
    return line;
}

logon_credentials make_logon_credentials(std::uint64_t timestamp_ms,
                                         std::span<const std::byte> nonce,
                                         std::string_view client_secret) {
    logon_credentials credentials;
    credentials.raw_data = std::to_string(timestamp_ms);
    credentials.raw_data += '.';
    credentials.raw_data += base64_encode(nonce);

    // SHA256 over the concatenation of the *rendered* RawData and the secret,
    // not over the raw nonce bytes: the exchange only ever sees RawData, so it
    // can only recompute the hash over that exact text.
    std::string hashed;
    hashed.reserve(credentials.raw_data.size() + client_secret.size());
    hashed += credentials.raw_data;
    hashed += client_secret;

    const auto digest = sha256(bytes_of(hashed));
    OPENSSL_cleanse(hashed.data(), hashed.size());
    credentials.password = base64_encode(digest);
    return credentials;
}

std::expected<std::array<std::byte, kLogonNonceBytes>, std::string> generate_logon_nonce() {
    std::array<std::byte, kLogonNonceBytes> nonce{};
    if (RAND_bytes(std::bit_cast<unsigned char*>(nonce.data()), static_cast<int>(nonce.size())) !=
        1) {
        return std::unexpected("deribit: OpenSSL CSPRNG failed to produce a Logon nonce");
    }
    return nonce;
}

fix_session::fix_session(session_config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.clock_ns == nullptr) {
        cfg_.clock_ns = &realtime_now_ns;
    }
}

fix::session_header fix_session::next_header(std::string_view type) {
    sending_time_ = fix::format_utc_timestamp(cfg_.clock_ns());
    return fix::session_header{
        .msg_type = type,
        .sender_comp_id = cfg_.client_id,
        .target_comp_id = cfg_.target_comp_id,
        .msg_seq_num = next_outbound_++,
        .sending_time = sending_time_,
    };
}

std::string fix_session::send(std::string_view type, std::span<const fix::field> body) {
    return fix::build_message(next_header(type), body);
}

std::expected<std::string, std::string> fix_session::build_logon() {
    const auto nonce = generate_logon_nonce();
    if (!nonce) {
        return std::unexpected(nonce.error());
    }
    constexpr std::uint64_t kNanosPerMilli = 1'000'000;
    return build_logon_with_nonce(cfg_.clock_ns() / kNanosPerMilli, *nonce);
}

std::string fix_session::build_logon_with_nonce(std::uint64_t timestamp_ms,
                                                std::span<const std::byte> nonce) {
    const logon_credentials credentials =
        make_logon_credentials(timestamp_ms, nonce, cfg_.client_secret);

    // Field order matches experiments/deribit_fix_probe.py, which Deribit's
    // testnet accepted. FIX does not require this order for non-group body
    // fields, but "same bytes the probe sent" is a better starting point than
    // "should be equivalent" when the first live Logon fails.
    const std::array<fix::field, 5> body = {
        fix::field{.tag = fix::tag::encrypt_method, .value = "0"},
        fix::field{.tag = fix::tag::heart_bt_int,
                   .value = std::to_string(cfg_.heartbeat_interval_seconds)},
        fix::field{.tag = fix::tag::username, .value = cfg_.client_id},
        fix::field{.tag = fix::tag::raw_data, .value = credentials.raw_data},
        fix::field{.tag = fix::tag::password, .value = credentials.password},
    };
    return send(fix::msg_type::logon, body);
}

std::string fix_session::build_market_data_request(std::string_view md_req_id,
                                                   std::string_view symbol) {
    // NoMDEntryTypes(267)=2 is followed by its two MDEntryType(269) members,
    // then NoRelatedSym(146)=1 by its Symbol(55). Repeating groups are
    // positional in FIX: a count field, then exactly that many members in
    // order. Building them as a flat ordered field list is correct; only
    // *parsing* them back into a structure is the part v1 skips
    // (fix_message.h).
    const std::array<fix::field, 8> body = {
        fix::field{.tag = fix::tag::md_req_id, .value = std::string(md_req_id)},
        fix::field{.tag = fix::tag::subscription_request_type, .value = "1"},
        fix::field{.tag = fix::tag::market_depth, .value = "0"},
        fix::field{.tag = fix::tag::no_md_entry_types, .value = "2"},
        fix::field{.tag = fix::tag::md_entry_type, .value = "0"},
        fix::field{.tag = fix::tag::md_entry_type, .value = "1"},
        fix::field{.tag = fix::tag::no_related_sym, .value = "1"},
        fix::field{.tag = fix::tag::symbol, .value = std::string(symbol)},
    };
    return send(fix::msg_type::market_data_request, body);
}

std::string fix_session::build_heartbeat() {
    return send(fix::msg_type::heartbeat, {});
}

std::string fix_session::build_heartbeat_response(std::string_view test_req_id) {
    const std::array<fix::field, 1> body = {
        fix::field{.tag = fix::tag::test_req_id, .value = std::string(test_req_id)},
    };
    return send(fix::msg_type::heartbeat, body);
}

std::string fix_session::build_test_request(std::string_view test_req_id) {
    const std::array<fix::field, 1> body = {
        fix::field{.tag = fix::tag::test_req_id, .value = std::string(test_req_id)},
    };
    return send(fix::msg_type::test_request, body);
}

std::string fix_session::build_logout(std::string_view text) {
    if (text.empty()) {
        return send(fix::msg_type::logout, {});
    }
    const std::array<fix::field, 1> body = {
        fix::field{.tag = fix::tag::text, .value = std::string(text)},
    };
    return send(fix::msg_type::logout, body);
}

inbound_check fix_session::on_inbound(const fix::parsed_message& message) {
    const auto seq_num = message.get_int(fix::tag::msg_seq_num);
    if (!seq_num || *seq_num < 0) {
        return inbound_check{
            .status = sequence_status::malformed,
            .expected = expected_inbound_,
            .received = 0,
            .missing = 0,
        };
    }
    const auto poss_dup = message.get(fix::tag::poss_dup_flag);
    const bool possible_duplicate = poss_dup.has_value() && *poss_dup == "Y";
    return on_inbound_seq_num(static_cast<std::uint64_t>(*seq_num), possible_duplicate);
}

inbound_check fix_session::on_inbound_seq_num(std::uint64_t msg_seq_num, bool possible_duplicate) {
    inbound_check check{
        .status = sequence_status::in_sequence,
        .expected = expected_inbound_,
        .received = msg_seq_num,
        .missing = 0,
    };

    if (possible_duplicate) {
        // An administrative resend. It is not evidence of loss and must not
        // move the expectation, or the real next message would then look like
        // a duplicate.
        check.status = sequence_status::possible_duplicate;
        return check;
    }

    if (msg_seq_num == expected_inbound_) {
        ++expected_inbound_;
        return check;
    }

    if (msg_seq_num < expected_inbound_) {
        check.status = sequence_status::duplicate;
        return check;
    }

    check.status = sequence_status::gap;
    check.missing = msg_seq_num - expected_inbound_;
    ++gaps_detected_;
    expected_inbound_ = msg_seq_num + 1;
    return check;
}

void fix_session::reset_sequence_numbers() {
    next_outbound_ = 1;
    expected_inbound_ = 1;
}

}  // namespace feed_handler::deribit
