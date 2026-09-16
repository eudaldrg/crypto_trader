#include "feed_handler/kraken/kraken_signing.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#include "feed_handler/logging.h"
#include "feed_handler/message_sink.h"

namespace feed_handler::kraken {
namespace {

constexpr std::string_view kHexDigits = "0123456789ABCDEF";
constexpr std::size_t kSha256Bytes = 32;
constexpr std::size_t kSha512Bytes = 64;

struct MacDeleter {
    void operator()(EVP_MAC* mac) const {
        EVP_MAC_free(mac);
    }
};
struct MacCtxDeleter {
    void operator()(EVP_MAC_CTX* ctx) const {
        EVP_MAC_CTX_free(ctx);
    }
};

bool IsUnreserved(char character) {
    const auto value = static_cast<unsigned char>(character);
    return (std::isalnum(value) != 0) || character == '-' || character == '_' || character == '.' ||
           character == '~';
}

std::span<const std::byte> BytesOf(std::string_view text) {
    return {std::bit_cast<const std::byte*>(text.data()), text.size()};
}

/// HMAC-SHA512 via OpenSSL 3's EVP_MAC interface.
std::expected<std::array<std::byte, kSha512Bytes>, std::string> HmacSha512(
    std::span<const std::byte> key, std::span<const std::byte> message) {
    const std::unique_ptr<EVP_MAC, MacDeleter> mac(EVP_MAC_fetch(nullptr, "HMAC", nullptr));
    if (!mac) {
        return std::unexpected("kraken: OpenSSL has no HMAC implementation");
    }
    const std::unique_ptr<EVP_MAC_CTX, MacCtxDeleter> ctx(EVP_MAC_CTX_new(mac.get()));
    if (!ctx) {
        return std::unexpected("kraken: EVP_MAC_CTX_new failed");
    }

    // OSSL_PARAM wants a non-const char*, so hold the digest name in a mutable
    // buffer rather than const_cast-ing a literal.
    std::array<char, 7> digest_name = {'S', 'H', 'A', '5', '1', '2', '\0'};
    const std::array<OSSL_PARAM, 2> params = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name.data(), 0),
        OSSL_PARAM_construct_end(),
    };

    if (EVP_MAC_init(ctx.get(), std::bit_cast<const unsigned char*>(key.data()), key.size(),
                     params.data()) != 1) {
        return std::unexpected("kraken: EVP_MAC_init failed");
    }
    if (EVP_MAC_update(ctx.get(), std::bit_cast<const unsigned char*>(message.data()),
                       message.size()) != 1) {
        return std::unexpected("kraken: EVP_MAC_update failed");
    }

    std::array<std::byte, kSha512Bytes> digest{};
    std::size_t produced = 0;
    if (EVP_MAC_final(ctx.get(), std::bit_cast<unsigned char*>(digest.data()), &produced,
                      digest.size()) != 1 ||
        produced != digest.size()) {
        return std::unexpected("kraken: EVP_MAC_final failed");
    }
    return digest;
}

std::expected<std::array<std::byte, kSha256Bytes>, std::string> Sha256(
    std::span<const std::byte> data) {
    std::array<std::byte, kSha256Bytes> digest{};
    unsigned int produced = 0;
    if (EVP_Digest(data.data(), data.size(), std::bit_cast<unsigned char*>(digest.data()),
                   &produced, EVP_sha256(), nullptr) != 1 ||
        produced != digest.size()) {
        return std::unexpected("kraken: SHA-256 failed");
    }
    return digest;
}

/// Reads the persisted nonce high-water mark. Returns nullopt for "no usable
/// value"; `reason` is filled in only when the file existed but could not be
/// used, because a missing file is the ordinary first run and not worth a
/// warning.
std::optional<std::uint64_t> ReadHighWaterMark(const std::filesystem::path& path,
                                               std::string& reason) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        reason = "cannot open it for reading";
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.pop_back();
    }

    std::uint64_t value = 0;
    const char* const first = std::to_address(text.cbegin());
    const char* const last = std::to_address(text.cend());
    const auto parsed = std::from_chars(first, last, value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != last) {
        reason = "contents are not a single decimal nonce";
        return std::nullopt;
    }
    return value;
}

/// Writes `value` via a sibling temp file plus rename. The rename is what
/// makes it safe: a crash mid-write would otherwise be able to leave a
/// *truncated*, i.e. smaller, mark behind, which is the one direction that
/// matters here. Returns false on any failure -- the caller treats persistence
/// as best effort.
bool WriteHighWaterMark(const std::filesystem::path& path, std::uint64_t value) {
    std::error_code error;
    const std::filesystem::path directory = path.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, error);
        if (!std::filesystem::is_directory(directory)) {
            return false;
        }
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output << value << '\n';
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

}  // namespace

std::string UrlEncode(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size());
    for (const char character : value) {
        if (IsUnreserved(character)) {
            encoded.push_back(character);
        } else if (character == ' ') {
            encoded.push_back('+');
        } else {
            const auto raw = static_cast<unsigned char>(character);
            encoded.push_back('%');
            encoded.push_back(kHexDigits[raw >> 4U]);
            encoded.push_back(kHexDigits[raw & 0x0FU]);
        }
    }
    return encoded;
}

