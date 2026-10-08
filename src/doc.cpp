// Splits a Doxygen doc comment into a description and CommonMark details
// (SPEC §4.2, §14.7). See docuconf/doc.hpp.
#include "docuconf/doc.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace docuconf {
namespace {

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string ltrim(const std::string& s) {
    std::size_t i = s.find_first_not_of(" \t");
    return i == std::string::npos ? "" : s.substr(i);
}

std::string rtrim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(rtrim(cur));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(rtrim(cur));
    return out;
}

// Strips `///`, `//!`, `/** */` and ` * ` margins when the text is a comment.
std::vector<std::string> strip_markers(std::vector<std::string> lines) {
    std::string first;
    for (const auto& l : lines)
        if (!ltrim(l).empty()) {
            first = ltrim(l);
            break;
        }
    bool line_comment = starts_with(first, "//");
    bool block_comment = starts_with(first, "/*");
    if (!line_comment && !block_comment) return lines;
    for (auto& l : lines) {
        std::string t = ltrim(l);
        std::string indent = l.substr(0, l.size() - t.size());
        for (const char* m : {"///<", "//!<", "///", "//!", "//", "/**<", "/*!<", "/**", "/*!", "/*"}) {
            if (starts_with(t, m)) {
                t = t.substr(std::string(m).size());
                indent.clear();
                break;
            }
        }
        if (block_comment) {
            if (ends_with(t, "*/")) t = rtrim(t.substr(0, t.size() - 2));
            std::string u = ltrim(t);
            if (starts_with(u, "*") && !starts_with(u, "**")) {
                t = u.substr(1);
                indent.clear();
            }
        }
        l = rtrim(indent + t);
    }
    return lines;
}

std::vector<std::string> unindent(std::vector<std::string> lines) {
    std::size_t indent = std::string::npos;
    for (const auto& l : lines)
        if (!ltrim(l).empty()) indent = std::min(indent, l.size() - ltrim(l).size());
    for (auto& l : lines) l = ltrim(l).empty() ? "" : l.substr(indent == std::string::npos ? 0 : indent);
    while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

// A Doxygen command at s[i] ('@' or '\\'), such as "@c": its name.
std::string command_at(const std::string& s, std::size_t i) {
    if (i >= s.size() || (s[i] != '@' && s[i] != '\\')) return "";
    if (i > 0 && !std::isspace(static_cast<unsigned char>(s[i - 1])) && s[i - 1] != '(') return "";
    std::size_t j = i + 1;
    while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
    return s.substr(i + 1, j - i - 1);
}

// The word after a command at s[from], without trailing punctuation, and
// where it ends.
std::pair<std::string, std::size_t> word_after(const std::string& s, std::size_t from) {
    std::size_t b = from;
    while (b < s.size() && s[b] == ' ') ++b;
    std::size_t e = b;
    while (e < s.size() && !std::isspace(static_cast<unsigned char>(s[e]))) ++e;
    while (e > b && std::string(".,;:!?)").find(s[e - 1]) != std::string::npos) --e;
    return {s.substr(b, e - b), e};
}

// Converts inline Doxygen commands and HTML tags to CommonMark, outside
// code spans.
std::string inline_markup(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '`') {
            std::size_t run = s.find_first_not_of('`', i) == std::string::npos ? s.size() - i
                                                                                 : s.find_first_not_of('`', i) - i;
            std::string fence(run, '`');
            std::size_t end = s.find(fence, i + run);
            std::size_t len = end == std::string::npos ? run : end + run - i;
            out += s.substr(i, len);
            i += len;
            continue;
        }
        std::string cmd = command_at(s, i);
        if (!cmd.empty() && i + 1 + cmd.size() < s.size() && s[i + 1 + cmd.size()] == ' ') {
            const char* wrap = nullptr;
            if (cmd == "c" || cmd == "p" || cmd == "ref") wrap = "`";
            else if (cmd == "a" || cmd == "e" || cmd == "em") wrap = "*";
            else if (cmd == "b") wrap = "**";
            if (wrap) {
                auto [w, end] = word_after(s, i + 1 + cmd.size());
                if (!w.empty()) {
                    out += wrap + w + wrap;
                    i = end;
                    continue;
                }
            }
        }
        static const std::pair<const char*, const char*> tags[] = {
            {"<tt>", "`"}, {"</tt>", "`"}, {"<code>", "`"}, {"</code>", "`"}, {"<b>", "**"}, {"</b>", "**"},
            {"<strong>", "**"}, {"</strong>", "**"}, {"<em>", "*"}, {"</em>", "*"}, {"<i>", "*"}, {"</i>", "*"}};
        bool tag = false;
        for (const auto& [from, to] : tags) {
            if (s.compare(i, std::string(from).size(), from) == 0) {
                out += to;
                i += std::string(from).size();
                tag = true;
                break;
            }
        }
        if (!tag) out += s[i++];
    }
    return out;
}

