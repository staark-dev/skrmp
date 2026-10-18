#include "lobby/settings_io.h"
#include "lobby/flat_json.h"
#include <cstdio>
#include <map>

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
    };
    using namespace skr::lobby;
    Settings settings;
    settings.nickname = "Existing_Player";
    check(SettingsFromJson(R"({"resolution":"1600x900","volume":25,"windowed":true})", settings), "supported settings payload");
    check(settings.resolution == "1600x900" && settings.volume == 25 && settings.windowed && settings.nickname == "Existing_Player", "patch preserves identity");
    for (const auto* invalid : {"{}", "garbage", R"({"volume":101})", R"({"volume":-1})", R"({"volume":"25"})", R"({"volume":1.5})", R"({"volume":10,"volume":20})", R"({"windowed":true,"resolution":"invalid"})", R"({"volume":12}trailing)", R"({"gtaPath":"override"})", R"({"volume":12,})"}) {
        Settings original = settings;
        check(!SettingsFromJson(invalid, settings), invalid);
        check(SettingsToJson(settings) == SettingsToJson(original), "invalid payload is transactional");
    }
    std::string nickname;
    check(NicknameFromJson(R"({"nickname":"Player_123"})", nickname) && nickname == "Player_123", "valid nickname");
    check(!NicknameFromJson(R"({"nickname":"Bad\n[game]"})", nickname), "INI injection rejected");
    check(!NicknameFromJson(R"({"nickname":"ab"})", nickname), "short nickname rejected");
    uint16_t connectPort = 0;
    std::string connectAddress;
    check(ConnectRequestFromJson(R"({"address":"LOCALHOST:07777","nickname":"Player_123"})",
                                 connectAddress, connectPort, nickname) &&
          connectAddress == "localhost" && connectPort == 7777 && nickname == "Player_123",
          "valid connect request");
    check(!ConnectRequestFromJson(R"({"serverId":"dev","nickname":"Player_123"})",
                                  connectAddress, connectPort, nickname),
          "connect requires address");
    std::string input = "; comment\r\n[player]\r\nnickname=Old\r\n[game]\r\nresolution=auto\r\n[updates]\r\nurl=https://example.invalid/update\r\n[ui]\r\nlast_server=dev\r\n[servers]\r\nlocalhost:7777=Local\r\n";
    auto merged = MergeSettingsIni(input, settings);
    check(merged.find("[servers]\r\nlocalhost:7777=Local") != std::string::npos, "personal servers preserved");
    check(merged.find("url=https://example.invalid/update") != std::string::npos && merged.find("last_server=dev") != std::string::npos, "unrelated launcher preferences preserved");
    check(merged.find("nickname=Existing_Player") != std::string::npos && merged.find("resolution=1600x900") != std::string::npos, "canonical launcher keys updated");
    check(MergeSettingsIni(merged, settings) == merged, "repeated saves do not grow INI");
    PersonalServer server;
    check(PersonalServerFromJson(R"({"name":"Test","address":"LOCALHOST:07777"})", server) && server.address == "localhost:7777", "key order independent server parser");
    check(!PersonalServerFromJson(R"({"name":"address","garbage":"localhost:7777"})", server), "key in value cannot impersonate address");
    check(!PersonalServerFromJson(R"({"address":"localhost:7777})", server), "unterminated string rejected");
    std::string address;
    for (const auto* invalid : {"999.0.0.1:7777", "host:0", "host:65536", "::1:7777", "-host:7777", "host/path:7777"}) check(!NormalizeServerAddress(invalid, address), invalid);
    std::map<std::string, detail::JsonScalar> values;
    check(!detail::ParseFlatJson(std::string(4097, ' '), values), "oversized payload rejected");
    std::printf("Lobby settings: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
