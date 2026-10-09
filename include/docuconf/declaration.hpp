// The declaration API: typed environment variables and file inputs, with
// CLI11 for --help and the optional command-line flags.
//
//     CLI::App app{"orders"};
//     docuconf::Declaration config{app, "orders"};
//
//     int port = 0;
//     config.add_var("PORT", port, "HTTP listen port").range(1, 65535).default_val(8080);
//
//     std::string database_url;
//     config.add_var("DATABASE_URL", database_url, "Primary Postgres connection string")
//         .secret().schemes({"postgres"});
//
//     DOCUCONF_PARSE(config, argc, argv);
//
// Variables are read from the environment only, as the platform validates
// the environment before deploy (SPEC §1.2). `--help` lists them in an
// "Environment" footer. A variable becomes a CLI11 option only when it asks
// for one with `.flag()`, for local development; a secret never can.
// docuconf parses every value with the spec's rules, reports all violations
// together, and exports the declaration as contract.cue.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "doc.hpp"
#include "duration.hpp"
#include "errors.hpp"
#include "files.hpp"
#include "keyset.hpp"
#include "spec.hpp"

/// 1 when the library was built with file inputs (DOCUCONF_FILE_INPUTS=ON in
/// CMake, the default); 0 for the environment-only build.
#ifndef DOCUCONF_FILE_INPUTS
#define DOCUCONF_FILE_INPUTS 1
#endif

namespace docuconf {

namespace detail {

template <class T, class = void>
struct has_member_schema : std::false_type {};
template <class T>
struct has_member_schema<T, std::void_t<decltype(T::json_schema())>> : std::true_type {};

// docuconf_json_schema(const T*) is found by argument-dependent lookup; it is
// what DOCUCONF_DEFINE_SCHEMA defines.
template <class T, class = void>
struct has_adl_schema : std::false_type {};
template <class T>
struct has_adl_schema<T, std::void_t<decltype(docuconf_json_schema(static_cast<const T*>(nullptr)))>>
    : std::true_type {};

}  // namespace detail

/// The JSON Schema of a type used as a `json` variable or a config file.
/// Give the type a `static nlohmann::json json_schema()` member, use
/// DOCUCONF_DEFINE_SCHEMA, or specialize this template.
template <class T, class = void>
struct json_schema {};

template <class T>
struct json_schema<T, std::enable_if_t<detail::has_member_schema<T>::value || detail::has_adl_schema<T>::value>> {
    static nlohmann::json get() {
        if constexpr (detail::has_member_schema<T>::value) return T::json_schema();
        else return docuconf_json_schema(static_cast<const T*>(nullptr));
    }
};

namespace detail {

template <class T>
struct optional_of {
    static constexpr bool value = false;
    using type = T;
};
template <class T>
struct optional_of<std::optional<T>> {
    static constexpr bool value = true;
    using type = T;
};

template <class T>
struct vector_of {
    static constexpr bool value = false;
    using item = void;
};
template <class T, class A>
struct vector_of<std::vector<T, A>> {
    static constexpr bool value = true;
    using item = T;
};

template <class T>
struct string_map_of {
    static constexpr bool value = false;
};
template <class V, class C, class A>
struct string_map_of<std::map<std::string, V, C, A>> {
    static constexpr bool value = true;
    using item = V;
};

template <class T>
struct is_duration : std::false_type {};
template <class R, class P>
struct is_duration<std::chrono::duration<R, P>> : std::true_type {};

template <class T, class = void>
struct has_schema : std::false_type {};
template <class T>
struct has_schema<T, std::void_t<decltype(json_schema<T>::get())>> : std::true_type {};

template <class T>
constexpr bool is_int_v = std::is_integral_v<T> && !std::is_same_v<T, bool>;

template <class>
constexpr bool always_false = false;

/// The contract type a C++ type maps to.
template <class U>
constexpr VarType var_type() {
    if constexpr (std::is_same_v<U, bool>) return VarType::Bool;
    else if constexpr (is_int_v<U>) return VarType::Int;
    else if constexpr (std::is_floating_point_v<U>) return VarType::Float;
    else if constexpr (std::is_same_v<U, std::string>) return VarType::String;
    else if constexpr (std::is_same_v<U, KeySet>) return VarType::KeySet;
    else if constexpr (is_duration<U>::value) return VarType::Duration;
    else if constexpr (std::is_enum_v<U>) return VarType::Enum;
    else if constexpr (vector_of<U>::value) return VarType::List;
    else if constexpr (has_schema<U>::value) return VarType::Json;
    else static_assert(always_false<U>, "docuconf: unsupported variable type; give it a json_schema()");
    return VarType::String;
}

template <class I>
BindCheck bind_check() {
    return [](const nlohmann::json& j) -> std::string {
        try {
            (void)j.get<I>();
            return "";
        } catch (const std::exception& e) {
            return e.what();
        }
    };
}

/// The JSON Schema DOCUCONF_DEFINE_SCHEMA writes for one member type.
template <class T>
nlohmann::json schema_of() {
    if constexpr (optional_of<T>::value) {
        return schema_of<typename optional_of<T>::type>();
    } else if constexpr (std::is_same_v<T, bool>) {
        return {{"type", "boolean"}};
    } else if constexpr (is_int_v<T>) {
        nlohmann::json s = {{"type", "integer"}};
        if constexpr (sizeof(T) < 8 || std::is_unsigned_v<T>) s["minimum"] = std::numeric_limits<T>::min();
        if constexpr (sizeof(T) < 8) s["maximum"] = std::numeric_limits<T>::max();
        return s;
    } else if constexpr (std::is_floating_point_v<T>) {
        return {{"type", "number"}};
    } else if constexpr (std::is_same_v<T, std::string>) {
        return {{"type", "string"}};
    } else if constexpr (has_schema<T>::value) {
        return json_schema<T>::get();
    } else if constexpr (vector_of<T>::value) {
        return {{"type", "array"}, {"items", schema_of<typename vector_of<T>::item>()}};
    } else if constexpr (string_map_of<T>::value) {
        return {{"type", "object"}, {"additionalProperties", schema_of<typename string_map_of<T>::item>()}};
    } else {
        static_assert(always_false<T>,
                      "docuconf: DOCUCONF_DEFINE_SCHEMA cannot describe this member type; give the member's "
                      "type its own schema or write json_schema() by hand");
        return {};
    }
}

template <class M>
void schema_property(nlohmann::json& schema, const char* name) {
    schema["properties"][name] = schema_of<M>();
    if constexpr (!optional_of<M>::value) schema["required"].push_back(name);
}

}  // namespace detail

class Declaration;

/// What every variable builder shares. Owned by its Declaration.
class VarBase {
public:
    virtual ~VarBase() = default;
    VarBase() = default;
    VarBase(const VarBase&) = delete;
    VarBase& operator=(const VarBase&) = delete;

