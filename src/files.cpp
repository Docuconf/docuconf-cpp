// File inputs (SPEC §4.6, §11.2 item 7): declaration rules and boot checks.
#include <cerrno>
#include <cmath>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>

#include "internal.hpp"

#if DOCUCONF_FILE_INPUTS
#include <toml++/toml.hpp>
#include <yaml-cpp/yaml.h>
#endif

namespace docuconf {

const char* to_string(FileType t) noexcept {
    switch (t) {
        case FileType::Config: return "config";
        case FileType::Tls: return "tls";
        case FileType::CaBundle: return "caBundle";
        case FileType::Keystore: return "keystore";
        case FileType::Text: return "text";
        case FileType::Binary: return "binary";
    }
    return "?";
}

namespace detail {
namespace {

const std::set<std::string> kReservedDirs = {
    "/",          "/app",       "/bin",       "/boot",      "/dev",       "/etc",           "/etc/pki",
    "/etc/ssl",   "/etc/ssl/certs", "/home",  "/lib",       "/lib64",     "/opt",           "/proc",
    "/root",      "/run",       "/sbin",      "/srv",       "/sys",       "/tmp",           "/usr",
    "/usr/lib",   "/usr/local", "/usr/share", "/var",       "/var/lib",   "/var/run",
};

// An absolute, normalised path: no "..", ".", "//" or trailing slash.
bool is_abs_path(const std::string& p) {
    if (p.size() < 2 || p[0] != '/' || p.back() == '/') return false;
    for (char c : p) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '/' || c == '-';
        if (!ok) return false;
    }
    if (p.find("//") != std::string::npos) return false;
    std::stringstream ss(p.substr(1));
    std::string seg;
    while (std::getline(ss, seg, '/'))
        if (seg == "." || seg == "..") return false;
    return true;
}

std::string parent_dir(const std::string& p) {
    auto slash = p.rfind('/');
    if (slash == 0 || slash == std::string::npos) return "/";
    return p.substr(0, slash);
}

}  // namespace

std::vector<std::string> validate_files(std::vector<FileSpec>& files, const std::vector<VarSpec>& vars) {
    std::vector<std::string> problems;
    std::map<std::string, std::string> mounts;
    std::set<std::string> names, path_envs;
    auto var = [&](const std::string& n) -> const VarSpec* {
        for (const auto& v : vars)
            if (v.name == n) return &v;
        return nullptr;
    };
    for (auto& f : files) {
        auto bad = [&](const std::string& m) { problems.push_back(f.name + ": " + m); };
        if (!is_input_name(f.name)) bad("file input name must be a DNS label: ^[a-z]([-a-z0-9]{0,40}[a-z0-9])?$");
        if (!names.insert(f.name).second) bad("declared twice");
        if (f.description.empty()) bad("needs a description: pass it to add_file(), or the doc comment to .doc()");
        else if (rune_count(f.description) < 5) bad("description must be at least 5 characters");
        if (auto d = check_details(f.details); !d.empty()) bad(d);
        if (!is_abs_path(f.path)) {
            bad("path " + quote(f.path) + " must be absolute and normalised (no ., .., // or trailing /)");
        } else {
            std::string dir = f.type == FileType::Tls ? f.path : parent_dir(f.path);
            if (kReservedDirs.count(dir))
                bad("would be mounted at " + dir + ", which hides what the image has there; use a directory of its own");
            auto [it, fresh] = mounts.emplace(dir, f.name);
            if (!fresh) bad("shares its mount directory " + dir + " with " + it->second);
        }
        if (!f.path_env.empty()) {
            if (!is_env_name(f.path_env)) bad("pathEnv must match ^[A-Z][A-Z0-9_]*$");
            if (var(f.path_env)) bad("pathEnv " + f.path_env + " must not also be a declared variable");
            if (!path_envs.insert(f.path_env).second) bad("pathEnv " + f.path_env + " is used by another file input");
        }
        if (f.reload != "restart" && f.reload != "watch") bad("reload must be \"restart\" or \"watch\"");
        if (f.max_size && *f.max_size == 0) bad("maxSize must be positive");
        if (f.deprecated) {
            if (auto d = check_deprecated(*f.deprecated); !d.empty()) bad(d);
            if (f.required)
                bad("a required file input cannot be deprecated: the platform could not stop supplying it; make it "
                    "optional first");
            if (!f.replaced_by.empty() && !is_input_name(f.replaced_by)) bad("replacedBy must be a file input name");
        }
        switch (f.type) {
            case FileType::Config:
                if (f.format != "json" && f.format != "yaml" && f.format != "toml")
                    bad("config format must be json, yaml or toml");
                break;
            case FileType::Tls:
                f.secret = true;
                for (const auto& a : f.key_algorithms)
                    if (a != "RSA" && a != "ECDSA" && a != "Ed25519")
                        bad("key algorithm " + quote(a) + " must be RSA, ECDSA or Ed25519");
                break;
            case FileType::CaBundle:
                if (f.min_certificates < 1) bad("minCertificates must be at least 1");
                break;
            case FileType::Keystore:
                f.secret = true;
                if (f.format != "pkcs12" && f.format != "jks") bad("keystore format must be pkcs12 or jks");
                if (!f.password_var.empty()) {
                    const VarSpec* v = var(f.password_var);
                    if (!v) bad("passwordVar " + f.password_var + " is not a declared variable");
                    else if (!v->secret) bad("passwordVar " + f.password_var + " must be a secret variable");
                    else if (v->type != VarType::String) bad("passwordVar " + f.password_var + " must be a string");
                }
                break;
            case FileType::Text:
                if (f.pattern) {
                    std::string err;
                    f.compiled = compile_pattern(*f.pattern, err);
                    if (!f.compiled) bad(err);
                }
                if (f.min_length && f.max_length && *f.min_length > *f.max_length) bad("minLength is above maxLength");
                break;
            case FileType::Binary: break;
        }
    }
    return problems;
}

#if DOCUCONF_FILE_INPUTS

namespace {

using nlohmann::json;

struct Ctx {
    const FileSpec& f;
    std::vector<Violation>& out;
    void v(Code c, const std::string& m) { out.push_back(Violation{f.name, c, m}); }
};

enum class Read { Ok, Absent, Failed };

// Reads one file. A missing file is file_missing when `must` is set, and
// simply absent otherwise.
Read read_file(Ctx& c, const std::string& path, bool must, std::string& content) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            if (!must) return Read::Absent;
            c.v(Code::FileMissing, path + " does not exist");
        } else {
            c.v(Code::FileUnreadable, path + " cannot be read: " + std::strerror(errno));
        }
        return Read::Failed;
    }
    if (S_ISDIR(st.st_mode)) {
        c.v(Code::FileUnreadable, path + " is a directory, not a file");
        return Read::Failed;
    }
    if (c.f.max_size && static_cast<std::uint64_t>(st.st_size) > *c.f.max_size) {
        c.v(Code::FileTooLarge,
            path + " is " + std::to_string(st.st_size) + " bytes, above maxSize " + std::to_string(*c.f.max_size));
        return Read::Failed;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        int e = errno;
        std::string hint = e == EACCES ? " (for a secret volume and a non-root user, set the pod's fsGroup)" : "";
        c.v(Code::FileUnreadable, path + " cannot be read: " + std::strerror(e) + hint);
        return Read::Failed;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        c.v(Code::FileUnreadable, path + " cannot be read");
        return Read::Failed;
    }
    content = ss.str();
    return Read::Ok;
}

