#pragma once
#include <cstdint>

namespace skr::protocol {
// Non-consuming structural check. Core remains responsible for semantics.
// open.mp 3d20f3dc0e26917264cdc8585c65913e080a143c, Shared/NetCode/core.hpp.
template<class Stream> bool ValidateStandardJoin(Stream& bs) {
    const int saved = bs.GetReadOffset();
    struct Restore {
        Stream& bs; int offset;
        ~Restore() { bs.SetReadOffset(offset); }
    } restore{bs, saved};
    auto skipString = [&]() {
        uint8_t length = 0;
        if (!bs.readUINT8(length)) return false;
        const int remaining = bs.GetNumberOfBitsUsed() - bs.GetReadOffset();
        if (remaining < static_cast<int>(length) * 8) return false;
        bs.SetReadOffset(bs.GetReadOffset() + length * 8);
        return true;
    };
    uint32_t version = 0, challenge = 0;
    uint8_t mod = 0;
    return bs.readUINT32(version) && bs.readUINT8(mod) && skipString() &&
        bs.readUINT32(challenge) && skipString() && skipString();
}
}
