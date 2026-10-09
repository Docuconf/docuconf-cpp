// Contract export (SPEC §4): the declaration as CUE data, deterministic,
// with variables and file inputs sorted by name.
#include <algorithm>
#include <sstream>

#include "docuconf/declaration.hpp"
#include "internal.hpp"

#ifndef DOCUCONF_VERSION
#define DOCUCONF_VERSION "0.0.0"
#endif

namespace docuconf {
namespace {

using ojson = nlohmann::ordered_json;

ojson value_json(const Value& v) {
    if (v.is_float()) return v.as_float();
    if (v.is_json()) return ojson::parse(v.as_json().dump());
    return ojson::parse(v.to_json().dump());
}

ojson var_json(const VarSpec& s) {
    ojson o;
    o["type"] = to_string(s.type);
    o["description"] = s.description;
    if (s.details) o["details"] = *s.details;
    if (s.required) o["required"] = true;
    if (s.secret) o["secret"] = true;
    if (!s.group.empty()) o["group"] = s.group;
    if (!s.examples.empty()) o["examples"] = s.examples;
    if (s.deprecated) {
        ojson d;
        d["message"] = *s.deprecated;
        if (!s.replaced_by.empty()) d["replacedBy"] = s.replaced_by;
        o["deprecated"] = d;
    }
    if (!s.config_key.empty()) o["configKey"] = s.config_key;
    if (s.default_value) o["default"] = value_json(*s.default_value);
    if (s.min) o["min"] = value_json(*s.min);
    if (s.max) o["max"] = value_json(*s.max);
    if (s.min_length) o["minLength"] = *s.min_length;
    if (s.max_length) o["maxLength"] = *s.max_length;
    if (s.pattern) o["pattern"] = *s.pattern;
    if (!s.schemes.empty()) o["schemes"] = s.schemes;
    if (s.type == VarType::Enum) o["values"] = s.values;
    if (s.type == VarType::Duration) o["encoding"] = to_string(s.duration_encoding);
    if (s.type == VarType::List) {
        o["items"] = s.items == ItemType::Int ? "int" : "string";
        o["encoding"] = to_string(s.list_encoding);
        if (s.list_encoding == ListEncoding::Csv) o["separator"] = s.separator;
        if (s.min_items) o["minItems"] = *s.min_items;
        if (s.max_items) o["maxItems"] = *s.max_items;
        if (s.item_min) o["itemMin"] = *s.item_min;
        if (s.item_max) o["itemMax"] = *s.item_max;
        if (s.item_min_length) o["itemMinLength"] = *s.item_min_length;
        if (s.item_max_length) o["itemMaxLength"] = *s.item_max_length;
    }
    if (s.type == VarType::KeySet) {
        o["encoding"] = to_string(s.list_encoding);
        if (s.list_encoding == ListEncoding::Csv) o["separator"] = s.separator;
        o["minKeys"] = s.min_keys;
        o["maxKeys"] = s.max_keys;
        if (s.key_min_length) o["keyMinLength"] = *s.key_min_length;
        if (s.key_max_length) o["keyMaxLength"] = *s.key_max_length;
    }
    if (s.schema) o["schema"] = ojson::parse(s.schema->dump());
    return o;
}

ojson file_json(const FileSpec& s) {
    ojson o;
    o["type"] = to_string(s.type);
    if (s.type == FileType::Config || s.type == FileType::Keystore) o["format"] = s.format;
    o["description"] = s.description;
    if (s.details) o["details"] = *s.details;
    if (s.required) o["required"] = true;
    if (s.secret) o["secret"] = true;
    o["path"] = s.path;
    if (!s.path_env.empty()) o["pathEnv"] = s.path_env;
    if (s.reload != "restart") o["reload"] = s.reload;
    if (s.max_size) o["maxSize"] = *s.max_size;
    if (!s.group.empty()) o["group"] = s.group;
    if (s.deprecated) {
        ojson d;
        d["message"] = *s.deprecated;
        if (!s.replaced_by.empty()) d["replacedBy"] = s.replaced_by;
        o["deprecated"] = d;
    }
    switch (s.type) {
        case FileType::Tls:
            if (!s.dns_names.empty()) o["dnsNames"] = s.dns_names;
            if (!s.key_algorithms.empty()) o["keyAlgorithms"] = s.key_algorithms;
            if (s.min_remaining) o["minRemaining"] = format_go_duration(*s.min_remaining);
            if (s.require_ca) o["requireCA"] = true;
            break;
        case FileType::CaBundle:
            if (s.min_certificates != 1) o["minCertificates"] = s.min_certificates;
            break;
        case FileType::Keystore:
            if (!s.password_var.empty()) o["passwordVar"] = s.password_var;
            break;
        case FileType::Text:
            if (s.pattern) o["pattern"] = *s.pattern;
            if (s.min_length) o["minLength"] = *s.min_length;
            if (s.max_length) o["maxLength"] = *s.max_length;
            break;
        case FileType::Config:
            if (s.schema) o["schema"] = ojson::parse(s.schema->dump());
            break;
        case FileType::Binary: break;
    }
    return o;
}

const char* const kKeywords[] = {"package", "import", "for", "in", "if", "let", "true", "false", "null", "func"};

std::string label(const std::string& k) {
    bool ident = !k.empty() && (std::isalpha(static_cast<unsigned char>(k[0])));
    for (char c : k)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) ident = false;
    for (const char* kw : kKeywords)
        if (k == kw) ident = false;
    return ident ? k : detail::quote(k);
}

std::string number(const ojson& v) {
    if (v.is_number_float()) {
        std::string s = detail::format_float(v.get<double>());
        if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
        return s;
    }
    return v.dump();
}

void emit(std::ostringstream& out, const ojson& v, int depth) {
    std::string pad(static_cast<std::size_t>(depth), '\t');
    if (v.is_object()) {
        if (v.empty()) {
            out << "{}";
            return;
        }
        out << "{\n";
        for (const auto& [k, x] : v.items()) {
            out << pad << '\t' << label(k) << ": ";
            emit(out, x, depth + 1);
            out << "\n";
        }
        out << pad << "}";
    } else if (v.is_array()) {
        out << "[";
        bool first = true;
        for (const auto& x : v) {
            if (!first) out << ", ";
            first = false;
            emit(out, x, depth);
        }
        out << "]";
    } else if (v.is_string()) {
        out << detail::quote(v.get<std::string>());
    } else if (v.is_number()) {
        out << number(v);
    } else {
        out << v.dump();
    }
}

std::string package_name(const std::string& service) {
    std::string p;
    for (char c : service) p += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    if (p.empty() || std::isdigit(static_cast<unsigned char>(p[0]))) p = "svc_" + p;
    return p;
}

}  // namespace