bool is_core_int(const std::string& s) {
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

json yaml_scalar(const YAML::Node& n) {
    const std::string& s = n.Scalar();
    const std::string& tag = n.Tag();
    if (tag == "!" || tag == "tag:yaml.org,2002:str") return s;  // quoted or !!str
    // YAML 1.2 core schema.
    if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") return nullptr;
    if (s == "true" || s == "True" || s == "TRUE") return true;
    if (s == "false" || s == "False" || s == "FALSE") return false;
    if (is_core_int(s)) {
        json j = json::parse(s[0] == '+' ? s.substr(1) : s, nullptr, false);
        if (!j.is_discarded()) return j;
    }
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'o')) {
        try {
            std::size_t pos = 0;
            long long v = std::stoll(s.substr(2), &pos, s[1] == 'x' ? 16 : 8);
            if (pos == s.size() - 2) return v;
        } catch (...) {
        }
    }
    if (s == ".inf" || s == ".Inf" || s == ".INF" || s == "+.inf") return std::numeric_limits<double>::infinity();
    if (s == "-.inf" || s == "-.Inf" || s == "-.INF") return -std::numeric_limits<double>::infinity();
    if (s == ".nan" || s == ".NaN" || s == ".NAN") return std::numeric_limits<double>::quiet_NaN();
    {
        json j = json::parse(s, nullptr, false);
        if (!j.is_discarded() && j.is_number()) return j;
        // ".5" and "1." are YAML floats but not JSON numbers.
        char* end = nullptr;
        if (s.find_first_of("0123456789") != std::string::npos &&
            s.find_first_not_of("+-.0123456789eE") == std::string::npos) {
            double d = std::strtod(s.c_str(), &end);
            if (end && *end == '\0') return d;
        }
    }
    return s;
}

