// File inputs (SPEC §4.6, §11.2 item 7): declaration rules and boot checks.
#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>

#include <toml++/toml.hpp>
#include <yaml-cpp/yaml.h>

#include "internal.hpp"

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
        if (rune_count(f.description) < 5) bad("description must be at least 5 characters");
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
        if (f.reload == "watch") {
            bad("reload \"watch\" is not supported: docuconf reads file inputs once, at boot; use \"restart\"");
        } else if (f.reload != "restart") {
            bad("reload must be \"restart\"");
        }
        if (f.max_size && *f.max_size == 0) bad("maxSize must be positive");
        if (f.deprecated && !f.replaced_by.empty() && !is_input_name(f.replaced_by))
            bad("replacedBy must be a file input name");
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

void check_config(Ctx& c, LoadedFile& lf) {
    std::string text = lf.content;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // a UTF-8 byte-order mark
    json doc;
    try {
        if (c.f.format == "json") {
            doc = json::parse(text);
        } else if (c.f.format == "yaml") {
            doc = yaml_to_json(YAML::Load(text));
        } else {
            doc = toml_to_json(toml::parse(text));
        }
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
        std::string path = f.path;
        if (!f.path_env.empty()) {
            auto it = env.find(f.path_env);
            if (it != env.end() && !it->second.empty()) path = it->second;
        }
        if (!file_root.empty() && !path.empty() && path[0] == '/') {
            std::string root = file_root;
            while (root.size() > 1 && root.back() == '/') root.pop_back();
            path = root + path;
        }
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

}  // namespace detail
}  // namespace docuconf
