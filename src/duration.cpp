#include "docuconf/duration.hpp"

#include <cstdint>
#include <limits>
#include <numeric>

namespace docuconf {
namespace {

using u64 = std::uint64_t;
constexpr u64 kSec = 1000000000ULL;
constexpr u64 kMax = static_cast<u64>(std::numeric_limits<std::int64_t>::max());

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Reads digits from s[i..]; returns false when there are none.
bool digits(const std::string& s, std::size_t& i, std::string& out) {
    std::size_t start = i;
    while (i < s.size() && is_digit(s[i])) ++i;
    out = s.substr(start, i - start);
    return !out.empty();
}

// whole units plus a decimal fraction of a unit, in nanoseconds; nullopt on
// overflow or on a fraction finer than a nanosecond.
std::optional<u64> nanos(const std::string& whole, const std::string& frac, u64 unit, bool exact) {
    u64 n = 0;
    for (char c : whole) {
        u64 d = static_cast<u64>(c - '0');
        if (n > (kMax - d) / 10) return std::nullopt;
        n = n * 10 + d;
    }
    if (unit != 0 && n > kMax / unit) return std::nullopt;
    n *= unit;
    if (!frac.empty()) {
        // fraction * unit / 10^len, exactly when exact is set
        u64 scale = 1;
        u64 part = 0;
        for (char c : frac) {
            if (scale > kMax / 10) {
                if (exact && c != '0') return std::nullopt;
                continue;  // Go drops digits beyond its precision
            }
            scale *= 10;
            part = part * 10 + static_cast<u64>(c - '0');
        }
        // part * unit / scale without overflow: unit is at most 3600e9.
        long double v = static_cast<long double>(part) * static_cast<long double>(unit) /
                        static_cast<long double>(scale);
        u64 add = static_cast<u64>(v);
        if (exact) {
            // part * unit / scale, in whole nanoseconds only.
            u64 g = std::gcd(unit, scale);
            u64 u = unit / g, sc = scale / g;
            if (part % sc != 0) return std::nullopt;
            u64 q = part / sc;
            if (u != 0 && q > kMax / u) return std::nullopt;
            add = q * u;
        }
        if (n > kMax - add) return std::nullopt;
        n += add;
    }
    return n;
}

std::optional<Duration> from_nanos(std::optional<u64> n) {
    if (!n || *n > kMax) return std::nullopt;
    return Duration(static_cast<std::int64_t>(*n));
}

std::optional<Duration> parse_iso8601(const std::string& s) {
    // P[nD][T[nH][nM][n[.f]S]]
    if (s.size() < 2 || s[0] != 'P') return std::nullopt;
    std::size_t i = 1;
    u64 total = 0;
    bool any = false;
    auto add = [&](std::optional<u64> n) {
        if (!n || total > kMax - *n) return false;
        total += *n;
        any = true;
        return true;
    };
    std::string num;
    if (i < s.size() && is_digit(s[i])) {
        if (!digits(s, i, num) || i >= s.size() || s[i] != 'D') return std::nullopt;
        ++i;
        if (!add(nanos(num, "", 86400 * kSec, true))) return std::nullopt;
    }
    if (i < s.size()) {
        if (s[i] != 'T') return std::nullopt;
        ++i;
        if (i >= s.size()) return std::nullopt;  // a T with nothing after it
        int stage = 0;                           // H, M, S in order
        while (i < s.size()) {
            std::string frac;
            if (!digits(s, i, num)) return std::nullopt;
            if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
                ++i;
                if (!digits(s, i, frac)) return std::nullopt;
            }
            if (i >= s.size()) return std::nullopt;
            char u = s[i++];
            if (u == 'H' && stage < 1 && frac.empty()) {
                stage = 1;
                if (!add(nanos(num, "", 3600 * kSec, true))) return std::nullopt;
            } else if (u == 'M' && stage < 2 && frac.empty()) {
                stage = 2;
                if (!add(nanos(num, "", 60 * kSec, true))) return std::nullopt;
            } else if (u == 'S' && stage < 3) {
                stage = 3;
                if (!add(nanos(num, frac, kSec, true))) return std::nullopt;
            } else {
                return std::nullopt;
            }
        }
    }
    if (!any) return std::nullopt;
    return from_nanos(total);
}

std::optional<Duration> parse_seconds(const std::string& s) {
    std::size_t i = 0;
    std::string whole, frac;
    if (!digits(s, i, whole)) return std::nullopt;
    if (i < s.size()) {
        if (s[i] != '.') return std::nullopt;
        ++i;
        if (!digits(s, i, frac) || i != s.size()) return std::nullopt;
    }
    return from_nanos(nanos(whole, frac, kSec, true));
}

std::optional<Duration> parse_timespan(const std::string& s) {
    // [d.]hh:mm:ss[.fffffff]
    std::size_t i = 0;
    std::string a, h, m, sec, frac;
    if (!digits(s, i, a)) return std::nullopt;
    std::string days;
    if (i < s.size() && s[i] == '.') {
        days = a;
        ++i;
        if (!digits(s, i, h)) return std::nullopt;
    } else {
        h = a;
    }
    if (h.size() > 2 || i >= s.size() || s[i] != ':') return std::nullopt;
    ++i;
    if (!digits(s, i, m) || m.size() != 2 || i >= s.size() || s[i] != ':') return std::nullopt;
    ++i;
    if (!digits(s, i, sec) || sec.size() != 2) return std::nullopt;
    if (i < s.size()) {
        if (s[i] != '.') return std::nullopt;
        ++i;
        if (!digits(s, i, frac) || frac.size() > 7 || i != s.size()) return std::nullopt;
    }
    if (std::stoi(h) > 23 || std::stoi(m) > 59 || std::stoi(sec) > 59) return std::nullopt;
    u64 total = 0;
    for (auto part : {nanos(days, "", 86400 * kSec, true), nanos(h, "", 3600 * kSec, true),
                      nanos(m, "", 60 * kSec, true), nanos(sec, frac, kSec, true)}) {
        if (!part || total > kMax - *part) return std::nullopt;
        total += *part;
    }
    return from_nanos(total);
}

}  // namespace