json yaml_to_json(const YAML::Node& n) {
    switch (n.Type()) {
        case YAML::NodeType::Null:
        case YAML::NodeType::Undefined: return nullptr;
        case YAML::NodeType::Scalar: return yaml_scalar(n);
        case YAML::NodeType::Sequence: {
            json a = json::array();
            for (const auto& x : n) a.push_back(yaml_to_json(x));
            return a;
        }
        case YAML::NodeType::Map: {
            json o = json::object();
            for (const auto& kv : n) o[kv.first.as<std::string>()] = yaml_to_json(kv.second);
            return o;
        }
    }
    return nullptr;
}

json toml_to_json(const toml::node& n) {
    if (auto t = n.as_table()) {
        json o = json::object();
        for (const auto& [k, v] : *t) o[std::string(k.str())] = toml_to_json(v);
        return o;
    }
    if (auto a = n.as_array()) {
        json arr = json::array();
        for (const auto& v : *a) arr.push_back(toml_to_json(v));
        return arr;
    }
    if (auto s = n.as_string()) return s->get();
    if (auto i = n.as_integer()) return i->get();
    if (auto f = n.as_floating_point()) return f->get();
    if (auto b = n.as_boolean()) return b->get();
    std::ostringstream ss;
    n.visit([&](const auto& x) { ss << x; });
    return ss.str();
}

}  // namespace

#endif

nlohmann::json parse_structured(const std::string& format, const std::string& content) {
    std::string text = content;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // a UTF-8 byte-order mark
    if (format == "json") return nlohmann::json::parse(text);
#if DOCUCONF_FILE_INPUTS
    if (format == "yaml") return yaml_to_json(YAML::Load(text));
    if (format == "toml") return toml_to_json(toml::parse(text));
#endif
    throw std::runtime_error("this build of docuconf cannot read " + format +
                             " (it was built with DOCUCONF_FILE_INPUTS=OFF)");
}

std::string under_root(const std::string& file_root, const std::string& path) {
    if (file_root.empty() || path.empty() || path[0] != '/') return path;
    std::string root = file_root;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    return root + path;
}

std::string resolved_path(const FileSpec& f, const Env& env, const std::string& file_root) {
    std::string path = f.path;
    if (!f.path_env.empty()) {
        auto it = env.find(f.path_env);
        if (it != env.end() && !it->second.empty()) path = it->second;
    }
    return under_root(file_root, path);
}

std::vector<std::string> watch_paths(const FileSpec& f, const std::string& resolved) {
    if (f.type == FileType::Tls) return {resolved + "/tls.crt", resolved + "/tls.key", resolved + "/ca.crt"};
    return {resolved};
}

std::string stat_fingerprint(const std::vector<std::string>& paths) {
    std::string out;
    for (const auto& p : paths) {
        struct stat st {};
        // stat follows symlinks: when Kubernetes swaps the ..data symlink of
        // a volume, the file it now points to has another inode.
        if (::stat(p.c_str(), &st) != 0) {
            out += "-;";
            continue;
        }
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(p, ec);
        out += std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" + std::to_string(st.st_size) + ":" +
               (ec ? std::string("?") : std::to_string(mtime.time_since_epoch().count())) + ";";
    }
    return out;
}

