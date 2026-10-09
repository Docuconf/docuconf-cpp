// KeySet: a set of secret keys that are all valid at once, so a key can be
// rotated without an outage (contract type `keySet`, SPEC §4.3, §6.1).
#pragma once

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace docuconf {

/// The keys of a `keySet` variable, for the side that verifies: webhook
/// signatures, inbound API keys, JWT HMAC verification, cookie-signing
/// fallbacks.
///
///     docuconf::KeySet webhook_keys;
///     config.add_var("WEBHOOK_KEYS", webhook_keys, "Keys that verify webhook signatures")
///         .key_length(32, 256);
///
/// A key set is always secret. The platform supplies it like a secret list,
/// as one Secret key holding `old,new` during a rotation. Keys are never
/// trimmed, and an empty key (a stray separator) is always out of range.
/// A KeySet prints as `***` through operator<<, to_string() and
/// nlohmann::json; read the keys with keys(), or better, check a candidate
/// with contains() or verify().
class KeySet {
public:
    KeySet() = default;
    explicit KeySet(std::vector<std::string> keys) : keys_(std::move(keys)) {}

    /// The keys, in the order the platform gave them.
    const std::vector<std::string>& keys() const noexcept { return keys_; }
    std::size_t size() const noexcept { return keys_.size(); }
    bool empty() const noexcept { return keys_.empty(); }

    /// Whether `candidate` is one of the keys, such as an API key a caller
    /// presents. Every key is compared in constant time, so the time taken
    /// does not say which key matched, or how much of one; it depends only
    /// on the number of keys and their lengths.
    bool contains(std::string_view candidate) const noexcept {
        unsigned found = 0;
        for (const auto& key : keys_) found |= equal(key, candidate);
        return found != 0;
    }

    /// Calls `check(key)` with every key, as a std::string_view, and returns
    /// whether any call returned true. It is for checks that need the key
    /// itself, such as an HMAC:
    ///
    ///     bool ok = keys.verify([&](std::string_view key) {
    ///         return constant_time_equal(hmac_sha256(key, body), signature);
    ///     });
    ///
    /// Every key is tried, even after one matches, so the time taken does
    /// not say which key matched. `check` should compare in constant time
    /// itself (CRYPTO_memcmp in OpenSSL).
    template <class Check>
    bool verify(Check&& check) const {
        bool ok = false;
        for (const auto& key : keys_) {
            if (check(std::string_view(key))) ok = true;
        }
        return ok;
    }

    bool operator==(const KeySet& o) const { return keys_ == o.keys_; }
    bool operator!=(const KeySet& o) const { return !(*this == o); }

private:
    std::vector<std::string> keys_;

    // 1 when a and b are equal, compared in time that depends only on their
    // lengths (like Go's subtle.ConstantTimeCompare).
    static unsigned equal(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return 0;
        volatile unsigned char diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
        return diff == 0 ? 1u : 0u;
    }
};

/// "***": a key set never prints its keys.
inline std::string to_string(const KeySet&) { return "***"; }

/// Writes "***".
inline std::ostream& operator<<(std::ostream& os, const KeySet&) { return os << "***"; }

/// Serializes as "***", so a struct holding a KeySet is safe to log as JSON.
inline void to_json(nlohmann::json& j, const KeySet&) { j = "***"; }

}  // namespace docuconf
