# M5.8 transport integration — audit and implementation plan

The existing native phone uses the paced queue in `phone_net.cpp`, RPC 240 / Native API step 9 requests, and step 7 responses on RPC 241. All SMS, contacts, history and profile payload codecs already exist. SQLite allocation, profile state, read watermarks and request receipts already exist. GPS remains entirely local. Phone/dialer still has no working calls. The namespace registry has a single Gateway key per direction; PHONE (5) was explicitly excluded from known namespaces. CORE and VEHICLES already dispatch through step 10.

The audit also found synchronous SQLite in packet callbacks and synchronous diagnostic file writes in phone client callbacks. Those must not carry over to PHONE v1. The official SDK database result pool is not a worker-thread API; moving its objects off-thread would be unsafe. The implementation therefore reuses the exact SQL/codecs in a project-owned store with one SQLite connection owned by one worker. The existing SQLite 3.36.0 x86 package used by the local open.mp build is reused; no new networking or renderer is added.

File plan:

- `shared/protocol/skrx_phone.h`: bounded/versioned namespace frame, typed payload validation, shared transport-selection rule. No nested API envelope.
- `native_api.h`, client/server API negotiation: additional explicit PHONE-v1 capability; retain CapPhone and CapPhoneSetup semantics.
- `skrx_gateway.h`, client/server Gateway dispatch: enable namespace 5 inside the existing step-10 handlers. No additional `(rpc,step,direction)` entries.
- `phone_net.{h,cpp}`: select one transport when Welcome is accepted, preserve paced queue/request IDs/typed response handlers, reject opposite-path traffic, remove phone callback file I/O.
- `phone_store.cpp`, `phone_worker.{h,cpp}`: extract existing SQL business logic, worker-owned SQLite, bounded queues, cancellation leases, exact-session responses and persistent-owner SMS pushes.
- `phone_backend.cpp`: main-thread SDK adapter and authentication/capability checks only; no SQL or disk I/O.
- `skr_gate_component.{hpp,cpp}`: own worker lifecycle, cancel on disconnect, drain bounded responses on tick. VEHICLES hooks remain unchanged.
- Server CMake: link matching-architecture existing SQLite and add independent integration tests/build outputs.
- Tests: real client codec/queue + real worker/store round trips on disposable databases; legacy fallback, replay/stale/malformed/capability cases, migration persistence and CORE/VEHICLES regressions.

PHONE v1 keeps operation numbers aligned with existing domain operations. Responses fit the 400-byte Gateway payload by using two history messages per Gateway page (legacy remains three). Calls and a separate notification-center opcode are not added as fake functionality. Existing SmsReceived carries real notifications. Profiles/activation are reused, not reimplemented or renumbered.

No GTA, deployment, signing, live database edits or protected build overwrite is part of this work. The old synchronous development-only SMS command cannot safely return an immediate DB message ID after asynchronous extraction; it will explicitly return failure rather than claim success. Native Messages remains the functional send path.
