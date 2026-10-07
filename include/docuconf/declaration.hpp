// The declaration API: docuconf metadata on top of CLI11 options.
//
//     CLI::App app{"orders"};
//     docuconf::Declaration config{app, "orders"};
//
//     int port = 0;
//     config.add_var("PORT", port, "HTTP listen port")->range(1, 65535)->default_val(8080);
//
//     std::string database_url;
//     config.add_var("DATABASE_URL", database_url, "Primary Postgres connection string")
//         ->secret()->schemes({"postgres"});
//
//     DOCUCONF_PARSE(config, argc, argv);
//
// Each variable is a CLI11 option (`--port`) bound to its environment
// variable with `->envname("PORT")`, so it shows up in `--help` and CLI11
// keeps choosing where a value comes from. docuconf adds the description,
// `secret` and the constraints, parses every value with the spec's rules,
// reports all violations together, and exports the declaration as
// contract.cue.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "duration.hpp"
#include "errors.hpp"
#include "files.hpp"
#include "spec.hpp"

namespace docuconf {

/// The JSON Schema of a type used as a `json` variable or a config file.
/// Give the type a `static nlohmann::json json_schema()` member, or
/// specialize this template.
template <class T, class = void>
struct json_schema {};

template <class T>
struct json_schema<T, std::void_t<decltype(T::json_schema())>> {
    static nlohmann::json get() { return T::json_schema(); }
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
};
template <class T, class A>
struct vector_of<std::vector<T, A>> {
    static constexpr bool value = true;
    using item = T;
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

}  // namespace detail

class Declaration;

/// What every variable builder shares. Owned by its Declaration.
class VarBase {
public:
    virtual ~VarBase() = default;
    const VarSpec& spec() const { return spec_; }
    /// The CLI11 option behind the variable, for CLI11-specific settings.
    CLI::Option* option() const { return option_; }

protected:
    friend class Declaration;
    VarSpec spec_;
    CLI::Option* option_ = nullptr;
    std::string raw_;
    bool required_explicit_ = false;
    bool finalized_ = false;
    std::vector<std::string> problems_;

    virtual void finalize() = 0;
    virtual void assign(const std::optional<Value>& v) = 0;
    virtual bool target_is_optional() const = 0;
};

/// A declared variable. Methods return `this` so they chain like CLI11's
/// `Option*` methods.
template <class T>
class Var : public VarBase {
public:
    using Target = typename detail::optional_of<T>::type;
    static constexpr VarType kType = detail::var_type<Target>();

    explicit Var(T& target) : target_(target) { spec_.type = kType; }

    // ---- every type ----

    /// Must be set by the platform; it then has no default.
    Var* required(bool r = true) {
        spec_.required = r;
        required_explicit_ = true;
        return this;
    }
    /// The value comes from a secret store and is never printed.
    Var* secret(bool s = true) {
        spec_.secret = s;
        return this;
    }
    Var* description(std::string d) {
        spec_.description = std::move(d);
        if (option_) option_->description(spec_.description);
        return this;
    }
    Var* group(std::string g) {
        spec_.group = std::move(g);
        return this;
    }
    Var* examples(std::vector<std::string> e) {
        spec_.examples = std::move(e);
        return this;
    }
    Var* deprecated(std::string message, std::string replaced_by = "") {
        spec_.deprecated = std::move(message);
        spec_.replaced_by = std::move(replaced_by);
        return this;
    }
    Var* config_key(std::string k) {
        spec_.config_key = std::move(k);
        return this;
    }

    /// The default, used when the variable is unset (or empty, for any type
    /// but string).
    Var* default_val(const Target& v) {
        if constexpr (std::is_enum_v<Target>) {
            enum_default_ = v;
        } else {
            spec_.default_value = to_value(v);
        }
        return this;
    }
    /// A string default; for a duration, in Go syntax such as "30s".
    template <class C, std::enable_if_t<std::is_same_v<C, char>, int> = 0>
    Var* default_val(const C* v) {
        if constexpr (detail::is_duration<Target>::value) {
            auto d = parse_go_duration(v);
            if (!d) problems_.push_back(spec_.name + ": default \"" + std::string(v) + "\" is not a Go duration");
            else spec_.default_value = Value(*d);
            return this;
        } else if constexpr (std::is_constructible_v<Target, const char*>) {
            return default_val(Target(v));
        } else {
            static_assert(detail::always_false<T>, "docuconf: a string default for a non-string variable");
        }
    }

