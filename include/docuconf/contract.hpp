// Contract-first mode (SPEC §11.2 item 11): validate an environment against
// a contract given as JSON (`cue export contract.cue`), with no C++
// declaration, and get typed values back.
//
//     auto contract = docuconf::Contract::from_json(text);
//     docuconf::Values values = contract.load({{"PORT", "9090"}});
//     std::int64_t port = values.get("PORT")->as_int();
//
// Every encoding of SPEC §5 is parsed (csv, json and indexed lists and key
// sets; go, iso8601, seconds and timespan durations), and every value is
// checked by the same code that checks a CLI11 declaration. File inputs
// (SPEC §4.6) are read from under DOCUCONF_FILE_ROOT and checked as the
// declaration API checks them. Values are layered as a host with config
// files layers them (SPEC §4.4, §4.7): the variable's default, then the
// selected profile's default, then a config-file overlay, then the
// environment. The environment-only build (DOCUCONF_FILE_INPUTS=OFF)
// rejects a contract with file inputs or overlays.
#pragma once

#include <functional>
#include <map>
#include <ostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "errors.hpp"
#include "keyset.hpp"
#include "spec.hpp"
#include "watched.hpp"

namespace docuconf {

/// A file input loaded in contract-first mode, after its boot checks.
struct FileValue {
    FileType type = FileType::Binary;
    std::string path;          // after DOCUCONF_FILE_ROOT
    std::string content;       // the bytes; for tls, tls.crt
    std::string key;           // tls: tls.key
    std::string ca;            // tls: ca.crt, when present
    nlohmann::json document;   // config: the parsed data
    std::size_t certificates = 0;  // caBundle
};

/// The typed value of every variable and file input in a contract: nullopt
/// for an optional input that is unset and has no default.
class Values {
public:
    Values() = default;
    Values(std::map<std::string, std::optional<Value>> values, std::vector<std::string> secrets,
           std::map<std::string, std::optional<FileValue>> files = {})
        : values_(std::move(values)), secrets_(std::move(secrets)), files_(std::move(files)) {}

    /// The value of a variable, nullptr when it is unset or not declared.
    const Value* get(const std::string& name) const;
    /// A keySet variable's keys, nullopt when it is unset or not a key set.
    std::optional<KeySet> key_set(const std::string& name) const;
    /// A file input, nullptr when it is absent or not declared.
    const FileValue* file(const std::string& name) const;
    /// Every declared file input, sorted by name.
    const std::map<std::string, std::optional<FileValue>>& files() const { return files_; }
    /// Every declared variable and its value, sorted by name.
    const std::map<std::string, std::optional<Value>>& all() const { return values_; }
    /// Whether the contract marks a variable `secret`.
    bool is_secret(const std::string& name) const;
    /// Every variable and file input as one JSON object, null for unset
    /// ones: a key set as its keys, a config file as its data, a text file
    /// as its text and any other file input as true. It holds secret
    /// values: do not log it.
    nlohmann::json to_json() const;
    /// Like to_json, with every secret replaced by "***".
    nlohmann::json to_redacted_json() const;

private:
    std::map<std::string, std::optional<Value>> values_;
    std::vector<std::string> secrets_;
    std::map<std::string, std::optional<FileValue>> files_;
};

/// Prints to_redacted_json(): secrets show as "***".
std::ostream& operator<<(std::ostream& os, const Values& values);

class Contract {
public:
    /// Reads a contract from its JSON form. Throws DeclarationError when the
    /// contract is not a valid v1alpha1 ConfigContract.
    static Contract from_json(const std::string& json);
    static Contract from_json(const char* json);
    static Contract from_json(const nlohmann::json& json);

    /// Validates `env`, as the whole environment: file inputs and overlays
    /// are read from under its DOCUCONF_FILE_ROOT. Throws ValidationError
    /// with every violation. Nothing is written to the termination log.
    Values load(const Env& env) const;

    /// Validates the process environment. On failure the violations are
    /// also written to the termination log. A variable that is set but not
    /// declared and is close to a declared name gets a warning on standard
    /// error.
    Values load_process_env() const;

    /// Like load(env), for a contract with inputs declared `reload: watch`
    /// (file inputs and overlays, SPEC §4.6.2, §4.7): the result's
    /// current() is reloaded, through the same checks, when a watched input's
    /// files change. Inputs declared `restart` and the environment keep
    /// their boot values. A reload that fails its checks keeps the previous
    /// Values and reports the violations to on_warning. Throws
    /// ValidationError when the first load fails.
    Watched<Values> watch(const Env& env) const;

    /// Where warnings go: a deprecated input that is set, a variable set
    /// both in the environment and in an overlay, a watched input whose
    /// change failed its checks. They name the input,
    /// never its value. Default: standard error.
    Contract& on_warning(std::function<void(const std::string&)> sink);

    const std::string& name() const { return name_; }
    const std::vector<VarSpec>& vars() const { return vars_; }
    const std::vector<FileSpec>& files() const { return files_; }
    const std::vector<Overlay>& overlays() const { return overlays_; }
    const std::optional<Profiles>& profiles() const { return profiles_; }

private:
    std::string name_;
    std::vector<VarSpec> vars_;
    std::vector<FileSpec> files_;
    std::vector<Overlay> overlays_;
    std::optional<Profiles> profiles_;
    std::function<void(const std::string&)> warn_;
};

}  // namespace docuconf
