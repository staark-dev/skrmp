# SKR Client — Master Plan

**Versiune:** 1.0 · **Surse publice verificate:** 17 octombrie 2026  
**Stare:** arhitectură și contracte propuse; nu constituie implementare sau certificare de compatibilitate.

## Cuprins

1. Obiectiv și decizii
2. Surse oficiale și constatări
3. Arhitectură și repo
4. Compatibilitate 0.3.7-R2
5. Launcher Windows
6. DLL/ASI: CMake și C++ minimale
7. Intro D3D9
8. CEF și samp-cef
9. API și handshake
10. Integrare SKRAC
11. Securitate și limite anti-spoof
12. Build și dependențe
13. Testare
14. Release și operare
15. Roadmap
16. Instrucțiuni agent AI
17. Checklist și întrebări deschise

## 1. Obiectiv și decizii

SKR Client oferă un launcher Windows propriu, modul C++ pentru GTA San Andreas, intro Direct3D 9, UI HTML/CSS/JS prin CEF, autentificare SKR obligatorie pentru un server open.mp și integrare cu SKRAC.

**Acces exclusiv prin SKR înseamnă autorizare obligatorie validată de server. Nu înseamnă dovadă infailibilă că utilizatorul execută launcherul original.** Orice client poate trimite pachete la portul public; serverul respinge conexiunile neautorizate sau le ține temporar fără privilegii până la verificare.

Decizii propuse:

- Modulul din GTA SA clasic este x86; launcherul, helperii și serverul pot avea arhitecturi diferite, cu IPC explicit.
- Launcher separat, inițial Win32/C++; CEF în launcher este opțional și separat de CEF din joc.
- Server open.mp cu extensie SKR Gate, preferabil prin SDK public.
- Identitate și tickete gestionate de backend, nu de DLL sau JavaScript.
- HTTPS pentru control; transportul jocului rămâne specific versiunii client/server selectate.
- Ticket scurt, unic, legat de server, cont și cheia efemeră de sesiune; challenge legat de conexiunea jocului.
- SKRAC se integrează prin adaptor; până la primirea SDK-ului real există doar mock pentru dezvoltare.
- 0.3.7-R2 este țintă de validat; documentul nu declară compatibilitate deja demonstrată.

Exemplele sunt fie schelete compilabile independente, fie pseudocod și contracte propuse marcate explicit. Nu distribuim GTA, surse SA-MP nepublice, executabile comerciale sau secrete de producție.

## 2. Surse oficiale și constatări

Verificarea a fost documentară, prin pagini publice ale proiectelor și producătorilor. Nu au fost compilate sursele upstream și nu a fost rulat GTA. Ramurile implicite și paginile indexate se schimbă; implementarea trebuie să fixeze commituri complete și hashuri.