    // ---- int, float, duration ----

    template <class B>
    Var* min(B b) {
        spec_.min = bound(b);
        return this;
    }
    template <class B>
    Var* max(B b) {
        spec_.max = bound(b);
        return this;
    }
    template <class B, class C>
    Var* range(B lo, C hi) {
        return min(lo)->max(hi);
    }

    // ---- string ----

    Var* min_length(std::uint64_t n) {
        spec_.min_length = n;
        return this;
    }
    Var* max_length(std::uint64_t n) {
        spec_.max_length = n;
        return this;
    }
    /// An RE2 pattern, matched anywhere in the value: anchor it with ^ and $.
    Var* pattern(std::string p) {
        spec_.pattern = std::move(p);
        return this;
    }
    /// Makes a string variable a `url`.
    Var* url() {
        static_assert(std::is_same_v<Target, std::string>, "docuconf: url() applies to a std::string");
        spec_.type = VarType::Url;
        return this;
    }
    /// Makes a string variable a `url` limited to these schemes.
    Var* schemes(std::vector<std::string> s) {
        url();
        spec_.schemes = std::move(s);
        return this;
    }
    /// Makes a string variable an `enum` of these values.
    Var* values(std::vector<std::string> v) {
        static_assert(std::is_same_v<Target, std::string>,
                      "docuconf: for a C++ enum, pass {name, value} pairs to values()");
        spec_.type = VarType::Enum;
        spec_.values = std::move(v);
        return this;
    }
    /// The names of a C++ enum's values, like CLI::CheckedTransformer.
    template <class E = Target, std::enable_if_t<std::is_enum_v<E>, int> = 0>
    Var* values(std::vector<std::pair<std::string, E>> mapping) {
        enum_map_ = std::move(mapping);
        spec_.values.clear();
        for (const auto& [name, value] : enum_map_) spec_.values.push_back(name);
        return this;
    }

    // ---- list ----

    Var* min_items(std::uint64_t n) {
        spec_.min_items = n;
        return this;
    }
    Var* max_items(std::uint64_t n) {
        spec_.max_items = n;
        return this;
    }
    Var* item_min(std::int64_t n) {
        spec_.item_min = n;
        return this;
    }
    Var* item_max(std::int64_t n) {
        spec_.item_max = n;
        return this;
    }
    Var* item_range(std::int64_t lo, std::int64_t hi) { return item_min(lo)->item_max(hi); }
    /// The csv separator, `,` by default, as CLI11's `->delimiter()`.
    Var* delimiter(std::string sep) {
        spec_.separator = std::move(sep);
        return this;
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
            return Value(static_cast<double>(b));
        } else if constexpr (detail::is_duration<Target>::value) {
            if constexpr (detail::is_duration<B>::value) {
                return Value(std::chrono::duration_cast<Duration>(b));
            } else {
                auto d = parse_go_duration(std::string(b));
                if (!d) problems_.push_back(spec_.name + ": bound \"" + std::string(b) + "\" is not a Go duration");
                return Value(d.value_or(Duration(0)));
            }
        } else {
            static_assert(detail::always_false<B>, "docuconf: min and max apply to int, float and duration");
            return Value();
        }
    }