    const VarSpec& spec() const { return spec_; }

protected:
    friend class Declaration;
    enum class FlagKind { Scalar, Bool, List };

    Declaration* decl_ = nullptr;
    VarSpec spec_;
    CLI::Option* option_ = nullptr;  // set only for a variable declared with flag()
    std::string flag_names_;
    std::string raw_;
    std::vector<std::string> raw_items_;
    bool required_explicit_ = false;
    bool finalized_ = false;
    std::vector<std::string> problems_;

    void make_flag(std::string names, FlagKind kind);

    virtual void finalize() = 0;
    virtual void assign(const std::optional<Value>& v) = 0;
    virtual bool target_is_optional() const = 0;
};

/// A declared variable. Its methods return `*this`, so they chain:
/// `config.add_var("PORT", port, "...").range(1, 65535).default_val(8080)`.
/// Methods that do not apply to `T` do not compile.
template <class T>
class Var : public VarBase {
public:
    using Target = typename detail::optional_of<T>::type;
    static constexpr VarType kType = detail::var_type<Target>();

    explicit Var(T& target) : target_(target) {
        spec_.type = kType;
        // A key set is always secret (SPEC §4.3).
        if constexpr (kType == VarType::KeySet) spec_.secret = true;
    }

    // ---- every type ----

    /// Must be set by the platform; it then has no default.
    Var& required(bool r = true) {
        spec_.required = r;
        required_explicit_ = true;
        return *this;
    }
    /// The value comes from a secret store and is never printed. A secret
    /// can never be a command-line flag.
    Var& secret(bool s = true) {
        spec_.secret = s;
        return *this;
    }
    Var& description(std::string d) {
        spec_.description = std::move(d);
        if (option_) option_->description(spec_.description);
        return *this;
    }
    /// Long-form documentation, in CommonMark: why the input exists and
    /// when to change it. Exported for `docuconf docs`; never read at
    /// runtime. Not blank, and at most 4000 characters.
    Var& details(std::string d) {
        spec_.details = std::move(d);
        return *this;
    }
    /// The input's Doxygen doc comment: its first paragraph is the
    /// description and the rest the details (see split_doc()).
    Var& doc(const std::string& comment) {
        Doc d = split_doc(comment);
        description(std::move(d.description));
        if (!d.details.empty()) spec_.details = std::move(d.details);
        return *this;
    }
    /// The contract's group; also the --help group of a flag.
    Var& group(std::string g) {
        spec_.group = std::move(g);
        if (option_) option_->group(spec_.group);
        return *this;
    }
    Var& examples(std::vector<std::string> e) {
        spec_.examples = std::move(e);
        return *this;
    }
    Var& deprecated(std::string message, std::string replaced_by = "") {
        spec_.deprecated = std::move(message);
        spec_.replaced_by = std::move(replaced_by);
        return *this;
    }
    Var& config_key(std::string k) {
        spec_.config_key = std::move(k);
        return *this;
    }

