#include <cstdlib>
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

std::string Violation::str() const { return input + ": " + message + " (" + to_string(code) + ")"; }

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

void write_termination_log(const std::vector<Violation>& violations) {
    std::string path;
    if (const char* p = std::getenv("DOCUCONF_TERMINATION_LOG"); p && *p) {
        path = p;
    } else {
        struct stat st {};
        if (::stat("/dev/termination-log", &st) != 0) return;
        path = "/dev/termination-log";
    }
    std::ofstream f(path, std::ios::trunc);
    if (f) f << format_violations(violations) << "\n";
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
