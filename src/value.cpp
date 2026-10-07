// Typed values: parsing the wire form (SPEC §5), checking constraints, and
// validating variable declarations. The declaration path and
// contract-first mode both use this code, so they accept exactly the same
// values.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>

#include "docuconf/duration.hpp"
#include "internal.hpp"

namespace docuconf {

const char* to_string(VarType t) noexcept {
    switch (t) {
        case VarType::String: return "string";
        case VarType::Int: return "int";
        case VarType::Float: return "float";
        case VarType::Bool: return "bool";
        case VarType::Duration: return "duration";
        case VarType::Url: return "url";
        case VarType::Enum: return "enum";
        case VarType::List: return "list";
        case VarType::Json: return "json";
    }
    return "?";
}

const char* to_string(ListEncoding e) noexcept {
    switch (e) {
        case ListEncoding::Csv: return "csv";
        case ListEncoding::Json: return "json";
        case ListEncoding::Indexed: return "indexed";
    }
    return "?";
}

const char* to_string(DurationEncoding e) noexcept {
    switch (e) {
        case DurationEncoding::Go: return "go";
        case DurationEncoding::Iso8601: return "iso8601";
        case DurationEncoding::Seconds: return "seconds";
        case DurationEncoding::Timespan: return "timespan";
    }
    return "?";
}

nlohmann::json Value::to_json() const {
    return std::visit(
        [](const auto& x) -> nlohmann::json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Duration>) {
                return format_go_duration(x);
            } else if constexpr (std::is_same_v<T, List>) {
                nlohmann::json a = nlohmann::json::array();
                for (const auto& i : x) a.push_back(i.to_json());
                return a;
            } else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(x)) return nullptr;
                return x;
            } else {
                return x;
            }
        },
        v_);
}

