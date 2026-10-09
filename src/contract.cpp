#include "docuconf/contract.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <limits>

#include "docuconf/duration.hpp"
#include "internal.hpp"

namespace docuconf {

const Value* Values::get(const std::string& name) const {
    auto it = values_.find(name);
    if (it == values_.end() || !it->second) return nullptr;
    return &*it->second;
}

bool Values::is_secret(const std::string& name) const {
    return std::find(secrets_.begin(), secrets_.end(), name) != secrets_.end();
}

std::optional<KeySet> Values::key_set(const std::string& name) const {
    const Value* v = get(name);
    if (!v || !v->is_list()) return std::nullopt;
    std::vector<std::string> keys;
    for (const auto& k : v->as_list()) {
        if (!k.is_string()) return std::nullopt;
        keys.push_back(k.as_string());
    }
    return KeySet(std::move(keys));
}

const FileValue* Values::file(const std::string& name) const {
    auto it = files_.find(name);
    if (it == files_.end() || !it->second) return nullptr;
    return &*it->second;
}

nlohmann::json Values::to_json() const {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [k, v] : values_) out[k] = v ? v->to_json() : nlohmann::json(nullptr);
    for (const auto& [k, f] : files_) {
        if (!f) out[k] = nullptr;
        else if (f->type == FileType::Config) out[k] = f->document;
        else if (f->type == FileType::Text) out[k] = f->content;
        else out[k] = true;
    }
    return out;
}

nlohmann::json Values::to_redacted_json() const {
    nlohmann::json out = to_json();
    for (const auto& s : secrets_)
        if (out.contains(s) && !out[s].is_null()) out[s] = "***";
    return out;
}

namespace {

using nlohmann::json;

struct Fields {
    const std::string& name;
    const json& spec;
    std::vector<std::string>& problems;