    /// Also accept the value as a command-line flag, for local development.
    /// The flag wins over the environment and gets the same checks; the
    /// platform's pre-deploy check never sees it. `names` uses CLI11's
    /// syntax (`"-p,--port"`); by default PORT becomes `--port`. A bool is a
    /// CLI11 flag (`--debug`, `--no-debug`, `--debug=false`); a list takes
    /// several arguments (`--origins a b`), each split on the delimiter.
    Var& flag(std::string names = "") {
        FlagKind kind = FlagKind::Scalar;
        if constexpr (std::is_same_v<Target, bool>) kind = FlagKind::Bool;
        else if constexpr (detail::vector_of<Target>::value || kType == VarType::KeySet) kind = FlagKind::List;
        make_flag(std::move(names), kind);
        return *this;
    }

    /// The default, used when the variable is unset (or empty, for any type
    /// but string).
    Var& default_val(const Target& v) {
        static_assert(kType != VarType::KeySet, "docuconf: a key set is secret, so it has no default");
        if constexpr (std::is_enum_v<Target>) {
            enum_default_ = v;
        } else {
            spec_.default_value = to_value(v);
        }
        return *this;
    }
    /// A string default; for a duration, in Go syntax such as "30s".
    template <class C, std::enable_if_t<std::is_same_v<C, char>, int> = 0>
    Var& default_val(const C* v) {
        if constexpr (detail::is_duration<Target>::value) {
            auto d = parse_go_duration(v);
            if (!d)
                problems_.push_back(spec_.name + ": default \"" + std::string(v) +
                                    "\" is not a Go duration; write it like 30s or 1m30s, or pass a "
                                    "std::chrono value");
            else spec_.default_value = Value(*d);
            return *this;
        } else if constexpr (std::is_constructible_v<Target, const char*>) {
            return default_val(Target(v));
        } else {
            static_assert(detail::always_false<C>, "docuconf: a string default for a non-string variable");
            return *this;
        }
    }

    // ---- int, float, duration ----

    template <class B>
    Var& min(B b) {
        spec_.min = bound(b);
        return *this;
    }
    template <class B>
    Var& max(B b) {
        spec_.max = bound(b);
        return *this;
    }
    template <class B, class C>
    Var& range(B lo, C hi) {
        min(lo);
        return max(hi);
    }

    // ---- string ----

    template <class U = Target>
    Var& min_length(std::uint64_t n) {
        static_assert(std::is_same_v<U, std::string>, "docuconf: min_length() applies to a std::string variable");
        spec_.min_length = n;
        return *this;
    }
    /// The most characters (Unicode code points) a string, url or json
    /// value may hold. A json value is measured as received, before parsing.
    template <class U = Target>
    Var& max_length(std::uint64_t n) {
        static_assert(std::is_same_v<U, std::string> || detail::var_type<U>() == VarType::Json,
                      "docuconf: max_length() applies to a std::string (string or url) or json variable");
        spec_.max_length = n;
        return *this;
    }
    /// An RE2 pattern, matched anywhere in the value: anchor it with ^ and $.
    template <class U = Target>
    Var& pattern(std::string p) {
        static_assert(std::is_same_v<U, std::string>, "docuconf: pattern() applies to a std::string variable");
        spec_.pattern = std::move(p);
        return *this;
    }
    /// Makes a string variable a `url`.
    template <class U = Target>
    Var& url() {
        static_assert(std::is_same_v<U, std::string>, "docuconf: url() applies to a std::string variable");
        spec_.type = VarType::Url;
        return *this;
    }
    /// Makes a string variable a `url` limited to these schemes.
    template <class U = Target>
    Var& schemes(std::vector<std::string> s) {
        static_assert(std::is_same_v<U, std::string>, "docuconf: schemes() applies to a std::string variable");
        spec_.type = VarType::Url;
        spec_.schemes = std::move(s);
        return *this;
    }
    /// Makes a string variable an `enum` of these values.
    template <class U = Target, std::enable_if_t<!std::is_enum_v<U>, int> = 0>
    Var& values(std::vector<std::string> v) {
        static_assert(std::is_same_v<U, std::string>,
                      "docuconf: values() applies to a std::string or a C++ enum variable");
        spec_.type = VarType::Enum;
        spec_.values = std::move(v);
        return *this;
    }
    /// The names of a C++ enum's values, like CLI::CheckedTransformer.
    template <class E = Target, std::enable_if_t<std::is_enum_v<E>, int> = 0>
    Var& values(std::vector<std::pair<std::string, E>> mapping) {
        enum_map_ = std::move(mapping);
        spec_.values.clear();
        for (const auto& [name, value] : enum_map_) spec_.values.push_back(name);
        return *this;
    }

