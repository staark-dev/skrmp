# Memory — SKR Platform

Actualizat: 18 octombrie 2026. Inspecție a checkout-ului local `C:\Users\Staark\Desktop\SKR Platform`, ramura `master`, HEAD `3981c7b7c8f43ad6a3a7645529e8565dfadb9e2f`.

## Cum se citește acest document

**Confirmat** înseamnă observat în surse sau în configurația locală; nu înseamnă testat în joc ori pregătit pentru producție. **Planificat** înseamnă TODO, brief sau master plan. **Recomandat** înseamnă pas propus în urma inspecției. Nu au fost rulate builduri, executabile, servere sau teste în această sesiune; nu sunt certificate rezultate istorice.

Sursele principale: `AGENTS.md`, `CMakeLists.txt`, `CMakePresets.json`, `dependencies.lock.json`, implementările din `apps/`, `client/`, `shared/`, `server/`, `services/`, `tools/`, `tests/unit/`. `STATUS.md`, `SKR_CLIENT_MASTER_PLAN.md` și `docs/skr-lobby-phase2-brief.md` sunt context istoric sau planificare și au discrepanțe față de cod.

La început existau modificări locale în `apps/launcher/game_launcher.cpp`, `apps/launcher/main.cpp`, `client/hud/hud.html`, `client/hud/icons/icons.svg`, `client/hud/phone.html`, `client/intro/urban_intro.cpp`, `client/lobby/lobby_manager.cpp`. Acest document descrie și conținutul necomis citit; nu atribuiți automat toate constatările commitului HEAD. Modificările existente trebuie păstrate.

## Scop și arhitectură

Proiect Windows pentru GTA San Andreas clasic x86: launcher propriu, DLL injectat în joc, intro D3D9/audio, interfață prin samp-cef și componentă server open.mp. Accesul exclusiv prin SKR și autentificarea completă prin backend sunt obiective, încă neîndeplinite în fluxul real.

Fluxul implementat de launcher este:

`SKRLauncher.exe → GTA suspendat → injectare samp.dll → injectare SKR.Client.dll → SkrRemoteInit → named pipe + inițializare client → ResumeThread`.

Separat există un circuit de laborator TCP pentru ticket/challenge/proof/heartbeat, construit din AuthService și biblioteca Gate. Acesta nu trebuie confundat cu integrarea efectivă în protocolul jocului. Componenta open.mp are propria implementare de stare/verificare, distinctă de biblioteca `SKRGate`.

## Structura și componentele reale

