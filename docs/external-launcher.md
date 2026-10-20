# Launcher experimental cu lobby WebView2 — 19 octombrie 2026

Implementare C++ compilabilă, activată explicit cu `SKR_EXTERNAL_LOBBY=ON`.
Buildul implicit păstrează managerul lobby existent. Modificările locale
anterioare din lobby/game-state și omp-cef au fost păstrate.

## Build și probă

SDK: pachetul oficial NuGet Microsoft.Web.WebView2 **1.0.2903.40**,
URL și SHA-256 în `dependencies.lock.json`. Extrage pachetul în
`deps/webview2/1.0.2903.40`. Licența BSD din `LICENSE.txt` trebuie păstrată.
Nu este inclus un WebView2 Runtime; se folosește Evergreen Runtime instalat.
Nu sunt dezactivate sandboxul, TLS sau verificarea hashului samp.dll.

```powershell
cmake -S . -B build-external-lobby -G "Visual Studio 18 2026" -A Win32 -DSKR_EXTERNAL_LOBBY=ON
cmake --build build-external-lobby --config Release --target SKRLauncher SKRClient SKRLobbyTests -j 4
./build-external-lobby/Release/SKRLobbyTests.exe
./build-external-lobby/Release/SKRLauncher.exe --lobby-smoke
```

Pentru VS2022 folosește generatorul `Visual Studio 17 2022` într-un alt
director de build. Buildul din această sesiune a folosit MSVC 19.51 / SDK
10.0.26100.0, Win32 Release.

Pachetul local pentru probă este în `out/external-lobby`. Pentru testul complet,
copiază împreună `SKRLauncher.exe`, `SKR.Client.dll` și directorul `SKR` lângă
instalarea GTA existentă, păstrând copii ale versiunilor precedente. Pachetul
nu conține GTA sau samp.dll. Nu am modificat instalarea jocului în această sesiune.
Executabilul din pachet poate afișa lobby-ul fără GTA; verifică prezența jocului
abia după Connect. Descoperirea GTA păstrează regulile launcherului existent:
directorul launcherului, părintele și calea Steam standard.

## Comportament

- Lobby fullscreen într-o fereastră WebView2; același HTML/CSS/JS.
- Host local virtual `https://skr-launcher.invalid`, mapat exclusiv la lobby.
  Navigarea top-level și ferestrele noi sunt blocate; mesajele native verifică
  exact URL-ul sursei și limita de 4096 bytes. Nu se expun obiecte COM către JS.
- Bridge string: nume eveniment, newline, payload existent. Sunt implementate
  nickname, preferințe, servere personale, catalog și query-uri asincrone,
  Connect și Quit. Catalogul folosește adaptorul HTTPS existent.
- Connect închide fereastra, trimite host/port/nickname prin IPC existent și
  pornește GTA suspendat cu DLL-ul pereche. Eșecul inițializării DLL este raportat.
- DLL-ul validează samp.dll R5 prin hashul existent, completează blocul Settings
  înainte de reluarea GTA și păstrează așteptările native de stare; nu instalează
  gate-ul care aștepta alegerea serverului în lobby-ul din joc.
- Managerul alternativ nu creează browserul lobby CEF, nu aplică patchurile de
  meniu sau hookurile de input ale lobby-ului și nu desenează cover/splash.
  Păstrează identificarea sesiunii SKR și integrarea CEF/escape menu a serverului.
- Exit code 42 redeschide lobby-ul; celelalte coduri închid launcherul, ca înainte.

## Proveniență

- WebView2 API: headerele pachetului fixat mai sus și
  https://learn.microsoft.com/en-us/microsoft-edge/webview2/get-started/win32
  (COM STA, controller, WebMessageReceived, PostWebMessageAsString).
- Settings R5 verificat în sursa SDK locală la commitul
  `6d4db99ab41f19d1a6a7c6cd48f5878bd1e14b62`:
  `deps/omp-cef/deps/samp-api/src/sampapi/0.3.7-R5-1/Settings.cpp` și headerul
  corespunzător. Consultat cu `git show HEAD:...`, nu presupus din branch tip.
- Așteptările native și ordinea bootstrapului: analiza statică locală existentă
  `D:/SA-MP Reverse/RE/client/research/docs/initialization_flow.md`.
  Nu au fost adăugate offseturi sau RPC-uri noi.
- Hash samp.dll acceptat: `B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2`.
  Checkout omp-cef observat: `fb5097b866c7cf51db774f96b7402cb260f8bb16`,
  cu modificările locale existente. Lockfile-ul anterior nu îl descrie integral.

## Validare și limite

- Build Win32 Release SKRLauncher, SKRClient, SKRLobbyTests: succes.
- SKRLobbyTests: **43 verificări, 0 eșecuri**.
- `node --check client/lobby/lobby.js`: succes.
- `git diff --check`: succes (avertismente privind normalizarea CRLF).
- `SKRLauncher.exe --lobby-smoke`: **exit code 0** pe WebView2 Runtime real.
  Verifică navigarea locală, execuția JS, existența bridge-ului și DOM/CSS;
  nu este verificare vizuală și nu testează conectarea GTA.

Nu s-a pornit GTA și nu s-au verificat conectarea, exit code 42, reseturile D3D,
ESC, UI vizual sau restricțiile de origine prin pagini ostile. Preferințele de
rezoluție/windowed/volum sunt persistate; aplicarea lor în joc nu este implementată
de managerul alternativ. Query-urile aflate în curs pot întârzia închiderea
ferestrei până la timeouturile adaptorului existent. Lipsa runtime-ului afișează
o eroare; nu există instalator automat.

Aceasta este o probă de dezvoltare: autentificarea/ticketele launcherului rămân
TODO, Mock SKRAC rămâne Development și nu este acceptabil pentru producție.
Autorizarea gameplay-ului rămâne responsabilitatea serverului. Mutarea lobby-ului
nu dovedește remedierea corupției heap în randarea CEF a paginilor serverului.
Nu este release reproductibil complet și nu certifică R2.