    // ---- list ----

    template <class U = Target>
    Var& min_items(std::uint64_t n) {
        static_assert(detail::vector_of<U>::value, "docuconf: min_items() applies to a std::vector variable");
        spec_.min_items = n;
        return *this;
    }
    template <class U = Target>
    Var& max_items(std::uint64_t n) {
        static_assert(detail::vector_of<U>::value, "docuconf: max_items() applies to a std::vector variable");
        spec_.max_items = n;
        return *this;
    }
    template <class U = Target>
    Var& item_min(std::int64_t n) {
        static_assert(detail::is_int_v<typename detail::vector_of<U>::item>,
                      "docuconf: item_min() applies to a std::vector of integers");
        spec_.item_min = n;
        return *this;
    }
    template <class U = Target>
    Var& item_max(std::int64_t n) {
        static_assert(detail::is_int_v<typename detail::vector_of<U>::item>,
                      "docuconf: item_max() applies to a std::vector of integers");
        spec_.item_max = n;
        return *this;
    }
    template <class U = Target>
    Var& item_range(std::int64_t lo, std::int64_t hi) {
        item_min<U>(lo);
        return item_max<U>(hi);
    }
    /// The fewest characters (Unicode code points) each item of a string
    /// list may hold, after the list is split.
    template <class U = Target>
    Var& item_min_length(std::uint64_t n) {
        static_assert(std::is_same_v<typename detail::vector_of<U>::item, std::string>,
                      "docuconf: item_min_length() applies to a std::vector<std::string>");
        spec_.item_min_length = n;
        return *this;
    }
    /// The most characters (Unicode code points) each item of a string list
    /// may hold, after the list is split.
    template <class U = Target>
    Var& item_max_length(std::uint64_t n) {
        static_assert(std::is_same_v<typename detail::vector_of<U>::item, std::string>,
                      "docuconf: item_max_length() applies to a std::vector<std::string>");
        spec_.item_max_length = n;
        return *this;
    }
    /// The csv separator, `,` by default.
    template <class U = Target>
    Var& delimiter(std::string sep) {
        static_assert(detail::vector_of<U>::value || std::is_same_v<U, KeySet>,
                      "docuconf: delimiter() applies to a std::vector or a docuconf::KeySet variable");
        spec_.separator = std::move(sep);
        return *this;
    }

    // ---- keySet ----

    /// The fewest keys (default 1, at least 1).
    template <class U = Target>
    Var& min_keys(std::uint64_t n) {
        static_assert(std::is_same_v<U, KeySet>, "docuconf: min_keys() applies to a docuconf::KeySet variable");
        spec_.min_keys = n;
        return *this;
    }
    /// The most keys (default 2, at least min_keys): two during a rotation.
    template <class U = Target>
    Var& max_keys(std::uint64_t n) {
        static_assert(std::is_same_v<U, KeySet>, "docuconf: max_keys() applies to a docuconf::KeySet variable");
        spec_.max_keys = n;
        return *this;
    }
    /// The shortest key, in characters (Unicode code points).
    template <class U = Target>
    Var& key_min_length(std::uint64_t n) {
        static_assert(std::is_same_v<U, KeySet>, "docuconf: key_min_length() applies to a docuconf::KeySet variable");
        spec_.key_min_length = n;
        return *this;
    }
    /// The longest key, in characters (Unicode code points).
    template <class U = Target>
    Var& key_max_length(std::uint64_t n) {
        static_assert(std::is_same_v<U, KeySet>, "docuconf: key_max_length() applies to a docuconf::KeySet variable");
        spec_.key_max_length = n;
        return *this;
    }
    /// key_min_length(lo) and key_max_length(hi).
    template <class U = Target>
    Var& key_length(std::uint64_t lo, std::uint64_t hi) {
        key_min_length<U>(lo);
        return key_max_length<U>(hi);
    }

private:
    T& target_;
    std::vector<std::pair<std::string, Target>> enum_map_;
    std::optional<Target> enum_default_;