| Director / fișier | Responsabilitate confirmată și limite |
|---|---|
| `apps/launcher/` | Win32 `wWinMain`, căutare GTA/DLL, pornire/injecție; motor de update și inventar local. Loginul, descărcarea manifestului și integrarea update-ului sunt TODO în `main.cpp`. |
| `client/bootstrap/dllmain.cpp` | Exporturi `SkrInitialize`, `SkrRemoteInit`, `SkrGetLaunchParams`, `SkrShutdown`; DllMain returnează doar TRUE. Inițializează hook-uri, lobby și adaptorul AC mock în Development. |
| `shared/ipc/launch_params.h` | IPC inline Win32: pipe per PID `\\.\pipe\SKR_<pid>`, structură cu magic/versiune, adresă, port, nickname, ticket opțional de 32 octeți. |
| `client/render-d3d9/` | Hook-uri D3D9 și panou pentru chatul nativ; integrare intro și etapa post-intro. |
| `client/intro/` | Intro urban, timeline/transport și audio miniaudio; resurse încărcate din folderul SKR de lângă DLL. |
| `client/samp-hooks/` | Patch-uri de șiruri/versionare/chat, semnătură SKR în memorie, thread de instalare; RVAs declarate pentru SA-MP 0.3.7-R5. |
| `client/game-state/` | Hook de procesare în joc; citește poziție, orientare radar, zone și date vehicul, trimite stare pentru afișare prin RPC privat 240. |
| `client/cef-bridge/` | Wrapper peste API samp-cef și exporturi plugin. `cef_initialize` atașează API-ul, `cef_quit` închide bridge-ul; `cef_samp_mainloop` este un no-op rezervat, nu mecanismul curent de tick. |
| `client/lobby/` | Orchestrare CEF, patch meniu GTA, citire/scriere setări, pagină HTML/CSS/JS și fundal. Play și salvarea setărilor din evenimentul CEF sunt amânate. |
| `client/hud/` | Pagini HUD, telefon, hartă și iconuri. Existența paginilor nu dovedește integrarea lor completă cu gamemode-ul. |
| `client/transport/` | `AuthClient`: mașină de stări pentru ticket/challenge/proof/result cu provider crypto injectat; nu este singur un socket TCP sau un backend HTTP. |
| `client/skrac/` | Adaptor mock, adaptor DLL extern și policy heartbeat/timeout/degradare. Bootstrap-ul folosește efectiv mock-ul. |
| `client/compatibility/` | Inventariere module, SHA-256, versiuni, informații sistem, rezultate teste și serializare profil; eticheta Certified este calcul local, nu dovadă externă. |
| `shared/crypto/` | Interfață de semnare, mock XOR/CNG și provider Ed25519 real cu libsodium. Ambele coexistă. |
| `shared/protocol/` | Structuri auth/heartbeat/transcript și contractul RPC client-state. |
| `shared/updater/` | Manifest, validare intrări și policy release/version/channel. |
| `server/skr-gate/` | Bibliotecă statică de logică auth; `cef_server_bridge.h` documentează doar contractul gamemode/samp-cef. |
| `server/skr-gate-omp/` | Componentă SDK open.mp: lifecycle, filtru CLIENT_JOIN RPC 25, stare jucător, autorizare și receptor RPC 240. Build separat. |
| `services/auth/` | Stub de serviciu în memorie: emite/consumă/revocă tickete; nu este API REST implementat. |
| `services/ops/` | Bibliotecă health, key manager, audit și incident response; nu demonstrează un serviciu operațional persistent. |
| `tools/` | Server TCP local, smoke client, fake_game și generator de nume vehicule. |
| `tests/unit/` | Teste pentru shared/crypto, ops, intro timeline/transport. `tests/integration`, `security`, `compatibility` sunt schelete cu `.gitkeep`. |
| `server/gamemode-integration/`, `licenses/` | Schelete cu `.gitkeep`, nu integrare gamemode/licențiere completă. |
| `deps/`, `third_party/` | Dependințe locale excluse din Git. Prezența lor pe acest calculator nu garantează un clone reproductibil. |
| `build/`, `build-gate/`, `out/`, `server-build/`, `server-runtime/` | Artefacte/configurații locale; runtime-ul conține executabil server, componente și directoare Pawn. Nu au fost executate sau validate aici. |

Brief-ul Phase 2 menționează un launcher WPF din altă ramură/sesiune. Checkout-ul inspectat are `apps/launcher/` și `apps/cef-host/`, fără surse `apps/launcher-ui/`; WPF nu trebuie prezentat drept componentă disponibilă aici.

## Fluxuri importante

### Lansare și inițializare

`main.cpp` caută `gta_sa.exe` lângă launcher, în directorul părinte și într-o locație Steam hardcodată. Caută `samp.dll` și `SKR.Client.dll` lângă launcher sau joc. Valorile implicite sunt `127.0.0.1:7777` și `SKR_Player`; ticketul nu este obținut de la backend.

`LaunchGame` creează procesul suspendat și folosește `VirtualAllocEx`, `WriteProcessMemory`, `CreateRemoteThread(LoadLibraryW)` pentru injecție. Calculează adresa exportului `SkrRemoteInit` prin RVA, trimite `LaunchParams` dintr-un thread de pipe și cheamă inițializarea remote. În modul curent lobby, linia de comandă are nickname, fără `-c -h -p` pentru auto-connect. Butonul Play nu implementează încă conectarea, deci fluxul complet spre server rămâne incomplet.

`SkrInitialize` tolerează eșecul citirii IPC în modul de dezvoltare, instalează D3D9/samp hooks și lobby și pornește AC mock cu `dev-session`. `SkrShutdown` eliberează componentele explicit. Nu se execută inițializare complexă în DllMain.

### Intro, lobby și setări

Intro-ul folosește D3D9 și miniaudio; caută `SKR/background.png`, `foreground.png`, `logo.png`, `SKR/audio/intro.mp3` lângă DLL. Codul tratează tranziția în funcție de starea jocului și timeout/timeline; comportamentul vizual exact necesită verificare în GTA.

Lobby-ul este implicit dezactivat și se activează prin markerul `SKR/lobby.on`. Tick-ul CEF este condus din hook-ul D3D9 după intro. Meniul GTA este patch-uit și browserul lobby creat când CEF este pregătit. Pagina se încarcă local din `SKR/lobby/lobby.html`.