std::optional<Duration> parse_go_duration(const std::string& s) {
    // Go: [-+]? ([0-9]*(\.[0-9]*)?[a-z]+)+, or "0".
    if (s.empty()) return std::nullopt;
    if (s == "0") return Duration(0);
    std::size_t i = 0;
    if (s[0] == '+') ++i;
    if (i < s.size() && s[i] == '-') return std::nullopt;  // negative durations are not allowed
    if (i >= s.size()) return std::nullopt;
    u64 total = 0;
    while (i < s.size()) {
        std::string whole, frac;
        bool pre = digits(s, i, whole);
        bool post = false;
        if (i < s.size() && s[i] == '.') {
            ++i;
            post = digits(s, i, frac);
        }
        if (!pre && !post) return std::nullopt;
        std::size_t us = i;
        while (i < s.size() && !is_digit(s[i]) && s[i] != '.') ++i;
        std::string unit = s.substr(us, i - us);
        u64 mult = 0;
        if (unit == "ns") mult = 1;
        else if (unit == "us" || unit == "\xC2\xB5s" || unit == "\xCE\xBCs") mult = 1000;
        else if (unit == "ms") mult = 1000000;
        else if (unit == "s") mult = kSec;
        else if (unit == "m") mult = 60 * kSec;
        else if (unit == "h") mult = 3600 * kSec;
        else return std::nullopt;
        auto n = nanos(whole, frac, mult, false);
        if (!n || total > kMax - *n) return std::nullopt;
        total += *n;
    }
    return from_nanos(total);
}

std::optional<Duration> parse_duration(DurationEncoding encoding, const std::string& s) {
    switch (encoding) {
        case DurationEncoding::Go: return parse_go_duration(s);
        case DurationEncoding::Iso8601: return parse_iso8601(s);
        case DurationEncoding::Seconds: return parse_seconds(s);
        case DurationEncoding::Timespan: return parse_timespan(s);
    }
    return std::nullopt;
}

std::string format_go_duration(Duration d) {
    std::int64_t n = d.count();
    if (n == 0) return "0s";
    std::string out;
    if (n < 0) {
        out = "-";
        n = -n;
    }
    static const std::pair<std::int64_t, const char*> units[] = {
        {3600000000000LL, "h"}, {60000000000LL, "m"}, {1000000000LL, "s"},
        {1000000LL, "ms"},      {1000LL, "us"},       {1LL, "ns"}};
    for (const auto& [unit, name] : units) {
        std::int64_t q = n / unit;
        if (q > 0) {
            out += std::to_string(q) + name;
            n -= q * unit;
        }
    }
    return out;
}

}  // namespace docuconf
