#include "client_policy.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace skr::gate_omp {
namespace {

constexpr size_t kMaxPolicyFileBytes = 256 * 1024;
constexpr unsigned kMaxJsonDepth = 32;
constexpr uint64_t kMinEnhancedTimeoutMs = 1000;
constexpr uint64_t kMaxEnhancedTimeoutMs = 120000;

class JsonCursor {
public:
    explicit JsonCursor(const std::string& input) : input_(input) {}

    bool End() {
        SkipWhitespace();
        return pos_ == input_.size();
    }

    bool Consume(char expected) {
        SkipWhitespace();
        if (pos_ >= input_.size() || input_[pos_] != expected) return false;
        ++pos_;
        return true;
    }

    bool ParseString(std::string& out) {
        SkipWhitespace();
        if (pos_ >= input_.size() || input_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (pos_ >= input_.size()) return false;
            const char escaped = input_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t code = 0;
                    if (!ParseHex4(code)) return false;
                    if (code <= 0x7F) out.push_back(static_cast<char>(code));
                    else out.push_back('?');
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool ParseBool(bool& out) {
        SkipWhitespace();
        if (MatchLiteral("true")) { out = true; return true; }
        if (MatchLiteral("false")) { out = false; return true; }
        return false;
    }

    bool ParseUnsigned(uint64_t& out) {
        SkipWhitespace();
        const size_t start = pos_;
        if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
        if (input_[pos_] == '0') {
            ++pos_;
            if (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
        } else {
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == '.' || input_[pos_] == 'e' || input_[pos_] == 'E')) return false;
        out = 0;
        for (size_t i = start; i < pos_; ++i) {
            const uint64_t digit = static_cast<uint64_t>(input_[i] - '0');
            if (out > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
            out = out * 10 + digit;
        }
        return true;
    }

    bool ParseFloat(float& out) {
        SkipWhitespace();
        const size_t start = pos_;
        // Accept optional minus
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            pos_ = start; return false;
        }
        // Integer part
        if (input_[pos_] == '0') ++pos_;
        else { while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_; }
        // Fractional part
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                pos_ = start; return false;
            }
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        // Exponent
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                pos_ = start; return false;
            }
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ == start) return false;
        std::string numStr(input_, start, pos_ - start);
        char* end = nullptr;
        out = std::strtof(numStr.c_str(), &end);
        return end == numStr.c_str() + numStr.size();
    }

    bool ParseStringArray(std::vector<std::string>& out) {
        if (!Consume('[')) return false;
        out.clear();
        SkipWhitespace();
        if (Consume(']')) return true;
        for (;;) {
            std::string value;
            if (!ParseString(value)) return false;
            out.push_back(std::move(value));
            SkipWhitespace();
            if (Consume(']')) return true;
            if (!Consume(',')) return false;
        }
    }

    bool SkipValue(unsigned depth = 0) {
        if (depth > kMaxJsonDepth) return false;
        SkipWhitespace();
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '"') {
            std::string ignored;
            return ParseString(ignored);
        }
        if (input_[pos_] == '{') {
            ++pos_;
            SkipWhitespace();
            if (Consume('}')) return true;
            for (;;) {
                std::string key;
                if (!ParseString(key) || !Consume(':') || !SkipValue(depth + 1)) return false;
                if (Consume('}')) return true;
                if (!Consume(',')) return false;
            }
        }
        if (input_[pos_] == '[') {
            ++pos_;
            SkipWhitespace();
            if (Consume(']')) return true;
            for (;;) {
                if (!SkipValue(depth + 1)) return false;
                if (Consume(']')) return true;
                if (!Consume(',')) return false;
            }
        }
        if (MatchLiteral("true") || MatchLiteral("false") || MatchLiteral("null")) return true;
        return SkipNumber();
    }

    size_t Position() const { return pos_; }