Evenimente implementate: `skr:lobby:settings:get`, `settings:save`, `play`, `quit`. Get scrie un sidecar `settings.json` și emite ready. Save emite save-pending, deoarece lipsesc cititoarele CefListValue în wrapper; Play doar loghează lipsa implementării Phase 2. Quit include `ExitProcess(0)`. Setările persistente sunt gestionate în `%APPDATA%\SKR\launcher.ini`, cu oglindirea opțiunii fullscreen în `sa-mp.cfg`.

Logurile sunt sub `SKR/`: `intro.log`, `lobby.log`, `game_state.log`. Nu trebuie incluse tickete sau secrete în ele.

### Autentificare și autorizare

Protocolul de laborator TCP are framing `[uint8 msgType][uint16 payloadLen][payload]`; tipurile includ Challenge 1, Proof 2, Result 3, AcHeartbeat 4, Ack 5, IssueTicket 10, TicketResponse 11. TranscriptBuilder leagă challenge-ul de joinId. AuthService ține ticketele în memorie și consumul este protejat cu mutex; `HashJoinId` este încă XOR-fold, explicit necriptografic.

Serverul de test, smoke client și fake_game folosesc MockSigningProvider. Ed25519Provider este implementat și are teste în `test_main.cpp`, dar existența sa nu înseamnă migrarea fluxurilor reale la crypto de producție.

**Componenta open.mp nu implementează încă garanția de autentificare din plan:** filtrul RPC25 verifică prefixul de versiune `SKR`; `onPlayerConnect` cheamă `AuthorizePlayer(id, 0)` cu TODO de verificare a tokenului. Challenge/VerifyProof există ca metode, dar calea de conectare citită nu condiționează autorizarea de un proof valid. Nici șirul de versiune, nici semnătura fixă în memorie nu sunt atestare sigură.

### Stare joc și HUD

Clientul citește memorie GTA/SA-MP și trimite RPC 240 conform `shared/protocol/client_state.h`. Serverul îl acceptă numai pentru jucători marcați autorizați, limitează frecvența la cel mult o acceptare la 50 ms, verifică versiunea și intervale numerice, curăță numele și scrie variabile `skr_*`, cu `skr_state_seq` actualizat ultimul. Datele sunt pentru afișare, controlate de client; nu trebuie folosite drept adevăr autoritar pentru economie/gameplay. Integrarea gamemode care consumă aceste variabile nu este implementată în directorul tracked dedicat.

### Update

Motorul poate compara manifeste, verifica SHA-256 în staging, crea backup, muta/șterge fișiere și încerca rollback. Nu există în launcher legătura completă cu descărcare HTTP, verificarea semnăturii și actualizarea inventarului. ReleaseGate evaluează versiuni, canale și maintenance; nu autentifică manifestul. Nu numiți acest circuit updater de producție sau descărcare delta funcțională doar pe baza motorului local.

## Dependințe și reproducibilitate

- Windows, MSVC/Visual Studio 2022 cu toolset v143, Windows SDK, CMake minimum 3.24; codul rădăcină cere C++20 și pointeri de 32 biți. Configurarea x64 sau non-Windows este respinsă.
- `deps/libsodium/include` și bibliotecile statice `Win32/{Debug,Release}/v143/static/libsodium.lib` sunt importate direct de CMake; ambele fișiere există local. Versiunea/proveniența libsodium nu este fixată de lockfile-ul inspectat.
- miniaudio este inclus din `deps/miniaudio-0.11.23`; implementarea sa are avertismente dezactivate separat în CMake.
- Biblioteci Windows: D3D9, BCrypt, version, WIC/windowscodecs, ole32, shell32, user32; instrumentele TCP folosesc ws2_32.
- Componenta server cere `deps/omp-sdk` și `deps/omp-network`, targeturi `OMP-SDK` și `OMP-Network`. CMake declară adăugarea network condiționată, dar îl linkează necondiționat: în practică este necesar. ABI/arhitectura trebuie potrivite serverului; nu se deduc din cerința x86 a jocului.
- `dependencies.lock.json` fixează samp-cef la `f14fe4fa7f0d036edf445ba02b4bddd4cccac377` și declară CEF windows32 `150.0.11+gb887805+chromium-150.0.7871.115`, cu hash de arhivă. Nu a fost verificată instalarea sau compatibilitatea lor efectivă.
- Lockfile-ul are încă UNKNOWN pentru open.mp server/SDK, plugin-sdk și hash samp-cef; are și note R2 depășite. Nu include toate dependințele folosite acum. Blocaj pentru un release reproductibil.
- Mențiunile GPL-3.0 samp-cef, MPL-2.0 open.mp, MIT SDK și BSD/notificări CEF provin din documentele locale; nu sunt audit juridic. `licenses/` nu are încă pachetul de notificări. `assets/minimap/` este explicit ignorat ca material derivat din GTA și nu trebuie redistribuit automat.

