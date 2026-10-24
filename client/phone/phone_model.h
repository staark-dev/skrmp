#pragma once
// PHONE-001 — SKR Street Phone: presentation model + per-app view state.
//
// The phone is a presentation layer. Everything in Model arrives from the
// gamemode (numbers, contacts, SMS, calls) or from the existing GPS engine;
// the client never stores or invents phone data. All storage is fixed-size so
// rendering never allocates; lists are bounded.
//
// Delivery states follow the server-authority rule: an outgoing message stays
// Sending until the server acknowledges it — the UI never claims delivery.
//
// Model::previewData marks representative data used only by the offline
// preview harness. The phone itself does not display a preview watermark. In
// game the model starts empty and is filled only by phone_net responses.

#include <cstdint>
#include <cstring>

#include "phone/phone_controller.h"
#include "phone/phone_text.h"
#include "protocol/phone_profile.h"

namespace skr::phone {

constexpr int kMaxContacts = 64;
constexpr int kMaxThreads = 16;
constexpr int kMaxMessages = 48;
constexpr int kMaxDestinations = 12;

template <size_t N>
inline void CopyText(char (&dst)[N], const char* src) {
    size_t i = 0;
    if (src)
        for (; i + 1 < N && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

struct Contact {
    char name[24] = {};
    char number[16] = {};
    uint8_t hue = 0;              // avatar colour index
};

struct ThreadSummary {
    char peer[24] = {};
    char number[16] = {};         // peer phone number (server-provided)
    char preview[48] = {};
    char time[8] = {};
    uint8_t unread = 0;
    uint8_t hue = 0;
};

enum class Delivery : uint8_t {
    Received = 0,                 // incoming
    Sending,                      // outgoing, no server acknowledgement yet
    Delivered,                    // outgoing, stored by the server (shown as "Sent")
    Failed,                       // outgoing, rejected by the server
};

struct Message {
    char text[121] = {};          // gamemode SKR_PHONE_SMS_MAX_LEN = 120
    char time[8] = {};
    Delivery delivery = Delivery::Received;
    uint32_t id = 0;              // server message id (0 = not yet stored)
    uint32_t requestId = 0;       // client request id while Sending
};

struct Conversation {
    char peer[24] = {};
    char number[16] = {};
    uint8_t hue = 0;
    Message items[kMaxMessages];
    int count = 0;
    bool loading = false;         // history request in flight
    bool hasMore = false;         // older pages exist on the server
    uint8_t pages = 0;            // history pages merged so far
};

// Server phone identity / backend availability, as seen by the client.
enum class SyncState : uint8_t {
    Offline = 0,                  // CapPhone not negotiated (old server / phone disabled)
    Loading,                      // waiting for PhoneSync
    Ready,                        // own number known
    Error,                        // server refused (e.g. not logged in yet)
};

enum class NavStatus : uint8_t { Idle, Calculating, Navigating, Arrived, Unavailable };

struct Destination {
    char name[32] = {};
    char area[40] = {};
    float distanceM = 0;
};

struct GpsInfo {
    NavStatus status = NavStatus::Idle;
    char destName[32] = {};
    char destArea[40] = {};
    float remainingM = 0;
    Destination list[kMaxDestinations];   // server-provided (API-003.3)
    int count = 0;
};

struct Caller {
    char name[24] = {};
    char number[16] = {};
    uint8_t hue = 0;
};

struct Banner {
    AppId app = AppId::Messages;
    char title[32] = {};
    char text[64] = {};
    char number[16] = {};         // Messages: sender, opened when the banner is tapped
    uint8_t hue = 0;
};

struct StatusInfo {
    char time[8] = {};
    char date[32] = {};
    char location[48] = {};
    uint8_t signal = 4;           // 0..4 bars
    uint8_t battery = 100;        // percent
};

struct PhoneSettings {
    float uiScale = 1.0f;
    char openKey[8] = "L";
    bool vibrate = true;
    bool sounds = true;
    bool banners = true;
};

struct Model {
    bool previewData = false;
    bool setupSupported = false;
    bool profileLoaded = false;
    bool profilePending = false;
    protocol::phone::Profile profile;
    SyncState sync = SyncState::Offline;
    char syncError[48] = {};
    char ownNumber[16] = {};
    bool contactsLoading = false;
    bool threadsLoading = false;
    bool threadsHasMore = false;
    // Contact add/remove: true while the server has not answered; result is
    // set once (Ok / Failed) and consumed by the input layer.
    enum class OpResult : uint8_t { None = 0, Ok, Failed };
    bool contactOpPending = false;
    OpResult contactOpResult = OpResult::None;
    // Short status line shown inside the phone (request failures, confirmations).
    char notice[64] = {};
    bool noticeError = false;
    uint64_t noticeAt = 0;
    StatusInfo status;
    Contact contacts[kMaxContacts];
    int contactCount = 0;
    ThreadSummary threads[kMaxThreads];
    int threadCount = 0;
    Conversation convo;
    GpsInfo gps;
    Caller caller;
    Banner banner;
    PhoneSettings settings;
};

// Text inputs. One field at most has keyboard focus.
enum class Field : uint8_t { None = 0, ContactSearch, ContactName, ContactNumber, SmsDraft, NewMessageTo };

// Byte limits (wire protocol: kMaxContactName 23, kPhoneNumberLen 7, kMaxSmsText 120).
constexpr size_t kFieldNameMax = 23;
constexpr size_t kFieldNumberMax = 7;
constexpr size_t kFieldSmsMax = 120;
constexpr size_t kFieldSearchMax = 23;

// Contacts page 1 shows either one contact or the add form.
enum class ContactsSub : uint8_t { Details = 0, Add };
// Messages page 1 shows either a conversation or the new-message recipient form.
enum class MessagesSub : uint8_t { Conversation = 0, Compose };

// Per-app view state. Each app owns its own struct; no app reads another's.
struct ContactsView {
    float scroll = 0;
    int selected = -1;
    ContactsSub sub = ContactsSub::Details;
    int detail = -1;              // contact index shown on page 1
    char detailNumber[16] = {};   // re-resolved after list refreshes
    bool confirmRemove = false;
    TextField search, name, number;
};
struct MessagesView {
    float listScroll = 0;
    float scroll = 0;
    int thread = -1;
    MessagesSub sub = MessagesSub::Conversation;
    TextField draft, to;
};
struct GpsView { float scroll = 0; int selected = -1; };
struct SettingsView { int focused = -1; };
struct PhoneView { uint64_t copiedAt = 0; };

struct Views {
    ContactsView contacts;
    MessagesView messages;
    GpsView gps;
    SettingsView settings;
    PhoneView phone;
    Field focus = Field::None;
};

} // namespace skr::phone