| Sursă | Rol și observație |
|---|---|
| [open.mp server](https://github.com/openmultiplayer/open.mp) | Se prezintă ca fiind compatibil cu SA-MP; README separă SDK-ul de headerele interne de rețea, instabile între versiuni. |
| [open.mp SDK](https://github.com/openmultiplayer/open.mp-sdk) | SDK server; README arată submodule recursive și targetul CMake `OMP-SDK`. Nu este SDK pentru clientul GTA. |
| [open.mp launcher](https://github.com/openmultiplayer/launcher), [releases](https://github.com/openmultiplayer/launcher/releases) | Referință de lansare și integrare client. Notele de release nu certifică SKR sau samp-cef pe R2. |
| [RakNet open.mp](https://github.com/openmultiplayer/RakNet) | Fork bazat pe RakNet 2.52, modificat pentru interoperabilitatea SA-MP. |
| [RakNet upstream arhivat](https://github.com/facebookarchive/RakNet) | Referință istorică. Nu presupuneți că o versiune RakNet 4.x înlocuiește protocolul folosit de open.mp. |
| [samp-cef](https://github.com/zottce/samp-cef), [README](https://github.com/zottce/samp-cef/blob/master/readme.md) | Proiect comunitar client/server, cu Rust și CEF; nu este componentă oficială open.mp. |
| [CEF surse](https://github.com/chromiumembedded/cef), [documentație](https://chromiumembedded.github.io/cef/) | Motor browser încorporabil; API și toolchain dependente de versiune. |
| [CEF tutorial](https://chromiumembedded.github.io/cef/tutorial), [General Usage](https://chromiumembedded.github.io/cef/general_usage.html) | Modele multiproces, wrapper și layoutul resurselor. |
| [CEF binary builds](https://cef-builds.spotifycdn.com/index.html) | Distribuții găzduite de Spotify și indicate de proiectul CEF; verificați arhitecturile efectiv disponibile. |
| [plugin-sdk GTA](https://github.com/DK22Pac/plugin-sdk) | SDK comunitar pentru GTA/ASI/CLEO; nu este SDK Rockstar. |
| [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) | Loader comunitar ASI; verificați x86, ordinea de încărcare și conflictele proxy DLL. |
| [Microsoft DLL best practices](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices) | Inițializarea complexă se amână în afara loader lock. |
| [Microsoft D3D9 lost devices](https://learn.microsoft.com/en-us/windows/win32/direct3d9/lost-devices) | Tratarea pierderii și resetării dispozitivului. |
| [Visual Studio / Build Tools](https://visualstudio.microsoft.com/downloads/), [Windows SDK](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/) | Compilator, biblioteci Windows, instrumente de diagnostic și semnare. |
| [CMake](https://cmake.org/download/) | Generator pentru buildurile C++ SKR. |

### Constatări care influențează proiectul

**samp-cef:** copia raw/indexată consultată descrie dependențe istorice, inclusiv CEF 89 și un toolchain Rust datat. Pagina HTML și copia indexată nu sunt o bază suficientă pentru alegerea unei configurații actuale unice. Inspectați commitul ales, `Cargo.lock` și scripturile de build. Nu transformați o versiune istorică din README în recomandare de producție și nu expuneți un browser vechi internetului deschis.

**open.mp:** compatibilitatea serverului cu protocolul SA-MP nu certifică ABI-ul unui mod client sau hookurile sale. Headerele interne de netcode nu sunt promise stabile; folosiți SDK-ul unde este posibil.

**SKRAC:** conversația nu oferă SDK, repository sau documentație oficială. Toate interfețele SKRAC din acest plan sunt propuneri, fără funcții atribuite unui produs neverificat.

**Licențe:** paginile consultate indică MPL-2.0 pentru open.mp, MIT pentru SDK și GPL-3.0 pentru samp-cef; CEF documentează BSD și notificări pentru dependențe. Analizați exact codul copiat, modificat, legat și redistribuit. Separarea în procese nu elimină automat obligațiile. Păstrați licențe, notificări, proveniență și obligațiile de furnizare a sursei aplicabile pachetului concret.

## 3. Arhitectură și repo

```text
SKR Launcher ── HTTPS ──► Auth/API ──► conturi, tickete, revocări
    │                       ▲                      ▲
    │ IPC local             │ HTTPS                │ identitate server
    ▼                       │                      │
GTA SA x86 + SKR.Client.asi ─┘               SKR Gate component
    ├─ intro D3D9                                  │
    ├─ UI / adaptor CEF                            ▼
    └─ adaptor SKRAC ── transport joc ──► open.mp + gamemode
```

| Modul | Responsabilitate | Limită de încredere |
|---|---|---|
| Launcher | instalare, update, login, pornire | nu acordă singur acces definitiv |
| DLL/ASI | lifecycle, randare, IPC, dovadă sesiune | rulează pe un sistem controlat de utilizator |
| CEF | UI și evenimente limitate | nu păstrează tokenuri și nu decide autorizarea |
| Auth/API | identitate, ticket, expirare, revocare | nu vede singur toate abuzurile de gameplay |
| SKR Gate | asociere cu conexiunea și blocare acces | nu crede declarațiile arbitrare ale clientului |
| SKRAC | semnale/verdict conform contractului real | nu înlocuiește validarea autoritară server |
| Gamemode | spawn, economie, comenzi | verifică autorizarea pe toate căile protejate |

Mașina de stări server:

```text
CONNECTED → QUARANTINED → CHALLENGED → VERIFYING → AUTHORIZED
               eșec / expirare / revocare → REJECTED → DISCONNECTED
```

Înainte de autorizare sunt blocate spawn, economie, comenzi cu efect, damage și actualizări cu efect asupra altora. Blocarea mișcării singură nu reprezintă o graniță completă de securitate. Dacă SDK-ul nu permite blocarea tuturor căilor relevante, este necesar un adaptor limitat la versiune sau un patch explicit în server, justificat și întreținut separat.

Primul spike verifică punctele reale de extensie. Nu se inventează callbackuri, entry pointuri sau ID-uri RPC.

```text
skr-client/
  README.md
  AGENTS.md
  CMakeLists.txt
  CMakePresets.json
  dependencies.lock.json
  apps/
    launcher/
    cef-host/                 # alternativă helper extern
  client/
    bootstrap/
    compatibility/
    render-d3d9/
    cef-bridge/
    transport/
    skrac/
  server/
    skr-gate/
    gamemode-integration/
  services/auth/
  shared/
    protocol/
    crypto/
  web/ui/
  assets/intro/
  tests/
    unit/
    integration/
    compatibility/
    security/
  tools/
    package/
    manifest/
    smoke/
  docs/
    architecture/
    protocol/
    compatibility/
    operations/
  third_party/
  licenses/
  .github/workflows/
```

Buildurile client și server sunt independente. Lockfile-ul conține URL, commit complet, versiune, hash arhivă, arhitectură, toolchain și licență pentru fiecare dependență. Valorile necunoscute rămân blocante pentru release, fără hashuri inventate.

## 4. Compatibilitate 0.3.7-R2

Ținta trebuie definită ca **client SA-MP 0.3.7-R2**, separat de orice server numit „R2”. Numele fișierului și textul de versiune pot fi imitate. Înregistrați SHA-256, dimensiune, arhitectură și proveniență pentru `gta_sa.exe`, `samp.dll`, modulele open.mp dacă există, loader și componentele SKR.

Compatibilitatea protocolului nu implică aceleași adrese, structuri de memorie, convenții de apel sau lanțuri grafice. Nu reutilizați offseturi R1 pe R2 fără verificare.

| Configurație | Statut în plan |
|---|---|
| GTA SA clasic x86 + client exact 0.3.7-R2 | țintă MVP, necertificată |
| Client 0.3.7-R1 | test și adaptor separat; în afara MVP |
| Alte revizii R / DL | nesuportate implicit |
| Lanț de lansare open.mp oficial selectat | candidat separat; validare de module și ordine |
| Definitive Edition / executabil necunoscut | în afara scopului; fără hookuri speculative |

Profilul de compatibilitate include builduri exacte, hashuri aprobate, adaptor, convenții de apel, simboluri/patternuri documentate și raport de teste. Un build necunoscut produce oprirea integrării SKR cu diagnostic, nu patchuri presupuse în memorie.

Criterii SKR propuse pentru certificarea R2: 50 cicluri launch/connect/quit, 100 alt-tab/reset, sesiune de două ore, input CEF, reconnect, compatibilitate SKRAC reală, respingerea ticketelor invalide și zero acces la gameplay înainte de grant. Acestea sunt obiective, nu rezultate obținute.

## 5. Launcher Windows

Flux: selectare instalare → verificare profil → manifest semnat → update în staging → login → pornire profil → IPC cu modulul → ticket → conectare → diagnostic.

- Nu presupuneți că `CreateProcess(gta_sa.exe)` încarcă automat SA-MP/open.mp. Implementați un adaptor de lansare verificat în sursele distribuției selectate, cu licența respectată. Protocolul `samp://` poate deschide altă aplicație înregistrată și nu este dovadă SKR.
- Nu presupuneți că launcherul upstream expune un SDK public stabil pentru injectare. Documentați mecanismul real înainte de implementare.
- Folosiți căi absolute, `CreateProcessW` cu aplicația explicită și citare Windows corectă; nu concatenați input în comenzi shell.
- IPC recomandat: named pipe cu nume aleator per lansare, DACL pentru utilizator, respingere remote, limite de mesaje și verificarea peer-ului. Procesele ostile cu aceleași drepturi sau administrator rămân în modelul de amenințări.
- Nu puneți tokenuri în CLI, URL, mediu global sau fișiere temporare. Folosiți IPC verificat și stocare Windows adecvată utilizatorului pentru refresh token, dacă este necesar.
- Utilizarea normală nu cere administrator. Instalarea în directoare protejate este flux separat.
- Updaterul gestionează doar fișierele SKR inventariate, cu backup/rollback și raportarea conflictelor cu moduri existente.

## 6. DLL/ASI: CMake și C++ minimale

În acest context, ASI este o bibliotecă PE încărcată de un ASI loader. Extensia singură nu asigură încărcarea. Folosiți loader x86 verificat și evitați conflictele cu alte proxy DLL, în special `d3d9.dll`.

### 6.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.24)
project(SKRClient VERSION 0.1.0 LANGUAGES CXX)
if(NOT WIN32)
  message(FATAL_ERROR "Scheletul necesita Windows")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configurati Win32 pentru modulul GTA SA")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(SKRLauncher WIN32 apps/launcher/main.cpp)
target_link_libraries(SKRLauncher PRIVATE user32)
add_library(SKRClient SHARED client/bootstrap/dllmain.cpp)
set_target_properties(SKRClient PROPERTIES
  OUTPUT_NAME "SKR.Client" PREFIX "" SUFFIX ".asi")
if(MSVC)
  foreach(target SKRLauncher SKRClient)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    target_link_options(${target} PRIVATE /DYNAMICBASE /NXCOMPAT)
  endforeach()
endif()
```

### 6.2 apps/launcher/main.cpp

```cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    MessageBoxW(nullptr, L"SKR Launcher: schelet de dezvoltare.",
                L"SKR Client", MB_OK | MB_ICONINFORMATION);
    return 0;
}
```

### 6.3 client/bootstrap/dllmain.cpp

```cpp
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
namespace { std::atomic<bool> started{false}; }

extern "C" __declspec(dllexport) BOOL __cdecl SkrInitialize() noexcept {
    // Apelat de adaptor DUPA revenirea din LoadLibrary.
    if (started.exchange(true)) return TRUE;
    // TODO: pasi expliciti cu rollback; fara initializare ascunsa.
    return TRUE;
}
extern "C" __declspec(dllexport) void __cdecl SkrShutdown() noexcept {
    // Implementarea finala opreste taskurile inainte de unload.
    started.store(false);
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE; // Fara CEF, D3D, retea, fire sau wait aici.
}
```

**Acest ASI este inert.** Loaderul nu este presupus a apela exportul arbitrar `SkrInitialize`. Implementați lifecycle-ul printr-un callback sigur al SDK/adaptorului, după ieșirea din loader lock. Pe x86 verificați decorarea exporturilor; pentru nume stabile folosiți un fișier `.def` și inspectați rezultatul cu `dumpbin /exports`.

Lifecycle final: identificare build → callback sigur → IPC → resurse grafice → bridge → ready. La eșec desfaceți pașii deja efectuați. Nu dezîncărcați DLL-ul cât timp hookuri, workeri sau callbackuri pot executa cod din el. Pentru MVP poate rămâne rezident până la ieșirea jocului, cu shutdown explicit al resurselor în punct sigur.

Pentru ieșire `.dll`, eliminați proprietatea `SUFFIX ".asi"`. Scheletul nu implementează lansarea jocului, hookuri, CEF, auth sau AC și nu a fost compilat în această sesiune.

## 7. Intro D3D9

Intro-ul este prezentare, nu control de securitate. MVP: logo static, fade de 2–3 secunde, progres și skip accesibil. Video se adaugă după evaluarea codecurilor, licențelor și performanței.

Preferință: callback de randare al adaptorului compatibil. Dacă integrarea cere hookuri `Present`/`EndScene` și `Reset`, păstrați convenția de apel, originalul și ordinea celorlalte module. Alegeți o singură cale de desen pentru a evita randarea dublă. Nu există offset universal în acest plan.

```cpp
// Interfata conceptuala, fara implementare si fara conectare la joc.
struct IDirect3DDevice9;
struct IntroRenderer {
    bool CreateResources(IDirect3DDevice9* device);
    void Draw(IDirect3DDevice9* device, float elapsedSeconds);
    void BeforeReset();
    bool AfterSuccessfulReset(IDirect3DDevice9* device);
    void Shutdown();
};
```

Implementare obligatorie:

- Apeluri D3D pe firul compatibil cu jocul; workerii livrează date prin cozi.
- Salvați/restaurați starea modificată, inclusiv targets, viewport, shaders, textures și streams după caz. Eșecul capturării stării nu trebuie să corupă cadrul.
- Desenați quad/textură cu alpha blending; nu ștergeți cadrul jocului cu `Clear` pentru logo.
- Înainte de reset eliberați resursele relevante din `D3DPOOL_DEFAULT`; recreați numai după reset reușit. Un reset eșuat nu reactivează rendererul.
- Fără I/O, HTTPS sau alocări mari pe render path. Testați alt-tab, rezoluții, fullscreen/windowed și overlay-uri.
- Măsurați CPU/GPU și frame pacing; ținta propusă sub 2 ms pentru overlay simplu se raportează la un sistem de referință declarat.

Consultați [Microsoft D3D9 lost devices](https://learn.microsoft.com/en-us/windows/win32/direct3d9/lost-devices) pentru lifecycle-ul resurselor.

## 8. CEF și samp-cef

### 8.1 Alegere explicită

**A — samp-cef:** fixați commitul, buildul Rust/CEF și ABI-ul real. Demonstrați compatibilitatea pluginului server cu open.mp și a clientului cu R2. Legătura din C++ se face numai prin interfața efectiv expusă de acel commit. Nu este bibliotecă pur C++ și nu se presupune că lucrează cu orice versiune CEF.

**B — CEF propriu:** host/helper separat, OSR, IPC și adaptor D3D9. Un host x64 poate comunica cu GTA x86 prin protocol explicit, însă nu puteți încărca `libcef.dll` x64 în procesul jocului x86. Această variantă este alternativă de arhitectură, fără compatibilitate demonstrată cu samp-cef.

Pentru CEF în proces, arhitectura bibliotecii corespunde gazdei. Disponibilitatea unei versiuni întreținute x86 este gate tehnic: dacă lipsește, decideți helper extern sau reducerea scopului, nu expunerea unui browser istoric vulnerabil.

### 8.2 Lifecycle și randare

Urmați mostra CEF a versiunii fixate: tratare subprocess în executabilul helper, inițializare doar în procesul browser, message loop și închidere browsere înainte de shutdown. Nimic în `DllMain`.

Pentru OSR software, copiați bufferul temporar din callbackul `OnPaint`, sincronizați către render thread și încărcați textura D3D9. Folosiți buffer dublu și limite de rezoluție/frecvență. Nu păstrați pointerul callbackului. Interop accelerat D3D11–D3D9 nu se presupune disponibil; trebuie demonstrat separat, inclusiv reset și sincronizare.

Inputul se capturează numai cât UI are focus; tratați DPI, coordonate, keyboard, mouse, wheel și IME. La închidere se restabilește controlul jocului.

### 8.3 Bridge propus

```json
{"v":1,"id":"request-42","type":"ui.settings.set","payload":{"musicVolume":0.5}}
```

Allowlist de evenimente, schemă strictă, maxim 16 KiB/mesaj și rate limit per browser. Nu există evenimente care acordă auth, execută shell, citesc arbitrar fișiere sau evaluează cod nativ. Validarea JS nu înlocuiește validarea nativă/server.

UI locală verificată, origine precisă și CSP restrictivă. Blocați navigări, popupuri, downloaduri și origini neaprobate. Nu puneți tokenuri în DOM sau `localStorage`. Sandbox activ unde integrarea îl suportă; o limitare demonstrată este gate pentru release și tipul de conținut permis. `--no-sandbox` nu este soluția implicită.

Distribuiți toate bibliotecile, resursele, localele și helperii ceruți de README-ul pachetului CEF ales, nu doar `libcef.dll`. Buildurile cu codecuri proprietare necesită analiză separată.

## 9. API și handshake

Tot protocolul de aici este **propunere SKR**, nu API existent open.mp, CEF sau SKRAC.

### 9.1 Secvență

1. Launcherul autentifică utilizatorul prin HTTPS. Dacă identitatea folosește OAuth/OIDC, selectați un furnizor real și Authorization Code cu PKCE.
2. DLL-ul creează o pereche efemeră de chei, folosind bibliotecă verificată și RNG criptografic. Launcherul primește cheia publică prin IPC; cheia privată nu pleacă la server.
3. Launcherul cere ticket legat de cont, cheia publică și server. Backendul verifică drepturi, revocări, versiune și politica AC.
4. Jocul se conectează la open.mp. Gate-ul alocă un `connection_id` aleator și `generation` pentru acea conexiune, fără a folosi simplul player ID ca identitate.
5. Serverul trimite nonce aleator de 32 octeți, identitate server, termen și identitatea conexiunii.
6. DLL-ul trimite ticketul și semnătura transcriptului pe un canal legat de aceeași conexiune de joc. Adaptorul concret se verifică în spike; nu se inventează ID-uri RPC sau pachete SA-MP.
7. Gate-ul cere backendului verificare și consum atomic. Backendul returnează contul și politica, nu preia identitatea din nickname.
8. Numai verdictul valid produce `AUTHORIZED` și permite gameplay.
9. Reconnect generează ticket/challenge noi. Răspunsurile asincrone întârziate nu autorizează alt jucător care primește același slot.

**Gate tehnic:** o dovadă HTTPS separată nu demonstrează singură cui aparține conexiunea UDP. IP + nickname nu este asociere sigură. Dacă nu putem lega challenge/reply de conexiunea reală, nu declarăm accesul exclusiv implementat.

### 9.2 Endpointuri

| Endpoint propus | Apelant | Funcție |
|---|---|---|
| `GET /v1/releases/stable` | launcher | manifest semnat |
| `POST /v1/join-tickets` | launcher autentificat | emitere ticket |
| `POST /internal/v1/join-tickets/redeem` | server autentificat | verificare și consum atomic |
| `POST /internal/v1/sessions/revoke` | serviciu autorizat | revocare și audit |
| `POST /v1/skr-ac/reports` | adaptor conform politicii | semnale, fără grant autonom |

Loginul/refreshul se definesc conform furnizorului ales. API-urile interne cer mTLS sau credențiale de serviciu echivalente, rotație și autorizare pentru `server_id`; tokenul jucătorului nu permite redeem intern.

Cerere ilustrativă:

```json
{
  "protocol": 1,
  "server_id": "skr-main",
  "client_build": "0.1.0",
  "compat_profile": "samp-037-r2-pending-validation",
  "session_public_key": "BASE64URL_PUBLIC_KEY"
}
```

Răspuns ilustrativ, fără token real:

```json
{"join_id":"OPAQUE_256_BIT_RANDOM_VALUE","expires_in":30,"server_id":"skr-main","protocol":1}
```

Backendul păstrează hashul `join_id`, contul, cheia publică, serverul, expirarea, starea și versiunea politicii. Ticketul opac simplifică revocarea. Un ticket semnat offline este alternativă, dar necesită în continuare protecție atomică la reutilizare și distribuție sigură a cheilor publice.

### 9.3 Transcript și codificare

Folosiți algoritm standard suportat de o bibliotecă verificată, de exemplu Ed25519. Nu implementați criptografie proprie. Transcriptul canonic propus:

```text
"SKR-JOIN-v1" || protocol_u16_be ||
len16(server_id) || server_id_utf8 ||
connection_id_16bytes || generation_u64_be || nonce_32bytes ||
len16(join_id) || join_id_ascii || expires_at_u64_be
```

`expires_at` este UNIX time în secunde dat de server. Pentru ID-uri definiți ASCII restrâns și maxime. Nu semnați JSON concatenat ambiguu. Verificatorul folosește cheia publică legată de ticket în backend, nu o cheie arbitrară furnizată în reply.

```text
Challenge {v, server_id, connection_id, generation, nonce, expires_at}
Proof     {v, join_id, signature}
Result    {v, status, error_code}
```

Framingul adaptorului SKR folosește lungimi verificate înainte de alocare, numere în network byte order și plafon inițial 2 KiB/mesaj handshake. Nu implementați fragmentare improvizată. Acest format logic nu stabilește un RPC ID în protocolul jocului.

### 9.4 Verificare server — pseudocod

```text
on_proof(connection, proof):
  require state == CHALLENGED and deadline_not_expired
  require valid_schema(proof) and supported_version(proof.v)
  snapshot = {connection.id, connection.generation, connection.nonce}
  state = VERIFYING
  asynchronously redeem_backend(proof, snapshot, server_identity)

on_result(snapshot, result):
  dispatch_to_server_thread()
  require live_connection_matches(snapshot.id, snapshot.generation)
  require state == VERIFYING and deadline_not_expired
  if result.valid_and_consumed and ac_policy_satisfied:
    bind_account_from_backend(result.account_id)
    clear_challenge()
    state = AUTHORIZED
    allow_gameplay()
  else:
    reject_and_disconnect()
```

Backendul verifică semnătura, expirarea și legăturile înaintea tranziției atomice `issued → redeemed`. Două cereri concurente nu obțin două granturi. Retry intern poate fi idempotent pentru exact aceeași operație/conexiune, fără reutilizare pe altă conexiune.

Praguri propuse: ticket 30 s, challenge 10 s, verificare cel mult 5 s în deadline. Backendul este autoritar pentru expirare; serverul folosește ceas monoton pentru timeout local. Ceasul clientului nu acordă acces.

Coduri: `AUTH_REQUIRED`, `TICKET_EXPIRED`, `TICKET_USED`, `BUILD_UNSUPPORTED`, `PROTOCOL_UNSUPPORTED`, `AC_REQUIRED`, `SERVICE_UNAVAILABLE`. Nu expuneți tokenuri sau reguli AC în mesaje. Backend indisponibil: noile conexiuni nu intră în gameplay. Pentru sesiunile existente definiți distinct o grație limitată și comportamentul revocărilor.

## 10. Integrare SKRAC

Fără SDK nu sunt confirmate exporturi, arhitectură, privilegii, detecții sau suport R2. Nu presupunem driver kernel.

Interfață internă propusă:

```cpp
#include <cstdint>
#include <string>
enum class AcState : std::uint8_t { Unknown, Starting, Ready, Degraded, Failed };
struct AcStatus {
    AcState state;
    std::string engineVersion;
    std::string policyVersion;
};
class ISkrAcAdapter {
public:
    virtual ~ISkrAcAdapter() = default;
    virtual bool Start(const std::string& sessionId) = 0;
    virtual AcStatus Poll() = 0;
    virtual void Stop() noexcept = 0;
};
```

Aceasta nu este API SKRAC real sau ABI stabil. Pentru DLL externă folosiți C ABI, structuri versionate cu `size`, tipuri fixe și ownership explicit; memoria se eliberează în modulul care a alocat-o. Nu transmiteți `std::string` între compilatoare/module incompatibile.

Mock-ul poate produce `Ready` doar în dezvoltare; producția respinge identitatea mock. Integrarea finală verifică mecanismul real de dovadă al motorului și, dacă există, verificarea server-side a acestuia.

Politică propusă:

- Motor lipsă sau versiune nesuportată: refuz cu remediere clară.
- Timeout sau crash AC: retry limitat ori disconnect; nu ban permanent automat.
- Detecție: incident cu regulă, versiune, dovezi minime și revizuire; incidentul și sancțiunea sunt distincte.
- Heartbeat cu sesiune, secvență și prospețime; heartbeatul nu dovedește singur integritatea procesului.
- Serverul verifică economie, mișcare și cooldownuri indiferent de verdictul AC.

Date necesare: SDK/licență, x86, lifecycle/threading, suport R2, semantică verdict, endpointuri/chei, retenție, diagnostic și false positives.

## 11. Securitate și limite anti-spoof

| Măsură | Ajută la | Nu dovedește |
|---|---|---|
| Ticket scurt unic | reducere replay și acces autorizat | binarul launcherului folosit |
| Challenge semnat | posesia cheii și legătura cu transcriptul | că cheia există doar în DLL original |
| Manifest semnat | proveniența pachetului distribuit | integritatea continuă a memoriei |
| Hashuri locale | corupție/modificări simple | sinceritatea unui client controlat |
| Nume proces / părinte | diagnostic | autenticitate |
| IP / HWID | semnal suplimentar limitat | persoană unică sau identitate sigură |
| AC user mode | creșterea costului abuzului | imposibilitatea emulării |

Un utilizator care controlează sistemul poate modifica DLL-ul, citi memoria, folosi sesiunea în timp real sau implementa protocolul într-un alt client. O cheie privată hardcodată în executabil poate fi extrasă. Cheile efemere reduc replay-ul pasiv al ticketelor furate, dar nu elimină furtul cheii sau relay-ul activ. Hardware keys și atestarea, dacă vor exista, nu certifică automat întreg procesul GTA.

Formula corectă: **„Serverul necesită autorizare SKR și verificări conform politicii curente.”** Nu promiteți „100% anti-spoof”, „DLL imposibil de extras” sau „doar launcherul original poate conecta”.

Controale obligatorii:

- TLS cu validarea certificatului și hostname-ului; fără bypass în release.
- Rate limit login/ticket/challenge/UDP, cu praguri globale și per cont/conexiune. NAT-ul nu devine identitate.
- Parsere cu dimensiuni maxime, overflow verificat și versiuni acceptate explicit.
- Chei backend/release protejate, rotație, revocare și separare staging/producție.
- Manifest semnat cu versiune, canal, platformă, arhitectură, hash SHA-256, dimensiune și key ID. Hashul singur nu autentifică manifestul.
- Update în staging, verificare înainte de swap și rollback numai la versiuni încă autorizate.
- Prevenirea path traversal și verificarea căilor finale la extracție; respingerea linkurilor nepermise.
- Politică sigură de încărcare DLL, fără căutare arbitrară în working directory. Verificarea Authenticode este distinctă de manifest.
- Fără parole, ticket complet, chei sau tokenuri în loguri. Dumpurile de memorie pot conține secrete.
- Colectare AC/telemetrie minimă, informată, retenție definită și acces controlat; fără scanarea arbitrară a fișierelor personale.

Modelul de amenințări acoperă client alternativ, ticket furat, replay, redeem concurent, schimbare server, slot reutilizat, IPC spoofing, XSS, update/dependință compromisă, cheie compromisă și indisponibilitate. Autentificarea nu rezolvă singură DDoS.

## 12. Build și dependențe

### 12.1 Mediu și schelet

Windows, Visual Studio cu C++ desktop și toolset x86, Windows SDK, CMake minimum 3.24 pentru schelet. CEF și samp-cef pot cere alt toolchain: urmați configurația commitului ales.

`d3d9.h` și import library provin din SDK-ul adecvat. Evitați D3DX pentru cod nou. Dacă dependența legacy cere DirectX SDK June 2010, documentați proveniența și conflictele; nu instalați pachete de pe site-uri necunoscute.

Copiați cele trei fișiere din secțiunea 6 în repo, apoi:

```powershell
cmake -S . -B out/client-x86 -G "Visual Studio 17 2022" -A Win32
cmake --build out/client-x86 --config Debug
cmake --build out/client-x86 --config Release
```

Generatorul este exemplu pentru VS 2022 instalat. Pentru alt Visual Studio alegeți generatorul disponibil și compatibil. Verificați PE x86, dependențele runtime și exporturile. Launcherul afișează numai mesaj; ASI-ul nu autentifică și nu desenează.

### 12.2 Surse

```powershell
git clone --recursive https://github.com/openmultiplayer/open.mp.git third_party/open.mp
git clone --recursive https://github.com/openmultiplayer/open.mp-sdk.git third_party/open.mp-sdk
git clone https://github.com/zottce/samp-cef.git third_party/samp-cef
git clone https://github.com/DK22Pac/plugin-sdk.git third_party/plugin-sdk
```

După clone fixați commiturile aprobate, actualizați submodulele conform checkoutului și completați lockfile-ul. Comenzile singure nu fixează dependențele. Inspectați scripturile de descărcare/build înainte de rulare.

Pentru server urmați [BUILD.md open.mp](https://github.com/openmultiplayer/open.mp/blob/master/BUILD.md) la commitul ales. Arhitectura componentei trebuie să corespundă serverului distribuit; jocul x86 nu impune server x86.

Fragment SDK conform README, în proiectul separat al componentei:

```cmake
add_subdirectory(third_party/open.mp-sdk)
# Dupa definirea targetului real:
target_link_libraries(SkrGate PRIVATE OMP-SDK)
```

Entry pointul, UID-ul și callbackurile se preiau din SDK-ul fixat, nu din nume inventate. Fragmentul nu creează o componentă funcțională.

CEF propriu: începeți cu distribuția binară standard, mostrele și wrapperul aferent, apoi păstrați layoutul runtime cerut. Buildul întregului Chromium/CEF folosește flux separat GN/Ninja și resurse substanțiale; nu este necesar primului prototip cu binare oficial indicate.

## 13. Testare

| Nivel | Probe | Criteriu |
|---|---|---|
| Unit | parser, transcript, expiry, state machine | fără crash sau grant pe input invalid |
| Crypto | vectori cunoscuți și transcript modificat | respingerea semnăturilor invalide |
| Auth | ticket expirat/folosit/alt server/altă cheie | un singur grant corect |
| Concurență | două redeem și slot player reutilizat | zero grant pe conexiune greșită |
| Gate | fiecare acțiune în fiecare stare | zero efect gameplay înainte de auth |
| Client real | profil exact R2, launch/quit/reconnect | raport complet conform secțiunii 4 |
| D3D9 | alt-tab/reset eșuat/rezoluții | restaurare fără leak progresiv |
| CEF | input/IME, crash, XSS, navigare | fără secrete sau execuție nativă arbitrară |
| SKRAC | mock/lipsă motor/timeout/verdict invalid | mock respins în producție; fără ban din timeout |
| Update | corupție/semnătură invalidă/traversal/întrerupere | instalare recuperabilă și căi respectate |
| Operare | backend căzut/revocare/rotație | comportament previzibil și auditabil |

Fuzzing pentru parsere native într-un harness separat de GTA; sanitizere unde toolchainul permite. Fixture-uri controlate pentru AC, fără ocolirea sistemelor reale. Testele folosesc conturi și server staging.

Raportul real include Windows, CPU/GPU/driver, hashuri executabile, commituri, configurație, pași și rezultat. Un checkbox fără dovezi nu certifică R2.

CI: build x86 client, build separat server, unit/integration, scanare secrete, verificare dependențe/licențe și pachet staging. Runnerul generic nu înlocuiește testarea D3D9/GTA pe Windows cu o copie legală a jocului.

## 14. Release și operare

```text
SKRClient/
  SKRLauncher.exe
  updater/
  client/SKR.Client.asi
  client/compatibility.json
  cef/                       # layout cerut de versiunea aleasa
  ui/
  assets/intro/
  release-manifest.json
  release-manifest.sig
  licenses/
  README-ro.md
```

Layoutul instalat în joc poate diferi de arhivă. Installerul mapează explicit fișierele SKR, inventariază pentru uninstall și raportează conflictele cu moduri/proxy DLL existente. Nu suprascrie tăcut.

Pași release:

1. Înghețare commituri, profiluri și protocol; audit licențe/SBOM.
2. Build curat cu proveniență și hashuri. Reproductibilitatea bit-cu-bit nu se afirmă fără măsurare.
3. Teste automate și matrice reală R2, cu raport arhivat.
4. Semnare binare eligibile; hashurile manifestului se calculează după semnare.
5. Semnare manifest cu cheie protejată separat de repo; verificare pachet final.
6. Canary intern → beta → stable gradual, cu versiuni minime și politici pe canal.
7. Monitorizare crash, latență auth, consum memorie și false positives; rollback la versiune sigură încă acceptată.

Uninstall șterge numai fișierele SKR inventariate; cache/date conform opțiunii utilizatorului. Loguri cu rotație/retenție. Crash dumps opt-in, filtrate și protejate.

Runbook incident: revocare chei/tokenuri, suspendare emitere ticket dacă este necesar, build remediat semnat, actualizare minimum supported version și audit. Nu implementați un kill switch cu execuție arbitrară pe calculatorul utilizatorului.

## 15. Roadmap

| Etapă | Livrabil | Gate de ieșire |
|---|---|---|
| M0 | lockfile, profil R2, licențe, contract SKRAC | builduri și interfețe reale identificate |
| M1 | launcher și ASI inert, lifecycle | inițializare sigură în afara loader lock |
| M2 | logo/fade D3D9 | reset și restaurare stare trecute |
| M3 | spike samp-cef sau CEF propriu | versiune sigură și compatibilitate R2 demonstrate |
| M4 | backend, ticket, challenge, gate | replay/concurență și blocare gameplay verificate |
| M5 | adaptor SKRAC real | SDK, erori și false positives testate |
| M6 | updater/manifest/installer | întrerupere și rollback testate |
| M7 | beta și raport compatibilitate | criterii secțiunea 13 îndeplinite |
| M8 | stable și suport | proprietar operațional și recuperare verificate |

Handshake-ul poate fi dezvoltat fără UI completă. Prioritizați demonstrarea gate-ului și a compatibilității înaintea animațiilor complexe. Termenele se estimează după M0, când necunoscutele CEF/R2/SKRAC sunt măsurate.

## 16. Instrucțiuni agent AI

Conținut recomandat pentru `AGENTS.md`:

```text
Proiect: SKR Client, launcher Windows + GTA SA x86 ASI + open.mp gate.

1. Citeste planul, lockfile si profilul de compatibilitate inainte de editare.
2. Lucreaza incremental; separa codul compilabil, pseudocodul si API-urile propuse.
3. Nu inventa API-uri, callbackuri, RPC ID-uri, offseturi sau teste trecute.
4. Consulta sursele oficiale la commitul fixat si pastreaza provenienta.
5. Prefera SDK-ul verificat in locul modificarilor nucleului open.mp.
6. Nu initializa CEF, grafica, retea, fire sau wait in DllMain.
7. Nu introduce secrete in cod, CLI, loguri sau fisiere temporare.
8. Respecta x86 pentru GTA si ABI-ul separat al serverului/helperilor.
9. Fiecare actiune protejata verifica autorizarea server-side.
10. Hashurile si heartbeatul client nu sunt atestare; fara promisiuni anti-spoof absolute.
11. Mock SKRAC este doar pentru dezvoltare si este respins in productie.
12. Nu dezactiva TLS, sandbox sau semnaturi pentru a trece testele.
13. Nu redistribui GTA, surse nepublice sau dependinte cu licenta neverificata.
14. Ruleaza verificarile relevante; declara exact ce nu ai putut rula.
15. Nu marca R2 compatibil fara matrice completata pe hashuri cunoscute.
16. Nu publica sau semna cu chei reale fara autorizarea proprietarului.
17. Raporteaza fisiere schimbate, validare, riscuri si blocaje reale.
```

Prima sarcină recomandată:

> Implementează M0 și scheletul M1 într-un branch de dezvoltare. Verifică buildurile și SDK-urile, creează lockfile fără valori inventate, compilează launcherul și ASI inert dacă mediul permite, documentează mecanismul real de lifecycle și listează informațiile SKRAC lipsă. Nu adăuga încă hookuri de rețea/grafică și nu declara compatibilitate R2 sau publica pachete.

Agentul poate continua munca independentă când o interfață este necunoscută, fără a transforma presupunerile în fapte.

## 17. Checklist și întrebări deschise

### Înainte de prototip funcțional

- [ ] Confirmat că ținta este clientul 0.3.7-R2.
- [ ] Hashuri și proveniență pentru GTA, client și loader.
- [ ] Release open.mp și SDK corespunzător selectate.
- [ ] Canal challenge legat de conexiunea reală demonstrat.
- [ ] Blocarea tuturor căilor de gameplay înainte de grant verificată.
- [ ] Lifecycle ASI în afara `DllMain` confirmat.
- [ ] Alegere CEF/samp-cef cu versiune întreținută și licență analizată.
- [ ] SDK SKRAC primit sau proiect marcat mock-only.
- [ ] Furnizor de identitate și model de cont stabilite.

### Înainte de stable

- [ ] Consum atomic, replay, reconnect și slot reuse testate.
- [ ] Fără secrete globale în client sau chei private în repo.
- [ ] UI izolată, parsere limitate și update semnat.
- [ ] Matrice R2 cu rezultate reale, nu presupuneri.
- [ ] Licențe și notificări pentru artefactele efectiv distribuite.
- [ ] Politică AC/date, retenție și false positives.
- [ ] Rollback, revocare și incident response testate.

**Rezultat urmărit:** utilizatorul autentifică un cont, primește componente verificate, pornește un profil certificat, vede intro și UI, iar serverul open.mp permite gameplay numai după validarea SKR și politica SKRAC. Sistemul reduce accesul neautorizat și replay-ul fără a pretinde control absolut asupra calculatorului utilizatorului.
