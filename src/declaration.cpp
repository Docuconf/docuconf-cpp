#include "docuconf/declaration.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

#include "internal.hpp"

namespace docuconf {

Declaration::Declaration(CLI::App& app, std::string service) : app_(app), service_(std::move(service)) {
    warn_ = [](const std::string& m) {
        if (m.rfind("docuconf: ", 0) == 0) std::cerr << m << std::endl;
        else std::cerr << "docuconf: warning: " << m << std::endl;
    };
    app_.add_option(kExportFlag, export_path_,
                    "Write the configuration contract (contract.cue) to this path, or - for standard output, "
                    "and exit without reading the environment")
        ->type_name("PATH")
        ->group("docuconf");
    // --help lists every variable and file input, which are read from the
    // environment and the filesystem rather than the command line.
    app_.footer([this] {
        try {
            check();
        } catch (const std::exception&) {
            // The declaration is reported when the app parses; list what is there.
        }
        return help_footer();
    });
}

Declaration::~Declaration() = default;

Declaration& Declaration::app_version(std::string v) {
    app_version_ = std::move(v);
    return *this;
}

void Declaration::on_warning(std::function<void(const std::string&)> sink) { warn_ = std::move(sink); }

void Declaration::before_declare(const std::string& name) {
    if (loaded_)
        throw DeclarationError({name + ": declared after the configuration was loaded; declare every variable and "
                                       "file input before DOCUCONF_PARSE (or parse()/load())"});
}

namespace {

std::string upper_type(VarType t) {
    std::string s = to_string(t);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::vector<std::string> split_names(const std::string& names) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : names) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != ' ') {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

void VarBase::make_flag(std::string names, FlagKind kind) {
    const std::string& name = spec_.name;
    if (option_) {
        problems_.push_back(name + ": flag() is called twice");
        return;
    }
    if (names.empty()) {
        names = "--";
        for (char c : name) names += c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string long_name;
    for (const auto& n : split_names(names))
        if (n.rfind("--", 0) == 0 && long_name.empty()) long_name = n;
    if (kind == FlagKind::Bool && !long_name.empty() && names.find('!') == std::string::npos)
        names += ",!--no-" + long_name.substr(2);
    CLI::App& app = decl_->app_;
    for (auto n : split_names(names)) {
        if (!n.empty() && n[0] == '!') n.erase(0, 1);
        if (n.empty() || n[0] != '-') continue;
        if (app.get_option_no_throw(n) != nullptr) {
            problems_.push_back(name + ": flag " + n +
                                " is already a CLI11 option; remove the app.add_option(\"" + n +
                                "\", ...) or app.add_flag call and keep this variable, or give flag() another "
                                "name");
            return;
        }
    }
    try {
        switch (kind) {
            case FlagKind::Bool: option_ = app.add_flag(names); break;
            case FlagKind::List: option_ = app.add_option(names, raw_items_, spec_.description); break;
            case FlagKind::Scalar: option_ = app.add_option(names, raw_, spec_.description); break;
        }
    } catch (const CLI::Error& e) {
        problems_.push_back(name + ": flag(\"" + names + "\"): " + e.what());
        option_ = nullptr;
        return;
    }
    option_->description(spec_.description);
    if (kind != FlagKind::Bool) option_->type_name(upper_type(spec_.type));
    if (!spec_.group.empty()) option_->group(spec_.group);
    flag_names_ = long_name.empty() ? split_names(names).front() : long_name;
}

File& Declaration::new_file(std::string name, FileType type, std::string description) {
    before_declare(name);
    auto f = std::make_unique<File>();
    f->spec_.name = std::move(name);
    f->spec_.type = type;
    f->spec_.description = std::move(description);
    if (type == FileType::Tls || type == FileType::Keystore) f->spec_.secret = true;
    if (type == FileType::Keystore) f->spec_.format = "pkcs12";
    File& raw = *f;
    files_.push_back(std::move(f));
    checked_ = false;
    return raw;
}

namespace {
const detail::LoadedFile& lf(const void* p) { return *static_cast<const detail::LoadedFile*>(p); }
}  // namespace

bool Declaration::loaded_present(const void* p) { return lf(p).present; }
const std::string& Declaration::loaded_path(const void* p) { return lf(p).path; }
const nlohmann::json& Declaration::loaded_document(const void* p) { return lf(p).document; }

#if DOCUCONF_FILE_INPUTS
File& Declaration::add_file(std::string name, TlsKeyPair& target, std::string description) {
    File& f = new_file(std::move(name), FileType::Tls, std::move(description));
    f.assign_ = [&target](const void* p) {
        const auto& l = lf(p);
        target = TlsKeyPair{};
        target.present = l.present;
        if (!l.present) return;
        target.dir = l.path;
        target.certificate_pem = l.content;
        target.key_pem = l.key;
        target.ca_pem = l.ca;
    };
    return f;
}

File& Declaration::add_file(std::string name, CaBundle& target, std::string description) {
    File& f = new_file(std::move(name), FileType::CaBundle, std::move(description));
    f.assign_ = [&target](const void* p) {
        const auto& l = lf(p);
        target = CaBundle{};
        target.present = l.present;
        if (!l.present) return;
        target.path = l.path;
        target.pem = l.content;
        target.certificates = l.certificates;
    };
    return f;
}

File& Declaration::add_file(std::string name, Keystore& target, std::string description) {
    File& f = new_file(std::move(name), FileType::Keystore, std::move(description));
    f.assign_ = [&target](const void* p) {
        const auto& l = lf(p);
        target = Keystore{};
        target.present = l.present;
        if (!l.present) return;
        target.path = l.path;
        target.data = l.content;
    };
    return f;
}

File& Declaration::add_file(std::string name, TextFile& target, std::string description) {
    File& f = new_file(std::move(name), FileType::Text, std::move(description));
    f.assign_ = [&target](const void* p) {
        const auto& l = lf(p);
        target = TextFile{};
        target.present = l.present;
        if (!l.present) return;
        target.path = l.path;
        target.content = l.content;
    };
    return f;
}

File& Declaration::add_file(std::string name, BinaryFile& target, std::string description) {
    File& f = new_file(std::move(name), FileType::Binary, std::move(description));
    f.assign_ = [&target](const void* p) {
        const auto& l = lf(p);
        target = BinaryFile{};
        target.present = l.present;
        if (!l.present) return;
        target.path = l.path;
        target.data = l.content;
    };
    return f;
}
#endif

namespace {

bool is_dns_label(const std::string& s) {
    if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-') return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// A default as it would be written in the environment, for --help.
std::string display(const VarSpec& spec, const Value& v) {
    if (v.is_string()) return v.as_string();
    if (v.is_list()) {
        std::string out;
        for (const auto& x : v.as_list()) {
            if (!out.empty()) out += spec.separator;
            out += x.is_string() ? x.as_string() : x.to_json().dump();
        }
        return out;
    }
    auto j = v.to_json();
    return j.is_string() ? j.get<std::string>() : j.dump();
}

bool looks_like_feature_flag(const std::string& name) {
    for (const char* p : {"FF_", "FEATURE_", "FEATURE_FLAG_", "ENABLE_"})
        if (name.rfind(p, 0) == 0) return true;
    return false;
}

std::string extension_format(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    if (ext == "json") return "json";
    if (ext == "yaml" || ext == "yml") return "yaml";
    if (ext == "toml") return "toml";
    return "";
}

}  // namespace

void Declaration::check() {
    if (checked_) return;
    std::vector<std::string> problems = problems_;
    if (!is_dns_label(service_))
        problems.push_back("service name " + detail::quote(service_) + " must be a DNS label ([a-z0-9-], at most 63)");
    std::set<std::string> names;
    for (auto& v : vars_) {
        if (!v->finalized_) {
            v->finalize();
            v->finalized_ = true;
        }
        problems.insert(problems.end(), v->problems_.begin(), v->problems_.end());
        auto p = detail::validate_var(v->spec_);
        problems.insert(problems.end(), p.begin(), p.end());
        if (!names.insert(v->spec_.name).second)
            problems.push_back(v->spec_.name + ": declared twice; remove one of the add_var calls");
        if (v->option_ && v->spec_.secret)
            problems.push_back(v->spec_.name + ": a secret cannot be a command-line flag (" + v->flag_names_ +
                               "): it would show in ps and shell history; remove .flag() and set it in the "
                               "environment");
        if (v->option_ && v->spec_.default_value)
            v->option_->default_str(display(v->spec_, *v->spec_.default_value));
        if (looks_like_feature_flag(v->spec_.name))
            warn_(v->spec_.name +
                  " looks like a feature flag; flags that change without a rollout belong in a flag service "
                  "(SPEC section 10)");
    }
    if (app_.get_config_ptr() != nullptr) {
        problems.push_back(
            "CLI11 config files (set_config) are not supported yet: values read from them would bypass the "
            "contract; set the variables in the environment");
    }
    std::vector<FileSpec> specs;
    for (auto& f : files_) {
        problems.insert(problems.end(), f->problems_.begin(), f->problems_.end());
        if (f->spec_.type == FileType::Config && f->spec_.format.empty()) {
            f->spec_.format = extension_format(f->spec_.path);
            if (f->spec_.format.empty())
                problems.push_back(f->spec_.name + ": cannot tell the config format from " +
                                   detail::quote(f->spec_.path) + "; call format(\"json\"|\"yaml\"|\"toml\")");
        }
        specs.push_back(f->spec_);
    }
    auto fp = detail::validate_files(specs, var_specs());
    problems.insert(problems.end(), fp.begin(), fp.end());
    if (!problems.empty()) throw DeclarationError(std::move(problems));
    for (std::size_t i = 0; i < files_.size(); ++i) files_[i]->spec_.compiled = specs[i].compiled;
    checked_ = true;
}

std::vector<VarSpec> Declaration::var_specs() const {
    std::vector<VarSpec> out;
    for (const auto& v : vars_) out.push_back(v->spec_);
    return out;
}

std::vector<FileSpec> Declaration::file_specs() const {
    std::vector<FileSpec> out;
    for (const auto& f : files_) out.push_back(f->spec_);
    return out;
}

namespace {

std::string pad(const std::string& s, std::size_t w) { return s.size() >= w ? s : s + std::string(w - s.size(), ' '); }

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (const auto& x : v) out += (out.empty() ? "" : sep) + x;
    return out;
}

}  // namespace

std::string Declaration::help_footer() const {
    std::size_t w = 0;
    for (const auto& v : vars_) w = std::max(w, v->spec_.name.size());
    for (const auto& f : files_) w = std::max(w, f->spec_.name.size());
    w += 2;
    std::string out;
    if (!vars_.empty()) {
        out += "Environment variables:\n";
        for (const auto& v : vars_) {
            const VarSpec& s = v->spec_;
            std::vector<std::string> attrs;
            if (s.type == VarType::Enum && !s.values.empty()) attrs.push_back("one of " + join(s.values, "|"));
            else if (s.type == VarType::List)
                attrs.push_back(std::string("list of ") + (s.items == ItemType::Int ? "int" : "string") +
                                ", separated by " + detail::quote(s.separator));
            else attrs.push_back(to_string(s.type));
            if (s.required) attrs.push_back("REQUIRED");
            if (s.default_value && !s.secret) attrs.push_back("default " + detail::quote(display(s, *s.default_value)));
            if (s.secret) attrs.push_back("secret");
            if (s.deprecated) attrs.push_back("deprecated");
            if (v->option_) attrs.push_back("or " + v->flag_names_);
            out += "  " + pad(s.name, w) + s.description + " [" + join(attrs, ", ") + "]\n";
        }
    }
    if (!files_.empty()) {
        if (!out.empty()) out += "\n";
        out += "Files:\n";
        for (const auto& f : files_) {
            const FileSpec& s = f->spec_;
            std::vector<std::string> attrs{std::string(to_string(s.type)) + " at " + s.path};
            if (!s.path_env.empty()) attrs.push_back("path from " + s.path_env);
            if (s.required) attrs.push_back("REQUIRED");
            if (s.secret) attrs.push_back("secret");
            out += "  " + pad(s.name, w) + s.description + " [" + join(attrs, ", ") + "]\n";
        }
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

void Declaration::load(const Env& env) { load_env(env, {}, false); }

void Declaration::load_env(const Env& env, const std::map<std::string, std::string>& sources, bool process) {
    check();
    loaded_ = true;
    std::set<std::string> declared;
    for (const auto& v : vars_) declared.insert(v->spec_.name);
    for (const auto& f : files_)
        if (!f->spec_.path_env.empty()) declared.insert(f->spec_.path_env);
    for (const auto& w : detail::undeclared_hints(env, declared)) warn_(w);

    std::vector<Violation> violations;
    std::vector<std::string> warnings;
    auto specs = var_specs();
    auto values = detail::load_vars(specs, env, nullptr, violations, &warnings);
    for (const auto& w : warnings) warn_(w);
    auto root = env.find("DOCUCONF_FILE_ROOT");
    auto files = detail::load_files(file_specs(), env, values, root == env.end() ? "" : root->second, violations);
    if (!violations.empty()) {
        for (auto& v : violations)
            if (auto it = sources.find(v.input); it != sources.end()) v.source = it->second;
        detail::write_termination_log(violations, env, process);
        throw ValidationError(std::move(violations));
    }
    for (auto& v : vars_) {
        auto it = values.find(v->spec_.name);
        v->assign(it == values.end() ? std::nullopt : it->second);
    }
    for (auto& f : files_) {
        const auto& loaded = files[f->spec_.name];
        if (f->spec_.deprecated && loaded.present)
            warn_(f->spec_.name + " is deprecated: " + *f->spec_.deprecated +
                  (f->spec_.replaced_by.empty() ? "" : "; use " + f->spec_.replaced_by));
        f->assign_(&loaded);
    }
}

void Declaration::parse(int argc, const char* const* argv) {
    check();
    // Export reads the declaration only, never the environment, so it runs
    // before CLI11 parses anything.
    std::string flag = kExportFlag;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string path;
        if (a == flag && i + 1 < argc) path = argv[i + 1];
        else if (a.rfind(flag + "=", 0) == 0) path = a.substr(flag.size() + 1);
        else continue;
        write_contract(path);
        throw CLI::Success();
    }
    app_.parse(argc, argv);
    // Variables come from the environment. A variable declared with flag()
    // takes the command line's raw value instead when it is given; docuconf
    // parses and checks both the same way.
    Env env = detail::process_env();
    std::map<std::string, std::string> sources;
    for (auto& v : vars_) {
        if (!v->option_ || v->option_->count() == 0) continue;
        const auto& results = v->option_->results();
        if (results.empty()) continue;
        if (v->spec_.type == VarType::List) {
            // Each argument is itself split on the separator, as CLI11's
            // delimiter does, so joining them keeps every item.
            env[v->spec_.name] = join(results, v->spec_.separator);
        } else {
            env[v->spec_.name] = results.back();
        }
        sources[v->spec_.name] = v->flag_names_;
    }
    load_env(env, sources, true);
}

bool Declaration::parse_or_exit(int argc, const char* const* argv, int& code, std::ostream& err) {
    try {
        parse(argc, argv);
        return false;
    } catch (const CLI::Error& e) {
        code = app_.exit(e, std::cout, err);
        return true;
    } catch (const ValidationError& e) {
        err << e.what() << std::endl;
        code = 1;
        return true;
    } catch (const ExportError& e) {
        err << e.what() << std::endl;
        code = 1;
        return true;
    } catch (const DeclarationError& e) {
        err << e.what() << std::endl;
        code = 2;
        return true;
    }
}

void Declaration::write_contract(const std::string& path) const {
    std::string cue = export_cue();
    if (path == "-") {
        std::cout << cue << std::flush;
        return;
    }
    auto fail = [&](int e) { throw ExportError("docuconf: cannot write " + path + ": " + std::strerror(e)); };
    // Write next to the target and rename, so a failed export never leaves
    // a truncated contract behind.
    std::string tmp = path + ".tmp-docuconf";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) fail(errno);
    bool ok = std::fwrite(cue.data(), 1, cue.size(), f) == cue.size();
    int e = errno;
    if (std::fclose(f) != 0 && ok) {
        ok = false;
        e = errno;
    }
    if (!ok) {
        std::remove(tmp.c_str());
        fail(e);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        e = errno;
        std::remove(tmp.c_str());
        fail(e);
    }
}

}  // namespace docuconf
