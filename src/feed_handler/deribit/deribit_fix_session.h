// Deribit-specific FIX.4.4 session mechanics: the messages this project sends
// (Logon, MarketDataRequest, Heartbeat, TestRequest, Logout) and the sequence
// number bookkeeping on both directions of the session.
//
// Pure logic -- no socket, no journal. The next slice adds the raw-socket
// client that drives this (decisions/0004: Deribit FIX is hand-rolled over its
// own fd from day one, so unlike Kraken it can join an epoll group later).
// Keeping the message construction and sequence tracking testable without a
// connection is the same split that made the Kraken signing correct before it
// was ever pointed at the live API.
//
// Wire details are exchanges/deribit.md, confirmed against the testnet by
// experiments/deribit_fix_probe.py.
//
// Credential handling follows the Kraken rules: `client_secret` exists only to
// be hashed into the Logon password, and neither it nor the derived password
// is ever logged or journaled. decisions/0004 keeps outbound traffic out of
// the journal structurally (JournalWriter has no outbound path), which is
// what stops a Logon from ever reaching disk.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "feed_handler/fix/fix_message.h"
#include "feed_handler/message_sink.h"

namespace feed_handler::deribit {

/// Deribit's fixed TargetCompID (exchanges/deribit.md).
inline constexpr std::string_view kTargetCompId = "DERIBITSERVER";

/// RawData's nonce is 32 random bytes, base64-encoded, as the probe sends.
inline constexpr std::size_t kLogonNonceBytes = 32;

/// Deribit's own HeartBtInt in the accepted Logon.
inline constexpr int kHeartbeatIntervalSeconds = 30;

struct SessionConfig {
    /// Deribit client id. Doubles as SenderCompID(49) and Username(553).
    std::string client_id;
    /// Deribit client secret. Never logged, journaled or echoed in an error.
    std::string client_secret;
    std::string target_comp_id{kTargetCompId};
    int heartbeat_interval_seconds = kHeartbeatIntervalSeconds;
    /// Source of SendingTime(52). A plain function pointer rather than a
    /// std::function so it costs nothing and so tests can pin the timestamp
    /// and compare whole messages byte for byte.
    std::uint64_t (*clock_ns)() = &RealtimeNowNs;
};

/// The two Logon fields derived from the client secret.
struct LogonCredentials {
    /// RawData(96) = "<timestamp_ms>.<base64(nonce)>".
    std::string raw_data;
    /// Password(554) = base64(SHA256(RawData || client_secret)).
    std::string password;
};

/// Builds the Logon credential pair from explicit inputs.
///
/// Deliberately takes the timestamp and nonce as parameters instead of
/// sampling them: that is what lets a test pin both and compare the result
/// against an independently computed known answer (Python hashlib/base64, the
/// same computation experiments/deribit_fix_probe.py performs) rather than
/// merely against itself.
LogonCredentials MakeLogonCredentials(std::uint64_t timestamp_ms, std::span<const std::byte> nonce,
                                      std::string_view client_secret);

/// kLogonNonceBytes bytes from OpenSSL's CSPRNG. A predictable nonce would let
/// a captured Logon be replayed, so this must not fall back to a plain PRNG --
/// it reports failure instead.
std::expected<std::array<std::byte, kLogonNonceBytes>, std::string> GenerateLogonNonce();

/// Outcome of checking one inbound MsgSeqNum(34).
enum class SequenceStatus : std::uint8_t {
    /// Exactly the expected number.
    kInSequence,
    /// Higher than expected: messages were lost. v1 does not repair this.
    kGap,
    /// Lower than expected and not flagged as a resend: the session state is
    /// inconsistent, which FIX treats as fatal to the session.
    kDuplicate,
    /// PossDupFlag(43)=Y -- an administratively resent message, which is
    /// explicitly not a gap and must not advance the expectation.
    kPossibleDuplicate,
    /// No usable MsgSeqNum(34) on the message at all.
    kMalformed,
};

std::string_view ToString(SequenceStatus status);

struct InboundCheck {
    SequenceStatus status = SequenceStatus::kMalformed;
    /// What the session expected to receive.
    std::uint64_t expected = 0;
    /// What actually arrived (0 when status is malformed).
    std::uint64_t received = 0;
    /// How many messages were skipped; non-zero only for `gap`.
    std::uint64_t missing = 0;

