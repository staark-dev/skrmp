#pragma once
#include <map>
#include <string>

namespace skr::lobby::detail {
struct JsonScalar { std::string text; bool quoted = false; };
// Deliberately restricted wire format: flat objects, string/bool/integer values.
// No nested objects, duplicate keys, null, floats or unsupported escapes.
inline bool ParseFlatJson(const std::string& input, std::map<std::string, JsonScalar>& out) {
    out.clear();
    if (input.size() > 4096) return false;
    size_t p = 0;
    auto space = [&] { while (p < input.size() && (input[p] == ' ' || input[p] == '\t' || input[p] == '\r' || input[p] == '\n')) ++p; };
    auto string = [&](std::string& value) {
        value.clear();
        if (p == input.size() || input[p++] != '"') return false;
        while (p < input.size()) {
            char c = input[p++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 32) return false;
            if (c == '\\') {
                if (p == input.size()) return false;
                c = input[p++];
                switch (c) {
                    case '"': case '\\': case '/': break;
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    default: return false;
                }
            }
            value += c;
        }
        return false;
    };
    space();
    if (p == input.size() || input[p++] != '{') return false;
    space();
    if (p < input.size() && input[p] == '}') { ++p; space(); return p == input.size(); }
    for (;;) {
        std::string key;
        if (!string(key)) return false;
        space();
        if (p == input.size() || input[p++] != ':') return false;
        space();
        JsonScalar value;
        if (p < input.size() && input[p] == '"') {
            value.quoted = true;
            if (!string(value.text)) return false;
        } else {
            size_t start = p;
            while (p < input.size() && input[p] != ',' && input[p] != '}' && input[p] != ' ' && input[p] != '\t' && input[p] != '\r' && input[p] != '\n') ++p;
            value.text = input.substr(start, p - start);
            if (value.text != "true" && value.text != "false") {
                if (value.text.empty()) return false;
                size_t i = value.text[0] == '-' ? 1 : 0;
                if (i == value.text.size() || (value.text[i] == '0' && i + 1 != value.text.size())) return false;
                for (; i < value.text.size(); ++i) if (value.text[i] < '0' || value.text[i] > '9') return false;
            }
        }
        if (!out.emplace(key, value).second) return false;
        space();
        if (p == input.size()) return false;
        if (input[p] == '}') { ++p; space(); return p == input.size(); }
        if (input[p++] != ',') return false;
        space();
    }
}
} // namespace skr::lobby::detail