    template <class B>
    Value bound(B b) {
        if constexpr (detail::is_int_v<Target>) {
            static_assert(std::is_arithmetic_v<B>, "docuconf: an int bound must be a number");
            return Value(static_cast<std::int64_t>(b));
        } else if constexpr (std::is_floating_point_v<Target>) {
            static_assert(std::is_arithmetic_v<B>, "docuconf: a float bound must be a number");
            return Value(static_cast<double>(b));
        } else if constexpr (detail::is_duration<Target>::value) {
            if constexpr (detail::is_duration<B>::value) {
                return Value(std::chrono::duration_cast<Duration>(b));
            } else {
                auto d = parse_go_duration(std::string(b));
                if (!d)
                    problems_.push_back(spec_.name + ": bound \"" + std::string(b) +
                                        "\" is not a Go duration; write it like 30s or 5m, or pass a "
                                        "std::chrono value");
                return Value(d.value_or(Duration(0)));
            }
        } else {
            static_assert(detail::always_false<B>,
                          "docuconf: min(), max() and range() apply to int, float and duration variables");
            return Value();
        }
    }

    Value to_value(const Target& v) const {
        if constexpr (std::is_same_v<Target, bool>) return Value(v);
        else if constexpr (detail::is_int_v<Target>) return Value(static_cast<std::int64_t>(v));
        else if constexpr (std::is_floating_point_v<Target>) return Value(static_cast<double>(v));
        else if constexpr (std::is_same_v<Target, std::string>) return Value(v);
        else if constexpr (std::is_same_v<Target, KeySet>) {
            Value::List out(v.keys().begin(), v.keys().end());
            return Value(std::move(out));
        } else if constexpr (detail::is_duration<Target>::value)
            return Value(std::chrono::duration_cast<Duration>(v));
        else if constexpr (std::is_enum_v<Target>) {
            for (const auto& [name, value] : enum_map_)
                if (value == v) return Value(name);
            return Value(std::string());
        } else if constexpr (detail::vector_of<Target>::value) {
            Value::List out;
            for (const auto& x : v) {
                if constexpr (std::is_same_v<typename detail::vector_of<Target>::item, std::string>) out.emplace_back(x);
                else out.emplace_back(static_cast<std::int64_t>(x));
            }
            return Value(std::move(out));
        } else {
            return Value::json(nlohmann::json(v));
        }
    }

    Target from_value(const Value& v) const {
        if constexpr (std::is_same_v<Target, bool>) return v.as_bool();
        else if constexpr (detail::is_int_v<Target>) return static_cast<Target>(v.as_int());
        else if constexpr (std::is_floating_point_v<Target>) return static_cast<Target>(v.as_float());
        else if constexpr (std::is_same_v<Target, std::string>) return v.as_string();
        else if constexpr (std::is_same_v<Target, KeySet>) {
            std::vector<std::string> keys;
            for (const auto& k : v.as_list()) keys.push_back(k.as_string());
            return KeySet(std::move(keys));
        } else if constexpr (detail::is_duration<Target>::value)
            return std::chrono::duration_cast<Target>(v.as_duration());
        else if constexpr (std::is_enum_v<Target>) {
            for (const auto& [name, value] : enum_map_)
                if (name == v.as_string()) return value;
            return Target{};
        } else if constexpr (detail::vector_of<Target>::value) {
            Target out;
            for (const auto& x : v.as_list()) {
                using I = typename detail::vector_of<Target>::item;
                if constexpr (std::is_same_v<I, std::string>) out.push_back(x.as_string());
                else out.push_back(static_cast<I>(x.as_int()));
            }
            return out;
        } else {
            return v.as_json().template get<Target>();
        }
    }

    template <class I>
    static void int_range(std::optional<Value>& lo, std::optional<Value>& hi) {
        // Export the C++ type's range when it is narrower than 64 bits
        // signed (SPEC §5), so the platform never accepts a value the app
        // cannot hold.
        constexpr auto tmin = static_cast<std::int64_t>(std::numeric_limits<I>::min());
        constexpr bool narrow_max = std::numeric_limits<I>::max() <
                                    static_cast<std::make_unsigned_t<std::int64_t>>(std::numeric_limits<std::int64_t>::max());
        if (tmin > std::numeric_limits<std::int64_t>::min() || std::is_unsigned_v<I>) {
            if (!lo || lo->as_int() < tmin) lo = Value(tmin);
        }
        if constexpr (narrow_max) {
            constexpr auto tmax = static_cast<std::int64_t>(std::numeric_limits<I>::max());
            if (!hi || hi->as_int() > tmax) hi = Value(tmax);
        }
    }