namespace detail {

Pattern::Pattern(const std::string& p) : source(p), re(p, re2::RE2::Quiet) {}

bool Pattern::matches(const std::string& s) const { return re2::RE2::PartialMatch(s, re); }

std::shared_ptr<const Pattern> compile_pattern(const std::string& p, std::string& error) {
    auto pat = std::make_shared<Pattern>(p);
    if (!pat->re.ok()) {
        error = "pattern " + quote(p) + " is not RE2 syntax (" + pat->re.error() +
                "); lookaround and backreferences are not supported";
        return nullptr;
    }
    return pat;
}

bool is_env_name(const std::string& s) {
    if (s.empty() || !(s[0] >= 'A' && s[0] <= 'Z')) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool is_input_name(const std::string& s) {
    // ^[a-z]([-a-z0-9]{0,40}[a-z0-9])?$
    if (s.empty() || s.size() > 42 || !(s[0] >= 'a' && s[0] <= 'z')) return false;
    if (s.back() == '-') return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    const auto* b = reinterpret_cast<const unsigned char*>(s.data());
    while (i < s.size()) {
        unsigned char c = b[i];
        std::size_t n;
        std::uint32_t cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size() || (b[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b[i + k] & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += n + 1;
    }
    return true;
}

std::size_t rune_count(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string quote(const std::string& s) {
    return nlohmann::json(s).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string format_float(double f) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, f);
    return std::string(buf, r.ptr);
}

namespace {

Problem invalid(std::string m) { return {Code::InvalidType, std::move(m)}; }

std::optional<std::int64_t> parse_int(const std::string& raw, Problem& err) {
    std::int64_t v = 0;
    const char* b = raw.data();
    const char* e = raw.data() + raw.size();
    auto r = std::from_chars(b, e, v, 10);
    if (r.ec == std::errc() && r.ptr == e) return v;
    std::string digits = raw;
    if (!digits.empty() && digits[0] == '-') digits.erase(0, 1);
    bool all_digits = !digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
    if (all_digits) {
        err = {Code::OutOfRange, "is outside the 64-bit integer range"};
    } else {
        err = invalid("is not a 64-bit integer");
    }
    return std::nullopt;
}

std::optional<double> parse_float(const std::string& raw) {
    double v = 0;
    const char* b = raw.data();
    const char* e = raw.data() + raw.size();
    // from_chars is locale-independent and takes no leading '+' or spaces.
    auto r = std::from_chars(b, e, v, std::chars_format::general);
    if (r.ec != std::errc() || r.ptr != e || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<Value> parse_items(ItemType item, const std::vector<std::string>& raws, Problem& err) {
    Value::List out;
    out.reserve(raws.size());
    for (std::size_t i = 0; i < raws.size(); ++i) {
        if (item == ItemType::String) {
            out.emplace_back(raws[i]);
            continue;
        }
        Problem p;
        auto n = parse_int(raws[i], p);
        if (!n) {
            err = {p.first, "has item " + std::to_string(i) + " that " + p.second};
            return std::nullopt;
        }
        out.emplace_back(*n);
    }
    return Value(std::move(out));
}

std::optional<Value> json_item(ItemType item, std::size_t i, const nlohmann::json& x, Problem& err) {
    std::string at = "has item " + std::to_string(i) + " that ";
    if (item == ItemType::String) {
        if (x.is_string()) return Value(x.get<std::string>());
        err = invalid(at + "is not a string");
        return std::nullopt;
    }
    if (x.is_number_integer() && !x.is_number_unsigned()) return Value(x.get<std::int64_t>());
    if (x.is_number_unsigned()) {
        auto u = x.get<std::uint64_t>();
        if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return Value(static_cast<std::int64_t>(u));
        err = {Code::OutOfRange, at + "is outside the 64-bit integer range"};
        return std::nullopt;
    }
    if (x.is_number_float()) {
        double f = x.get<double>();
        if (std::isfinite(f) && std::floor(f) == f && std::fabs(f) >= 9.2e18) {
            err = {Code::OutOfRange, at + "is outside the 64-bit integer range"};
            return std::nullopt;
        }
    }
    err = invalid(at + "is not a 64-bit integer");
    return std::nullopt;
}

std::vector<std::string> split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        std::size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, p - start));
        start = p + sep.size();
    }
}

const re2::RE2& url_re() {
    static const re2::RE2 re("^([a-zA-Z][a-zA-Z0-9+.-]*)://[^\\s]+$");
    return re;
}

}  // namespace

std::optional<Value> parse_wire(const VarSpec& spec, const std::vector<std::string>& raws, Problem& err) {
    if (spec.type == VarType::List && spec.list_encoding == ListEncoding::Indexed)
        return parse_items(spec.items, raws, err);
    const std::string& raw = raws.at(0);
    switch (spec.type) {
        case VarType::String:
        case VarType::Url:
        case VarType::Enum: return Value(raw);
        case VarType::Int: {
            auto n = parse_int(raw, err);
            if (!n) return std::nullopt;
            return Value(*n);
        }
        case VarType::Float: {
            auto f = parse_float(raw);
            if (!f) {
                err = invalid("is not a finite number such as 0.5");
                return std::nullopt;
            }
            return Value(*f);
        }
        case VarType::Bool: {
            std::string l = raw;
            std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return std::tolower(c); });
            if (l == "true") return Value(true);
            if (l == "false") return Value(false);
            err = invalid("is not true or false");
            return std::nullopt;
        }
        case VarType::Duration: {
            auto d = parse_duration(spec.duration_encoding, raw);
            if (d) return Value(*d);
            switch (spec.duration_encoding) {
                case DurationEncoding::Go: err = invalid("is not a duration such as \"1m30s\""); break;
                case DurationEncoding::Iso8601: err = invalid("is not an ISO 8601 duration such as \"PT90S\""); break;
                case DurationEncoding::Seconds:
                    err = invalid("is not a number of seconds such as \"90\" or \"1.5\"");
                    break;
                case DurationEncoding::Timespan:
                    err = invalid("is not a TimeSpan such as \"00:01:30\" or \"1.02:03:04.5\"");
                    break;
            }
            return std::nullopt;
        }
        case VarType::List: {
            if (spec.list_encoding == ListEncoding::Csv) return parse_items(spec.items, split(raw, spec.separator), err);
            nlohmann::json arr = nlohmann::json::parse(raw, nullptr, false);
            if (arr.is_discarded() || !arr.is_array()) {
                err = invalid("is not a JSON array such as [\"a\",\"b\"]");
                return std::nullopt;
            }
            Value::List out;
            for (std::size_t i = 0; i < arr.size(); ++i) {
                auto item = json_item(spec.items, i, arr[i], err);
                if (!item) return std::nullopt;
                out.push_back(std::move(*item));
            }
            return Value(std::move(out));
        }
        case VarType::Json: {
            nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
            if (j.is_discarded()) {
                err = invalid("is not valid JSON");
                return std::nullopt;
            }
            return Value::json(std::move(j));
        }
    }
    err = invalid("has an unknown type");
    return std::nullopt;
}

