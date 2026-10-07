// Contract-first mode (SPEC §11.2 item 11): validate an environment against
// a contract given as JSON (`cue export contract.cue`), with no C++
// declaration, and get typed values back.
//
//     auto contract = docuconf::Contract::from_json(text);
//     docuconf::Values values = contract.load({{"PORT", "9090"}});
//     std::int64_t port = values.get("PORT")->as_int();
//
// Every encoding of SPEC §5 is parsed (csv, json and indexed lists; go,
// iso8601, seconds and timespan durations), and every value is checked by
// the same code that checks a CLI11 declaration. Only variables are
// loaded: file inputs and overlays in the contract are ignored. Profiles
// are honoured.
#pragma once

#include <map>
#include <ostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "errors.hpp"
#include "spec.hpp"

namespace docuconf {

/// The typed value of every variable in a contract: nullopt for an
/// optional variable that is unset and has no default.
class Values {
public:
    Values() = default;
    Values(std::map<std::string, std::optional<Value>> values, std::vector<std::string> secrets)
        : values_(std::move(values)), secrets_(std::move(secrets)) {}

    /// The value of a variable, nullptr when it is unset or not declared.
    const Value* get(const std::string& name) const;
    /// Every declared variable and its value, sorted by name.
    const std::map<std::string, std::optional<Value>>& all() const { return values_; }
    /// Whether the contract marks a variable `secret`.
    bool is_secret(const std::string& name) const;
    /// Every variable as one JSON object, null for unset ones. It holds
    /// secret values: do not log it.
    nlohmann::json to_json() const;
    /// Like to_json, with every secret replaced by "***".
    nlohmann::json to_redacted_json() const;

private:
    std::map<std::string, std::optional<Value>> values_;
    std::vector<std::string> secrets_;
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

    /// Validates `env`, as the whole environment. Throws ValidationError
    /// with every violation. Nothing is written to the termination log.
    Values load(const Env& env) const;

    /// Validates the process environment. On failure the violations are
    /// also written to the termination log. A variable that is set but not
    /// declared and is close to a declared name gets a warning on standard
    /// error.
    Values load_process_env() const;

    const std::string& name() const { return name_; }
    const std::vector<VarSpec>& vars() const { return vars_; }

private:
    std::string name_;
    std::vector<VarSpec> vars_;
    std::optional<Profiles> profiles_;
};

}  // namespace docuconf