    void finalize() override {
        if constexpr (detail::is_int_v<Target>) {
            if ((!spec_.min || spec_.min->is_int()) && (!spec_.max || spec_.max->is_int()))
                int_range<Target>(spec_.min, spec_.max);
        } else if constexpr (std::is_enum_v<Target>) {
            if (enum_map_.empty()) problems_.push_back(spec_.name + ": a C++ enum needs values({{name, value}, ...})");
            if (enum_default_) spec_.default_value = to_value(*enum_default_);
        } else if constexpr (detail::vector_of<Target>::value) {
            using I = typename detail::vector_of<Target>::item;
            static_assert(std::is_same_v<I, std::string> || detail::is_int_v<I>,
                          "docuconf: list items are std::string or an integer type");
            if constexpr (detail::is_int_v<I>) {
                spec_.items = ItemType::Int;
                std::optional<Value> lo, hi;
                if (spec_.item_min) lo = Value(*spec_.item_min);
                if (spec_.item_max) hi = Value(*spec_.item_max);
                int_range<I>(lo, hi);
                if (lo) spec_.item_min = lo->as_int();
                if (hi) spec_.item_max = hi->as_int();
            } else {
                spec_.items = ItemType::String;
            }
        } else if constexpr (kType == VarType::Json) {
            spec_.schema = json_schema<Target>::get();
            spec_.bind = detail::bind_check<Target>();
        }
        // A variable that is not std::optional and has no default must be
        // set, so the app never sees an uninitialised value.
        if (!detail::optional_of<T>::value && !spec_.default_value && !required_explicit_) spec_.required = true;
    }

    void assign(const std::optional<Value>& v) override {
        if constexpr (detail::optional_of<T>::value) {
            if (v) target_ = from_value(*v);
            else target_ = std::nullopt;
        } else {
            if (v) target_ = from_value(*v);
        }
    }

    bool target_is_optional() const override { return detail::optional_of<T>::value; }
};

/// The file input builder. Methods return `*this`; those that do not apply
/// to the input's type are reported as declaration problems.
class File {
public:
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    const FileSpec& spec() const { return spec_; }

    /// Where the app reads the input: a directory for TLS, a file otherwise.
    File& path(std::string p) {
        spec_.path = std::move(p);
        return *this;
    }
    /// An environment variable the platform sets to the path.
    File& path_env(std::string name) {
        spec_.path_env = std::move(name);
        return *this;
    }
    File& required(bool r = true) {
        spec_.required = r;
        return *this;
    }
    File& secret(bool s = true) {
        spec_.secret = s;
        return *this;
    }
    File& max_size(std::uint64_t bytes) {
        spec_.max_size = bytes;
        return *this;
    }
    /// Only "restart" is supported: docuconf reads files once, at boot.
    File& reload(std::string r) {
        spec_.reload = std::move(r);
        return *this;
    }
    File& group(std::string g) {
        spec_.group = std::move(g);
        return *this;
    }
    File& description(std::string d) {
        spec_.description = std::move(d);
        return *this;
    }
    /// Long-form documentation, in CommonMark (see Var::details()).
    File& details(std::string d) {
        spec_.details = std::move(d);
        return *this;
    }
    /// The input's Doxygen doc comment: its first paragraph is the
    /// description and the rest the details (see split_doc()).
    File& doc(const std::string& comment) {
        Doc d = split_doc(comment);
        spec_.description = std::move(d.description);
        if (!d.details.empty()) spec_.details = std::move(d.details);
        return *this;
    }
    File& deprecated(std::string message, std::string replaced_by = "") {
        spec_.deprecated = std::move(message);
        spec_.replaced_by = std::move(replaced_by);
        return *this;
    }
    /// config: "json", "yaml" or "toml" (default: from the extension).
    /// keystore: "pkcs12" (default) or "jks".
    File& format(std::string f) {
        spec_.format = std::move(f);
        return *this;
    }
    // tls
    File& dns_names(std::vector<std::string> names) {
        return only(FileType::Tls, "dnsNames", [&] { spec_.dns_names = std::move(names); });
    }
    File& key_algorithms(std::vector<std::string> algs) {
        return only(FileType::Tls, "keyAlgorithms", [&] { spec_.key_algorithms = std::move(algs); });
    }
    /// At least this much validity left, in Go syntax such as "720h".
    File& min_remaining(const std::string& d) {
        return only(FileType::Tls, "minRemaining", [&] {
            auto v = parse_go_duration(d);
            if (!v)
                problems_.push_back(spec_.name + ": minRemaining \"" + d +
                                    "\" is not a Go duration; write it like 720h, or pass a std::chrono value");
            spec_.min_remaining = v;
        });
    }
    /// At least this much validity left, such as `std::chrono::hours(720)`
    /// or `720h` with std::chrono_literals.
    template <class R, class P>
    File& min_remaining(std::chrono::duration<R, P> d) {
        return only(FileType::Tls, "minRemaining",
                    [&] { spec_.min_remaining = std::chrono::duration_cast<Duration>(d); });
    }
    File& min_remaining(const char* d) { return min_remaining(std::string(d)); }
    File& require_ca(bool r = true) {
        return only(FileType::Tls, "requireCA", [&] { spec_.require_ca = r; });
    }
    // caBundle
    File& min_certificates(std::uint64_t n) {
        return only(FileType::CaBundle, "minCertificates", [&] { spec_.min_certificates = n; });
    }
    // keystore
    /// The secret variable holding the keystore password.
    File& password_var(std::string name) {
        return only(FileType::Keystore, "passwordVar", [&] { spec_.password_var = std::move(name); });
    }
    // text
    File& pattern(std::string p) {
        return only(FileType::Text, "pattern", [&] { spec_.pattern = std::move(p); });
    }
    File& min_length(std::uint64_t n) {
        return only(FileType::Text, "minLength", [&] { spec_.min_length = n; });
    }
    File& max_length(std::uint64_t n) {
        return only(FileType::Text, "maxLength", [&] { spec_.max_length = n; });
    }

private:
    friend class Declaration;
    FileSpec spec_;
    std::vector<std::string> problems_;
    std::function<void(const void*)> assign_;  // takes a detail::LoadedFile

