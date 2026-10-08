// The declaration model shared by the CLI11 declaration API and
// contract-first mode: one VarSpec per variable, one FileSpec per file
// input, and typed values.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace docuconf {

/// Durations are held in nanoseconds, as Go and the contract do.
using Duration = std::chrono::nanoseconds;

enum class VarType { String, Int, Float, Bool, Duration, Url, Enum, List, Json };
enum class ItemType { String, Int };
enum class ListEncoding { Csv, Json, Indexed };
enum class DurationEncoding { Go, Iso8601, Seconds, Timespan };

const char* to_string(VarType t) noexcept;
const char* to_string(ListEncoding e) noexcept;
const char* to_string(DurationEncoding e) noexcept;

/// A typed value of one of the contract types. `string`, `url` and `enum`
/// values are strings; list items are strings or ints.
class Value {
public:
    using List = std::vector<Value>;
    using Storage = std::variant<std::string, std::int64_t, double, bool, Duration, List, nlohmann::json>;

    Value() : v_(std::string()) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(const char* s) : v_(std::string(s)) {}
    template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T i) : v_(static_cast<std::int64_t>(i)) {}
    Value(double f) : v_(f) {}
    Value(bool b) : v_(b) {}
    Value(Duration d) : v_(d) {}
    Value(List l) : v_(std::move(l)) {}
    static Value json(nlohmann::json j) {
        Value v;
        v.v_ = std::move(j);
        return v;
    }

    bool is_string() const { return std::holds_alternative<std::string>(v_); }
    bool is_int() const { return std::holds_alternative<std::int64_t>(v_); }
    bool is_float() const { return std::holds_alternative<double>(v_); }
    bool is_bool() const { return std::holds_alternative<bool>(v_); }
    bool is_duration() const { return std::holds_alternative<Duration>(v_); }
    bool is_list() const { return std::holds_alternative<List>(v_); }
    bool is_json() const { return std::holds_alternative<nlohmann::json>(v_); }

    const std::string& as_string() const { return std::get<std::string>(v_); }
    std::int64_t as_int() const { return std::get<std::int64_t>(v_); }
    double as_float() const { return std::get<double>(v_); }
    bool as_bool() const { return std::get<bool>(v_); }
    Duration as_duration() const { return std::get<Duration>(v_); }
    const List& as_list() const { return std::get<List>(v_); }
    const nlohmann::json& as_json() const { return std::get<nlohmann::json>(v_); }

    /// The value as JSON: durations in canonical Go form (`1m30s`), lists
    /// as arrays.
    nlohmann::json to_json() const;

    bool operator==(const Value& o) const { return v_ == o.v_; }
    bool operator!=(const Value& o) const { return !(*this == o); }

private:
    Storage v_;
};

namespace detail {
struct Pattern;  // a compiled RE2 pattern
}

/// Checks that a `json` value or config file binds to the app's own type.
/// Returns an empty string when it does, or why it does not.
using BindCheck = std::function<std::string(const nlohmann::json&)>;

/// One environment variable of the contract (SPEC §4.2, §4.3).
struct VarSpec {
    std::string name;
    VarType type = VarType::String;
    std::string description;
    bool required = false;
    bool secret = false;
    std::optional<Value> default_value;

    // int, float, duration
    std::optional<Value> min;
    std::optional<Value> max;
    // string; max_length also bounds a url or a json value. Lengths count
    // characters (Unicode code points), never bytes.
    std::optional<std::uint64_t> min_length;
    std::optional<std::uint64_t> max_length;
    std::optional<std::string> pattern;
    // url
    std::vector<std::string> schemes;
    // enum
    std::vector<std::string> values;
    // list
    ItemType items = ItemType::String;
    ListEncoding list_encoding = ListEncoding::Csv;
    std::string separator = ",";
    std::optional<std::uint64_t> min_items;
    std::optional<std::uint64_t> max_items;
    std::optional<std::int64_t> item_min;
    std::optional<std::int64_t> item_max;
    // Bounds on the length of each item of a string list, in characters.
    std::optional<std::uint64_t> item_min_length;
    std::optional<std::uint64_t> item_max_length;
    // duration
    DurationEncoding duration_encoding = DurationEncoding::Go;
    // json
    std::optional<nlohmann::json> schema;
    BindCheck bind;

    std::string group;
    std::vector<std::string> examples;
    std::optional<std::string> deprecated;
    std::string replaced_by;
    std::string config_key;

    /// Set by validation: the compiled pattern.
    std::shared_ptr<const detail::Pattern> compiled;
};

enum class FileType { Config, Tls, CaBundle, Keystore, Text, Binary };
const char* to_string(FileType t) noexcept;

/// One file input of the contract (SPEC §4.6).
struct FileSpec {
    std::string name;
    FileType type = FileType::Binary;
    std::string description;
    bool required = false;
    bool secret = false;
    std::string path;
    std::string path_env;
    std::string reload = "restart";
    std::optional<std::uint64_t> max_size;
    std::string group;
    std::optional<std::string> deprecated;
    std::string replaced_by;

    // config: "json", "yaml" or "toml"; keystore: "pkcs12" or "jks"
    std::string format;
    std::optional<nlohmann::json> schema;
    BindCheck bind;
    // tls
    std::vector<std::string> dns_names;
    std::vector<std::string> key_algorithms;
    std::optional<Duration> min_remaining;
    bool require_ca = false;
    // caBundle
    std::uint64_t min_certificates = 1;
    // keystore
    std::string password_var;
    // text
    std::optional<std::string> pattern;
    std::optional<std::uint64_t> min_length;
    std::optional<std::uint64_t> max_length;

    std::shared_ptr<const detail::Pattern> compiled;
};

/// The environment a load sees: variable name to raw value.
using Env = std::map<std::string, std::string>;

/// Profiles (SPEC §4.4): config-file defaults selected by a variable.
struct Profiles {
    std::string selector;
    std::string default_profile;
    std::map<std::string, std::map<std::string, Value>> defaults;
};

}  // namespace docuconf