## Build și rulare

Comenzi deduse din configurația curentă, **nerulate în această inspecție**. Se execută din rădăcina repository-ului, după pregătirea dependințelor locale:

```powershell
cmake --preset x86-debug
cmake --build --preset debug
# sau
cmake --preset x86-release
cmake --build --preset release
```

Preseturile folosesc generatorul Visual Studio 17 2022, Win32; configurația efectivă este aleasă de build preset (`Debug`/`Release`), nu doar de CMAKE_BUILD_TYPE. Artefactele sunt în `out/x86-debug/Debug/` sau `out/x86-release/Release/`.

Cele 12 targeturi declarate la rădăcină sunt: `SKRShared`, `SKRLauncher`, `SKRClient`, `SKRIntroTests`, `SKRGate`, `SKRAuth`, `SKROps`, `SKRTests`, `SKROpsTests`, `SKRTestServer`, `SKRSmokeClient`, `SKRFakeGame`. `SKRClient` produce `SKR.Client.dll`; `SKRGate` din acest build este bibliotecă statică, nu componenta DLL server.

CMake copiază automat numai fișierele lobby HTML/CSS/JS și fundalul său în `SKR/lobby/` lângă DLL. Nu asigură în aceeași regulă deployment complet pentru samp.dll, samp-cef/CEF, intro audio/imagini sau paginile HUD.

Teste disponibile după build:

```powershell
.\out\x86-debug\Debug\SKRTests.exe
.\out\x86-debug\Debug\SKROpsTests.exe
.\out\x86-debug\Debug\SKRIntroTests.exe
# terminal separat, pentru circuitul local TCP:
.\out\x86-debug\Debug\SKRTestServer.exe
# alt terminal:
.\out\x86-debug\Debug\SKRSmokeClient.exe
.\out\x86-debug\Debug\SKRFakeGame.exe 7710
```

Portul implicit al serverului de test este 7710. Fake_game include încărcare DLL, IPC și circuit auth/heartbeat; nu substituie GTA/D3D9 real. Nu există în CMake-ul rădăcină înregistrare `add_test` pentru aceste executabile. Nu presupuneți că numerele istorice de teste din STATUS mai sunt actuale.

Build server separat, de adaptat arhitecturii serverului instalat (exemplul Win32 este configurația documentată local, nu certificare ABI):

```powershell
cmake -S server/skr-gate-omp -B build-gate -G "Visual Studio 17 2022" -A Win32 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-gate --config Debug
# rezultat așteptat din configurație: build-gate/Debug/SKRGate.dll
```

Componenta folosește C++17 și include crypto mock; nu linkează automat biblioteca auth de la rădăcină. Pentru deployment trebuie verificat mecanismul de încărcare a componentelor din serverul selectat; indicația veche din STATUS despre `plugins/` nu trebuie aplicată orbește unei implementări `IComponent`.

Pentru rularea launcherului sunt necesare GTA clasic x86, samp.dll compatibil și SKR.Client.dll. Pentru UI mai sunt necesare pluginul samp-cef, runtime-ul CEF și resursele locale. Acestea nu sunt descărcate automat de launcherul actual. Nu a fost pornit jocul sau serverul pentru confirmarea lanțului complet.

## Convenții și reguli de lucru

Urmați `AGENTS.md`: editări incrementale; separați codul existent de pseudocod/API propus; nu inventați offseturi, callbackuri, RPC-uri sau teste trecute. Verificați surse upstream la commitul fixat când modificați integrarea. Namespace-uri `skr::*`, headere `.h` în client/shared și `.hpp` în componenta open.mp; targeturi CMake `SKR*`. MSVC folosește `/W4 /permissive- /EHsc`, `/DYNAMICBASE /NXCOMPAT`, cu excepții locale pentru terți.

