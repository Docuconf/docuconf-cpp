// The payment webhook key set, WEBHOOK_KEYS: its declaration, shared by the
// service and its test, and the signature check.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <docuconf/docuconf.hpp>

namespace orders {

/// Declares WEBHOOK_KEYS, bound to `keys`: a secret csv list of one or two
/// keys, each 32 to 256 characters. Unset, `keys` stays empty.
inline void declare_webhook_keys(docuconf::Declaration& config, std::optional<std::vector<std::string>>& keys) {
    config.add_var("WEBHOOK_KEYS", keys)
        .doc(R"(
            /// Keys that verify the signature on incoming payment webhooks.
            ///
            /// A webhook is accepted when it is signed with any key in the list, so
            /// the key can be rotated without turning webhooks away. To rotate:
            ///
            ///  1. add the new key as the second item, and roll out;
            ///  2. switch the sender to the new key;
            ///  3. remove the old key, and roll out.
            ///
            /// Each key is 32 to 256 characters, so an empty or truncated key fails
            /// at boot. Without this variable, the service rejects every webhook.
        )")
        .secret()
        .min_items(1)
        .max_items(2)
        .item_min_length(32)
        .item_max_length(256);
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
inline bool verify(const std::vector<std::string>& keys, const std::string& body, const std::string& signature) {
    auto got = from_hex(signature);
    if (!got) return false;
    bool ok = false;
    for (const auto& key : keys) {
        std::string want = hmac_sha256(key, body);
        // Constant time, and every key is checked, so the time taken does
        // not say which one matched.
        ok |= got->size() == want.size() && CRYPTO_memcmp(got->data(), want.data(), want.size()) == 0;
    }
    return ok;
}

}  // namespace orders