    void bad(const std::string& m) { problems.push_back(name + ": " + m); }
    const json* get(const char* k) const {
        auto it = spec.find(k);
        if (it == spec.end() || it->is_null()) return nullptr;
        return &*it;
    }
    std::optional<std::string> str(const char* k) {
        const json* v = get(k);
        if (!v) return std::nullopt;
        if (!v->is_string()) {
            bad(std::string(k) + " must be a string");
            return std::nullopt;
        }
        return v->get<std::string>();
    }
    bool boolean(const char* k) {
        const json* v = get(k);
        if (!v) return false;
        if (!v->is_boolean()) {
            bad(std::string(k) + " must be a boolean");
            return false;
        }
        return v->get<bool>();
    }
    std::optional<std::uint64_t> uint(const char* k) {
        const json* v = get(k);
        if (!v) return std::nullopt;
        if (v->is_number_unsigned() || (v->is_number_integer() && v->get<std::int64_t>() >= 0))
            return v->get<std::uint64_t>();
        bad(std::string(k) + " must be a non-negative integer");
        return std::nullopt;
    }
    std::optional<std::int64_t> int64(const char* k) {
        const json* v = get(k);
        if (!v) return std::nullopt;
        if (v->is_number_integer() && !v->is_number_unsigned()) return v->get<std::int64_t>();
        if (v->is_number_unsigned() &&
            v->get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return v->get<std::int64_t>();
        bad(std::string(k) + " must be a 64-bit integer");
        return std::nullopt;
    }
    std::vector<std::string> strs(const char* k) {
        const json* v = get(k);
        std::vector<std::string> out;
        if (!v) return out;
        if (!v->is_array() || !std::all_of(v->begin(), v->end(), [](const json& x) { return x.is_string(); })) {
            bad(std::string(k) + " must be a list of strings");
            return out;
        }
        for (const auto& x : *v) out.push_back(x.get<std::string>());
        return out;
    }
    std::optional<Value> bound(const char* k, VarType t) {
        const json* v = get(k);
        if (!v) return std::nullopt;
        if (t == VarType::Int) {
            return int64(k).has_value() ? std::optional<Value>(Value(*int64(k))) : std::nullopt;
        }
        if (t == VarType::Float && v->is_number()) return Value(v->get<double>());
        if (t == VarType::Duration && v->is_string()) {
            if (auto d = parse_go_duration(v->get<std::string>())) return Value(*d);
        }
        bad(std::string(k) + " is not a " + to_string(t) + " value");
        return std::nullopt;
    }
};

void read_deprecated(Fields& f, std::optional<std::string>& message, std::string& replaced_by) {
    const json* d = f.get("deprecated");
    if (!d) return;
    if (!d->is_object() || !d->contains("message") || !(*d)["message"].is_string()) {
        f.bad("deprecated must be an object with a message");
        return;
    }
    message = (*d)["message"].get<std::string>();
    if (d->contains("replacedBy")) {
        if ((*d)["replacedBy"].is_string()) replaced_by = (*d)["replacedBy"].get<std::string>();
        else f.bad("deprecated.replacedBy must be a string");
    }
}

FileSpec file_from_json(const std::string& name, const json& j, std::vector<std::string>& problems) {
    FileSpec spec;
    spec.name = name;
    if (!j.is_object()) {
        problems.push_back(name + ": must be an object");
        return spec;
    }
    Fields f{name, j, problems};
    std::string type = f.str("type").value_or("");
    static const std::map<std::string, FileType> types = {
        {"config", FileType::Config}, {"tls", FileType::Tls},   {"caBundle", FileType::CaBundle},
        {"keystore", FileType::Keystore}, {"text", FileType::Text}, {"binary", FileType::Binary}};
    auto t = types.find(type);
    if (t == types.end()) {
        f.bad("unknown file type " + detail::quote(type));
        return spec;
    }
    spec.type = t->second;
    spec.description = f.str("description").value_or("");
    spec.details = f.str("details");
    spec.required = f.boolean("required");
    spec.secret = f.boolean("secret");
    spec.path = f.str("path").value_or("");
    spec.path_env = f.str("pathEnv").value_or("");
    spec.reload = f.str("reload").value_or("restart");
    spec.max_size = f.uint("maxSize");
    spec.group = f.str("group").value_or("");
    read_deprecated(f, spec.deprecated, spec.replaced_by);
    spec.format = f.str("format").value_or(spec.type == FileType::Keystore ? "pkcs12" : "");
    if (const json* s = f.get("schema")) spec.schema = *s;
    spec.dns_names = f.strs("dnsNames");
    spec.key_algorithms = f.strs("keyAlgorithms");
    if (auto m = f.str("minRemaining")) {
        spec.min_remaining = parse_go_duration(*m);
        if (!spec.min_remaining) f.bad("minRemaining " + detail::quote(*m) + " is not a Go duration");
    }
    spec.require_ca = f.boolean("requireCA");
    spec.min_certificates = f.uint("minCertificates").value_or(1);
    spec.password_var = f.str("passwordVar").value_or("");
    spec.pattern = f.str("pattern");
    spec.min_length = f.uint("minLength");
    spec.max_length = f.uint("maxLength");
    return spec;
}

Overlay overlay_from_json(const std::string& name, const json& j, std::vector<std::string>& problems) {
    Overlay ov;
    ov.name = name;
    std::string label = "overlay " + name;
    if (!j.is_object()) {
        problems.push_back(label + ": must be an object");
        return ov;
    }
    Fields f{label, j, problems};
    if (!detail::is_input_name(name)) f.bad("name must be a DNS label");
    ov.format = f.str("format").value_or("");
    if (ov.format != "json" && ov.format != "yaml" && ov.format != "toml") f.bad("format must be json, yaml or toml");
    ov.path = f.str("path").value_or("");
    if (ov.path.empty() || ov.path[0] != '/' || ov.path.back() == '/' || ov.path.find("//") != std::string::npos ||
        ov.path.find("/./") != std::string::npos || ov.path.find("/../") != std::string::npos)
        f.bad("path " + detail::quote(ov.path) + " must be absolute and normalised");
    ov.key_separator = f.str("keySeparator").value_or("");
    if (ov.key_separator != ":" && ov.key_separator != ".") f.bad("keySeparator must be \":\" or \".\"");
    ov.reload = f.str("reload").value_or("restart");
    if (ov.reload == "watch")
        f.bad("reload \"watch\" is not supported: docuconf reads overlays once, at boot; use \"restart\"");
    else if (ov.reload != "restart") f.bad("reload must be \"restart\"");
    return ov;
}

VarSpec var_from_json(const std::string& name, const json& j, std::vector<std::string>& problems) {
    VarSpec spec;
    spec.name = name;
    if (!j.is_object()) {
        problems.push_back(name + ": must be an object");
        return spec;
    }
    Fields f{name, j, problems};
    std::string type = f.str("type").value_or("");
    static const std::map<std::string, VarType> types = {
        {"string", VarType::String}, {"int", VarType::Int},   {"float", VarType::Float},
        {"bool", VarType::Bool},     {"duration", VarType::Duration}, {"url", VarType::Url},
        {"enum", VarType::Enum},     {"list", VarType::List}, {"json", VarType::Json},
        {"keySet", VarType::KeySet}};
    auto t = types.find(type);
    if (t == types.end()) {
        f.bad("unknown type " + detail::quote(type));
        return spec;
    }
    spec.type = t->second;
    spec.description = f.str("description").value_or("");
    // Docs only: checked with the declaration (validate_var), never read.
    spec.details = f.str("details");
    spec.required = f.boolean("required");
    spec.secret = f.boolean("secret");
    if (spec.type == VarType::Int || spec.type == VarType::Float || spec.type == VarType::Duration) {
        spec.min = f.bound("min", spec.type);
        spec.max = f.bound("max", spec.type);
    }
    spec.min_length = f.uint("minLength");
    spec.max_length = f.uint("maxLength");
    spec.pattern = f.str("pattern");
    spec.schemes = f.strs("schemes");
    spec.values = f.strs("values");
    spec.examples = f.strs("examples");
    spec.group = f.str("group").value_or("");
    spec.config_key = f.str("configKey").value_or("");
    if (spec.type == VarType::Duration) {
        std::string enc = f.str("encoding").value_or("go");
        if (enc == "go") spec.duration_encoding = DurationEncoding::Go;
        else if (enc == "iso8601") spec.duration_encoding = DurationEncoding::Iso8601;
        else if (enc == "seconds") spec.duration_encoding = DurationEncoding::Seconds;
        else if (enc == "timespan") spec.duration_encoding = DurationEncoding::Timespan;
        else f.bad("unknown duration encoding " + detail::quote(enc));
    }
    if (spec.type == VarType::List || spec.type == VarType::KeySet) {
        if (spec.type == VarType::List) {
            std::string items = f.str("items").value_or("");
            if (items == "string") spec.items = ItemType::String;
            else if (items == "int") spec.items = ItemType::Int;
            else f.bad("list items " + detail::quote(items) + " must be \"string\" or \"int\"");
        } else {
            spec.items = ItemType::String;
            spec.min_keys = f.uint("minKeys").value_or(1);
            spec.max_keys = f.uint("maxKeys").value_or(2);
            spec.key_min_length = f.uint("keyMinLength");
            spec.key_max_length = f.uint("keyMaxLength");
        }
        std::string enc = f.str("encoding").value_or("csv");
        if (enc == "csv") {
            spec.list_encoding = ListEncoding::Csv;
            spec.separator = f.str("separator").value_or(",");
        } else if (enc == "json") {
            spec.list_encoding = ListEncoding::Json;
        } else if (enc == "indexed") {
            spec.list_encoding = ListEncoding::Indexed;
        } else {
            f.bad("unknown list encoding " + detail::quote(enc));
        }
        spec.min_items = f.uint("minItems");
        spec.max_items = f.uint("maxItems");
    }
    spec.item_min = f.int64("itemMin");
    spec.item_max = f.int64("itemMax");
    spec.item_min_length = f.uint("itemMinLength");
    spec.item_max_length = f.uint("itemMaxLength");
    if (spec.type == VarType::Json) {
        if (const json* s = f.get("schema")) spec.schema = *s;
    }
    read_deprecated(f, spec.deprecated, spec.replaced_by);
    if (const json* d = f.get("default")) {
        std::string err;
        auto v = detail::value_from_json(spec, *d, err);
        if (v) spec.default_value = std::move(v);
        else f.bad("default " + err);
    }
    return spec;
}

}  // namespace

Contract Contract::from_json(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) throw DeclarationError({"contract is not valid JSON"});
    return from_json(j);
}