    template <class F>
    File& only(FileType t, const char* field, F&& set) {
        if (spec_.type != t) {
            problems_.push_back(spec_.name + ": " + field + " does not apply to a " + to_string(spec_.type) + " file");
        } else {
            set();
        }
        return *this;
    }
};

/// The declaration: the docuconf metadata of each input, on a CLI11 app.
class Declaration {
public:
    /// `service` is the contract's metadata.name (a DNS label).
    Declaration(CLI::App& app, std::string service);
    Declaration(const Declaration&) = delete;
    Declaration& operator=(const Declaration&) = delete;
    ~Declaration();

    /// metadata.appVersion, such as a version or git SHA.
    Declaration& app_version(std::string v);

    /// Declares an environment variable bound to `target`. It is read from
    /// the environment only; `.flag()` also makes it a command-line option.
    ///
    /// The C++ type picks the contract type: bool; integer types (`int`,
    /// `std::uint16_t`...); `double`/`float`; `std::string` (`string`, or
    /// `url` with schemes(), or `enum` with values()); `std::chrono`
    /// durations; a C++ enum with values({{name, value}}); `std::vector` of
    /// strings or integers (a `csv` list); docuconf::KeySet (a `keySet`,
    /// always secret); any other type with a json_schema() (a `json`
    /// value, bound with nlohmann's from_json).
    /// Wrap it in `std::optional` for an optional variable with no default;
    /// any other variable without a default is required.
    ///
    /// Leave `description` out to take it from `.doc()`, the input's
    /// Doxygen doc comment, whose first paragraph is the description and
    /// the rest the details.
    template <class T>
    Var<T>& add_var(std::string name, T& target, std::string description = "") {
        before_declare(name);
        auto var = std::make_unique<Var<T>>(target);
        Var<T>* raw = var.get();
        raw->decl_ = this;
        raw->spec_.name = std::move(name);
        raw->spec_.description = std::move(description);
        vars_.push_back(std::move(var));
        checked_ = false;
        return *raw;
    }

#if DOCUCONF_FILE_INPUTS
    File& add_file(std::string name, TlsKeyPair& target, std::string description = "");
    File& add_file(std::string name, CaBundle& target, std::string description = "");
    File& add_file(std::string name, Keystore& target, std::string description = "");
    File& add_file(std::string name, TextFile& target, std::string description = "");
    File& add_file(std::string name, BinaryFile& target, std::string description = "");

    /// A structured config file bound to T, which needs a json_schema() and
    /// nlohmann's from_json.
    template <class T>
    File& add_file(std::string name, ConfigFile<T>& target, std::string description = "") {
        static_assert(detail::has_schema<T>::value, "docuconf: a config file type needs a json_schema()");
        File& f = new_file(std::move(name), FileType::Config, std::move(description));
        f.spec_.schema = json_schema<T>::get();
        f.spec_.bind = detail::bind_check<T>();
        f.assign_ = [&target](const void* p) { bind_config(target, p); };
        return f;
    }
#else
    template <class T>
    File& add_file(std::string, T&, std::string = "") {
        static_assert(detail::always_false<T>,
                      "docuconf: this build has no file inputs (DOCUCONF_FILE_INPUTS=OFF); configure docuconf "
                      "with -DDOCUCONF_FILE_INPUTS=ON to declare files");
        return *static_cast<File*>(nullptr);
    }
#endif

    /// Checks the declaration (SPEC §11.2 item 2). Throws DeclarationError.
    void check();

