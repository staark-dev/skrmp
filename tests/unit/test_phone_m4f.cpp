// PHONE-M4F regression tests: bounded UTF-8 text fields (phone_text.h),
// cross-app navigation (Controller::Jump), press-feedback ids for the M4F
// actions, and the wire limits the UI fields are sized against. No Win32,
// ImGui or game memory.
#include "phone/phone_controller.h"
#include "phone/phone_model.h"
#include "phone/phone_renderer.h"
#include "phone/phone_text.h"
#include "protocol/phone_protocol.h"

#include <cstdio>
#include <cstring>
#include <set>

using namespace skr::phone;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

namespace {

void TestTextInsert() {
    TextField f;
    CHECK(TextInsert(f, 'a', 5, FieldKind::Text) && f.len == 1 && std::strcmp(f.text, "a") == 0, "ascii insert");
    CHECK(!TextInsert(f, 0x08, 5, FieldKind::Text) && f.len == 1, "control char rejected");
    CHECK(!TextInsert(f, 0x7F, 5, FieldKind::Text), "DEL rejected");
    CHECK(!TextInsert(f, 0x85, 5, FieldKind::Text), "C1 control rejected");
    CHECK(TextInsert(f, 0x0219, 5, FieldKind::Text) && f.len == 3, "romanian s-comma is 2 bytes");
    CHECK(std::strcmp(f.text, "a\xC8\x99") == 0, "s-comma encoded as C8 99");
    CHECK(TextInsert(f, 'b', 5, FieldKind::Text) && TextInsert(f, 'c', 5, FieldKind::Text) && f.len == 5, "fill to limit");
    CHECK(!TextInsert(f, 'd', 5, FieldKind::Text) && f.len == 5, "byte limit enforced");
    TextField g;
    TextInsert(g, 'x', 4, FieldKind::Text); TextInsert(g, 'y', 4, FieldKind::Text); TextInsert(g, 'z', 4, FieldKind::Text);
    CHECK(!TextInsert(g, 0x0103, 4, FieldKind::Text) && g.len == 3, "multi-byte char that would overflow is rejected whole");
    CHECK(TextInsert(g, 0x1F600, 8, FieldKind::Text) && g.len == 7, "4-byte emoji fits");
    CHECK(!TextInsert(g, 0xD800, 20, FieldKind::Text), "lone surrogate rejected");
    TextField big;
    for (int i = 0; i < 300; ++i) TextInsert(big, 'q', 1000, FieldKind::Text);
    CHECK(big.len == TextField::kStorage - 1 && big.text[big.len] == 0, "storage cap even with huge maxBytes");
}

void TestDigits() {
    TextField f;
    CHECK(!TextInsert(f, 'a', 7, FieldKind::Digits) && !TextInsert(f, '-', 7, FieldKind::Digits), "digits field rejects letters");
    for (char ch : {'1', '0', '0', '0', '0', '0', '1', '9'}) TextInsert(f, static_cast<uint32_t>(ch), kFieldNumberMax, FieldKind::Digits);
    CHECK(f.len == 7 && std::strcmp(f.text, "1000001") == 0, "number field stops at 7 digits");
    CHECK(skr::protocol::phone::IsValidPhoneNumber(f.text, f.len), "typed number passes wire validation");
    TextField z;
    TextInsert(z, '0', 7, FieldKind::Digits);
    for (int i = 0; i < 6; ++i) TextInsert(z, '5', 7, FieldKind::Digits);
    CHECK(!skr::protocol::phone::IsValidPhoneNumber(z.text, z.len), "leading zero invalid on the wire");
}

void TestBackspace() {
    TextField f;
    TextInsert(f, 'a', 20, FieldKind::Text);
    TextInsert(f, 0x021B, 20, FieldKind::Text);          // ț (2 bytes)
    TextInsert(f, 0x1F600, 20, FieldKind::Text);         // emoji (4 bytes)
    CHECK(f.len == 7, "setup lengths");
    CHECK(TextBackspace(f) && f.len == 3, "backspace removes whole 4-byte sequence");
    CHECK(TextBackspace(f) && f.len == 1 && std::strcmp(f.text, "a") == 0, "backspace removes whole 2-byte sequence");
    CHECK(TextBackspace(f) && f.len == 0 && f.text[0] == 0, "backspace to empty");
    CHECK(!TextBackspace(f), "backspace on empty is a no-op");
}

void TestSetAndHelpers() {
    TextField f;
    TextSet(f, "Sal\xC8\x99", 4);                         // cut inside ș
    CHECK(f.len == 3 && std::strcmp(f.text, "Sal") == 0, "TextSet never splits a sequence");
    TextSet(f, "1000008", kFieldNumberMax);
    CHECK(f.len == 7 && std::strcmp(f.text, "1000008") == 0, "TextSet exact");
    TextSet(f, nullptr, 7);
    CHECK(f.len == 0, "TextSet null clears");
    TextField sp;
    TextSet(sp, "   ", 10);
    CHECK(!TextHasContent(sp), "spaces only = no content (send disabled)");
    TextSet(sp, " hi", 10);
    CHECK(TextHasContent(sp), "content detected");
    CHECK(ContainsNoCase("SKR_Player", "skr") && ContainsNoCase("SKR_Player", "PLAY"), "search is case-insensitive");
    CHECK(ContainsNoCase("1000001", "0001") && !ContainsNoCase("Sweet", "smoke"), "search substring");
    CHECK(ContainsNoCase("anything", ""), "empty query matches all");
}

void TestFieldLimitsMatchWire() {
    namespace pp = skr::protocol::phone;
    CHECK(kFieldNameMax == pp::kMaxContactName, "name field = wire contact name limit");
    CHECK(kFieldNumberMax == pp::kPhoneNumberLen, "number field = wire number length");
    CHECK(kFieldSmsMax == pp::kMaxSmsText, "draft field = wire SMS limit");
    CHECK(kFieldSmsMax < TextField::kStorage && kFieldNameMax < TextField::kStorage, "fields fit storage");
    // A full draft of 2-byte characters still encodes into one ClientRequest.
    pp::SmsSendRequest req;
    TextField draft;
    while (TextInsert(draft, 0x0103, kFieldSmsMax, FieldKind::Text)) {}
    CHECK(draft.len == 120, "60 x 'ă' = 120 bytes");
    std::memcpy(req.recipientNumber, "1000001", 7); req.recipientNumberLen = 7;
    std::memcpy(req.text, draft.text, draft.len); req.textLen = draft.len;
    uint8_t buf[256];
    CHECK(pp::EncodeSmsSend(req, buf, sizeof(buf)) > 0, "full UTF-8 draft encodes");
}

uint64_t Open(Controller& c) {
    uint64_t t = 1000;
    c.Open(t);
    t += 5000; c.Update(t);
    return t;
}

void TestJump() {
    Controller c;
    uint64_t t = Open(c);
    CHECK(c.GetState() == State::Home, "home after open");
    CHECK((c.Jump(Screen{AppId::Messages, 1}, t)), "jump home -> conversation");
    CHECK(c.Kind() == TransitionKind::Zoom && c.Forward(), "cross-app jump zooms forward");
    t += 1000; c.Update(t);
    CHECK((c.GetState() == State::AppActive && (c.Current() == Screen{AppId::Messages, 1})), "lands on Messages page 1");
    CHECK((!c.Jump(Screen{AppId::Messages, 1}, t)), "jump to current screen ignored");
    CHECK(c.Back(t), "back from conversation");
    t += 1000; c.Update(t);
    CHECK(((c.Current() == Screen{AppId::Messages, 0})), "back goes to thread list (page 0)");

    // Contacts details -> Messages conversation (Send message).
    CHECK(c.GoHome(t), "home");
    t += 1000; c.Update(t);
    c.OpenApp(AppId::Contacts, t); t += 1000; c.Update(t);
    c.OpenPage(1, t); t += 1000; c.Update(t);
    CHECK(((c.Current() == Screen{AppId::Contacts, 1})), "contact details");
    CHECK((c.Jump(Screen{AppId::Messages, 1}, t)), "contacts -> conversation");
    CHECK((!c.Jump(Screen{AppId::Phone, 0}, t)), "no jump mid-transition");
    t += 1000; c.Update(t);
    CHECK(((c.Current() == Screen{AppId::Messages, 1})), "conversation from contacts");

    // Same app: slide in the page direction.
    CHECK((c.Jump(Screen{AppId::Messages, 0}, t) && c.Kind() == TransitionKind::Slide && !c.Forward()), "same-app jump slides back");
    t += 1000; c.Update(t);

    Controller hidden;
    CHECK((!hidden.Jump(Screen{AppId::Messages, 1}, 10)), "no jump while hidden");
    CHECK((!c.Jump(Screen{AppId::Count, 0}, t)), "invalid app rejected");
}

void TestHitIds() {
    std::set<uint16_t> ids;
    bool unique = true;
    for (int a = 0; a <= static_cast<int>(Action::OpenBanner); ++a)
        for (int i = 0; i < 64; ++i)
            unique = ids.insert(HitId(static_cast<Action>(a), static_cast<uint8_t>(i))).second && unique;
    CHECK(unique, "HitId unique for every action x 64 indices (M4F actions no longer alias)");
    CHECK(HitId(Action::OpenContact, 17) != HitId(Action::OpenContact, 1), "contact 17 != contact 1 (old 4-bit index aliased)");

    Controller c;
    uint64_t t = Open(c);
    c.Press(HitId(Action::SendSms, 0), t);
    CHECK(c.PressAmount(HitId(Action::SendSms, 0), t) == 1.0f, "16-bit press id held");
    CHECK(c.PressAmount(HitId(Action::FocusField, 0), t) == 0.0f, "other id not pressed");
}

void TestModelDefaults() {
    Model m;
    CHECK(m.sync == SyncState::Offline && !m.ownNumber[0], "model starts offline with no number (no fabricated identity)");
    CHECK(!m.contactCount && !m.threadCount && !m.convo.count, "no sample contacts / threads / messages");
    CHECK(m.contactOpResult == Model::OpResult::None && !m.contactOpPending, "no pending contact op");
    Views v;
    CHECK(v.focus == Field::None && !v.contacts.confirmRemove, "no focus, no confirm sheet");
    Message msg;
    CHECK(msg.id == 0 && msg.requestId == 0 && msg.delivery == Delivery::Received, "message defaults");
}

} // namespace

int main() {
    TestTextInsert();
    TestDigits();
    TestBackspace();
    TestSetAndHelpers();
    TestFieldLimitsMatchWire();
    TestJump();
    TestHitIds();
    TestModelDefaults();
    std::printf("\nPhone M4F tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
