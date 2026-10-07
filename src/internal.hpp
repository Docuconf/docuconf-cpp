// Internal API shared by the declaration path, contract-first mode, export
// and the file checks. Not installed.
#pragma once

#ifndef DOCUCONF_FILE_INPUTS
#define DOCUCONF_FILE_INPUTS 1
#endif

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "docuconf/errors.hpp"
#include "docuconf/spec.hpp"

namespace docuconf {
namespace detail {

struct Pattern {
    std::string source;
    re2::RE2 re;
    explicit Pattern(const std::string& p);
    /// RE2 partial match: the pattern matches anywhere in the value.
    bool matches(const std::string& s) const;
};

/// Compiles an RE2 pattern; returns the error for one that RE2 rejects
/// (lookaround, backreferences and other non-RE2 syntax).
std::shared_ptr<const Pattern> compile_pattern(const std::string& p, std::string& error);

bool is_env_name(const std::string& s);
bool is_input_name(const std::string& s);
bool valid_utf8(const std::string& s);
std::size_t rune_count(const std::string& s);
/// JSON-quoted, for messages.
std::string quote(const std::string& s);
/// Shortest round-trip decimal, locale-independent.
std::string format_float(double f);

using Problem = std::pair<Code, std::string>;

/// Parses the raw environment string of a variable in its wire encoding
/// (SPEC §5). For an indexed list, `raws` holds one string per item;
/// otherwise exactly one.
std::optional<Value> parse_wire(const VarSpec& spec, const std::vector<std::string>& raws, Problem& err);

/// Checks a typed value against the variable's constraints. Each message
/// starts with the value, or "value" for a secret.
std::vector<Problem> check_value(const VarSpec& spec, const Value& v);

/// Converts a typed JSON value (a default in a contract, a profile
/// default) to a Value of the variable's type.
std::optional<Value> value_from_json(const VarSpec& spec, const nlohmann::json& j, std::string& err);

/// Validates a variable declaration (SPEC §11.2 item 2) and compiles its
/// pattern. Returns one message per problem, each prefixed with the name.
std::vector<std::string> validate_var(VarSpec& spec);

/// Validates file inputs against each other and the variables (mount rules,
/// pathEnv, passwordVar). Compiles text patterns.
std::vector<std::string> validate_files(std::vector<FileSpec>& files, const std::vector<VarSpec>& vars);

/// Loads every variable: reads and parses the environment, applies the
/// selected profile's defaults and then the declared defaults, and checks
/// constraints. Returns the values (nullopt for an unset optional) and
/// appends violations in variable order.
std::map<std::string, std::optional<Value>> load_vars(const std::vector<VarSpec>& vars, const Env& env,
                                                      const Profiles* profiles,
                                                      std::vector<Violation>& violations,
                                                      std::vector<std::string>* warnings);

/// The loaded content of one file input, for binding.
struct LoadedFile {
    bool present = false;
    std::string path;              // resolved path (after DOCUCONF_FILE_ROOT)
    std::string content;           // file bytes; for tls, tls.crt
    std::string key;               // tls.key
    std::string ca;                // ca.crt
    nlohmann::json document;       // config files, parsed
    std::size_t certificates = 0;  // caBundle
};

/// Checks every file input at boot (SPEC §11.2 item 7). `vars` are the
/// loaded variable values, for keystore passwords; `env` holds pathEnv
/// values; `file_root` is DOCUCONF_FILE_ROOT.
std::map<std::string, LoadedFile> load_files(const std::vector<FileSpec>& files, const Env& env,
                                             const std::map<std::string, std::optional<Value>>& vars,
                                             const std::string& file_root, std::vector<Violation>& violations);

/// TLS key pair checks: parse, key match, validity and minRemaining, DNS
/// names, key algorithm, chain to ca.crt. `ca` is null without requireCA.
std::vector<Problem> check_tls(const FileSpec& spec, const std::string& crt, const std::string& key,
                               const std::string* ca);
/// Counts the PEM certificates of a CA bundle.
std::vector<Problem> check_ca_bundle(const FileSpec& spec, const std::string& pem, std::size_t& count);
/// Opens a PKCS#12 keystore, or verifies a JKS keystore's integrity MAC.
std::vector<Problem> check_keystore(const FileSpec& spec, const std::string& data, const std::string& password);

/// JSON Schema validation messages (empty when valid). Values are left out
/// of the messages when `secret` is set.
std::vector<std::string> validate_schema(const nlohmann::json& schema, const nlohmann::json& doc, bool secret);

/// Writes violations to DOCUCONF_TERMINATION_LOG, or /dev/termination-log
/// when it exists.
void write_termination_log(const std::vector<Violation>& violations);

/// "docuconf: N configuration problems:\n  ..." for a ValidationError.
std::string format_violations(const std::vector<Violation>& violations);

/// Reads the process environment.
Env process_env();

}  // namespace detail
}  // namespace docuconf
