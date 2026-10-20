# Plan SKR Multiplayer pornind de la external-lobby

Data: 19 octombrie 2026. Stare: plan de implementare; nu reprezintă funcții
livrate sau compatibilitate certificată. Nu înlocuiește master planul în privința
securității, licențelor și verificării pe hashuri.

## 1. Decizia de arhitectură

Construim experiența multiplayer SKR peste clientul binar samp.dll R5 verificat.
Acesta asigură transportul și sincronizarea SA-MP existente; SKR controlează
lansarea, identitatea, pregătirea resurselor, interfața, intro-ul și extensiile
gameplay. Serverul este open.mp compilat din surse fixate, cu extensii prin SDK.

Această etapă nu presupune reconstruirea samp.dll din decompilare. RE din
`D:/SA-MP Reverse` se folosește pentru verificarea ABI-urilor și a punctelor de
integrare strict necesare. Nu copiem sursele decompilate în pachetul distribuit.
Un client independent de samp.dll ar fi un proiect ulterior, cu propriul
protocol/compatibilitate și un buget separat de implementare și testare.

Păstrăm WebView2 în launcher și CEF în joc: lobby-ul nu revine în procesul GTA.
Pentru încărcare folosim o interfață D3D9 simplă, disponibilă înaintea CEF.

| Componentă | Responsabilitate |
|---|---|
| SKRLauncher.exe | Lobby, cont, catalog, manifest, download, instalare, lansare, revenire |
| SKR.Client.dll, x86 | Lifecycle GTA, adaptor R5, intro, setări, bridge și stare sesiune |
| samp.dll R5 validat | Bootstrap multiplayer și sincronizarea SA-MP existentă |
| CEF + integrarea omp-cef | HUD, chat, meniuri, pagini de gameplay și input în joc |
| open.mp din surse fixate | Serverul multiplayer și API-urile disponibile componentei |
| SKRGate | Autorizare, negocierea extensiilor și verificarea acțiunilor protejate |
| Gamemode | Reguli de joc și verificarea autorizării înaintea acțiunilor |
| Backend HTTPS | Conturi, ticketing, catalog și metadate pentru resurse |
| Host/CDN HTTPS | Fișiere versionate, fără acces la secretele conturilor |

Nu creăm o a doua conexiune RakNet pentru aceeași sesiune. Extensiile se atașează
transportului verificat al clientului sau folosesc un canal separat explicit,
după verificarea posibilităților SDK. Nu asignăm ID-uri RPC în acest document.

## 2. Baza de la care pornim

`out/external-lobby` este pachetul de probă, iar sursele rămân în repository.
Nu edităm executabilele din out ca sursă de adevăr.

Snapshot identificat prin SHA-256:

- SKRLauncher.exe: `878A672BB61AAD100677F0A683290245CF7391600B80845DD49CC91A83684696`.
- SKR.Client.dll: `F004226544CD4ADDE913103FB788B7DA79281E5BDBE91587552A747860C1C608`.
- samp.dll acceptat: `B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2`.

Există: lobby WebView2, bridge local, persistența unui subset de setări,
servere personale/catalog/query, lansare suspendată, injecție și IPC cu
host/port/nickname, manager alternativ fără browserul CEF al lobby-ului.

Documentate anterior: build x86 reușit, 43 verificări de setări și smoke WebView2.
Nu rerulăm aceste rezultate ca și cum ar valida GTA. Conectarea completă,
CEF în joc, ieșirea cu cod 42 și setările native rămân de demonstrat runtime.

Probleme observate în sursele actuale:

1. `server/skr-gate-omp/skr_gate_component.cpp`, `onPlayerConnect`: apelează
   `AuthorizePlayer(id, 0)` înaintea implementării verificării tokenului.
   Filtrul versiunii clientului nu constituie autentificare.
2. `services/auth/auth_service.cpp`: HashJoinId folosește încă XOR-fold;
   nu este hash criptografic și trebuie înlocuit înainte de producție.
3. `client/intro/urban_intro.cpp`: BeginLoading sare necondiționat intro-ul.
4. `external_lobby_manager.cpp`: încărcarea locală CEF este încercată după intro,
   numai pentru sesiune SKR. Dacă intro-ul ar aștepta resurse CEF, apare o
   dependență circulară. Ordinea trebuie refăcută înainte de legarea progresului.
5. Corupția heap CEF investigată anterior nu este demonstrată ca remediată prin
   mutarea lobby-ului. Paginile CEF din joc folosesc în continuare randare proprie.
