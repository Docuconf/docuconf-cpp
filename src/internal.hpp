// Internal API shared by the declaration path, contract-first mode, export
// and the file checks. Not installed.
#pragma once

#ifndef DOCUCONF_FILE_INPUTS
#define DOCUCONF_FILE_INPUTS 1
#endif

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "docuconf/doc.hpp"
#include "docuconf/errors.hpp"
#include "docuconf/spec.hpp"
#include "docuconf/watched.hpp"

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
/// Checks an input's details (SPEC §4.2): not blank and at most
/// kMaxDetails characters. Returns the problem, or an empty string.
std::string check_details(const std::optional<std::string>& details);
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
// raw is the wire string a value was parsed from, when there is one: a json
// value's maxLength measures it.
std::vector<Problem> check_value(const VarSpec& spec, const Value& v, const std::string* raw = nullptr);

/// Converts a typed JSON value (a default in a contract, a profile
/// default) to a Value of the variable's type.
std::optional<Value> value_from_json(const VarSpec& spec, const nlohmann::json& j, std::string& err);

/// Validates a variable declaration (SPEC §11.2 item 2) and compiles its
/// pattern. Returns one message per problem, each prefixed with the name.
std::vector<std::string> validate_var(VarSpec& spec);

/// Validates file inputs against each other and the variables (mount rules,
/// pathEnv, passwordVar). Compiles text patterns.
std::vector<std::string> validate_files(std::vector<FileSpec>& files, const std::vector<VarSpec>& vars);

/// The longest `deprecated` message, in characters (SPEC §4.2).
inline constexpr std::size_t kMaxDeprecated = 500;
/// Checks a `deprecated` message: not blank, at most kMaxDeprecated
/// characters. Returns the problem, or an empty string.
std::string check_deprecated(const std::string& message);

/// A variable's value from a config-file overlay (SPEC §4.7), converted to
/// the wire strings it stands for and checked like an environment value.
struct Layer {
    std::string source;             // "overlay <name>"
    std::vector<std::string> raws;  // one wire string, or the items of a list
    bool items = false;             // raws are list items
    bool bad = false;               // the value was already reported
};

/// The profile in effect (SPEC §4.4): the selector's value when the
/// environment sets it, or profiles.default.
std::string selected_profile(const std::vector<VarSpec>& vars, const Profiles& profiles, const Env& env);

/// Loads every variable: reads and parses the environment, then the
/// overlays' values, the selected profile's defaults and the declared
/// defaults, in that order of precedence, and checks constraints. Returns
/// the values (nullopt for an unset optional) and appends violations in
/// variable order. Warnings (a deprecated variable that is set, a variable
/// set both in the environment and an overlay) never hold a value.
std::map<std::string, std::optional<Value>> load_vars(const std::vector<VarSpec>& vars, const Env& env,
                                                      const Profiles* profiles,
                                                      std::vector<Violation>& violations,
                                                      std::vector<std::string>* warnings,
                                                      const std::map<std::string, Layer>* overlays = nullptr);

/// Parses a structured file (`json`, `yaml` or `toml`) into JSON. Throws
/// std::exception with the parser's message when it does not parse. Only
/// json is available in the environment-only build.
nlohmann::json parse_structured(const std::string& format, const std::string& text);

/// Reads each overlay (SPEC §4.7) from under `file_root` and returns the
/// value it holds for each variable with a configKey, converted to wire
/// strings. A missing overlay is skipped; one that does not parse, or is
/// not an object, is file_malformed for the overlay. A bad value is
/// invalid_type for the variable. A secret is never taken from an overlay.
/// With `docs`, an overlay found there is not read again: its data (nullopt
/// for a missing overlay) is used as it is. Every overlay that was read
/// without a violation is added to it.
std::map<std::string, Layer> load_overlays(const std::vector<Overlay>& overlays, const std::vector<VarSpec>& vars,
                                           const std::string& selector, const std::string& file_root,
                                           std::vector<Violation>& violations, std::vector<std::string>& warnings,
                                           std::map<std::string, std::optional<nlohmann::json>>* docs = nullptr);

/// `path` under DOCUCONF_FILE_ROOT, when one is set and the path is absolute.
std::string under_root(const std::string& file_root, const std::string& path);

/// Where a file input is read: its pathEnv's value when set, else its path,
/// under DOCUCONF_FILE_ROOT.
std::string resolved_path(const FileSpec& f, const Env& env, const std::string& file_root);

/// The files whose changes a watched input reloads on: the file, or a TLS
/// directory's tls.crt, tls.key and ca.crt.
std::vector<std::string> watch_paths(const FileSpec& f, const std::string& resolved);

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

/// Writes violations to DOCUCONF_TERMINATION_LOG as `env` sets it, or, with
/// `device`, to /dev/termination-log when it exists.
void write_termination_log(const std::vector<Violation>& violations, const Env& env, bool device);

/// One warning per variable set in `env` that is not declared but is within
/// two edits of a declared name (one for names shorter than 6). Values are
/// never included.
std::vector<std::string> undeclared_hints(const Env& env, const std::set<std::string>& declared);

/// "docuconf: N configuration problems:\n  ..." for a ValidationError.
std::string format_violations(const std::vector<Violation>& violations);

/// Reads the process environment.
Env process_env();

}  // namespace detail
}  // namespace docuconf