private:
    void SkipWhitespace() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }

    bool MatchLiteral(const char* literal) {
        const size_t start = pos_;
        while (*literal) {
            if (pos_ >= input_.size() || input_[pos_] != *literal) { pos_ = start; return false; }
            ++pos_;
            ++literal;
        }
        return true;
    }

    bool ParseHex4(uint32_t& out) {
        if (input_.size() - pos_ < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
            else return false;
            out = (out << 4) | digit;
        }
        return true;
    }

    bool SkipNumber() {
        const size_t start = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-') ++pos_;
        if (pos_ >= input_.size()) return false;
        if (input_[pos_] == '0') ++pos_;
        else {
            if (!std::isdigit(static_cast<unsigned char>(input_[pos_]))) { pos_ = start; return false; }
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || !std::isdigit(static_cast<unsigned char>(input_[pos_]))) return false;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        }
        return pos_ > start;
    }

    const std::string& input_;
    size_t pos_ = 0;
};

struct ParsedPolicy {
    bool standard = true;
    bool enhanced = true;
    bool allow03DL = false;
    uint64_t timeoutMs = 10000;
    bool hasAllowlist = false;
    std::vector<std::string> allowlist;
    std::vector<std::string> requireEnhancedFor;
    std::vector<std::string> warnings;
    ServerArtwork artwork;
    HudConfig hud;
};

// SSRF-safe: only HTTPS URLs with a public-looking host are accepted.
bool IsValidArtworkUrl(const std::string& url) {
    if (url.empty()) return true;  // optional field
    if (url.size() > 512) return false;
    if (url.rfind("https://", 0) != 0) return false;
    // Block localhost and private-network hosts after "https://"
    const std::string host = url.substr(8, url.find('/', 8) - 8);
    if (host.empty()) return false;
    if (host.find("localhost") != std::string::npos) return false;
    if (host.find("127.") == 0) return false;
    if (host.find("10.") == 0) return false;
    if (host.find("192.168.") == 0) return false;
    if (host.find("169.254.") == 0) return false;
    if (host.rfind("172.", 0) == 0) {
        // 172.16.0.0 – 172.31.255.255
        const int second = atoi(host.c_str() + 4);
        if (second >= 16 && second <= 31) return false;
    }
    if (host == "[::1]") return false;
    return true;
}

bool ParseArtworkObject(JsonCursor& cursor, ServerArtwork& artwork, std::string& error) {
    if (!cursor.Consume('{')) { error = "server_artwork must be a JSON object"; return false; }
    if (cursor.Consume('}')) return true;
    std::unordered_set<std::string> seen;
    for (;;) {
        std::string key;
        if (!cursor.ParseString(key) || !cursor.Consume(':')) { error = "invalid server_artwork member"; return false; }
        if (!seen.insert(key).second) { error = "duplicate server_artwork key: " + key; return false; }
        if (key == "logo_url") {
            if (!cursor.ParseString(artwork.logoUrl)) { error = "logo_url must be a string"; return false; }
            if (!IsValidArtworkUrl(artwork.logoUrl)) { error = "logo_url must be a valid HTTPS URL (no private hosts)"; return false; }
        } else if (key == "banner_url") {
            if (!cursor.ParseString(artwork.bannerUrl)) { error = "banner_url must be a string"; return false; }
            if (!IsValidArtworkUrl(artwork.bannerUrl)) { error = "banner_url must be a valid HTTPS URL (no private hosts)"; return false; }
        } else if (!cursor.SkipValue()) {
            error = "invalid JSON value for server_artwork key: " + key;
            return false;
        }
        if (cursor.Consume('}')) return true;
        if (!cursor.Consume(',')) { error = "expected ',' or '}' in server_artwork"; return false; }
    }
}

