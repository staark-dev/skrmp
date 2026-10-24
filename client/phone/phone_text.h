#pragma once
// PHONE-M4F — SKR Street Phone: bounded UTF-8 text fields.
//
// Pure editing helpers for the phone's text inputs (contact name/number,
// search, SMS draft). Fixed storage, byte limits match the wire protocol
// (phone_protocol.h counts bytes, not characters), and edits never split a
// UTF-8 sequence. No Win32, no ImGui: unit-tested in test_phone_m4f.cpp.

#include <cstddef>
#include <cstdint>

namespace skr::phone {

enum class FieldKind : uint8_t { Text, Digits };

struct TextField {
    static constexpr size_t kStorage = 128;
    char text[kStorage] = {};
    uint8_t len = 0;              // bytes, excluding the terminator
};

// Encodes a Unicode code point; returns the byte count (0 = not encodable).
inline size_t EncodeUtf8(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = static_cast<char>(cp); return 1; }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;          // lone surrogate
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

// Appends one code point if it is printable, allowed for the kind and fits in
// maxBytes (<= kStorage - 1). Returns true when the field changed.
inline bool TextInsert(TextField& f, uint32_t cp, size_t maxBytes, FieldKind kind) {
    if (maxBytes > TextField::kStorage - 1) maxBytes = TextField::kStorage - 1;
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return false;   // control chars
    if (kind == FieldKind::Digits && (cp < '0' || cp > '9')) return false;
    char enc[4];
    const size_t n = EncodeUtf8(cp, enc);
    if (!n || f.len + n > maxBytes) return false;
    for (size_t i = 0; i < n; ++i) f.text[f.len + i] = enc[i];
    f.len = static_cast<uint8_t>(f.len + n);
    f.text[f.len] = 0;
    return true;
}

// Removes the last whole code point.
inline bool TextBackspace(TextField& f) {
    if (!f.len) return false;
    size_t n = f.len - 1;
    while (n > 0 && (static_cast<unsigned char>(f.text[n]) & 0xC0) == 0x80) --n;
    f.len = static_cast<uint8_t>(n);
    f.text[n] = 0;
    return true;
}

inline void TextClear(TextField& f) {
    f.len = 0;
    f.text[0] = 0;
}

inline void TextSet(TextField& f, const char* s, size_t maxBytes) {
    TextClear(f);
    if (!s) return;
    if (maxBytes > TextField::kStorage - 1) maxBytes = TextField::kStorage - 1;
    size_t n = 0;
    while (s[n] && n < maxBytes) ++n;
    // Cut inside a sequence: drop its partial bytes, lead byte included.
    while (n > 0 && s[n] && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    for (size_t i = 0; i < n; ++i) f.text[i] = s[i];
    f.len = static_cast<uint8_t>(n);
    f.text[n] = 0;
}

// True when the text has something other than spaces.
inline bool TextHasContent(const TextField& f) {
    for (size_t i = 0; i < f.len; ++i)
        if (f.text[i] != ' ') return true;
    return false;
}

// Case-insensitive ASCII substring match (UTF-8 bytes compared as-is).
inline bool ContainsNoCase(const char* hay, const char* needle) {
    if (!needle || !*needle) return true;
    if (!hay) return false;
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && lower(*a) == lower(*b)) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

} // namespace skr::phone