nlohmann::ordered_json Declaration::export_json() const {
    const_cast<Declaration*>(this)->check();
    ojson c;
    c["apiVersion"] = "docuconf.dev/v1alpha1";
    c["kind"] = "ConfigContract";
    ojson meta;
    meta["name"] = service_;
    if (!app_version_.empty()) meta["appVersion"] = app_version_;
    meta["generator"] = {{"language", "cpp"}, {"sdk", "docuconf-cpp"}, {"version", DOCUCONF_VERSION}};
    c["metadata"] = meta;
    auto vars = var_specs();
    std::sort(vars.begin(), vars.end(), [](const VarSpec& a, const VarSpec& b) { return a.name < b.name; });
    ojson vj = ojson::object();
    for (const auto& v : vars) vj[v.name] = var_json(v);
    c["vars"] = vj;
    auto files = file_specs();
    if (!files.empty()) {
        std::sort(files.begin(), files.end(), [](const FileSpec& a, const FileSpec& b) { return a.name < b.name; });
        ojson fj = ojson::object();
        for (const auto& f : files) fj[f.name] = file_json(f);
        c["files"] = fj;
    }
    return c;
}

std::string Declaration::export_cue() const {
    ojson c = export_json();
    std::ostringstream out;
    out << "// Code generated by docuconf. DO NOT EDIT.\n";
    out << "package " << package_name(service_) << "\n\n";
    out << "import \"docuconf.dev/contract\"\n\n";
    out << "contract.#Contract & ";
    emit(out, c, 0);
    out << "\n";
    return out.str();
}

}  // namespace docuconf