bool is_drop_command(const std::string& c) {
    static const char* drop[] = {"param",   "tparam",   "return", "returns",     "retval",   "throw",
                                 "throws",  "exception", "author", "authors",     "date",     "version",
                                 "since",   "ingroup",  "addtogroup", "defgroup", "file",     "internal",
                                 "endinternal", "copyright", "pre", "post", "invariant"};
    for (const char* d : drop)
        if (c == d) return true;
    return false;
}

// Converts block-level Doxygen to CommonMark, line by line.
std::vector<std::string> convert(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    std::string fence;      // the fence of an open Markdown code block
    bool doxy_code = false;  // inside @code or @verbatim
    for (const auto& line : lines) {
        std::string t = ltrim(line);
        std::string indent = line.substr(0, line.size() - t.size());
        std::string cmd = command_at(t, 0);
        if (doxy_code) {
            if (cmd == "endcode" || cmd == "endverbatim") {
                out.push_back(indent + "```");
                doxy_code = false;
            } else {
                out.push_back(line);
            }
            continue;
        }
        if (!fence.empty()) {
            out.push_back(line);
            if (starts_with(t, fence) && t.find_first_not_of(fence[0]) == std::string::npos) fence.clear();
            continue;
        }
        if (starts_with(t, "```") || starts_with(t, "~~~")) {
            fence = t.substr(0, t.find_first_not_of(t[0]) == std::string::npos ? t.size() : t.find_first_not_of(t[0]));
            out.push_back(line);
            continue;
        }
        if (cmd == "code" || cmd == "verbatim") {
            std::string lang = cmd == "code" ? "cpp" : "";
            std::string rest = t.substr(1 + cmd.size());
            if (starts_with(rest, "{.") && rest.find('}') != std::string::npos)
                lang = rest.substr(2, rest.find('}') - 2);
            out.push_back(indent + "```" + lang);
            doxy_code = true;
            continue;
        }
        if (line.size() - t.size() >= 4) {
            out.push_back(line);  // indented code
            continue;
        }
        if (!cmd.empty() && (t.size() == cmd.size() + 1 || t[cmd.size() + 1] == ' ')) {
            std::string rest = ltrim(t.substr(1 + cmd.size()));
            if (is_drop_command(cmd) || cmd == "{" || cmd == "}") continue;
            if (cmd == "brief" || cmd == "short" || cmd == "details") t = rest;
            else if (cmd == "li" || cmd == "arg") t = "- " + rest;
            else if (cmd == "note") t = "**Note:** " + rest;
            else if (cmd == "warning") t = "**Warning:** " + rest;
            else if (cmd == "attention") t = "**Attention:** " + rest;
            else if (cmd == "see" || cmd == "sa") t = "See " + rest;
            if (rest.empty() && (cmd == "brief" || cmd == "details")) continue;
        }
        out.push_back(inline_markup(indent + t));
    }
    return out;
}

bool starts_paragraph(const std::string& line) {
    std::string t = ltrim(line);
    if (line.size() - t.size() >= 4) return false;
    if (starts_with(t, "```") || starts_with(t, "~~~") || starts_with(t, "#") || starts_with(t, ">") ||
        starts_with(t, "- ") || starts_with(t, "* ") || starts_with(t, "+ ") || starts_with(t, "|"))
        return false;
    std::size_t d = 0;
    while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
    return !(d > 0 && (t.compare(d, 2, ". ") == 0 || t.compare(d, 2, ") ") == 0));
}

std::string one_line(const std::vector<std::string>& lines, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to; ++i) {
        std::string word;
        for (char c : lines[i] + " ") {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!word.empty()) out += (out.empty() ? "" : " ") + word;
                word.clear();
            } else {
                word += c;
            }
        }
    }
    if (ends_with(out, ".") && !ends_with(out, "..")) out.pop_back();
    return out;
}

}  // namespace

Doc split_doc(const std::string& comment) {
    auto lines = convert(unindent(strip_markers(split_lines(comment))));
    // Dropped tags can leave blank runs.
    std::vector<std::string> tidy;
    for (auto& l : lines)
        if (!(l.empty() && (tidy.empty() || tidy.back().empty()))) tidy.push_back(l);
    while (!tidy.empty() && tidy.back().empty()) tidy.pop_back();
    Doc doc;
    if (tidy.empty()) return doc;
    if (!starts_paragraph(tidy.front())) {
        doc.description = one_line(tidy, 0, tidy.size());
        return doc;
    }
    std::size_t end = 0;
    while (end < tidy.size() && !tidy[end].empty()) ++end;
    doc.description = one_line(tidy, 0, end);
    for (std::size_t i = end + 1; i < tidy.size(); ++i) doc.details += tidy[i] + (i + 1 < tidy.size() ? "\n" : "");
    return doc;
}

}  // namespace docuconf
