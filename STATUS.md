# SKR Client — Status Report

**Data:** 17 octombrie 2026  
**Working tree:** curat, 0 modificări nepuse in commit

---

## Ce este făcut

### Milestone 0+1 — Schelet proiect (`6f8e1ac`)
- Structura de directoare completă (client/, server/, shared/, apps/, services/, tools/, tests/)
- CMakeLists.txt rădăcină cu 11 targeturi, toate compilează pe Win32 x86
- Launcher Win32 minimal (`apps/launcher/`)
- `.gitignore`, `AGENTS.md`, `SKR_CLIENT_MASTER_PLAN.md`, `LICENSE`

### Milestone 2 — DLL Client + D3D9 Intro (`65ebb62`, `c350f6e`)
- Modulul client este `.dll` (nu `.asi`) — injectat de launcher via `CreateRemoteThread` + `LoadLibraryW`
- `dllmain.cpp` cu exporturi `SkrInitialize()` / `SkrShutdown()` — DllMain este gol (regula #6)
- Renderer D3D9 intro cu fade-in/fade-out și skip

### Milestone 3 — CEF Bridge + samp-cef (`c41836a`)
- Bridge pentru samp-cef la commit fixat `f14fe4fa7f0d036edf445ba02b4bddd4cccac377`
- Plugin exports (`cef_plugin_exports.cpp`) pentru integrarea cu CEF din joc
- GPL-3.0 — samp-cef afectează redistribuirea

### Milestone 4 — Auth + Ticketing + SKR Gate (`2256961`)
- Protocol binar: `[uint8_t msgType][uint16_t payloadLen][payload]`
- Mesaje: Challenge(1), Proof(2), Result(3), AcHeartbeat(4), AcHeartbeatAck(5), IssueTicket(10), TicketResponse(11)
- `TranscriptBuilder` — challenge + joinId legat de sesiune
- `AuthService` (services/auth/) — emite tickete, verifică proof
- `SKR Gate` (server/skr-gate/) — library pentru verificare server-side
- Mock crypto cu XOR (Ed25519 planificat pentru producție)

### Milestone 5 — SKRAC Adapter + AC Policy (`4a89c39`)
- C ABI extern (`__cdecl`) pentru DLL-ul SKRAC real
- Structuri versionate cu `structSize`
- `AcPolicyEnforcer` — heartbeat scheduling, timeout/retry, degraded grace period
- Mock-ul SKRAC este respins în producție (regula #11)

### Milestone 6 — Updater Engine (`e04a53b`)
- `UpdateEngine` — staging dir, plan update, verify, apply
- `ReleaseManifest` + `FileInventory` — hash fișiere, inventar local
- Descărcare delta (doar fișierele schimbate)

### Milestone 7 — Compat Profile + Release Gate + Tests (`fa4b233`)
- `compat_profile.cpp` — detectează versiunea GTA SA via `GetFileVersionInfo`
- Target schimbat de la `samp-037-r2` la `openmp-native`
- `ReleaseGate` — validare pre-lansare
- Framework unit test (`tests/unit/test_main.cpp`)

### Milestone 8 — Operații (`fee0c8a`)
- `HealthMonitor` — stare componente, degraded/critical
- `KeyManager` — rotație chei, revocare, validare
- `AuditLog` — buffer circular, severitate, filtrare
- `IncidentResponder` — auto-check rată eșec auth, praguri alarmă
- 17 unit teste — toate trec

### R2 Removal + open.mp Native (`aa1b171`)
- `r2_adapter.cpp` eliminat din build (cod mort pentru open.mp native)
- Master plan actualizat: secțiunea 4 → "conectare nativă open.mp"
- `compat_profile.h` → `openmp-native`

### open.mp Gate Plugin + Game Launcher (`b0346db`)
- **SKR Gate open.mp plugin** (`server/skr-gate-omp/`)
  - Implementează `IComponent` + `PlayerConnectEventHandler`
  - UID: `0x534B5247` ("SKRG")
  - CMakeLists.txt separat, linkează OMP-SDK
  - Metode: IssueChallenge, VerifyProof, AuthorizePlayer, RejectPlayer, ProcessAcHeartbeat
- **Game Launcher** (`apps/launcher/game_launcher.cpp`)
  - `CreateProcessW` cu `CREATE_SUSPENDED`
  - `InjectDll()`: `VirtualAllocEx` + `WriteProcessMemory` + `CreateRemoteThread` cu `LoadLibraryW`
  - `ResumeThread` după injecție
  - `LaunchConfig` struct: gtaSaPath, clientDllPath, serverAddress, serverPort, playerName
- **deps/omp-sdk/** descărcat local (gitignored)

### Test Tools (`36b6aa8`, `2595205`)
- **SKRTestServer** (`tools/test_server/`) — server TCP Winsock pe `127.0.0.1:7710`, integrează AuthService + Gate
- **SKRSmokeClient** (`tools/smoke_client/`) — client de test, rulează flow-ul complet de auth + 3 AC heartbeats
- **SKRFakeGame** (`tools/fake_game/`) — smoke test fără GTA SA:
  - Phase 1: LoadLibrary → exports → init/shutdown → FreeLibrary
  - Phase 2: Auth handshake complet contra SKRTestServer
  - Phase 3: 5 AC heartbeats cu verificare secvență/acțiune
  - **16/16 pași trec end-to-end**

---

## Ce NU este făcut (necesar pentru test real)

### Prioritate 1 — Comunicare launcher ↔ DLL client
- [ ] **Named pipe IPC** — launcherul trebuie să transmită clientului DLL:
  - Adresa serverului + port
  - Join ticket (joinId) obținut de la backend
  - Numele jucătorului
- [ ] DLL-ul client trebuie să asculte pe pipe după ce este injectat, înainte de `ResumeThread`

### Prioritate 2 — Conectare reală la server open.mp
- [ ] **Integrare protocol RakNet** — clientul DLL trebuie să se conecteze la portul UDP open.mp (7777) prin RakNet
- [ ] **Hook-uri open.mp** — interceptare connect pentru a injecta ticket-ul SKR în handshake
- [ ] **SKR Gate plugin testat pe server open.mp real** — pluginul compilează, dar nu a fost încărcat niciodată într-un server open.mp real

### Prioritate 3 — Backend REST API
- [ ] **Endpoint `POST /v1/auth/login`** — autentificare user (email/parolă sau OAuth)
- [ ] **Endpoint `POST /v1/auth/ticket`** — emitere joinId legat de cont + server
- [ ] **Endpoint `GET /v1/releases/stable`** — manifest actualizări
- [ ] **TLS obligatoriu** — toate endpoint-urile HTTPS (regula #12)

### Prioritate 4 — Crypto producție
- [ ] **Ed25519 real** — înlocuire mock XOR din `shared/crypto/crypto.cpp`
- [ ] Librărie: libsodium sau ed25519-donna (x86 compatibil)
- [ ] Key management real — generare/rotație/revocare cu chei persistate

### Prioritate 5 — UI
- [ ] **CEF login screen** — formular autentificare în overlay
- [ ] **CEF server browser** — lista servere sau conectare directă
- [ ] **samp-cef integrare efectivă** — pluginul samp-cef necesită GTA SA real pentru a funcționa

### Prioritate 6 — Securitate producție
- [ ] **Semnare cod** — launcher-ul și DLL-ul trebuie semnate (regula #16 — necesită autorizare proprietar)
- [ ] **Verificare semnătură manifest** — updater-ul descarcă, dar nu verifică semnătura
- [ ] **SKRAC real** — adaptorul extern este gata, dar DLL-ul SKRAC real nu există încă
- [ ] **Sandbox client** — izolare procese CEF

---

## Cum se testează acum (dev flow local)

### Build complet (toate cele 11 targeturi)
```bash
cmake -G "Visual Studio 17 2022" -A Win32 -B build -S .
cmake --build build --config Debug
```

### Rulare smoke test fără GTA SA
```bash
# Terminal 1: pornește serverul de test
build\Debug\SKRTestServer.exe

# Terminal 2: rulează fake_game
build\Debug\SKRFakeGame.exe 7710
# Așteptat: 16/16 steps passed
```

### Rulare smoke client standalone
```bash
# Terminal 1: serverul de test (dacă nu rulează deja)
build\Debug\SKRTestServer.exe

# Terminal 2: smoke client
build\Debug\SKRSmokeClient.exe
# Așteptat: 12 steps passed
```

### Rulare unit tests
```bash
build\Debug\SKRTests.exe
build\Debug\SKROpsTests.exe
```

### Build gate plugin open.mp (separat)
```bash
cmake -G "Visual Studio 17 2022" -A Win32 -B build-gate -S server/skr-gate-omp -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-gate --config Debug
# Produce: build-gate\Debug\SKRGate.dll
```

---

## Ce trebuie pentru un test REAL cu GTA SA

1. **GTA San Andreas** instalat (versiunea clasică, x86)
2. **Server open.mp** descărcat și configurat cu `SKRGate.dll` în `plugins/`
3. **Named pipe IPC** implementat (launcher → DLL)
4. **RakNet hook** în DLL pentru conectare la serverul open.mp
5. **Backend** minim (poate fi mock-ul local extins la HTTP) pentru login + ticket
6. **samp-cef** compilat și distribuit alături de client (GPL-3.0)

### Pași concreți:
1. Implementează named pipe — launcherul trimite config, DLL-ul citește
2. Hook connect din DLL pe protocolul RakNet al open.mp
3. Pornește server open.mp local cu SKRGate.dll
4. Lansează GTA SA prin SKRLauncher.exe
5. Verifică: DLL injectat → pipe citit → connect la server → challenge/proof → authorized → heartbeats

---

## Structura repository

```
SKR Platform/
├── apps/launcher/          # Launcher Win32 (main, updater, inventory, game_launcher)
├── client/
│   ├── bootstrap/          # dllmain.cpp (SkrInitialize/SkrShutdown)
│   ├── cef-bridge/         # samp-cef bridge + plugin exports
│   ├── compatibility/      # compat_profile (openmp-native)
│   ├── render-d3d9/        # D3D9 intro renderer + hook
│   ├── skrac/              # AC mock adapter + external adapter + policy
│   └── transport/          # auth_client (TCP)
├── server/
│   ├── skr-gate/           # Gate library (verificare auth)
│   └── skr-gate-omp/       # open.mp plugin (IComponent)
├── services/
│   ├── auth/               # AuthService (ticketing)
│   └── ops/                # Health, KeyManager, AuditLog, IncidentResponder
├── shared/
│   ├── crypto/             # Mock crypto (XOR, Ed25519 planned)
│   ├── protocol/           # messages.h (framing, types)
│   └── updater/            # manifest, release_gate
├── tools/
│   ├── test_server/        # Winsock TCP server (127.0.0.1:7710)
│   ├── smoke_client/       # Auth + AC smoke test client
│   └── fake_game/          # DLL load + auth + AC fără GTA
├── tests/unit/             # test_main.cpp, test_ops.cpp (17 tests)
├── deps/                   # omp-sdk (gitignored)
├── CMakeLists.txt          # 11 targets, Win32 x86
├── AGENTS.md               # Reguli agent AI
├── SKR_CLIENT_MASTER_PLAN.md
└── STATUS.md               # ← acest fișier
```

---

*Generat: 17 octombrie 2026*