std::string EncodePostData(std::span<const std::pair<std::string, std::string>> params) {
    std::string body;
    for (const auto& [key, value] : params) {
        if (!body.empty()) {
            body.push_back('&');
        }
        body += UrlEncode(key);
        body.push_back('=');
        body += UrlEncode(value);
    }
    return body;
}

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

std::expected<std::vector<std::byte>, std::string> Base64Decode(std::string_view text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character)) == 0) {
            cleaned.push_back(character);
        }
    }
    if (cleaned.empty() || (cleaned.size() % 4) != 0) {
        return std::unexpected("kraken: base64 input is not a multiple of 4 characters");
    }

    std::vector<unsigned char> decoded((cleaned.size() / 4) * 3);
    const int written =
        EVP_DecodeBlock(decoded.data(), std::bit_cast<const unsigned char*>(cleaned.data()),
                        static_cast<int>(cleaned.size()));
    if (written < 0) {
        return std::unexpected("kraken: base64 input is not valid base64");
    }

    // EVP_DecodeBlock always rounds up to a multiple of 3; the '=' padding
    // tells us how many of those trailing bytes are filler.
    std::size_t length = static_cast<std::size_t>(written);
    for (auto character = cleaned.rbegin(); character != cleaned.rend() && *character == '=';
         ++character) {
        if (length == 0) {
            break;
        }
        --length;
    }

    std::vector<std::byte> result(length);
    for (std::size_t index = 0; index < length; ++index) {
        result[index] = static_cast<std::byte>(decoded[index]);
    }
    OPENSSL_cleanse(decoded.data(), decoded.size());
    return result;
}

std::expected<std::string, std::string> SignPrivateRequest(std::string_view url_path,
                                                           std::string_view nonce,
                                                           std::string_view post_data,
                                                           std::string_view api_secret_b64) {
    auto secret = Base64Decode(api_secret_b64);
    if (!secret) {
        return std::unexpected("kraken: API secret is not valid base64");
    }

    std::string nonce_and_body;
    nonce_and_body.reserve(nonce.size() + post_data.size());
    nonce_and_body += nonce;
    nonce_and_body += post_data;

    const auto body_digest = Sha256(BytesOf(nonce_and_body));
    if (!body_digest) {
        OPENSSL_cleanse(secret->data(), secret->size());
        return std::unexpected(body_digest.error());
    }

    std::vector<std::byte> message;
    message.reserve(url_path.size() + body_digest->size());
    const auto path_bytes = BytesOf(url_path);
    message.insert(message.end(), path_bytes.begin(), path_bytes.end());
    message.insert(message.end(), body_digest->begin(), body_digest->end());

    const auto signature = HmacSha512(*secret, message);
    OPENSSL_cleanse(secret->data(), secret->size());
    if (!signature) {
        return std::unexpected(signature.error());
    }
    return Base64Encode(*signature);
}

std::uint64_t NonceGenerator::Next() {
    constexpr std::uint64_t kNanosPerMicro = 1000;
    return NextFrom(RealtimeNowNs() / kNanosPerMicro);
}

std::uint64_t NonceGenerator::NextFrom(std::uint64_t now_micros) {
    last_ = std::max(now_micros, last_ + 1);
    return last_;
}

void NonceGenerator::SeedAtLeast(std::uint64_t value) {
    last_ = std::max(last_, value);
}

PersistentNonceSource::PersistentNonceSource(std::filesystem::path state_file)
    : state_file_(std::move(state_file)) {
    if (state_file_.empty()) {
        return;
    }

    std::string reason;
    const std::optional<std::uint64_t> persisted = ReadHighWaterMark(state_file_, reason);
    if (!persisted) {
        if (!reason.empty()) {
            LogWarn("kraken: ignoring nonce state file " + state_file_.string() + " (" + reason +
                    "); falling back to the wall clock");
        }
        return;
    }

    seeded_from_ = *persisted;
    // Seeding the mark rather than the next value keeps the max(now, last + 1)
    // rule as the single place progress is decided: the next nonce comes out
    // as max(persisted + 1, now_micros).
    generator_.SeedAtLeast(*persisted);
    LogInfo("kraken: nonce high-water mark " + std::to_string(*persisted) + " restored from " +
            state_file_.string());
}

std::uint64_t PersistentNonceSource::Next() {
    const std::uint64_t value = generator_.Next();
    Persist(value);
    return value;
}

std::uint64_t PersistentNonceSource::NextFrom(std::uint64_t now_micros) {
    const std::uint64_t value = generator_.NextFrom(now_micros);
    Persist(value);
    return value;
}

void PersistentNonceSource::Persist(std::uint64_t value) {
    if (state_file_.empty()) {
        return;
    }
    // At most one write per signed REST call, i.e. one per (re)connect, so
    // there is nothing to gain from batching it -- and a batched mark is
    // exactly the mark that would be stale after a crash.
    if (WriteHighWaterMark(state_file_, value)) {
        write_failed_ = false;
        return;
    }
    if (!write_failed_) {
        write_failed_ = true;
        LogWarn("kraken: cannot persist the nonce high-water mark to " + state_file_.string() +
                "; continuing with the in-memory wall-clock nonce only");
    }
}

}  // namespace feed_handler::kraken
