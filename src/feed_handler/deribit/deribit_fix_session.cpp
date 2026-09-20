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
std::string Base64Encode(std::span<const std::byte> data) {
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

std::array<std::byte, kSha256Bytes> Sha256(std::span<const std::byte> data) {
    std::array<std::byte, kSha256Bytes> digest{};
    unsigned int produced = 0;
    if (EVP_Digest(data.data(), data.size(), std::bit_cast<unsigned char*>(digest.data()),
                   &produced, EVP_sha256(), nullptr) != 1 ||
        produced != digest.size()) {
        return {};
    }
    return digest;
}

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

std::string_view ToString(SequenceStatus status) {
    switch (status) {
        case SequenceStatus::kGap:
            return "gap";
        case SequenceStatus::kDuplicate:
            return "duplicate";
        case SequenceStatus::kPossibleDuplicate:
            return "possible-duplicate";
        case SequenceStatus::kMalformed:
            return "malformed";
        case SequenceStatus::kInSequence:
            break;
    }
    return "in-sequence";
}

std::string InboundCheck::Describe() const {
    std::string line(ToString(status));
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

LogonCredentials MakeLogonCredentials(std::uint64_t timestamp_ms, std::span<const std::byte> nonce,
                                      std::string_view client_secret) {
    LogonCredentials credentials;
    credentials.raw_data = std::to_string(timestamp_ms);
    credentials.raw_data += '.';
    credentials.raw_data += Base64Encode(nonce);

    // SHA256 over the concatenation of the *rendered* RawData and the secret,
    // not over the raw nonce bytes: the exchange only ever sees RawData, so it
    // can only recompute the hash over that exact text.
    std::string hashed;
    hashed.reserve(credentials.raw_data.size() + client_secret.size());
    hashed += credentials.raw_data;
    hashed += client_secret;

    const auto digest = Sha256(BytesOf(hashed));
    OPENSSL_cleanse(hashed.data(), hashed.size());
    credentials.password = Base64Encode(digest);
    return credentials;
}

std::expected<std::array<std::byte, kLogonNonceBytes>, std::string> GenerateLogonNonce() {
    std::array<std::byte, kLogonNonceBytes> nonce{};
    if (RAND_bytes(std::bit_cast<unsigned char*>(nonce.data()), static_cast<int>(nonce.size())) !=
        1) {
        return std::unexpected("deribit: OpenSSL CSPRNG failed to produce a Logon nonce");
    }
    return nonce;
}

FixSession::FixSession(SessionConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.clock_ns == nullptr) {
        cfg_.clock_ns = &RealtimeNowNs;
    }
}

fix::SessionHeader FixSession::NextHeader(std::string_view type) {
    sending_time_ = fix::FormatUtcTimestamp(cfg_.clock_ns());
    return fix::SessionHeader{
        .msg_type = type,
        .sender_comp_id = cfg_.client_id,
        .target_comp_id = cfg_.target_comp_id,
        .msg_seq_num = next_outbound_++,
        .sending_time = sending_time_,
    };
}

std::string FixSession::Send(std::string_view type, std::span<const fix::Field> body) {
    return fix::BuildMessage(NextHeader(type), body);
}

std::expected<std::string, std::string> FixSession::BuildLogon() {
    const auto nonce = GenerateLogonNonce();
    if (!nonce) {
        return std::unexpected(nonce.error());
    }
    constexpr std::uint64_t kNanosPerMilli = 1'000'000;
    return BuildLogonWithNonce(cfg_.clock_ns() / kNanosPerMilli, *nonce);
}

std::string FixSession::BuildLogonWithNonce(std::uint64_t timestamp_ms,
                                            std::span<const std::byte> nonce) {
    const LogonCredentials credentials =
        MakeLogonCredentials(timestamp_ms, nonce, cfg_.client_secret);

    // Field order matches experiments/deribit_fix_probe.py, which Deribit's
    // testnet accepted. FIX does not require this order for non-group body
    // fields, but "same bytes the probe sent" is a better starting point than
    // "should be equivalent" when the first live Logon fails.
    const std::array<fix::Field, 5> body = {
        fix::Field{.tag = fix::tag::kEncryptMethod, .value = "0"},
        fix::Field{.tag = fix::tag::kHeartBtInt,
                   .value = std::to_string(cfg_.heartbeat_interval_seconds)},
        fix::Field{.tag = fix::tag::kUsername, .value = cfg_.client_id},
        fix::Field{.tag = fix::tag::kRawData, .value = credentials.raw_data},
        fix::Field{.tag = fix::tag::kPassword, .value = credentials.password},
    };
    return Send(fix::msg_type::kLogon, body);
}

std::string FixSession::BuildMarketDataRequest(std::string_view md_req_id,
                                               std::span<const std::string> symbols) {
    // NoMDEntryTypes(267)=2 is followed by its two MDEntryType(269) members,
    // then NoRelatedSym(146)=N by its N Symbol(55) members. Repeating groups
    // are positional in FIX: a count field, then exactly that many members in
    // order. Building them as a flat ordered field list is correct; only
    // *parsing* them back into a structure needs fix::ReadGroup.
    std::vector<fix::Field> body = {
        fix::Field{.tag = fix::tag::kMdReqId, .value = std::string(md_req_id)},
        fix::Field{.tag = fix::tag::kSubscriptionRequestType, .value = "1"},
        fix::Field{.tag = fix::tag::kMarketDepth, .value = "0"},
        fix::Field{.tag = fix::tag::kNoMdEntryTypes, .value = "2"},
        fix::Field{.tag = fix::tag::kMdEntryType, .value = "0"},
        fix::Field{.tag = fix::tag::kMdEntryType, .value = "1"},
        fix::Field{.tag = fix::tag::kNoRelatedSym, .value = std::to_string(symbols.size())},
    };
    body.reserve(body.size() + symbols.size());
    for (const std::string& symbol : symbols) {
        body.push_back(fix::Field{.tag = fix::tag::kSymbol, .value = symbol});
    }
    return Send(fix::msg_type::kMarketDataRequest, body);
}

std::string FixSession::BuildHeartbeat() {
    return Send(fix::msg_type::kHeartbeat, {});
}

std::string FixSession::BuildHeartbeatResponse(std::string_view test_req_id) {
    const std::array<fix::Field, 1> body = {
        fix::Field{.tag = fix::tag::kTestReqId, .value = std::string(test_req_id)},
    };
    return Send(fix::msg_type::kHeartbeat, body);
}

std::string FixSession::BuildTestRequest(std::string_view test_req_id) {
    const std::array<fix::Field, 1> body = {
        fix::Field{.tag = fix::tag::kTestReqId, .value = std::string(test_req_id)},
    };
    return Send(fix::msg_type::kTestRequest, body);
}

std::string FixSession::BuildLogout(std::string_view text) {
    if (text.empty()) {
        return Send(fix::msg_type::kLogout, {});
    }
    const std::array<fix::Field, 1> body = {
        fix::Field{.tag = fix::tag::kText, .value = std::string(text)},
    };
    return Send(fix::msg_type::kLogout, body);
}

InboundCheck FixSession::OnInbound(const fix::ParsedMessage& message) {
    const auto seq_num = message.GetInt(fix::tag::kMsgSeqNum);
    if (!seq_num || *seq_num < 0) {
        return InboundCheck{
            .status = SequenceStatus::kMalformed,
            .expected = expected_inbound_,
            .received = 0,
            .missing = 0,
        };
    }
    const auto poss_dup = message.Get(fix::tag::kPossDupFlag);
    const bool possible_duplicate = poss_dup.has_value() && *poss_dup == "Y";
    return OnInboundSeqNum(static_cast<std::uint64_t>(*seq_num), possible_duplicate);
}

InboundCheck FixSession::OnInboundSeqNum(std::uint64_t msg_seq_num, bool possible_duplicate) {
    InboundCheck check{
        .status = SequenceStatus::kInSequence,
        .expected = expected_inbound_,
        .received = msg_seq_num,
        .missing = 0,
    };

    if (possible_duplicate) {
        // An administrative resend. It is not evidence of loss and must not
        // move the expectation, or the real next message would then look like
        // a duplicate.
        check.status = SequenceStatus::kPossibleDuplicate;
        return check;
    }

    if (msg_seq_num == expected_inbound_) {
        ++expected_inbound_;
        return check;
    }

    if (msg_seq_num < expected_inbound_) {
        check.status = SequenceStatus::kDuplicate;
        return check;
    }

    check.status = SequenceStatus::kGap;
    check.missing = msg_seq_num - expected_inbound_;
    ++gaps_detected_;
    expected_inbound_ = msg_seq_num + 1;
    return check;
}

void FixSession::ResetSequenceNumbers() {
    next_outbound_ = 1;
    expected_inbound_ = 1;
}

}  // namespace feed_handler::deribit