bool ParseHudObject(JsonCursor& cursor, HudConfig& hud, std::vector<std::string>& warnings,
                    std::string& error) {
    if (!cursor.Consume('{')) { error = "hud must be a JSON object"; return false; }
    if (cursor.Consume('}')) return true;
    std::unordered_set<std::string> seen;
    for (;;) {
        std::string key;
        if (!cursor.ParseString(key) || !cursor.Consume(':')) { error = "invalid hud member"; return false; }
        if (!seen.insert(key).second) { error = "duplicate hud key: " + key; return false; }
        if (key == "default_radar_style") {
            std::string value;
            if (!cursor.ParseString(value)) { error = "default_radar_style must be a string"; return false; }
            if (value == "round" || value == "default") {
                hud.defaultRadarStyle = protocol::RadarStyle::Round;
            } else if (value == "square") {
                hud.defaultRadarStyle = protocol::RadarStyle::Square;
            } else {
                error = "default_radar_style must be \"round\", \"square\" or \"default\"";
                return false;
            }
        } else if (key == "default_radar_size") {
            std::string value;
            if (!cursor.ParseString(value)) { error = "default_radar_size must be a string"; return false; }
            if (value == "default") {
                hud.defaultRadarSize = protocol::RadarSizeMode::Default;
            } else if (value == "custom") {
                hud.defaultRadarSize = protocol::RadarSizeMode::Custom;
            } else {
                error = "default_radar_size must be \"default\" or \"custom\"";
                return false;
            }
        } else if (key == "default_radar_scale") {
            if (!cursor.ParseFloat(hud.defaultRadarScale)) {
                error = "default_radar_scale must be a number";
                return false;
            }
            if (hud.defaultRadarScale < protocol::kRadarMinScale ||
                hud.defaultRadarScale > protocol::kRadarMaxScale) {
                error = "default_radar_scale must be between " +
                        std::to_string(protocol::kRadarMinScale) + " and " +
                        std::to_string(protocol::kRadarMaxScale);
                return false;
            }
        } else if (key == "enable_speedometer") {
            if (!cursor.ParseBool(hud.enableSpeedometer)) {
                error = "enable_speedometer must be boolean"; return false;
            }
        } else if (key == "enable_minimap") {
            if (!cursor.ParseBool(hud.enableMinimap)) {
                error = "enable_minimap must be boolean"; return false;
            }
        } else if (key == "enable_phone") {
            if (!cursor.ParseBool(hud.enablePhone)) {
                error = "enable_phone must be boolean"; return false;
            }
        } else if (!cursor.SkipValue()) {
            error = "invalid JSON value for hud key: " + key;
            return false;
        } else {
            warnings.push_back("unknown hud key ignored: " + key);
        }
        if (cursor.Consume('}')) return true;
        if (!cursor.Consume(',')) { error = "expected ',' or '}' in hud"; return false; }
    }
}

bool ParsePolicyObject(JsonCursor& cursor, ParsedPolicy& parsed, std::string& error) {
    if (!cursor.Consume('{')) { error = "client_policy must be a JSON object"; return false; }
    std::unordered_set<std::string> seen;
    if (cursor.Consume('}')) return true;
    for (;;) {
        std::string key;
        if (!cursor.ParseString(key) || !cursor.Consume(':')) { error = "invalid client_policy member"; return false; }
        if (!seen.insert(key).second) { error = "duplicate client_policy key: " + key; return false; }
        if (key == "allow_standard_clients") {
            if (!cursor.ParseBool(parsed.standard)) { error = key + " must be boolean"; return false; }
        } else if (key == "allow_enhanced_clients") {
            if (!cursor.ParseBool(parsed.enhanced)) { error = key + " must be boolean"; return false; }
        } else if (key == "allowed_client_types") {
            if (!cursor.ParseStringArray(parsed.allowlist)) { error = key + " must be an array of strings"; return false; }
            parsed.hasAllowlist = true;
        } else if (key == "allow_03dl") {
            if (!cursor.ParseBool(parsed.allow03DL)) { error = key + " must be boolean"; return false; }
        } else if (key == "enhanced_negotiation_timeout_ms") {
            if (!cursor.ParseUnsigned(parsed.timeoutMs)) { error = key + " must be an unsigned integer"; return false; }
        } else if (key == "require_enhanced_for") {
            if (!cursor.ParseStringArray(parsed.requireEnhancedFor)) { error = key + " must be an array of strings"; return false; }
        } else {
            if (!cursor.SkipValue()) { error = "invalid JSON value for client_policy key: " + key; return false; }
            parsed.warnings.push_back("unknown client_policy key ignored: " + key);
        }
        if (cursor.Consume('}')) return true;
        if (!cursor.Consume(',')) { error = "expected ',' or '}' in client_policy"; return false; }
    }
}

