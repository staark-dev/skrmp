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