namespace {

std::string show(const Value& v) {
    if (v.is_string()) return quote(v.as_string());
    if (v.is_int()) return std::to_string(v.as_int());
    if (v.is_float()) return format_float(v.as_float());
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_duration()) return format_go_duration(v.as_duration());
    return v.to_json().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

}  // namespace

std::vector<Problem> check_value(const VarSpec& spec, const Value& v) {
    std::string shown = spec.secret ? "value" : show(v);
    std::vector<Problem> out;
    auto push = [&](Code c, const std::string& m) { out.emplace_back(c, shown + " " + m); };
    auto wrong = [&] { push(Code::InvalidType, std::string("is not a ") + to_string(spec.type) + " value"); };
    switch (spec.type) {
        case VarType::String: {
            if (!v.is_string()) return wrong(), out;
            const auto& s = v.as_string();
            std::uint64_t n = rune_count(s);
            if (spec.min_length && n < *spec.min_length)
                push(Code::OutOfRange, "is " + std::to_string(n) + " characters, below minLength " +
                                           std::to_string(*spec.min_length));
            if (spec.max_length && n > *spec.max_length)
                push(Code::OutOfRange, "is " + std::to_string(n) + " characters, above maxLength " +
                                           std::to_string(*spec.max_length));
            if (spec.compiled && !spec.compiled->matches(s))
                push(Code::PatternMismatch, "does not match pattern " + spec.compiled->source);
            break;
        }
        case VarType::Int: {
            if (!v.is_int()) return wrong(), out;
            auto i = v.as_int();
            if (spec.min && spec.min->is_int() && i < spec.min->as_int())
                push(Code::OutOfRange, "is below min " + std::to_string(spec.min->as_int()));
            if (spec.max && spec.max->is_int() && i > spec.max->as_int())
                push(Code::OutOfRange, "is above max " + std::to_string(spec.max->as_int()));
            break;
        }
        case VarType::Float: {
            if (!v.is_float()) return wrong(), out;
            double f = v.as_float();
            if (!std::isfinite(f)) push(Code::InvalidType, "is not a finite number");
            if (spec.min && spec.min->is_float() && f < spec.min->as_float())
                push(Code::OutOfRange, "is below min " + format_float(spec.min->as_float()));
            if (spec.max && spec.max->is_float() && f > spec.max->as_float())
                push(Code::OutOfRange, "is above max " + format_float(spec.max->as_float()));
            break;
        }
        case VarType::Bool:
            if (!v.is_bool()) wrong();
            break;
        case VarType::Duration: {
            if (!v.is_duration()) return wrong(), out;
            auto d = v.as_duration();
            if (spec.min && spec.min->is_duration() && d < spec.min->as_duration())
                push(Code::OutOfRange, "is below min " + format_go_duration(spec.min->as_duration()));
            if (spec.max && spec.max->is_duration() && d > spec.max->as_duration())
                push(Code::OutOfRange, "is above max " + format_go_duration(spec.max->as_duration()));
            break;
        }
        case VarType::Url: {
            if (!v.is_string()) return wrong(), out;
            std::string scheme;
            if (!re2::RE2::FullMatch(v.as_string(), url_re(), &scheme)) {
                push(Code::InvalidType, "is not a URL of the form scheme://...");
            } else if (!spec.schemes.empty() &&
                       std::find(spec.schemes.begin(), spec.schemes.end(), scheme) == spec.schemes.end()) {
                push(Code::InvalidScheme, "has scheme " + scheme + ", not one of " + join(spec.schemes, ", "));
            }
            break;
        }
        case VarType::Enum: {
            if (!v.is_string()) return wrong(), out;
            if (std::find(spec.values.begin(), spec.values.end(), v.as_string()) == spec.values.end())
                push(Code::NotInEnum, "is not one of " + join(spec.values, ", "));
            break;
        }
        case VarType::List: {
            if (!v.is_list()) return wrong(), out;
            const auto& items = v.as_list();
            std::uint64_t n = items.size();
            if (spec.min_items && n < *spec.min_items)
                push(Code::TooFewItems,
                     "has " + std::to_string(n) + " items, below minItems " + std::to_string(*spec.min_items));
            if (spec.max_items && n > *spec.max_items)
                push(Code::TooManyItems,
                     "has " + std::to_string(n) + " items, above maxItems " + std::to_string(*spec.max_items));
            for (const auto& x : items) {
                bool ok = spec.items == ItemType::Int ? x.is_int() : x.is_string();
                if (!ok) {
                    push(Code::InvalidType, "has an item that is not a " +
                                                std::string(spec.items == ItemType::Int ? "int" : "string"));
                    return out;
                }
            }
            if (spec.items == ItemType::Int) {
                auto lo = spec.item_min.value_or(std::numeric_limits<std::int64_t>::min());
                auto hi = spec.item_max.value_or(std::numeric_limits<std::int64_t>::max());
                // One violation per variable: the first item out of bounds.
                for (const auto& x : items) {
                    auto i = x.as_int();
                    if (i < lo) {
                        push(Code::OutOfRange,
                             "has item " + std::to_string(i) + ", below itemMin " + std::to_string(lo));
                        break;
                    }
                    if (i > hi) {
                        push(Code::OutOfRange,
                             "has item " + std::to_string(i) + ", above itemMax " + std::to_string(hi));
                        break;
                    }
                }
            }
            break;
        }
        case VarType::Json: {
            if (!v.is_json()) return wrong(), out;
            if (spec.schema) {
                for (const auto& m : validate_schema(*spec.schema, v.as_json(), spec.secret))
                    push(Code::SchemaMismatch, m);
            }
            if (out.empty() && spec.bind) {
                std::string e = spec.bind(v.as_json());
                if (!e.empty())
                    push(Code::SchemaMismatch, "does not bind to the app's type" + (spec.secret ? "" : ": " + e));
            }
            break;
        }
    }
    return out;
}