bool ParseRoot(const std::string& json, ParsedPolicy& parsed, std::string& error) {
    JsonCursor cursor(json);
    if (!cursor.Consume('{')) { error = "root JSON value must be an object"; return false; }
    bool foundPolicy = false;
    std::unordered_set<std::string> seen;
    if (!cursor.Consume('}')) {
        for (;;) {
            std::string key;
            if (!cursor.ParseString(key) || !cursor.Consume(':')) { error = "invalid root JSON member"; return false; }
            if (!seen.insert(key).second) { error = "duplicate root key: " + key; return false; }
            if (key == "client_policy") {
                if (!ParsePolicyObject(cursor, parsed, error)) return false;
                foundPolicy = true;
            } else if (key == "server_artwork") {
                if (!ParseArtworkObject(cursor, parsed.artwork, error)) return false;
            } else if (key == "hud") {
                if (!ParseHudObject(cursor, parsed.hud, parsed.warnings, error)) return false;
            } else if (!cursor.SkipValue()) {
                error = "invalid JSON value for root key: " + key;
                return false;
            }
            if (cursor.Consume('}')) break;
            if (!cursor.Consume(',')) { error = "expected ',' or '}' in root object"; return false; }
        }
    }
    if (!cursor.End()) { error = "trailing data after root JSON object"; return false; }
    if (!foundPolicy) parsed.warnings.push_back("client_policy missing; backward-compatible defaults applied");
    return true;
}

} // namespace

void ClientPolicy::ResetDefaults() {
    valid_ = false;
    allowSamp037_ = true;
    allowEnhanced_ = true;
    allowOpenMp_ = true;
    allowSamp03DL_ = false;
    enhancedTimeoutMs_ = 10000;
    warnings_.clear();
    artwork_ = ServerArtwork{};
    hud_ = HudConfig{};
}

bool ClientPolicy::LoadFromFile(const char* path, std::string& error) {
    ResetDefaults();
    if (!path || !*path) { error = "policy path is empty"; return false; }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = std::string("policy file is missing or inaccessible: ") + path;
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > kMaxPolicyFileBytes) {
        error = "policy file is unreadable or exceeds 256 KiB";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) { error = "failed while reading policy file"; return false; }
    return LoadFromJson(buffer.str(), error);
}