    /// True when the session can no longer be trusted and the connection
    /// should be torn down and re-logged-on. This is the single thing the
    /// future client has to branch on.
    bool SessionBroken() const {
        return status == SequenceStatus::kGap || status == SequenceStatus::kDuplicate ||
               status == SequenceStatus::kMalformed;
    }

    /// One-line, credential-free description for a log record.
    std::string Describe() const;
};

/// One FIX session's message construction and sequence state.
///
/// One instance per connection incarnation, driven by that connection's own
/// thread; not thread safe and does not need to be (decisions/0004).
///
/// Every build_* call consumes an outbound MsgSeqNum(34), so messages must be
/// sent in the order they were built -- the exchange rejects a session whose
/// inbound numbering has holes, and a built-but-unsent message leaves one.
class FixSession {
  public:
    explicit FixSession(SessionConfig cfg);

    /// Logon(35=A) with a freshly generated nonce. Fails only if the CSPRNG
    /// does; no sequence number is consumed in that case.
    std::expected<std::string, std::string> BuildLogon();

    /// Logon(35=A) from a caller-supplied timestamp and nonce. The test seam,
    /// and the reason the crypto is verifiable against a reference value.
    std::string BuildLogonWithNonce(std::uint64_t timestamp_ms, std::span<const std::byte> nonce);

    /// MarketDataRequest(35=V): snapshot + updates, full depth, Bid and Offer,
    /// one symbol (exchanges/deribit.md).
    std::string BuildMarketDataRequest(std::string_view md_req_id, std::string_view symbol);

    /// Heartbeat(35=0) sent on our own timer.
    std::string BuildHeartbeat();

    /// Heartbeat(35=0) answering a TestRequest, echoing its TestReqID(112).
    /// Failing to echo it is why an otherwise-healthy session gets logged out.
    std::string BuildHeartbeatResponse(std::string_view test_req_id);

    /// TestRequest(35=1), for prodding a peer that has gone quiet.
    std::string BuildTestRequest(std::string_view test_req_id);

    /// Logout(35=5) for a clean shutdown.
    std::string BuildLogout(std::string_view text = {});

    /// Checks `message`'s MsgSeqNum(34) against the expectation and advances
    /// it. Honours PossDupFlag(43).
    InboundCheck OnInbound(const fix::ParsedMessage& message);

    /// Same check from a bare sequence number, for a caller that already has
    /// one (and for tests).
    ///
    /// On a gap the expectation jumps to `msg_seq_num + 1` rather than staying
    /// put: v1's response to a gap is to drop the session and re-logon
    /// (decisions/0004 -- no ResendRequest/SequenceReset gap fill), so holding
    /// the old expectation would only turn one lost message into an endless
    /// stream of identical gap reports before the reconnect lands.
    InboundCheck OnInboundSeqNum(std::uint64_t msg_seq_num, bool possible_duplicate = false);

    /// Restarts both counters for a new session. Deribit accepted a fresh
    /// session starting at MsgSeqNum 1 without ResetSeqNumFlag(141) in the
    /// probe, so no 141 is sent; a reconnect simply calls this.
    void ResetSequenceNumbers();

    /// MsgSeqNum(34) the next outbound message will carry.
    std::uint64_t NextOutboundSeqNum() const {
        return next_outbound_;
    }

    /// MsgSeqNum(34) the next inbound message is expected to carry.
    std::uint64_t ExpectedInboundSeqNum() const {
        return expected_inbound_;
    }

    std::uint64_t GapsDetected() const {
        return gaps_detected_;
    }

    const SessionConfig& Config() const {
        return cfg_;
    }

  private:
    fix::SessionHeader NextHeader(std::string_view type);
    std::string Send(std::string_view type, std::span<const fix::Field> body);

    SessionConfig cfg_;
    std::string sending_time_;
    std::uint64_t next_outbound_ = 1;
    std::uint64_t expected_inbound_ = 1;
    std::uint64_t gaps_detected_ = 0;
};

}  // namespace feed_handler::deribit