std::optional<Value> value_from_json(const VarSpec& spec, const nlohmann::json& j, std::string& err) {
    auto wrong = [&] {
        err = j.dump() + " is not a " + to_string(spec.type) + " value";
        return std::nullopt;
    };
    auto as_int = [](const nlohmann::json& x) -> std::optional<std::int64_t> {
        if (x.is_number_unsigned()) {
            auto u = x.get<std::uint64_t>();
            if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
            return static_cast<std::int64_t>(u);
        }
        if (x.is_number_integer()) return x.get<std::int64_t>();
        return std::nullopt;
    };
    switch (spec.type) {
        case VarType::String:
        case VarType::Url:
        case VarType::Enum:
            if (!j.is_string()) return wrong();
            return Value(j.get<std::string>());
        case VarType::Int: {
            auto i = as_int(j);
            if (!i) return wrong();
            return Value(*i);
        }
        case VarType::Float:
            if (!j.is_number()) return wrong();
            return Value(j.get<double>());
        case VarType::Bool:
            if (!j.is_boolean()) return wrong();
            return Value(j.get<bool>());
        case VarType::Duration: {
            if (!j.is_string()) return wrong();
            auto d = parse_go_duration(j.get<std::string>());
            if (!d) return wrong();
            return Value(*d);
        }
        case VarType::List: {
            if (!j.is_array()) return wrong();
            Value::List out;
            for (const auto& x : j) {
                if (spec.items == ItemType::String) {
                    if (!x.is_string()) return wrong();
                    out.emplace_back(x.get<std::string>());
                } else {
                    auto i = as_int(x);
                    if (!i) return wrong();
                    out.emplace_back(*i);
                }
            }
            return Value(std::move(out));
        }
        case VarType::Json: return Value::json(j);
    }
    return wrong();
}