6. Lockfile-ul nu reflectă complet checkouturile locale și distribuția CEF reală.
   STATUS.md conține informații istorice care nu substituie auditul surselor.

## 3. Fluxul propus

Lobby → verificări locale → cont → manifest resurse → download/verificare →
ticket scurt pentru server → GTA suspendat → inițializare SKR/R5 → reluare GTA →
încărcare cu intro SKR → conexiune multiplayer → autorizare server-side →
resurse obligatorii pregătite → UI CEF pregătit → intrare în gameplay.

Autentificarea contului poate preceda downloadul, dar ticketul de intrare se
emite după descărcările lungi, pentru a nu expira în timpul pregătirii.
Verificarea permisiunii de a descărca un pachet privat este separată de
autorizarea conexiunii de joc.

Stările de mai sus sunt contract propus, nu callbackuri existente. Fiecare are
timeout, eroare explicabilă și proprietar. Stările de progres nu înseamnă
autorizare și un eveniment JS nu poate declara jucătorul autorizat.

## 4. Etapele și criteriile de acceptare

### P0 — Fixăm baza reproductibilă

- Păstrăm snapshotul pachetului și toate modificările locale existente;
  inventariem separat patchurile SKR, omp-cef și open.mp.
- Completăm lockfile pentru binar/server/SDK/network/CEF și submodule:
  commit, sursă, hash, arhitectură, toolchain, licențe și patchuri locale.
- Construim open.mp din checkoutul ales și înregistrăm ABI-ul real al
  serverului și componentei. Buildul serverului rămâne separat de x86 GTA.
- Adăugăm preset/config pentru external-lobby și un inventar al artefactelor;
  păstrăm posibilitatea de revenire la pachetul anterior.

Acceptare: alt checkout poate produce aceeași configurație funcțională cu
dependențe identificate; nicio intrare UNKNOWN necesară pachetului de test.
Identitatea biților necesită separat un build determinist și nu se presupune.

### P1 — Lansare și conectare stabile

- Validăm mai întâi GTA + R5 pe un server open.mp de test, cu UI minim.
- Separăm verificarea instalării, pornirea, injecția, IPC și inițializarea DLL.
- Facem timeouturile efective, inclusiv conexiunea/scrierea pipe; timeoutul
  declarat în CreateNamedPipe nu limitează automat un apel sincron blocant.
- Verificăm rezultatele threadurilor remote și recuperarea după eșec.
- Înlocuim redenumirile globale/necondiționate de DLL-uri cu un mecanism de
  compatibilitate verificat; orice modificare temporară are restaurare și
  recuperare după crash, fără suprascrierea fișierelor preexistente.
- Supravegherea procesului și operațiile lungi nu blochează firul UI.
- După ieșire/crash/eșec, launcherul revine într-o stare utilizabilă, conform
  politicii explicite a utilizatorului. Codul 42 este păstrat și testat.

Acceptare: minimum 10 cicluri start/connect/disconnect/restart pe hashuri
cunoscute, fără procese orfane, fișiere rămase redenumite sau blocări IPC.

### P2 — Autorizație reală înaintea gameplay-ului

- Eliminăm autorizarea implicită după filtrul de versiune.
- Legăm loginul launcherului de un ticket unic, scurt, asociat contului,
  serverului și cheii efemere a sesiunii.
- Folosim implementările criptografice verificate; eliminăm XOR și mockurile
  din traseul de producție. Cheile private ale backendului rămân pe backend.
- Asociem proof-ul cu challenge-ul și conexiunea concretă; un ticket consumat
  nu poate fi reutilizat pe altă conexiune. IP-ul sau nickname-ul nu sunt dovadă.
- Gate-ul ține sesiunea Pending fără privilegii până la verificare. Gamemode-ul
  verifică starea la spawn, bani, inventar, comenzi și fiecare acțiune protejată.
- Negociem versiunea/capabilitățile SKR prin mesaje versionate. Inventariem mai
  întâi RPC-urile existente din shared/protocol și evităm conflictele.

Acceptare: ticket valid acceptat o dată; lipsă/expirat/replay/alt server/altă
conexiune/date malformate respinse; indisponibilitatea backendului nu autorizează.
Un client care imită versiunea SKR nu primește acces doar din acel motiv.

### P3 — Resurse înainte de GTA

- Backendul/catalogul furnizează identitatea serverului și locația manifestului.
  Pentru IP:port introdus manual avem înregistrare/descoperire explicită; nu
  deducem automat un endpoint de încredere din adresa UDP.
- Manifestul semnat descrie versiune, căi relative, dimensiuni, SHA-256,
  dependențe și politica de compatibilitate. Schema și endpointurile vor fi
  proiectate separat; nu sunt API-uri existente declarate aici.
