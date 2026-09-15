// Kraken REST private-endpoint request signing, plus the nonce source it
// needs. Pure functions with no network or I/O, so they are unit-testable
// without credentials. See exchanges/kraken.md, "Auth".
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
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
std::string url_encode(std::string_view value);

/// Joins `params` into a urlencoded POST body in the given order. Order is
/// significant: the signature is computed over this exact byte string, so the
/// body that gets sent must be the same object that was signed.
std::string encode_post_data(std::span<const std::pair<std::string, std::string>> params);

std::string base64_encode(std::span<const std::byte> data);

/// Strict base64 decode. The error string never echoes the input, because the
/// only thing this is used on is the API secret.
std::expected<std::vector<std::byte>, std::string> base64_decode(std::string_view text);

/// Computes Kraken's `API-Sign` header value:
///
///   base64(HMAC-SHA512(base64decode(secret),
///                      url_path || SHA256(nonce || post_data)))
///
/// `post_data` is the exact urlencoded body being sent (which itself contains
/// `nonce=<nonce>`). Neither the secret nor any part of it appears in the
/// error string on failure.
std::expected<std::string, std::string> sign_private_request(std::string_view url_path,
                                                             std::string_view nonce,
                                                             std::string_view post_data,
                                                             std::string_view api_secret_b64);

/// Kraken requires a strictly increasing nonce per API key. A raw millisecond
/// timestamp (what experiments/kraken_l3_probe.py uses) collides when two
/// calls land in the same millisecond, which a reconnect loop will do -- so
/// take microseconds and force progress with max(now, last + 1).
///
/// No cross-process persistence in v1: this binary is meant to run
/// continuously, not restart in tight loops. A restart that rewinds the clock
/// (or a second process sharing the key) would still need a persisted
/// high-water mark.
class nonce_generator {
  public:
    /// Next nonce from the current wall clock, in microseconds.
    std::uint64_t next();

    /// Test seam: same rule against a caller-supplied "now", so same-timestamp
    /// and backwards-clock cases are directly exercisable.
    std::uint64_t next_from(std::uint64_t now_micros);

    std::uint64_t last() const {
        return last_;
    }

  private:
    std::uint64_t last_ = 0;
};

}  // namespace feed_handler::kraken
