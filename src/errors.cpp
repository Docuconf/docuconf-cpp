#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <fstream>
#include <sys/stat.h>

#include "internal.hpp"

extern char** environ;

namespace docuconf {

const char* to_string(Code code) noexcept {
    switch (code) {
        case Code::MissingRequired: return "missing_required";
        case Code::InvalidType: return "invalid_type";
        case Code::OutOfRange: return "out_of_range";
        case Code::PatternMismatch: return "pattern_mismatch";
        case Code::NotInEnum: return "not_in_enum";
        case Code::InvalidScheme: return "invalid_scheme";
        case Code::TooFewItems: return "too_few_items";
        case Code::TooManyItems: return "too_many_items";
        case Code::FileMissing: return "file_missing";
        case Code::FileUnreadable: return "file_unreadable";
        case Code::FileTooLarge: return "file_too_large";
        case Code::FileMalformed: return "file_malformed";
        case Code::SchemaMismatch: return "schema_mismatch";
        case Code::CertificateInvalid: return "certificate_invalid";
        case Code::CertificateExpiring: return "certificate_expiring";
        case Code::CertificateNameMismatch: return "certificate_name_mismatch";
        case Code::KeyMismatch: return "key_mismatch";
        case Code::KeystoreUnreadable: return "keystore_unreadable";
    }
    return "unknown";
}

std::string Violation::str() const {
    return input + (source.empty() ? "" : " (" + source + ")") + ": " + message + " (" + to_string(code) + ")";
}

ValidationError::ValidationError(std::vector<Violation> violations)
    : std::runtime_error(detail::format_violations(violations)), violations_(std::move(violations)) {}

bool ValidationError::has(Code code) const {
    for (const auto& v : violations_)
        if (v.code == code) return true;
    return false;
}

std::vector<Code> ValidationError::codes_for(const std::string& input) const {
    std::vector<Code> out;
    for (const auto& v : violations_)
        if (v.input == input) out.push_back(v.code);
    return out;
}

namespace {
std::string declaration_message(const std::vector<std::string>& problems) {
    if (problems.size() == 1) return "docuconf: invalid declaration: " + problems[0];
    std::string out = "docuconf: invalid declaration:";
    for (const auto& p : problems) out += "\n  " + p;
    return out;
}
}  // namespace

DeclarationError::DeclarationError(std::vector<std::string> problems)
    : std::logic_error(declaration_message(problems)), problems_(std::move(problems)) {}

namespace detail {

std::string format_violations(const std::vector<Violation>& violations) {
    std::string out = "docuconf: " + std::to_string(violations.size()) + " configuration problem" +
                      (violations.size() == 1 ? "" : "s") + ":";
    for (const auto& v : violations) out += "\n  " + v.str();
    return out;
}

void write_termination_log(const std::vector<Violation>& violations, const Env& env, bool device) {
    std::string path;
    if (auto it = env.find("DOCUCONF_TERMINATION_LOG"); it != env.end() && !it->second.empty()) {
        path = it->second;
    } else {
        if (!device) return;
        struct stat st {};
        if (::stat("/dev/termination-log", &st) != 0) return;
        path = "/dev/termination-log";
    }
    std::ofstream f(path, std::ios::trunc);
    if (f) f << format_violations(violations) << "\n";
}

namespace {

// The optimal string alignment distance: insertions, deletions,
// substitutions and swaps of adjacent characters each cost 1.
std::size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<std::size_t> prev2(b.size() + 1), prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) cur[j] = std::min(cur[j], prev2[j - 2] + 1);
        }
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::vector<std::string> undeclared_hints(const Env& env, const std::set<std::string>& declared) {
    std::vector<std::string> out;
    if (declared.empty()) return out;
    for (const auto& [k, value] : env) {
        (void)value;  // never printed
        if (declared.count(k) || k.rfind("DOCUCONF_", 0) == 0) continue;
        if (auto i = k.rfind("__"); i != std::string::npos && i > 0 && declared.count(k.substr(0, i))) continue;
        std::string best;
        std::size_t best_dist = 3;
        for (const auto& name : declared) {
            std::size_t limit = name.size() < 6 ? 1 : 2;
            std::size_t d = edit_distance(upper(k), upper(name));
            if (d <= limit && d < best_dist) {
                best = name;
                best_dist = d;
            }
        }
        if (!best.empty()) out.push_back("docuconf: " + k + " is set but not declared; did you mean " + best + "?");
    }
    return out;
}

Env process_env() {
    Env env;
    for (char** e = environ; e && *e; ++e) {
        std::string kv(*e);
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        env.emplace(kv.substr(0, eq), kv.substr(eq + 1));
    }
    return env;
}

}  // namespace detail
}  // namespace docuconf
