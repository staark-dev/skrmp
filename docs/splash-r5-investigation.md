# Splash R5 — verificare statică locală, 19 octombrie 2026

Stare: constatări de analiză, nu implementare sau validare în joc.
Nu au fost modificate sau copiate binarele/sursele decompilate din directorul RE.

## Fișiere identificate

- `D:/SA-MP Reverse/samp.dll`, SHA-256
  `b72b5dbe725f81864ca3f78bc7063bda56cc05fc7188af822fa7a754432553a2`:
  coincide cu R5 acceptat de `client/samp-loader/samp_loader.cpp`.
- `D:/SA-MP Reverse/gta_sa.bak.exe`, SHA-256
  `a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26`:
  citit numai pentru verificarea apelului de mai jos. Nu certifică alte executabile.

## Confirmat

Inventarul `RE/client/data/hook_inventory.tsv` identifică adresa GTA
`0x590099` în `CLoadingScreen::RenderSplash`, la +0x139 față de `0x58FF60`.
Decompilarea `RE/client/reconstructed/decompiled/part_165_100a9650-100aaeb0.c`
arată că detectorul/patcherul GTA din SA-MP înlocuiește cinci bytes cu NOP.

Verificarea independentă a binarelor cu Python/Capstone a confirmat:

- samp.dll RVA `0xAA956`: referința la adresa GTA `0x590099`;
- samp.dll RVA `0xAA96B`: valoarea `0x90909090`, urmată de scrierea în
  acea adresă (RVA `0xAA977`); al cincilea NOP apare și în decompilare;
- în executabilul GTA analizat, adresa `0x590099` conține
  `E8 C2 7A 19 00`, un apel la `0x727B60`.

Acestea confirmă suprimarea unui apel din desenarea splash-ului GTA. Nu
identifică singure o textură SA-MP sau întregul ecran văzut de utilizator.
Nu trebuie înlocuit arbitrar acest punct cu un hook care presupune alt ABI.

`RE/client/research/docs/rendering_pipeline.md` descrie separat proxy-ul D3D9
SA-MP și interfața desenată din Present. Proiectul SKR are deja un hook de
prezentare specific R5 în `client/intro/urban_intro.cpp`, cu verificarea
instrucțiunii call și a destinației așteptate.

În același fișier, `BeginLoading()` marchează necondiționat intro-ul Done și
revine înainte de încărcarea imaginilor. Aceasta explică de ce intro-ul SKR nu
acoperă imaginea curentă. Reactivarea lui ar fi un overlay existent; nu trebuie
prezentată drept înlocuire directă a rutinei native de splash.

## Încă de verificat

Identificarea exactă a ecranului observat (splash GTA / încărcare / conectare),
traseul complet al resurselor sale, ABI-ul punctului de înlocuire și ordinea
față de hookurile omp-cef. Pentru o înlocuire directă trebuie păstrate apelurile
de încărcare, progresul real și lifecycle-ul D3D9; se schimbă numai desenarea.

Nu s-au pornit GTA/SA-MP, nu s-a modificat cod compilabil și nu s-a generat un
executabil nou în această investigație. Nu există rezultat runtime sau
certificare R2.