namespace {

std::string json_kind(const nlohmann::json& j) {
    if (j.is_object()) return "an object";
    if (j.is_array()) return "a list";
    if (j.is_string()) return "a string";
    if (j.is_boolean()) return "a bool";
    if (j.is_number()) return "a number";
    return "null";
}

// A native scalar as the env value it stands for (SPEC §4.7): a string as
// it is, a bool as true or false, a number with an integral value as a
// base-10 integer (50.0 is 50), any other in shortest round-trip decimal.
std::optional<std::string> scalar_text(const nlohmann::json& x) {
    if (x.is_string()) return x.get<std::string>();
    if (x.is_boolean()) return std::string(x.get<bool>() ? "true" : "false");
    if (x.is_number_unsigned()) return std::to_string(x.get<std::uint64_t>());
    if (x.is_number_integer()) return std::to_string(x.get<std::int64_t>());
    if (x.is_number_float()) {
        double f = x.get<double>();
        if (std::isfinite(f) && std::trunc(f) == f && std::fabs(f) < 9223372036854775808.0)
            return std::to_string(static_cast<std::int64_t>(f));
        return format_float(f);
    }
    return std::nullopt;
}

// The value at a key path, matching keys exactly; nullptr when absent.
const nlohmann::json* lookup_key(const nlohmann::json& doc, const std::string& key, const std::string& sep) {
    const nlohmann::json* cur = &doc;
    std::size_t start = 0;
    while (true) {
        std::size_t p = key.find(sep, start);
        std::string part = key.substr(start, p == std::string::npos ? std::string::npos : p - start);
        if (!cur->is_object()) return nullptr;
        auto it = cur->find(part);
        if (it == cur->end()) return nullptr;
        cur = &*it;
        if (p == std::string::npos) return cur;
        start = p + sep.size();
    }
}

// The overlay's data: nullopt when it is missing (an overlay is optional)
// or does not read or parse (reported in `violations`).
std::optional<nlohmann::json> read_overlay(const Overlay& ov, const std::string& file_root,
                                           std::vector<Violation>& violations) {
    std::string path = under_root(file_root, ov.path);
    auto fail = [&](Code code, const std::string& m) { violations.push_back(Violation{ov.name, code, m}); };
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (errno != ENOENT && errno != ENOTDIR) fail(Code::FileUnreadable, path + " cannot be read: " + std::strerror(errno));
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in || S_ISDIR(st.st_mode)) {
        fail(Code::FileUnreadable, path + " cannot be read");
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    nlohmann::json doc;
    try {
        doc = parse_structured(ov.format, ss.str());
    } catch (const std::exception& e) {
        fail(Code::FileMalformed, path + " is not valid " + ov.format + ": " + e.what());
        return std::nullopt;
    }
    if (!doc.is_object()) {
        fail(Code::FileMalformed, path + " does not hold an object at its top level");
        return std::nullopt;
    }
    return doc;
}

}  // namespace