- Implementăm download HTTPS, progres în bytes, retry/cancel, verificare,
  staging și activare atomică a unei versiuni complete; recuperare după crash.
- Protejăm extragerea arhivelor și căile față de traversal, reparse points,
  depășirea dimensiunilor și scrierea în afara rădăcinii de resurse.
- Cache separat pe server și versiune. Nu amestecăm fișierele a două servere.
- Actualizările executabilelor SKR au autoritate de semnare separată de
  pachetele de conținut ale serverului. Un server nu poate înlocui arbitrar DLL-uri.
- Separăm distribuția de montarea în joc: UI HTML/CSS/imaginile sunt primul
  tip suportat. DFF/TXD și alte asseturi GTA necesită adaptor/streamer verificat;
  simpla descărcare nu le face automat disponibile în lumea jocului.

Acceptare: download întrerupt reluat/reîncercat sigur; hash/semnătură greșite și
cale nepermisă respinse; versiunea anterioară rămâne utilizabilă la eșec;
pachetul UI corect este selectat pentru serverul ales.

### P4 — Intro SKR și înlocuirea splash-ului

- Continuăm analiza din docs/splash-r5-investigation.md: identificăm exact
  ecranul observat și diferențiem splash GTA, încărcare și conectare SA-MP.
- Pentru înlocuirea directă verificăm callsite, ABI, bytes așteptați, ordinea
  față de inițializarea R5 și ceilalți hookuri. Modificăm numai desenarea.
- Reactivarea overlay-ului SKR existent este un pas separat, util pentru probă;
  nu o raportăm ca înlocuire nativă deja demonstrată.
- Intro-ul poate arăta încărcarea locală și fazele de conectare. Procentele
  reprezintă măsurători reale ale unei etape; nu inventăm progres global.
- Inițializăm runtime-ul necesar resurselor/CEF independent de finalizarea
  intro-ului. Intro-ul nu este proprietarul bootstrapului multiplayer sau CEF.
- Predarea către gameplay cere conexiune validă, autorizare, resurse obligatorii
  și pagina necesară pregătite. Pentru sesiuni fără intro server există traseu
  explicit. Timeouturile produc eroare/revenire, nu acces acordat implicit.

Acceptare: pornire rece și cu cache, rezoluții diferite, focus pierdut, Alt-Tab,
reset D3D și eșec resurse; niciun cadru nativ nedorit în secvența verificată,
fără a rupe încărcarea sau mesajele de eroare. Validare prin captură runtime.

### P5 — CEF stabil, apoi HUD și meniuri

- Alegem și fixăm o singură integrare CEF pentru joc, pornind din omp-cef local
  dacă reproduce și trece verificările; nu inițiem imediat alt motor CEF.
- Reproducem crashul heap și audităm ownershipul browserelor/texturilor,
  distrugerea, sincronizarea dintre UI/render, device loss și ordinea hookurilor.
- Un singur proprietar pentru lifecycle; browser create/show/hide/close sunt
  operații cu stări explicite. Fără inițializare CEF/thread/rețea în DllMain.
- Bridge cu allowlist de evenimente, scheme, tipuri, limite, versiuni și
  verificarea browserului/originii/main frame. Conținutul serverului nu primește
  API de filesystem, procese sau secrete ale launcherului.
- Prioritate input explicită: dialog > meniu > chat > gameplay; testăm focusul,
  mouse-ul, tastele apăsate și restaurarea inputului la închiderea browserului.
- Livrăm întâi o pagină minimă, apoi HUD, chat, ESC și inventar. Fiecare funcție
  de gameplay cere validare pe server; aspectul UI nu acordă permisiuni.

Acceptare: 30 minute de sesiune cu pagină minimă, apoi 2 ore cu UI complet,
repetarea deschiderii/închiderii, reconnect, Alt-Tab și reset D3D; fără crash,
input blocat sau creștere necontrolată a memoriei. Paginile neautorizate nu
pot apela bridge-ul privilegiat. Aceste durate sunt praguri propuse, nu teste rulate.

### P6 — Setări native și extensii de gameplay

- Snapshot de setări la lansare, transmis printr-un contract IPC versionat;
  negociere explicită a compatibilității launcher/DLL. Nu mărim structurile fără versiune.
- Rezoluție/mod afișare întâi, apoi audio, draw distance/efecte/AA și input.
  Fiecare are adaptor verificat pe hashul GTA, validare și citirea rezultatului.