    Value to_value(const Target& v) const {
        if constexpr (std::is_same_v<Target, bool>) return Value(v);
        else if constexpr (detail::is_int_v<Target>) return Value(static_cast<std::int64_t>(v));
        else if constexpr (std::is_floating_point_v<Target>) return Value(static_cast<double>(v));
        else if constexpr (std::is_same_v<Target, std::string>) return Value(v);
        else if constexpr (detail::is_duration<Target>::value)
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
        else if constexpr (detail::is_duration<Target>::value)
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

/// The file input builder. Methods that do not apply to the input's type
/// are reported as declaration problems.
class File {
public:
    const FileSpec& spec() const { return spec_; }

    /// Where the app reads the input: a directory for TLS, a file otherwise.
    File* path(std::string p) {
        spec_.path = std::move(p);
        return this;
    }
    /// An environment variable the platform sets to the path.
    File* path_env(std::string name) {
        spec_.path_env = std::move(name);
        return this;
    }
    File* required(bool r = true) {
        spec_.required = r;
        return this;
    }
    File* secret(bool s = true) {
        spec_.secret = s;
        return this;
    }
    File* max_size(std::uint64_t bytes) {
        spec_.max_size = bytes;
        return this;
    }
    /// Only "restart" is supported: docuconf reads files once, at boot.
    File* reload(std::string r) {
        spec_.reload = std::move(r);
        return this;
    }
    File* group(std::string g) {
        spec_.group = std::move(g);
        return this;
    }
    File* deprecated(std::string message, std::string replaced_by = "") {
        spec_.deprecated = std::move(message);
        spec_.replaced_by = std::move(replaced_by);
        return this;
    }
    /// config: "json", "yaml" or "toml" (default: from the extension).
    /// keystore: "pkcs12" (default) or "jks".
    File* format(std::string f) {
        spec_.format = std::move(f);
        return this;
    }
    // tls
    File* dns_names(std::vector<std::string> names) { return only(FileType::Tls, "dnsNames", [&] { spec_.dns_names = std::move(names); }); }
    File* key_algorithms(std::vector<std::string> algs) {
        return only(FileType::Tls, "keyAlgorithms", [&] { spec_.key_algorithms = std::move(algs); });
    }
    /// At least this much validity left, in Go syntax such as "720h".
    File* min_remaining(const std::string& d) {
        return only(FileType::Tls, "minRemaining", [&] {
            auto v = parse_go_duration(d);
            if (!v) problems_.push_back(spec_.name + ": minRemaining \"" + d + "\" is not a Go duration");
            spec_.min_remaining = v;
        });
    }
    File* require_ca(bool r = true) { return only(FileType::Tls, "requireCA", [&] { spec_.require_ca = r; }); }
    // caBundle
    File* min_certificates(std::uint64_t n) {
        return only(FileType::CaBundle, "minCertificates", [&] { spec_.min_certificates = n; });
    }
    // keystore
    /// The secret variable holding the keystore password.
    File* password_var(std::string name) {
        return only(FileType::Keystore, "passwordVar", [&] { spec_.password_var = std::move(name); });
    }
    // text
    File* pattern(std::string p) { return only(FileType::Text, "pattern", [&] { spec_.pattern = std::move(p); }); }
    File* min_length(std::uint64_t n) { return only(FileType::Text, "minLength", [&] { spec_.min_length = n; }); }
    File* max_length(std::uint64_t n) { return only(FileType::Text, "maxLength", [&] { spec_.max_length = n; }); }

private:
    friend class Declaration;
    FileSpec spec_;
    std::vector<std::string> problems_;
    std::function<void(const void*)> assign_;  // takes a detail::LoadedFile

    template <class F>
    File* only(FileType t, const char* field, F&& set) {
        if (spec_.type != t) {
            problems_.push_back(spec_.name + ": " + field + " does not apply to a " + to_string(spec_.type) + " file");
        } else {
            set();
        }
        return this;
    }
};

/// The declaration: a CLI11 app plus the docuconf metadata of each input.
class Declaration {
public:
    /// `service` is the contract's metadata.name (a DNS label).
    Declaration(CLI::App& app, std::string service);
    Declaration(const Declaration&) = delete;
    Declaration& operator=(const Declaration&) = delete;
    ~Declaration();

    /// metadata.appVersion, such as a version or git SHA.
    Declaration& app_version(std::string v);

    /// Declares an environment variable bound to `target`, as a CLI11
    /// option named after it (PORT becomes --port, with envname PORT).
    ///
    /// The C++ type picks the contract type: bool; integer types (`int`,
    /// `std::uint16_t`...); `double`/`float`; `std::string` (`string`, or
    /// `url` with schemes(), or `enum` with values()); `std::chrono`
    /// durations; a C++ enum with values({{name, value}}); `std::vector` of
    /// strings or integers (a `csv` list); any other type with a
    /// json_schema() (a `json` value, bound with nlohmann's from_json).
    /// Wrap it in `std::optional` for an optional variable with no default;
    /// any other variable without a default is required.
    template <class T>
    Var<T>* add_var(std::string name, T& target, std::string description) {
        auto var = std::make_unique<Var<T>>(target);
        Var<T>* raw = var.get();
        raw->spec_.name = std::move(name);
        raw->spec_.description = std::move(description);
        register_var(std::move(var));
        return raw;
    }

    File* add_file(std::string name, TlsKeyPair& target, std::string description);
    File* add_file(std::string name, CaBundle& target, std::string description);
    File* add_file(std::string name, Keystore& target, std::string description);
    File* add_file(std::string name, TextFile& target, std::string description);
    File* add_file(std::string name, BinaryFile& target, std::string description);

    /// A structured config file bound to T, which needs a json_schema() and
    /// nlohmann's from_json.
    template <class T>
    File* add_file(std::string name, ConfigFile<T>& target, std::string description) {
        static_assert(detail::has_schema<T>::value, "docuconf: a config file type needs a json_schema()");
        File* f = new_file(std::move(name), FileType::Config, std::move(description));
        f->spec_.schema = json_schema<T>::get();
        f->spec_.bind = detail::bind_check<T>();
        f->assign_ = [&target](const void* p) { bind_config(target, p); };
        return f;
    }

    /// Checks the declaration (SPEC §11.2 item 2). Throws DeclarationError.
    void check();

    /// Parses the command line and the environment through CLI11, then
    /// validates every variable and file input and binds the values.
    /// `--docuconf-export <path>` writes contract.cue and throws
    /// CLI::Success instead, without reading the environment. Throws
    /// CLI::ParseError (as CLI11 does), DeclarationError or ValidationError
    /// (after writing the termination log).
    void parse(int argc, const char* const* argv);

    /// Validates `env` as the whole environment and binds the values,
    /// without CLI11. For tests and for apps that parse CLI11 themselves.
    void load(const Env& env);

    /// Calls parse() and turns every outcome into an exit code, as
    /// CLI11_PARSE does: returns true when main should return `code`.
    bool parse_or_exit(int argc, const char* const* argv, int& code, std::ostream& err = std::cerr);

    /// The contract as CUE (SPEC §4), deterministic and sorted by name.
    std::string export_cue() const;
    /// The contract as JSON, the form `cue export` produces.
    nlohmann::ordered_json export_json() const;
    /// Writes export_cue() to `path` ("-" for stdout).
    void write_contract(const std::string& path) const;

    std::vector<VarSpec> var_specs() const;
    std::vector<FileSpec> file_specs() const;

    /// Where warnings go (deprecated variables set, feature-flag names).
    /// Default: standard error.
    void on_warning(std::function<void(const std::string&)> sink);

    /// The option that triggers export, `--docuconf-export`.
    static constexpr const char* kExportFlag = "--docuconf-export";

private:
    CLI::App& app_;
    std::string service_;
    std::string app_version_;
    std::vector<std::unique_ptr<VarBase>> vars_;
    std::vector<std::unique_ptr<File>> files_;
    std::function<void(const std::string&)> warn_;
    std::string export_path_;
    bool checked_ = false;

    void register_var(std::unique_ptr<VarBase> var);
    File* new_file(std::string name, FileType type, std::string description);

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
/// printed to standard error).
#define DOCUCONF_PARSE(declaration, argc, argv)                                 \
    do {                                                                        \
        int docuconf_exit_code_ = 0;                                            \
        if ((declaration).parse_or_exit((argc), (argv), docuconf_exit_code_)) \
            return docuconf_exit_code_;                                         \
    } while (false)
