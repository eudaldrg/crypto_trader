// Kraken REST private-endpoint request signing, plus the nonce source it
// needs. The signing functions and `NonceGenerator` are pure -- no network, no
// I/O -- so they are unit-testable without credentials. The one exception is
// `PersistentNonceSource`, a thin wrapper that reads/writes the nonce
// high-water mark file; it is kept separate precisely so the rule it wraps
// stays pure. See exchanges/kraken.md, "Auth".
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace feed_handler::kraken {

/// Percent-encodes `value` per application/x-www-form-urlencoded, matching
/// Python's urllib.parse.urlencode (the reference the probe signs with):
/// unreserved characters pass through, space becomes '+', everything else
/// becomes %HH with uppercase hex.
std::string UrlEncode(std::string_view value);

/// Joins `params` into a urlencoded POST body in the given order. Order is
/// significant: the signature is computed over this exact byte string, so the
/// body that gets sent must be the same object that was signed.
std::string EncodePostData(std::span<const std::pair<std::string, std::string>> params);

std::string Base64Encode(std::span<const std::byte> data);

/// Strict base64 decode. The error string never echoes the input, because the
/// only thing this is used on is the API secret.
std::expected<std::vector<std::byte>, std::string> Base64Decode(std::string_view text);

/// Computes Kraken's `API-Sign` header value:
///
///   base64(HMAC-SHA512(base64decode(secret),
///                      url_path || SHA256(nonce || post_data)))
///
/// `post_data` is the exact urlencoded body being sent (which itself contains
/// `nonce=<nonce>`). Neither the secret nor any part of it appears in the
/// error string on failure.
std::expected<std::string, std::string> SignPrivateRequest(std::string_view url_path,
                                                           std::string_view nonce,
                                                           std::string_view post_data,
                                                           std::string_view api_secret_b64);

/// Kraken requires a strictly increasing nonce per API key. A raw millisecond
/// timestamp (what experiments/kraken_l3_probe.py uses) collides when two
/// calls land in the same millisecond, which a reconnect loop will do -- so
/// take microseconds and force progress with max(now, last + 1).
///
/// In-memory only, by design: no I/O, no clock read beyond `next()`, so the
/// rule itself stays trivially testable. Surviving a *restart* is
/// `PersistentNonceSource`'s job.
class NonceGenerator {
  public:
    /// Next nonce from the current wall clock, in microseconds.
    std::uint64_t Next();

    /// Test seam: same rule against a caller-supplied "now", so same-timestamp
    /// and backwards-clock cases are directly exercisable.
    std::uint64_t NextFrom(std::uint64_t now_micros);

    /// Raises the high-water mark to `value`, never lowers it, so the next
    /// nonce is at least `value + 1`. Seeds from a persisted mark.
    void SeedAtLeast(std::uint64_t value);

    std::uint64_t Last() const {
        return last_;
    }

  private:
    std::uint64_t last_ = 0;
};

/// A `NonceGenerator` whose high-water mark survives a process restart.
///
/// Kraken's strictly-increasing-per-API-key rule is not self-healing: one
/// nonce below a value already used gets every later call with that key
/// rejected until the clock catches back up again. The wall clock alone covers
/// this in practice, but not a restart coinciding with a backwards clock step
/// (an NTP correction, say), so the mark is also kept in a small local file.
///
/// Everything about that file is best effort and defense in depth, never a
/// startup dependency: a missing file (the ordinary first run), an unparseable
/// one, and an unwritable directory each fall back to plain clock-only
/// behavior, with a warning for the two that are not expected. On startup the
/// generator is seeded so its next nonce is `max(persisted + 1, now_micros)` --
/// neither source is trusted on its own. A nonce is not a secret (unlike the
/// API key, secret and WS token), so it is stored and logged in the clear.
///
/// Default-constructed, this persists nothing and behaves exactly like a bare
/// `NonceGenerator`.
class PersistentNonceSource {
  public:
    /// Clock-only: no file, no I/O.
    PersistentNonceSource() = default;

    /// Seeds from `state_file` if it holds a usable mark, and writes every
    /// nonce issued afterwards back to it. An empty path means "no
    /// persistence".
    explicit PersistentNonceSource(std::filesystem::path state_file);

    /// Next nonce from the current wall clock, persisted before it is used.
    std::uint64_t Next();

    /// Test seam, matching `NonceGenerator::next_from`: the same rule and the
    /// same persistence, against a caller-supplied "now".
    std::uint64_t NextFrom(std::uint64_t now_micros);

    std::uint64_t Last() const {
        return generator_.Last();
    }

    /// True when a state file is configured, whether or not writes to it are
    /// currently succeeding.
    bool Persisting() const {
        return !state_file_.empty();
    }

    const std::filesystem::path& StateFile() const {
        return state_file_;
    }

    /// The mark read at construction; 0 when there was no usable one.
    std::uint64_t SeededFrom() const {
        return seeded_from_;
    }

  private:
    void Persist(std::uint64_t value);

    NonceGenerator generator_;
    std::filesystem::path state_file_;
    std::uint64_t seeded_from_ = 0;
    /// Latched so an unwritable directory warns once rather than on every
    /// signed call.
    bool write_failed_ = false;
};

}  // namespace feed_handler::kraken