bool ClientPolicy::LoadFromJson(const std::string& json, std::string& error) {
    ResetDefaults();
    ParsedPolicy parsed;
    if (!ParseRoot(json, parsed, error)) return false;
    if (parsed.timeoutMs < kMinEnhancedTimeoutMs || parsed.timeoutMs > kMaxEnhancedTimeoutMs) {
        error = "enhanced_negotiation_timeout_ms must be between 1000 and 120000";
        return false;
    }
    if (!parsed.requireEnhancedFor.empty()) {
        error = "require_enhanced_for entries are not implemented; use an empty array";
        return false;
    }

    bool listed037 = !parsed.hasAllowlist;
    bool listedEnhanced = !parsed.hasAllowlist;
    bool listedOpenMp = !parsed.hasAllowlist;
    std::unordered_set<std::string> listed;
    for (const std::string& type : parsed.allowlist) {
        if (!listed.insert(type).second) { error = "duplicate allowed_client_types entry: " + type; return false; }
        if (type == "samp-0.3.7") listed037 = true;
        else if (type == "skr-enhanced") listedEnhanced = true;
        else if (type == "openmp") listedOpenMp = true;
        else { error = "unsupported allowed_client_types entry: " + type; return false; }
    }

    allowSamp037_ = parsed.standard && listed037;
    allowOpenMp_ = parsed.standard && listedOpenMp;
    allowSamp03DL_ = parsed.standard && parsed.allow03DL;
    allowEnhanced_ = parsed.enhanced && listedEnhanced;
    enhancedTimeoutMs_ = parsed.timeoutMs;
    artwork_ = std::move(parsed.artwork);
    hud_ = parsed.hud;
    warnings_ = std::move(parsed.warnings);

    if (parsed.hasAllowlist && !parsed.standard && (listed037 || listedOpenMp))
        warnings_.push_back("standard client entries disabled by allow_standard_clients=false");
    if (parsed.hasAllowlist && !parsed.enhanced && listedEnhanced)
        warnings_.push_back("skr-enhanced disabled by allow_enhanced_clients=false");
    if (parsed.allow03DL && !parsed.standard)
        warnings_.push_back("allow_03dl disabled by allow_standard_clients=false");

    if (!allowSamp037_ && !allowOpenMp_ && !allowSamp03DL_ && !allowEnhanced_) {
        error = "client policy has no effective allowed client type";
        return false;
    }
    valid_ = true;
    error.clear();
    return true;
}

AdmissionDecision ClientPolicy::Evaluate(WireClientType type) const {
    if (!valid_) return AdmissionDecision::DenyInvalidPolicy;
    switch (type) {
        case WireClientType::OpenMp:
            return allowOpenMp_ ? AdmissionDecision::Allow : AdmissionDecision::DenyOpenMp;
        case WireClientType::Samp03DL:
            return allowSamp03DL_ ? AdmissionDecision::Allow : AdmissionDecision::DenySamp03DL;
        case WireClientType::Samp037:
            if (allowSamp037_) return AdmissionDecision::Allow;
            if (allowEnhanced_) return AdmissionDecision::RequireEnhanced;
            return AdmissionDecision::DenySamp037;
        default:
            return AdmissionDecision::DenyUnsupported;
    }
}

std::string ClientPolicy::AdvertisedClients() const {
    if (!valid_) return "none";
    std::string value;
    const auto append = [&value](const char* name) {
        if (!value.empty()) value += ", ";
        value += name;
    };
    if (allowSamp037_) append("SA-MP 0.3.7");
    if (allowEnhanced_) append("SKR Client");
    if (allowOpenMp_) append("open.mp");
    if (allowSamp03DL_) append("SA-MP 0.3.DL");
    return value;
}

const char* ClientPolicy::ModeName() const {
    if (!valid_) return "disabled";
    const bool standard = allowSamp037_ || allowOpenMp_ || allowSamp03DL_;
    if (standard && allowEnhanced_) return "hybrid";
    if (allowEnhanced_) return "enhanced";
    return "standard";
}

const char* AdmissionReason(AdmissionDecision decision) {
    switch (decision) {
        case AdmissionDecision::Allow: return "allowed";
        case AdmissionDecision::RequireEnhanced: return "SKR Enhanced negotiation required";
        case AdmissionDecision::DenySamp037: return "SA-MP 0.3.7 disabled by policy";
        case AdmissionDecision::DenySamp03DL: return "SA-MP 0.3.DL disabled by policy";
        case AdmissionDecision::DenyOpenMp: return "open.mp Client disabled by policy";
        case AdmissionDecision::DenyEnhanced: return "SKR Enhanced disabled by policy";
        case AdmissionDecision::DenyUnsupported: return "unsupported client protocol";
        case AdmissionDecision::DenyInvalidPolicy: return "client policy failed to initialize";
        default: return "unknown client policy decision";
    }
}

} // namespace skr::gate_omp
