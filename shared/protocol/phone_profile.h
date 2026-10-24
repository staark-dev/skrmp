#pragma once
#include "protocol/native_api.h"
namespace skr::protocol::phone {
// M5 extension, only sent with CapPhoneSetup. No M4 payload changes.
struct Profile {
    uint32_t requestId = 0;
    uint8_t stage = 0; // welcome, language, number, personalization, complete
    uint8_t language = 0; // 0 = English (only supported translation)
    uint8_t wallpaper = 0; // image / procedural skyline
    uint8_t scale = 100;
    uint8_t banners = 1;
};
inline bool ValidProfile(const Profile& p) {
    return p.stage <= 4 && p.language == 0 && p.wallpaper <= 1 &&
           p.scale >= 80 && p.scale <= 125 && p.banners <= 1;
}
inline size_t EncodeProfile(const Profile& p, uint8_t* out, size_t cap) {
    if (!ValidProfile(p)) return 0;
    api::Writer w(out, cap);
    w.U32(p.requestId); w.U8(p.stage); w.U8(p.language); w.U8(p.wallpaper);
    w.U8(p.scale); w.U8(p.banners); return w.size();
}
inline bool DecodeProfile(const uint8_t* data, size_t len, Profile& p) {
    api::Reader r(data, len);
    p.requestId=r.U32(); p.stage=r.U8(); p.language=r.U8();
    p.wallpaper=r.U8(); p.scale=r.U8(); p.banners=r.U8();
    return r.Done() && ValidProfile(p);
}
}
