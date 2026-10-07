// JSON Schema validation for config files and json variables, with
// pboettch/json-schema-validator (draft 7; $ref, $defs and the common
// keywords of later drafts work the same).
#include <nlohmann/json-schema.hpp>

#include "internal.hpp"

namespace docuconf {
namespace detail {
namespace {

class Collector : public nlohmann::json_schema::error_handler {
public:
    explicit Collector(bool secret) : secret_(secret) {}
    void error(const nlohmann::json::json_pointer& ptr, const nlohmann::json& instance,
               const std::string& message) override {
        std::string where = ptr.to_string();
        std::string at = where.empty() ? std::string("at the root") : "at " + where;
        // A message can quote the value (format checks do), so a secret
        // gets none.
        std::string m = secret_ ? at + ": the value does not satisfy the schema" : at + ": " + message;
        if (!secret_ && !instance.is_object() && !instance.is_array()) {
            std::string shown = instance.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            if (shown.size() <= 80) m += " (got " + shown + ")";
        }
        messages.push_back(std::move(m));
    }
    std::vector<std::string> messages;

private:
    bool secret_;
};

// Unknown formats (such as "uint32" from some generators) are accepted:
// they are annotations, not assertions, in JSON Schema.
void lenient_format_check(const std::string& format, const std::string& value) {
    try {
        nlohmann::json_schema::default_string_format_check(format, value);
    } catch (const std::invalid_argument&) {
        throw;  // the value does not match a known format
    } catch (const std::logic_error&) {
        return;  // a format the checker does not know
    }
}

}  // namespace

std::vector<std::string> validate_schema(const nlohmann::json& schema, const nlohmann::json& doc, bool secret) {
    if (schema.is_boolean()) {
        if (schema.get<bool>()) return {};
        return {"the schema accepts no value"};
    }
    try {
        nlohmann::json_schema::json_validator v(nullptr, lenient_format_check);
        v.set_root_schema(schema);
        Collector c(secret);
        v.validate(doc, c);
        // Report the first few: one violation, several messages.
        std::vector<std::string> out;
        if (!c.messages.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < c.messages.size() && i < 3; ++i) {
                if (i) joined += "; ";
                joined += c.messages[i];
            }
            out.push_back("does not match the schema: " + joined);
        }
        return out;
    } catch (const std::exception& e) {
        return {std::string("cannot be checked: the schema is invalid: ") + e.what()};
    }
}

}  // namespace detail
}  // namespace docuconf