std::map<std::string, Layer> load_overlays(const std::vector<Overlay>& overlays, const std::vector<VarSpec>& vars,
                                           const std::string& selector, const std::string& file_root,
                                           std::vector<Violation>& violations, std::vector<std::string>& warnings,
                                           std::map<std::string, std::optional<nlohmann::json>>* docs) {
    std::map<std::string, Layer> out;
    for (const auto& ov : overlays) {
        std::optional<nlohmann::json> doc;
        auto pinned = docs ? docs->find(ov.name) : std::map<std::string, std::optional<nlohmann::json>>::iterator{};
        if (docs && pinned != docs->end()) {
            doc = pinned->second;
        } else {
            std::size_t before = violations.size();
            doc = read_overlay(ov, file_root, violations);
            if (docs && violations.size() == before) (*docs)[ov.name] = doc;
        }
        if (!doc) continue;
        std::string source = "overlay " + ov.name;
        for (const auto& v : vars) {
            if (v.config_key.empty() || v.name == selector) continue;
            const nlohmann::json* val = lookup_key(*doc, v.config_key, ov.key_separator);
            if (!val || val->is_null()) continue;  // null is unset
            if (auto prev = out.find(v.name); prev != out.end()) {
                warnings.push_back(v.name + " is set in " + prev->second.source + " and in " + source +
                                   "; the first wins");
                continue;
            }
            Layer l;
            l.source = source;
            auto bad = [&](const std::string& m) {
                violations.push_back(Violation{v.name, Code::InvalidType, source + ", at " + v.config_key + ": " + m});
                l.bad = true;
            };
            if (v.secret) {
                // Never print it: the value is secret material in a ConfigMap.
                bad("is secret, and a secret is never read from an overlay; supply it through the environment");
            } else if (v.type == VarType::Json) {
                l.raws.push_back(val->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            } else if (v.type == VarType::List || v.type == VarType::KeySet) {
                if (!val->is_array()) {
                    bad("is " + json_kind(*val) + ", not a list");
                } else {
                    l.items = true;
                    for (std::size_t i = 0; i < val->size(); ++i) {
                        auto t = scalar_text((*val)[i]);
                        if (!t) {
                            bad("item " + std::to_string(i) + " is " + json_kind((*val)[i]) + ", not a scalar");
                            break;
                        }
                        l.raws.push_back(*t);
                    }
                }
            } else if (auto t = scalar_text(*val)) {
                // An empty value is unset, as in the environment.
                if (t->empty() && v.type != VarType::String) continue;
                l.raws.push_back(*t);
            } else {
                bad("is " + json_kind(*val) + ", not a scalar");
            }
            out[v.name] = std::move(l);
        }
    }
    return out;
}

#if DOCUCONF_FILE_INPUTS

namespace {

void check_config(Ctx& c, LoadedFile& lf) {
    json doc;
    try {
        doc = parse_structured(c.f.format, lf.content);
    } catch (const std::exception& e) {
        std::string why = e.what();
        if (c.f.secret) why = "parse error";
        c.v(Code::FileMalformed, "is not valid " + c.f.format + ": " + why);
        return;
    }
    if (c.f.schema) {
        auto msgs = validate_schema(*c.f.schema, doc, c.f.secret);
        for (const auto& m : msgs) c.v(Code::SchemaMismatch, m);
        if (!msgs.empty()) return;
    }
    if (c.f.bind) {
        std::string e = c.f.bind(doc);
        if (!e.empty()) {
            c.v(Code::SchemaMismatch, "does not bind to the app's type" + (c.f.secret ? "" : ": " + e));
            return;
        }
    }
    lf.document = std::move(doc);
}

void check_text(Ctx& c, const std::string& s) {
    if (!valid_utf8(s)) {
        c.v(Code::FileMalformed, "is not valid UTF-8 text");
        return;
    }
    std::uint64_t n = rune_count(s);
    if (c.f.min_length && n < *c.f.min_length)
        c.v(Code::OutOfRange, "is " + std::to_string(n) + " characters, below minLength " + std::to_string(*c.f.min_length));
    if (c.f.max_length && n > *c.f.max_length)
        c.v(Code::OutOfRange, "is " + std::to_string(n) + " characters, above maxLength " + std::to_string(*c.f.max_length));
    if (c.f.compiled && !c.f.compiled->matches(s)) c.v(Code::PatternMismatch, "does not match pattern " + c.f.compiled->source);
}

}  // namespace

std::map<std::string, LoadedFile> load_files(const std::vector<FileSpec>& files, const Env& env,
                                             const std::map<std::string, std::optional<Value>>& vars,
                                             const std::string& file_root, std::vector<Violation>& violations) {
    std::map<std::string, LoadedFile> out;
    for (const auto& f : files) {
        LoadedFile& lf = out[f.name];
        std::string path = resolved_path(f, env, file_root);
        lf.path = path;
        Ctx c{f, violations};
        std::size_t before = violations.size();

        if (f.type == FileType::Tls) {
            struct stat st {};
            if (::stat(path.c_str(), &st) != 0) {
                if (f.required) c.v(Code::FileMissing, path + " does not exist");
                continue;
            }
            if (!S_ISDIR(st.st_mode)) {
                c.v(Code::FileUnreadable, path + " is not a directory holding tls.crt and tls.key");
                continue;
            }
            Read r1 = read_file(c, path + "/tls.crt", true, lf.content);
            Read r2 = read_file(c, path + "/tls.key", true, lf.key);
            Read r3 = Read::Ok;
            if (f.require_ca) r3 = read_file(c, path + "/ca.crt", true, lf.ca);
            if (r1 != Read::Ok || r2 != Read::Ok || r3 != Read::Ok) continue;
            for (auto& [code, msg] : check_tls(f, lf.content, lf.key, f.require_ca ? &lf.ca : nullptr)) c.v(code, msg);
            if (violations.size() == before) lf.present = true;
            continue;
        }

        Read r = read_file(c, path, f.required, lf.content);
        if (r != Read::Ok) continue;
        switch (f.type) {
            case FileType::Config: check_config(c, lf); break;
            case FileType::Text: check_text(c, lf.content); break;
            case FileType::CaBundle:
                for (auto& [code, msg] : check_ca_bundle(f, lf.content, lf.certificates)) c.v(code, msg);
                break;
            case FileType::Keystore: {
                std::string password;
                if (!f.password_var.empty()) {
                    auto it = vars.find(f.password_var);
                    if (it != vars.end() && it->second && it->second->is_string()) password = it->second->as_string();
                }
                for (auto& [code, msg] : check_keystore(f, lf.content, password)) c.v(code, msg);
                break;
            }
            case FileType::Binary:
            case FileType::Tls: break;
        }
        if (violations.size() == before) lf.present = true;
    }
    return out;
}

#else

// The environment-only build has no add_file, so there is nothing to load.
std::map<std::string, LoadedFile> load_files(const std::vector<FileSpec>&, const Env&,
                                             const std::map<std::string, std::optional<Value>>&,
                                             const std::string&, std::vector<Violation>&) {
    return {};
}

#endif

}  // namespace detail
}  // namespace docuconf
