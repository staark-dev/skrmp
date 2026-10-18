# Continuare lobby — 18 octombrie 2026

## Ce a fost recuperat

Au fost păstrate modificările locale din repository-ul principal și din
repository-ul separat `deps/omp-cef`. `skr_local_api.cpp`, `.h` și
`std_rotate_stub.cpp` erau untracked în repository-ul dependenței.
Nu s-au creat commituri și nu s-a făcut deployment în instalarea GTA.

Snapshot-ul complet al modificărilor locale omp-cef este păstrat în
`patches/omp-cef-skr-local.patch`, inclusiv fișierele untracked. Acest lucru
este necesar deoarece `deps/` este ignorat de repository-ul principal.
Patch-ul poate fi aplicat din rădăcina unui checkout omp-cef curat la baza
indicată mai jos cu `git apply <cale-absolută-spre-patch>`.
Nu îl aplicați din nou peste checkout-ul local deja modificat.

`patches/pawn-stdlib-skr.patch` adaugă în `deps/pawn-stdlib/omp_core.inc`
callback-ul standard `forward OnGameModeIntro();`, apelat de componenta SKR Gate
pentru RPC 241 (`shared/protocol/session_info.h`). Se aplică la fel, din
rădăcina checkout-ului `deps/pawn-stdlib`.

## Proveniență și limite

- omp-cef: origin `https://github.com/aurora-mp/omp-cef.git`, baza
  `e41ead35c52c0245f40670e36d2d6648ce04c222`.
- Sursele originale `browser/client.cpp` de la această bază au fost
  consultate prin `git show HEAD:src/client/core/browser/client.cpp`.
- `skr_local_api.*` reprezintă extensie locală SKR, nu API upstream samp-cef.
- Semnăturile CEF au fost verificate în headerele distribuției locale
  `148.0.10+g7ee53f5+chromium-148.0.7778.218_windows32`.
- Lockfile-ul principal declară încă samp-cef și CEF 150. Nu descrie complet
  această integrare omp-cef/CEF 148. Buildurile locale nu sunt un release
  reproductibil și nu certifică R2 sau alte hashuri GTA/SA-MP.

## Corecții efectuate

- Formularul serverelor personale are submit, validare, anulare, Escape,
  focus limitat la dialog, confirmări native și erori. Eliminarea este
  anunțată după primirea listei native actualizate.
- Filtrul separat „Personale” a fost eliminat; serverele adăugate apar direct
  în lista „Toate”, cu eticheta Personal și acțiunea de eliminare.
- Statisticile necunoscute ale serverelor personale nu mai arată valori
  inventate. Catalogul public neconfigurat răspunde cu listă goală.
- Setările acceptate sunt rezoluție, mod fullscreen/fereastră și preferința
  volum SKR. Payload-ul este plat și validat; nu acceptă schimbarea GTA path.
- Nickname-ul are handler nativ separat și validare 3–24 caractere ASCII
  alfanumerice/underscore. Pagina încarcă nickname-ul salvat.
- INI-ul este înlocuit prin fișier temporar după scriere reușită. Sunt
  păstrate secțiunile servers, updates, ui și preferințele necunoscute.
  Se folosesc cheile player/game citite de launcherul WPF din worktree.
- Interfața anunță succesul salvării numai după confirmarea nativă.
  Eșecurile și timeout-urile păstrează modificările neaplicate.
- Controalele fără adaptor implementat sunt dezactivate. Preferințele de
  afișare se aplică la următoarea pornire; volumul este doar persistat,
  integrarea audio live nu este finalizată.
- Evenimentele C++ către JS sunt executate pe threadul CEF UI și trimise
  doar către pagina lobby înregistrată. Evenimentele locale către C++
  verifică ID-ul browserului, URL-ul și main frame-ul, cu payload maxim 4096
  bytes; nu sunt retransmise către server. Acesta nu este un mecanism de
  autorizare a gameplay-ului sau de atestare a clientului.
- Loaderul folosește LoadLibraryExW cu căutare în directorul DLL și
  directoarele implicite, fără modificarea globală SetDllDirectoryW.
- Resursele web sunt copiate la fiecare build SKRClient, inclusiv când
  schimbările sunt exclusiv în HTML/CSS/JS.

## Validare

- Build Win32 Release SKRClient: succes.
- Build Win32 Release omp-cef target Client: succes.
- SKRLobbyTests: 41 verificări, zero eșecuri. Acoperă parsarea payload-urilor,
  respingerea datelor invalide, păstrarea identității și a secțiunilor INI,
  idempotenta merge-ului, nickname-uri și adrese server.
- node --check client/lobby/lobby.js: succes.
- git diff --check pentru ambele repository-uri: succes.
- git apply --reverse --check pentru snapshot-ul omp-cef: succes.
- Nu s-a pornit GTA în această sesiune. Logurile istorice confirmă afișarea
  lobby-ului pentru o versiune anterioară, nu validează buildurile noi.
- Nu s-au testat salvarea efectivă în profilul utilizatorului, lifecycle-ul
  CEF, restricțiile de origine sau reseturile D3D9 într-un runtime real.

## Rămase

Pregătirea conectării este limitată exclusiv la samp.dll 0.3.7-R5 cu
SHA-256 `B72B5DBE725F81864CA3F78BC7063BDA56CC05FC7188AF822FA7A754432553A2`.
Clientul validează hashul și semnătura PE, completează blocul Settings verificat
cu host, port și nickname după apăsarea Connect. Încărcarea târzie prin
`LoadLibrary` nu execută însă bootstrapul complet R5 când GTA a pornit fără
parametrii SA-MP, deci `CNetGame` nu este creat. Experimentele cu apelarea
directă a `Samp_Init` și cu sărirea temporară a construcției `CNetGame` au fost
eliminate: primul a intrat în `SCM_CallOpcode` fără starea prealabilă necesară,
iar al doilea a lăsat GTA să intre în lume. Activarea târzie a bootstrapului
nativ complet rămâne de implementat și de validat. Orice abatere de hash sau
semnătură PE este respinsă.
După Connect, meniul GTA rămâne suprimat, iar browserul lobby rămâne vizibil
și păstrează inputul cât timp sesiunea este în curs. `CNetGame::Connect` este
chemat o singură dată când obiectul devine disponibil. Browserul este ascuns
și inputul jocului este restaurat numai după ce starea nativă `CNetGame` devine
`CONNECTED` (valoarea 5 din SDK-ul R5 verificat). După 30 de secunde fără
confirmare, lobby-ul rămâne vizibil, afișează timeout-ul și continuă să
urmărească starea nativă, deoarece încărcarea DLL-ului nu poate fi anulată.

Proveniența adreselor și structurii: `deps/omp-cef/deps/samp-api` pentru
0.3.7-R5-1, detectorul PE din omp-cef și analiza statică locală
`D:/SA-MP Reverse/RE/client/research/docs/initialization_flow.md`.

Resume, disconnect, Escape în joc, favoriți persistenți, catalog public real,
setările native de gameplay și audio live rămân neimplementate.

Validarea în GTA trebuie să includă salvare și reîncărcare nickname/setări,
adăugare/eliminare server, erori la scriere, navigare cu tastatura și
verificarea faptului că paginile remote nu pot invoca evenimentele locale.
