// The payment webhook key set, WEBHOOK_KEYS: its declaration, shared by the
// service and its test, and the signature check.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <docuconf/docuconf.hpp>

namespace orders {

/// Declares WEBHOOK_KEYS, bound to `keys`: a key set (always secret) of
/// one or two keys, each 32 to 256 characters. Unset, `keys` stays empty.
inline void declare_webhook_keys(docuconf::Declaration& config, std::optional<docuconf::KeySet>& keys) {
    config.add_var("WEBHOOK_KEYS", keys)
        .doc(R"(
            /// Keys that verify the signature on incoming payment webhooks.
            ///
            /// A webhook is accepted when it is signed with any key in the set, so the
            /// key can be rotated without turning webhooks away. Each key is 32 to 256
            /// characters, so an empty or truncated key fails at boot. Without this
            /// variable, the service rejects every webhook.
        )")
        .key_length(32, 256);
}

/// The HMAC-SHA256 of `body` under `key`, as raw bytes.
inline std::string hmac_sha256(const std::string& key, const std::string& body) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(body.data()),
         body.size(), mac, &len);
    return std::string(reinterpret_cast<const char*>(mac), len);
}

/// Decodes lower- or upper-case hex; nullopt when `hex` is not hex.
inline std::optional<std::string> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>(hi * 16 + lo));
    }
    return out;
}

/// Whether `signature`, the hex-encoded HMAC-SHA256 of `body`, was made
/// with any of `keys`. Accepting every key in the set is what lets a key be
/// rotated: during the overlap the old and the new key both work.
inline bool verify(const docuconf::KeySet& keys, const std::string& body, const std::string& signature) {
    auto got = from_hex(signature);
    if (!got) return false;
    // KeySet::verify tries every key, even after one matches, and the
    // comparison is constant time, so the time taken does not say which
    // key matched.
    return keys.verify([&](std::string_view key) {
        std::string want = hmac_sha256(std::string(key), body);
        return got->size() == want.size() && CRYPTO_memcmp(got->data(), want.data(), want.size()) == 0;
    });
}

}  // namespace orders