std::vector<std::string> validate_var(VarSpec& spec) {
    std::vector<std::string> problems;
    auto bad = [&](const std::string& m) { problems.push_back(spec.name + ": " + m); };
    if (!is_env_name(spec.name)) bad("variable name must match ^[A-Z][A-Z0-9_]*$");
    if (rune_count(spec.description) < 5) bad("description must be at least 5 characters");
    if (spec.required && spec.default_value) bad("a required variable must not have a default");
    if (spec.secret && spec.default_value) bad("a secret must not have a default");
    if (spec.secret && !spec.examples.empty()) bad("a secret must not have examples");
    bool numeric = spec.type == VarType::Int || spec.type == VarType::Float || spec.type == VarType::Duration;
    if (!numeric && (spec.min || spec.max)) bad(std::string("min and max do not apply to a ") + to_string(spec.type) + " variable");
    if (spec.type != VarType::String && (spec.min_length || spec.max_length || spec.pattern))
        bad(std::string("minLength, maxLength and pattern do not apply to a ") + to_string(spec.type) + " variable");
    if (spec.type != VarType::Url && !spec.schemes.empty())
        bad(std::string("schemes do not apply to a ") + to_string(spec.type) + " variable");
    if (spec.type == VarType::Enum && spec.values.empty()) bad("an enum needs at least one value");
    if (spec.type != VarType::List && (spec.min_items || spec.max_items))
        bad(std::string("minItems and maxItems do not apply to a ") + to_string(spec.type) + " variable");
    if ((spec.item_min || spec.item_max) && !(spec.type == VarType::List && spec.items == ItemType::Int))
        bad("itemMin and itemMax only apply to a list of ints");
    if (spec.type == VarType::List && spec.list_encoding == ListEncoding::Csv && spec.separator.empty())
        bad("separator must not be empty");
    if (spec.min_length && spec.max_length && *spec.min_length > *spec.max_length) bad("minLength is above maxLength");
    if (spec.min_items && spec.max_items && *spec.min_items > *spec.max_items) bad("minItems is above maxItems");
    if (spec.item_min && spec.item_max && *spec.item_min > *spec.item_max) bad("itemMin is above itemMax");
    auto kind_ok = [&](const Value& b) {
        switch (spec.type) {
            case VarType::Int: return b.is_int();
            case VarType::Float: return b.is_float();
            case VarType::Duration: return b.is_duration();
            default: return false;
        }
    };
    if (spec.min && numeric && !kind_ok(*spec.min)) bad(std::string("min is not a ") + to_string(spec.type) + " value");
    if (spec.max && numeric && !kind_ok(*spec.max)) bad(std::string("max is not a ") + to_string(spec.type) + " value");
    if (spec.min && spec.max && kind_ok(*spec.min) && kind_ok(*spec.max)) {
        bool above = spec.type == VarType::Int     ? spec.min->as_int() > spec.max->as_int()
                     : spec.type == VarType::Float ? spec.min->as_float() > spec.max->as_float()
                                                   : spec.min->as_duration() > spec.max->as_duration();
        if (above) bad("min is above max");
    }
    if (spec.pattern) {
        std::string err;
        spec.compiled = compile_pattern(*spec.pattern, err);
        if (!spec.compiled) bad(err);
    }
    if (spec.deprecated && !spec.replaced_by.empty() && !is_env_name(spec.replaced_by))
        bad("replacedBy must be a variable name");
    if (spec.default_value && problems.empty()) {
        for (const auto& [code, msg] : check_value(spec, *spec.default_value)) bad("default " + msg);
    }
    return problems;
}