std::ostream& operator<<(std::ostream& os, const Values& values) { return os << values.to_redacted_json().dump(); }

Contract Contract::from_json(const char* text) { return from_json(std::string(text)); }

Contract Contract::from_json(const nlohmann::json& top) {
    if (!top.is_object()) throw DeclarationError({"contract must be a JSON object"});
    if (top.value("kind", std::string()) != "ConfigContract")
        throw DeclarationError({"contract kind must be \"ConfigContract\""});
    if (top.value("apiVersion", std::string()) != "docuconf.dev/v1alpha1")
        throw DeclarationError({"contract apiVersion is not supported; this SDK reads \"docuconf.dev/v1alpha1\""});
    Contract c;
    if (top.contains("metadata") && top["metadata"].is_object())
        c.name_ = top["metadata"].value("name", std::string());
    std::vector<std::string> problems;
    if (top.contains("vars") && !top["vars"].is_null()) {
        if (!top["vars"].is_object()) {
            problems.push_back("vars must be an object");
        } else {
            for (const auto& [name, spec] : top["vars"].items()) {
                std::vector<std::string> p;
                VarSpec v = var_from_json(name, spec, p);
                if (p.empty()) p = detail::validate_var(v);
                problems.insert(problems.end(), p.begin(), p.end());
                c.vars_.push_back(std::move(v));
            }
        }
    }
    std::sort(c.vars_.begin(), c.vars_.end(), [](const VarSpec& a, const VarSpec& b) { return a.name < b.name; });

    if (top.contains("profiles") && top["profiles"].is_object()) {
        const json& p = top["profiles"];
        Profiles prof;
        prof.selector = p.value("selector", std::string());
        prof.default_profile = p.value("default", std::string());
        auto find = [&](const std::string& n) -> const VarSpec* {
            for (const auto& v : c.vars_)
                if (v.name == n) return &v;
            return nullptr;
        };
        if (!find(prof.selector))
            problems.push_back("profile selector " + detail::quote(prof.selector) + " must be a declared variable");
        if (p.contains("defaults") && p["defaults"].is_object()) {
            for (const auto& [profile, vals] : p["defaults"].items()) {
                auto& out = prof.defaults[profile];
                if (!vals.is_object()) continue;
                for (const auto& [k, val] : vals.items()) {
                    const VarSpec* var = find(k);
                    if (!var) {
                        problems.push_back("profile " + profile + ": " + k + " is not a declared variable");
                        continue;
                    }
                    if (var->secret) {
                        problems.push_back("profile " + profile + ": " + k +
                                           " is secret, and a secret has no value in a config file");
                        continue;
                    }
                    std::string err;
                    auto v = detail::value_from_json(*var, val, err);
                    if (!v) {
                        problems.push_back("profile " + profile + ": " + k + ": " + err);
                        continue;
                    }
                    for (const auto& [code, msg] : detail::check_value(*var, *v))
                        problems.push_back("profile " + profile + ": " + k + ": " + msg);
                    out.emplace(k, std::move(*v));
                }
            }
        }
        c.profiles_ = std::move(prof);
    }

    if (top.contains("files") && !top["files"].is_null()) {
        if (!top["files"].is_object()) {
            problems.push_back("files must be an object");
        } else {
            for (const auto& [name, spec] : top["files"].items()) c.files_.push_back(file_from_json(name, spec, problems));
            std::sort(c.files_.begin(), c.files_.end(),
                      [](const FileSpec& a, const FileSpec& b) { return a.name < b.name; });
            auto p = detail::validate_files(c.files_, c.vars_);
            problems.insert(problems.end(), p.begin(), p.end());
        }
    }
    if (top.contains("overlays") && !top["overlays"].is_null()) {
        if (!top["overlays"].is_object()) {
            problems.push_back("overlays must be an object");
        } else {
            for (const auto& [name, spec] : top["overlays"].items())
                c.overlays_.push_back(overlay_from_json(name, spec, problems));
            std::sort(c.overlays_.begin(), c.overlays_.end(),
                      [](const Overlay& a, const Overlay& b) { return a.name < b.name; });
        }
    }
#if !DOCUCONF_FILE_INPUTS
    if (!c.files_.empty() || !c.overlays_.empty())
        problems.push_back("the contract has file inputs or overlays, which this build of docuconf cannot read (it "
                           "was built with DOCUCONF_FILE_INPUTS=OFF)");
#endif
    if (!problems.empty()) throw DeclarationError(std::move(problems));
    return c;
}

