// Errors: declaration mistakes and boot-time violations.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace docuconf {

/// A stable, machine-readable violation code (SPEC §11.2 item 5). Every
/// docuconf SDK reports the same codes.
enum class Code {
    MissingRequired,
    InvalidType,
    OutOfRange,
    PatternMismatch,
    NotInEnum,
    InvalidScheme,
    TooFewItems,
    TooManyItems,
    FileMissing,
    FileUnreadable,
    FileTooLarge,
    FileMalformed,
    SchemaMismatch,
    CertificateInvalid,
    CertificateExpiring,
    CertificateNameMismatch,
    KeyMismatch,
    KeystoreUnreadable,
};

/// The code as the spec writes it, such as `missing_required`.
const char* to_string(Code code) noexcept;

/// One problem found while loading configuration. `message` never contains
/// the value of a secret variable or the contents of a secret file.
struct Violation {
    /// The environment variable name or the file input name.
    std::string input;
    Code code;
    /// A human-readable explanation, without the input name.
    std::string message;
    /// Where the value came from when it was not the environment: the
    /// command-line flag, such as `--port`. Empty otherwise.
    std::string source = {};

    /// `INPUT: message (code)`, or `INPUT (--flag): message (code)`.
    std::string str() const;
};

/// Every violation found at boot, reported together.
class ValidationError : public std::runtime_error {
public:
    explicit ValidationError(std::vector<Violation> violations);

    const std::vector<Violation>& violations() const noexcept { return violations_; }
    /// Whether any violation has the given code.
    bool has(Code code) const;
    /// The codes reported for one input, in order.
    std::vector<Code> codes_for(const std::string& input) const;

private:
    std::vector<Violation> violations_;
};

/// Mistakes in the declaration itself: an invalid name, a short
/// description, a default that breaks its own constraints, a pattern that
/// is not RE2, a file mounted over a reserved directory. These are
/// programming errors, found before any value is read.
class DeclarationError : public std::logic_error {
public:
    explicit DeclarationError(std::vector<std::string> problems);

    const std::vector<std::string>& problems() const noexcept { return problems_; }

private:
    std::vector<std::string> problems_;
};

/// The contract could not be written (`--docuconf-export` to a path that
/// cannot be created). The message is one line naming the path and why.
class ExportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace docuconf
