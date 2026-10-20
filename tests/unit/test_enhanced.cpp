#include "protocol/enhanced.h"
#include "protocol/join_validation.h"
#include <bitstream.hpp>
#include <cstdio>
#include <string>

int main() {
    using namespace skr::protocol;
    int failures = 0, checks = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
    };
    EnhancedSession session;
    check(session.mode == ClientMode::Standard, "standard by default");
    check(!session.Confirm(1, 7, 1, 0), "premature confirm");
    check(!session.Begin(2, 7, 1, 0), "incompatible protocol");
    check(!session.Begin(1, 255, 1, 0), "unsupported capabilities");
    check(session.Begin(1, 7, 42, 100), "hello");
    check(!session.Begin(1, 7, 43, 101), "duplicate hello");
    check(!session.Confirm(1, 7, 41, 102), "wrong connection token");
    check(!session.Confirm(2, 7, 42, 102), "wrong confirm version");
    check(session.Confirm(1, 7, 42, 102), "confirmation");
    check(session.mode == ClientMode::Enhanced, "enhanced after confirmation");
    session = {};
    check(!session.Confirm(1, 7, 42, 103), "reconnect discards previous handshake");
    session.Begin(1, 7, 43, 100);
    check(!session.Confirm(1, 7, 43, 10100), "deadline is exclusive");
    session.Expire(10100);
    check(session.mode == ClientMode::Standard, "interrupted handshake falls back");
    // Real pinned NetworkBitStream, all 0..255 length values including old overflow >64.
    for (unsigned length = 0; length <= 255; ++length) {
        NetworkBitStream bs;
        bs.writeUINT32(4057); bs.writeUINT8(0);
        bs.writeUINT8(4); bs.Write("Test", 4);
        bs.writeUINT32(123);
        bs.writeUINT8(0);
        bs.writeUINT8(static_cast<uint8_t>(length));
        std::string version(length, 'X');
        if (length) bs.Write(version.data(), length);
        const auto full = bs.GetNumberOfBitsUsed();
        check(ValidateStandardJoin(bs), "complete join accepted structurally");
        check(bs.GetReadOffset() == 0, "offset restored on success");
        for (int bits = 0; bits < full; ++bits) {
            bs.SetWriteOffset(bits);
            check(!ValidateStandardJoin(bs), "truncated join rejected");
            check(bs.GetReadOffset() == 0, "offset restored on truncation");
        }
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