Contract& Contract::on_warning(std::function<void(const std::string&)> sink) {
    warn_ = std::move(sink);
    return *this;
}

Values Contract::load(const Env& env) const {
    std::vector<Violation> violations;
    std::vector<std::string> warnings;
    auto root_it = env.find("DOCUCONF_FILE_ROOT");
    std::string root = root_it == env.end() ? "" : root_it->second;
    std::string selector = profiles_ ? profiles_->selector : "";
    auto layers = detail::load_overlays(overlays_, vars_, selector, root, violations, warnings);
    auto values =
        detail::load_vars(vars_, env, profiles_ ? &*profiles_ : nullptr, violations, &warnings, &layers);
    auto loaded = detail::load_files(files_, env, values, root, violations);
    std::map<std::string, std::optional<FileValue>> files;
    for (const auto& f : files_) {
        auto it = loaded.find(f.name);
        if (it == loaded.end() || !it->second.present) {
            files[f.name] = std::nullopt;
            continue;
        }
        const auto& l = it->second;
        if (f.deprecated)
            warnings.push_back(f.name + " is deprecated: " + *f.deprecated +
                               (f.replaced_by.empty() ? "" : "; use " + f.replaced_by));
        files[f.name] = FileValue{f.type, l.path, l.content, l.key, l.ca, l.document, l.certificates};
    }
    for (const auto& w : warnings) {
        if (warn_) warn_(w);
        else std::cerr << "docuconf: warning: " << w << std::endl;
    }
    if (!violations.empty()) throw ValidationError(std::move(violations));
    std::vector<std::string> secrets;
    for (const auto& v : vars_)
        if (v.secret) secrets.push_back(v.name);
    for (const auto& f : files_)
        if (f.secret) secrets.push_back(f.name);
    return Values(std::move(values), std::move(secrets), std::move(files));
}

Values Contract::load_process_env() const {
    Env env = detail::process_env();
    std::set<std::string> declared;
    for (const auto& v : vars_) declared.insert(v.name);
    for (const auto& w : detail::undeclared_hints(env, declared)) std::cerr << w << std::endl;
    try {
        return load(env);
    } catch (const ValidationError& e) {
        detail::write_termination_log(e.violations(), env, true);
        throw;
    }
}

}  // namespace docuconf
