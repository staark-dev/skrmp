# SKR Runtime client rejection reasons

Date: 2026-10-21  
Target runtime: `server-runtime-branded-test/` only

## Result

Task #16 admission decisions are unchanged: SA-MP 0.3.7, SKR Enhanced and
open.mp are allowed by the hybrid policy, while SA-MP 0.3.DL remains denied.
Rejection presentation never temporarily admits a denied client.

The five stable reason codes are:

- `UNSUPPORTED_CLIENT_VERSION`
- `CLIENT_VERSION_NOT_ALLOWED`
- `ENHANCED_REQUIRED`
- `AUTHENTICATION_FAILED`
- `ENHANCED_NEGOTIATION_TIMEOUT`

## Protocol audit

LegacyNetwork recognizes the fixed wire versions 4057 (SA-MP 0.3.7) and 4062
(SA-MP 0.3.DL) and validates the challenge token before creating an `IPlayer`.
An invalid or unsupported wire version is rejected by the existing SA-MP new
connection result RPC (RPC 130) with `VersionMismatch`, followed by the normal
disconnect path.

The supported RPC 130 result values are `Ignore`, `VersionMismatch`, `BadName`,
`BadMod`, `NoPlayerSlot` and `Success`. Vanilla SA-MP/open.mp clients do not
define an arbitrary pre-admission text field in this result. RakNet's built-in
connection failures (`ID_NO_FREE_INCOMING_CONNECTIONS`, `ID_CONNECTION_BANNED`,
`ID_INVALID_PASSWORD` and `ID_DISCONNECTION_NOTIFICATION`) are also fixed
identifiers, not custom text containers.

Consequently:

| Client/path | Player-visible behavior | Detailed reason |
| --- | --- | --- |
| Unknown legacy wire version, before `IPlayer` exists | Client's standard version-mismatch/disconnect UI | Server-side LegacyNetwork behavior only |
| Vanilla SA-MP 0.3.DL denied by Gate | Existing RPC 130 `VersionMismatch`, then disconnect | Structured server log |
| Vanilla client rejected for auth/enhanced policy | Existing RPC 130 `BadMod`, then disconnect | Structured server log |
| Negotiated SKR Enhanced client | Existing SKR RPC 241, versioned fixed 8-byte rejection envelope, then RPC 130 and disconnect | CEF lobby message or launcher message, stable code and supported-client list |
| Older SKR client | Ignores the unknown RPC 241 step, then receives the standard RPC 130/disconnect | Structured server log |

RPC 241 was already the SKR session-information channel. The rejection envelope
uses that RPC with a new step value; no new packet ID, RakNet ABI or open.mp ABI
was introduced.

## Implementation

- `shared/protocol/rejection.h` owns the fixed 8-byte envelope, reason codes,
  strict decoder and friendly text.
- Gate sends the structured notice only when SKR Enhanced capability is known,
  always retains the standard legacy rejection, records a non-sensitive
  structured log and then kicks.
- The embedded client emits the existing CEF bridge event
  `skr:connection:rejected`.
- The external-lobby build transports the same reason through a namespaced GTA
  process exit code; the launcher decodes it and returns to the existing lobby.
- The non-external launcher displays the decoded message using its existing
  Windows message path.

No authentication proof, token, public key, credential or private material is
included in the rejection payload or rejection log.

## Validation evidence

Release builds completed for Gate, the internal-lobby client/launcher and the
external-lobby client/launcher.

- Rejection protocol tests: 40 checks, 0 failures.
- Task #16 client-policy tests: passed.
- Enhanced negotiation tests: 588301 checks, 0 failures.
- Final isolated-runtime smoke test: server started, loaded Gate UID
  `00000000534b5247`, Cef and RakNetLegacyNetwork, published
  `allowed_clients=SA-MP 0.3.7, SKR Client, open.mp`, reported SA-MP 0.3.DL as
  denied, and started Legacy Network on port 7777.
- Real runtime evidence is in `server-runtime-branded-test/log.txt`, notably the
  final smoke run beginning at line 23152 and policy lines 23210-23217.
- The same log contains a real earlier Task #16 SA-MP 0.3.DL rejection at lines
  22952-22954. It proves the deny decision, but predates the final Task #17
  presentation DLL and is not claimed as a final client-display test.

Final test Gate SHA-256:
`7EDA977407BC83D6EC79D99CC273A741C3469BAD400CEE18AEB48AF255CB2D3E`.

## Test limitations

No licensed GTA SA installation plus controlled SA-MP 0.3.7, SA-MP 0.3.DL,
open.mp and SKR Enhanced client matrix was available to this automated session.
Therefore no genuine final-build client screenshots were captured, and the
following remain manual acceptance tests:

1. Confirm the exact stock text rendered by each vanilla client for RPC 130.
2. Confirm the final-build CEF/launcher message on an SKR Enhanced rejection.
3. Re-run successful joins with SA-MP 0.3.7, SKR Enhanced and open.mp.
4. Re-run the SA-MP 0.3.DL denial against the final Task #17 Gate DLL.

Screenshots are deliberately not simulated or fabricated. Compatibility and
the deny decision take precedence over cosmetic text.

## Isolation and recovery

The stable `server-runtime/` was not deployed to. Its 1528 existing files match
the full backup by SHA-256; the backup has one additional recovery README.
The pre-change checkpoint is:

`C:\Users\Staark\Desktop\SKR-Backups\Task17-Checkpoint-20261021-130655`

All 13 entries in that checkpoint's `SHA256SUMS.txt` verify successfully.