namespace {

std::optional<std::string> unresolved_reference(const std::string& raw) {
    for (const char* scheme : {"vault:", "op://", "ref+"}) {
        if (raw.rfind(scheme, 0) == 0) return std::string(scheme);
    }
    return std::nullopt;
}

bool is_index(const std::string& s) {
    if (s.empty() || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    return s == "0" || s[0] != '0';
}

// The items of the indexed list `name` (SPEC §5): how many there are from
// NAME__0 on, and whether a higher index is set past a gap.
std::pair<std::size_t, std::optional<std::size_t>> indexed_items(const Env& env, const std::string& name) {
    std::string prefix = name + "__";
    std::set<std::size_t> set;
    bool beyond = false;
    for (auto it = env.lower_bound(prefix); it != env.end() && it->first.rfind(prefix, 0) == 0; ++it) {
        std::string n = it->first.substr(prefix.size());
        if (!is_index(n)) continue;
        if (n.size() > 18) {
            beyond = true;  // too large to be anything but past a gap
            continue;
        }
        set.insert(static_cast<std::size_t>(std::stoull(n)));
    }
    std::size_t count = 0;
    while (set.count(count)) ++count;
    std::optional<std::size_t> gap;
    if (beyond || set.size() > count) gap = count;
    return {count, gap};
}

Violation violation(const VarSpec& spec, Code code, std::string message) {
    return Violation{spec.name, code, std::move(message)};
}

}  // namespace

std::map<std::string, std::optional<Value>> load_vars(const std::vector<VarSpec>& vars, const Env& env,
                                                      const Profiles* profiles,
                                                      std::vector<Violation>& violations,
                                                      std::vector<std::string>* warnings) {
    const std::map<std::string, Value>* profile_defaults = nullptr;
    if (profiles) {
        auto sel = env.find(profiles->selector);
        std::string selected =
            (sel != env.end() && !sel->second.empty()) ? sel->second : profiles->default_profile;
        auto it = profiles->defaults.find(selected);
        if (it != profiles->defaults.end()) profile_defaults = &it->second;
    }
    std::map<std::string, std::optional<Value>> out;
    for (const auto& spec : vars) {
        // The raw strings: one, or one per item of an indexed list.
        std::vector<std::string> raws;
        bool present = false;
        bool failed = false;
        if (spec.type == VarType::List && spec.list_encoding == ListEncoding::Indexed) {
            auto [count, gap] = indexed_items(env, spec.name);
            if (gap) {
                violations.push_back(violation(spec, Code::InvalidType,
                                               "items must be numbered from " + spec.name +
                                                   "__0 with no gap, but " + spec.name + "__" +
                                                   std::to_string(*gap) + " is not set"));
                continue;
            }
            for (std::size_t i = 0; i < count; ++i) raws.push_back(env.at(spec.name + "__" + std::to_string(i)));
            present = count > 0;
        } else {
            auto it = env.find(spec.name);
            if (it != env.end() && (spec.type == VarType::String || !it->second.empty())) {
                raws.push_back(it->second);
                present = true;
            }
        }

        std::optional<Value> found;
        if (present) {
            if (spec.deprecated && warnings) {
                warnings->push_back(spec.name + " is deprecated: " + *spec.deprecated +
                                    (spec.replaced_by.empty() ? "" : "; use " + spec.replaced_by));
            }
            if (spec.secret) {
                for (const auto& r : raws) {
                    if (auto scheme = unresolved_reference(r)) {
                        violations.push_back(violation(
                            spec, Code::InvalidType,
                            "holds an unresolved " + *scheme +
                                " reference; the injector that should resolve it did not run"));
                        failed = true;
                        break;
                    }
                }
            }
            if (!failed && spec.type != VarType::Int && spec.type != VarType::Float && spec.type != VarType::Bool) {
                for (const auto& r : raws) {
                    if (!valid_utf8(r)) {
                        violations.push_back(violation(spec, Code::InvalidType, "value is not valid UTF-8"));
                        failed = true;
                        break;
                    }
                }
            }
            if (failed) continue;
            Problem err;
            found = parse_wire(spec, raws, err);
            if (!found) {
                std::string what = (raws.size() == 1 && !spec.secret) ? quote(raws[0]) : "value";
                violations.push_back(violation(spec, err.first, what + " " + err.second));
                continue;
            }
        } else if (profile_defaults && profile_defaults->count(spec.name)) {
            found = profile_defaults->at(spec.name);
        } else if (spec.default_value) {
            found = spec.default_value;
        }

        if (!found) {
            if (spec.required) {
                violations.push_back(violation(spec, Code::MissingRequired, "is required but not set"));
            } else {
                out[spec.name] = std::nullopt;
            }
            continue;
        }
        auto problems = check_value(spec, *found);
        if (problems.empty()) {
            out[spec.name] = std::move(found);
            continue;
        }
        std::string hint;
        if (spec.secret && found->is_string() && !found->as_string().empty() && found->as_string().back() == '\n')
            hint = " (the value ends in a newline: was the secret created from a file?)";
        for (auto& [code, msg] : problems) violations.push_back(violation(spec, code, msg + hint));
    }
    return out;
}

}  // namespace detail
}  // namespace docuconf
