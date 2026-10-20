#pragma once

// SKR query rule: published by the SKR Gate server component in the standard
// SA-MP/open.mp query rules (opcode 'r'), next to open.mp's own "version" and
// "allowed_clients". The SKR lobby reads it to mark servers with native SKR
// support; servers without it get a warning before connecting.
//
// open.mp reserves "version" and "allowed_clients" (LegacyNetwork), so SKR
// uses its own rule instead of changing those.

namespace skr::protocol {

constexpr const char* kSkrQueryRule = "skr_client";
constexpr const char* kSkrQueryRuleValue = "2";   // SKR protocol version

} // namespace skr::protocol