- Pentru schimbări live ale afișării: confirmare și revenire pe timeout/eșec.
  La restart păstrăm ultima configurație validată, nu doar ultima solicitată.
- HUD/chat/radar au preferințe locale și eventual reguli impuse explicit de
  server; clientul aplică politica negociată fără a confunda UI cu autorizarea.
- Extensiile suplimentare se introduc individual, după o verificare a
  capabilităților motorului GTA/R5; sursele open.mp nu elimină limitele clientului.

Acceptare: UI distinge cerut/salvat/aplicat/respins; valori neacceptate au
fallback explicabil; setările persistă și nu corup configurația jocului.

### P7 — Pachet alpha și apoi producție

- Pachet alpha instalabil cu versiuni compatibile, licențe/notificări,
  diagnostic fără tokenuri, recuperare și canal separat de update.
- Matrice pe hash GTA + samp + CEF + build SKR + OS/GPU/driver.
- Fără certificare R2 până la profil și matrice complete pentru R2.
- Verificăm licențele tuturor componentelor reale; omp-cef local declară MIT,
  iar vechiul lock samp-cef declară GPL-3.0. Nu tratăm cele două proiecte ca
  aceeași dependență și nu deducem licența întregului pachet dintr-un singur fișier.
- Nu redistribuim GTA, surse nepublice sau binare SA-MP fără drepturi verificate.
  Instalarea poate valida fișierele furnizate legal de utilizator.
- Producția cere SDK SKRAC real și politica finală; Mock SKRAC rămâne numai dev.
  Semnarea/publicarea cu cheile proprietarului reprezintă o acțiune separată autorizată.

Acceptare alpha: traseul complet trece pe configurația documentată, inclusiv
eșecurile și recuperarea. Acceptare producție: auditul securității și licențelor,
SDK AC real, distribuție și operare validate. Hashurile/heartbeatul nu sunt atestare.

## 5. Primul increment executabil

P0 + P1 pe o singură configurație GTA/R5/open.mp, cu un server de test și fără
HUD CEF complex. Rezultatul trebuie să fie: lobby → Connect → GTA → server →
ieșire → lobby, cu diagnostic exact pentru fiecare eșec. Urmează P2, apoi
manifestul/downloadul P3 și o pagină CEF minimă pentru P5. Investigația P4 poate
continua după stabilizarea lansării; livrarea intro-ului cu așteptare după CEF
depinde de eliminarea dependenței circulare. HUD complet vine după stabilitatea CEF.

## 6. Surse și proveniență consultate pentru plan

- `server-build/open.mp` HEAD `3d20f3dc0e26917264cdc8585c65913e080a143c`,
  README și LICENSE.md citite cu git show. Există modificări locale; acesta este
  un candidat de bază, nu o declarație de build/release curat.
  https://github.com/openmultiplayer/open.mp/tree/3d20f3dc0e26917264cdc8585c65913e080a143c
- README upstream separă SDK-ul stabil de headerele interne netcode/core,
  considerate instabile. Folosim întâi SDK; orice dependență internă are motiv,
  pin și teste dedicate. Un fork de core se justifică numai pentru un blocaj demonstrat.
- `deps/omp-sdk` HEAD `08cc69a32d96c0eb8fe063d26c78259e54ca853f`,
  README și headere SDK/network citite la commit.
  https://github.com/openmultiplayer/open.mp-sdk/tree/08cc69a32d96c0eb8fe063d26c78259e54ca853f
- `deps/omp-network` HEAD `f223fbd90e414f6c35f1d20cf916314ea5dcff6f`,
  origin https://github.com/openmultiplayer/open.mp-network.git.
- `deps/omp-cef` HEAD `fb5097b866c7cf51db774f96b7402cb260f8bb16`, cu patchuri
  locale; app.cpp la commit confirmă coada browserelor în așteptarea resurselor.
  Origin https://github.com/aurora-mp/omp-cef.git. Exporturile skr_cef_* sunt
  extensii locale și trebuie versionate/documentate ca atare.
- SAMP-API HEAD `6d4db99ab41f19d1a6a7c6cd48f5878bd1e14b62` și analiza locală
  RE, conform docs/external-launcher.md și docs/splash-r5-investigation.md.
- https://github.com/openmultiplayer/open.mp — sursa oficială și licența MPL-2.0.
- https://github.com/openmultiplayer/open.mp-sdk/blob/master/LICENSE.md —
  referință oficială a licenței SDK; verificarea de distribuție folosește pinul ales.

Validarea acestui increment: inspecție surse, hashuri ale pachetului, verificare
de formatare. Nu s-au compilat sau testat funcțiile propuse prin crearea planului.