    /// Parses the command line through CLI11 and reads the process
    /// environment, then validates every variable and file input and binds
    /// the values. `--docuconf-export <path>` writes contract.cue and throws
    /// CLI::Success instead, without reading the environment. Throws
    /// CLI::ParseError (as CLI11 does), DeclarationError, ExportError or
    /// ValidationError (after writing the termination log).
    void parse(int argc, const char* const* argv);

    /// Validates `env` as the whole environment and binds the values,
    /// without CLI11 and without reading the process environment, for tests.
    /// `DOCUCONF_FILE_ROOT` and `DOCUCONF_TERMINATION_LOG` are read from
    /// `env` too. Throws DeclarationError or ValidationError.
    void load(const Env& env);

    /// Calls parse() and turns every outcome into an exit code, as
    /// CLI11_PARSE does: returns true when main should return `code` (0
    /// after --help or --docuconf-export, 1 for configuration problems or a
    /// contract that cannot be written, 2 for a mistake in the declaration,
    /// CLI11's own codes for a bad command line).
    bool parse_or_exit(int argc, const char* const* argv, int& code, std::ostream& err = std::cerr);

    /// The contract as CUE (SPEC §4), deterministic and sorted by name.
    std::string export_cue() const;
    /// The contract as JSON, the form `cue export` produces.
    nlohmann::ordered_json export_json() const;
    /// Writes export_cue() to `path` ("-" for stdout), through a temporary
    /// file that is renamed into place. Throws ExportError.
    void write_contract(const std::string& path) const;

    std::vector<VarSpec> var_specs() const;
    std::vector<FileSpec> file_specs() const;

    /// The "Environment" and "Files" sections --help shows.
    std::string help_footer() const;

    /// Where warnings go (deprecated variables set, feature-flag names,
    /// likely typos in the environment). Default: standard error.
    void on_warning(std::function<void(const std::string&)> sink);

    /// The option that triggers export, `--docuconf-export`.
    static constexpr const char* kExportFlag = "--docuconf-export";

private:
    friend class VarBase;
    CLI::App& app_;
    std::string service_;
    std::string app_version_;
    std::vector<std::unique_ptr<VarBase>> vars_;
    std::vector<std::unique_ptr<File>> files_;
    std::function<void(const std::string&)> warn_;
    std::string export_path_;
    std::vector<std::string> problems_;
    bool checked_ = false;
    bool loaded_ = false;

    void before_declare(const std::string& name);
    void load_env(const Env& env, const std::map<std::string, std::string>& sources, bool process);
    File& new_file(std::string name, FileType type, std::string description);

    template <class T>
    static void bind_config(ConfigFile<T>& target, const void* p);
    static bool loaded_present(const void* p);
    static const std::string& loaded_path(const void* p);
    static const nlohmann::json& loaded_document(const void* p);
};

template <class T>
void Declaration::bind_config(ConfigFile<T>& target, const void* p) {
    target.present = loaded_present(p);
    if (!target.present) return;
    target.path = loaded_path(p);
    target.value = loaded_document(p).template get<T>();
}

}  // namespace docuconf

/// Like CLI11_PARSE: parses, validates and binds, or returns the exit code
/// from main (0 after --help or --docuconf-export, 1 with every violation
/// printed to standard error, 2 for a mistake in the declaration).
#define DOCUCONF_PARSE(declaration, argc, argv)                                 \
    do {                                                                        \
        int docuconf_exit_code_ = 0;                                            \
        if ((declaration).parse_or_exit((argc), (argv), docuconf_exit_code_)) \
            return docuconf_exit_code_;                                         \
    } while (false)

#define DOCUCONF_DETAIL_SCHEMA_MEMBER(m) \
    ::docuconf::detail::schema_property<decltype(docuconf_schema_type_::m)>(docuconf_schema_, #m);

/// Writes the JSON Schema of a plain struct from its members' types, next to
/// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE with the same member list: an object
/// with those properties, every member that is not a std::optional
/// required. Member types: bool, integers (with their range), floating
/// point, std::string, std::vector, std::map<std::string, V>, std::optional
/// and any type that has a schema itself. Use it at namespace scope.
#define DOCUCONF_DEFINE_SCHEMA(Type, ...)                                                       \
    [[maybe_unused]] inline nlohmann::json docuconf_json_schema(const Type*) {                  \
        using docuconf_schema_type_ = Type;                                                     \
        nlohmann::json docuconf_schema_ = {{"type", "object"},                                  \
                                           {"properties", nlohmann::json::object()},            \
                                           {"required", nlohmann::json::array()}};              \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(DOCUCONF_DETAIL_SCHEMA_MEMBER, __VA_ARGS__))   \
        if (docuconf_schema_["required"].empty()) docuconf_schema_.erase("required");           \
        return docuconf_schema_;                                                                \
    }