Păstrați inițializarea în afara DllMain, arhitectura x86 a jocului și ABI distinct pentru server. Fiecare acțiune protejată trebuie autorizată server-side. Mock SKRAC rămâne exclusiv dev; nu dezactivați TLS, sandbox sau semnături ca soluție la teste. Nu introduceți secrete în surse/loguri/CLI și nu publicați ori semnați cu chei reale fără autorizarea proprietarului. Nu declarați compatibilitate R2 fără matrice completă pe hashuri cunoscute.

## Stare actuală, riscuri și discrepanțe

1. **Blocant producție:** autorizarea open.mp pe prefix de versiune și accountId 0 poate fi imitată; verificarea tokenului nu este conectată. Mesajul de log „ticket-based auth” nu schimbă implementarea.
2. **Bug observat în parserul RPC25:** bufferul `verStr[65]` este terminat la indexul `verLen` chiar când lungimea primită depășește 64. Necesită validare înaintea indexării. Unele erori de parsare întorc allow; comportamentul de respingere trebuie revizuit.
3. **Compatibilitate fragilă:** profilul se numește openmp-native, dar launcherul injectează samp.dll, iar hook-urile sunt pentru R5 și adrese GTA fixe. Aceste denumiri nu demonstrează client open.mp nativ sau compatibilitate R2/generală. Nu a fost verificată o matrice completă de hashuri.
4. **IPC/lansare:** `ConnectNamedPipe` este sincron; argumentul timeout nu limitează automat toate așteptările. `pipeThread.join()` poate bloca. Launcherul citește rezultatul inițializării remote, dar nu îl folosește ca motiv de eșec înainte de resume; verificarea stărilor de wait merită revizuită.
5. **Crypto/autentificare:** Ed25519 există, dar mock-urile sunt încă active în test tools și gate open.mp, iar ticket hash este XOR. Lipsesc API HTTPS de login/ticket, persistență și fluxul complet de validare a sesiunii.
6. **Lobby incomplet:** dezactivat implicit; Play și salvarea din CEF sunt stub-uri. Brief-ul Phase 2 este intenție, nu dovadă de implementare.
7. **Update/release:** lipsesc orchestrarea de download și verificarea semnăturii; rollback-ul reface fișierele salvate, fără a demonstra eliminarea tuturor fișierelor noi după un eșec. Necesită teste de tranzacție și validarea deployment-ului înainte de utilizare reală.
8. **Documente istorice:** STATUS declară working tree curat, 11 targeturi și IPC/Ed25519 lipsă; checkout-ul are modificări, 12 targeturi și ambele implementări. Master plan amestecă ASI/R2 cu direcția ulterior schimbată. Aceste fișiere nu au fost modificate.
9. **Validare limitată:** existența binarelor, logurilor și runtime-ului local nu dovedește că sursele curente compilează sau trec testele. Nu s-au verificat în această sesiune joc real, reset D3D9, lifecycle CEF, securitate, semnare sau distribuție.

## Pași următori recomandați

1. Păstrați și clarificați modificările locale; stabiliți snapshot-ul de referință. Rulați buildurile și testele existente pe acel snapshot, consemnând configurația și rezultatele reale.
2. Corectați validarea lungimilor RPC25 și înlocuiți autorizarea pe branding cu validarea ticket/proof legată de conexiune. Conectați auth real și acoperiți server-side toate acțiunile protejate.
3. Fixați versiunile/hashurile serverului, SDK/network, samp.dll/GTA și dependințelor; documentați profilul R5 efectiv și selectați explicit direcția de client. Verificați ABI server separat.
4. Finalizați fluxul launcher → backend → IPC → AuthClient → gate, migrați utilizările mock la providerul potrivit și înlocuiți hash-ul ticketului cu SHA-256 real. Integrați SKRAC numai după contract/SDK real.
5. Implementați Phase 2 lobby: citire argumente CEF, salvare confirmată, selectare/conectare server, tranziție gameplay/pause. Brief-ul este punct de pornire, cu revalidarea presupunerilor sale.
6. Faceți deployment reproductibil pentru DLL, CEF, samp-cef, intro și HUD; verificați resursele/licențele. Completați lockfile-ul și notificările înainte de release.
7. Completați orchestrarea update-ului semnat și testele de eșec/rollback; validați în GTA/server real și adăugați matrice compatibilitate, integrare și securitate. Actualizați apoi documentele istorice într-o sarcină separată.
