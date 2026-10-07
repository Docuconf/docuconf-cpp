#include "docuconf/declaration.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

#include "internal.hpp"

namespace docuconf {

Declaration::Declaration(CLI::App& app, std::string service) : app_(app), service_(std::move(service)) {
    warn_ = [](const std::string& m) { std::cerr << "docuconf: warning: " << m << std::endl; };
    app_.add_option(kExportFlag, export_path_,
                    "Write the configuration contract (contract.cue) to this path, or - for standard output, "
                    "and exit without reading the environment")
        ->type_name("PATH")
        ->group("docuconf");
}

Declaration::~Declaration() = default;

Declaration& Declaration::app_version(std::string v) {
    app_version_ = std::move(v);
    return *this;
}

void Declaration::on_warning(std::function<void(const std::string&)> sink) { warn_ = std::move(sink); }

void Declaration::register_var(std::unique_ptr<VarBase> var) {
    std::string flag = "--";
    for (char c : var->spec_.name) flag += c == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string type_name = to_string(var->spec_.type);
    std::transform(type_name.begin(), type_name.end(), type_name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    // CLI11 finds the raw value (the command line, then the environment);
    // docuconf parses and checks it with the spec's rules.
    var->option_ = app_.add_option(flag, var->raw_, var->spec_.description)
                       ->envname(var->spec_.name)
                       ->type_name(type_name);
    vars_.push_back(std::move(var));
    checked_ = false;
}

File* Declaration::new_file(std::string name, FileType type, std::string description) {
    auto f = std::make_unique<File>();
    f->spec_.name = std::move(name);
    f->spec_.type = type;
    f->spec_.description = std::move(description);
    if (type == FileType::Tls || type == FileType::Keystore) f->spec_.secret = true;
    if (type == FileType::Keystore) f->spec_.format = "pkcs12";
    File* raw = f.get();
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

File* Declaration::add_file(std::string name, TlsKeyPair& target, std::string description) {
    File* f = new_file(std::move(name), FileType::Tls, std::move(description));
    f->assign_ = [&target](const void* p) {
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

File* Declaration::add_file(std::string name, CaBundle& target, std::string description) {
    File* f = new_file(std::move(name), FileType::CaBundle, std::move(description));
    f->assign_ = [&target](const void* p) {
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

File* Declaration::add_file(std::string name, Keystore& target, std::string description) {
    File* f = new_file(std::move(name), FileType::Keystore, std::move(description));
    f->assign_ = [&target](const void* p) {
        const auto& l = lf(p);
        target = Keystore{};
        target.present = l.present;
        if (!l.present) return;
        target.path = l.path;
        target.data = l.content;
    };
    return f;
}

File* Declaration::add_file(std::string name, TextFile& target, std::string description) {
    File* f = new_file(std::move(name), FileType::Text, std::move(description));
    f->assign_ = [&target](const void* p) {
        const auto& l = lf(p);
        target = TextFile{};
        target.present = l.present;
        if (!l.present) return;
        target.path = l.path;
        target.content = l.content;
    };
    return f;
}

File* Declaration::add_file(std::string name, BinaryFile& target, std::string description) {
    File* f = new_file(std::move(name), FileType::Binary, std::move(description));
    f->assign_ = [&target](const void* p) {
        const auto& l = lf(p);
        target = BinaryFile{};
        target.present = l.present;
        if (!l.present) return;
        target.path = l.path;
        target.data = l.content;
    };
    return f;
}

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
    std::vector<std::string> problems;
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
        if (!names.insert(v->spec_.name).second) problems.push_back(v->spec_.name + ": declared twice");
        if (v->spec_.default_value) v->option_->default_str(display(v->spec_, *v->spec_.default_value));
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

void Declaration::load(const Env& env) {
    check();
    std::vector<Violation> violations;
    std::vector<std::string> warnings;
    auto specs = var_specs();
    auto values = detail::load_vars(specs, env, nullptr, violations, &warnings);
    for (const auto& w : warnings) warn_(w);
    auto root = env.find("DOCUCONF_FILE_ROOT");
    auto files = detail::load_files(file_specs(), env, values, root == env.end() ? "" : root->second, violations);
    if (!violations.empty()) {
        detail::write_termination_log(violations);
        throw ValidationError(std::move(violations));
    }
    for (auto& v : vars_) {
        auto it = values.find(v->spec_.name);
        v->assign(it == values.end() ? std::nullopt : it->second);
    }
    for (auto& f : files_) f->assign_(&files[f->spec_.name]);
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
    // CLI11 skips an empty environment variable, but an empty string is a
    // present value for a string variable (SPEC §5), so docuconf starts
    // from the process environment and lays CLI11's results over it.
    Env env = detail::process_env();
    for (auto& v : vars_) {
        if (v->option_->count() > 0) env[v->spec_.name] = v->raw_;
    }
    load(env);
}

bool Declaration::parse_or_exit(int argc, const char* const* argv, int& code, std::ostream& err) {
    try {
        parse(argc, argv);
        return false;
    } catch (const CLI::ParseError& e) {
        code = app_.exit(e, std::cout, err);
        return true;
    } catch (const ValidationError& e) {
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
        std::cout << cue;
        return;
    }
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("docuconf: cannot write " + path);
    f << cue;
}

}  // namespace docuconf
