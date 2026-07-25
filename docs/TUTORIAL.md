# Tutorial: macemu installeren en gebruiken

Deze handleiding gaat ervan uit dat je niets hebt geïnstalleerd behalve macOS.
Alles werkt ook op Linux; waar het verschilt staat het erbij.

---

## 1. Wat je nodig hebt

| Onderdeel | Waarvoor | Verplicht? |
|---|---|---|
| Xcode command line tools | C++-compiler (`clang++`) | ja |
| CMake | bouwsysteem | ja |
| SDL2 | een écht venster op je scherm | nee (zonder SDL2 werkt alles headless) |
| Python 3 | test-`.exe`'s genereren | nee (ze staan al in de repo) |

Op macOS zit Python 3 er al op. Xcode CLT en CMake installeert het script zo nodig
voor je.

---

## 2. Installeren — de snelle manier

```bash
git clone <deze repo>
cd macemu
./install.sh
```

Dat is alles. Het script:

1. kijkt of je op Apple Silicon of Intel zit,
2. installeert `cmake` en `sdl2` via Homebrew als ze ontbreken,
3. bouwt `macemu` en `macemu-peinfo`,
4. draait de volledige testsuite,
5. draait een echte `.exe` als rooktest.

Wil je de programma's overal kunnen aanroepen:

```bash
./install.sh --install     # kopieert naar /usr/local/bin (vraagt om sudo)
```

Geen SDL2 en dat ook zo houden:

```bash
./install.sh --no-sdl
```

### Heb je geen Homebrew?

Installeer het eenmalig:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Daarna opnieuw `./install.sh`.

### Handmatig bouwen (als je liever zelf de touwtjes hebt)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
cd build && ctest --output-on-failure
```

### Linux

```bash
sudo apt install build-essential cmake libsdl2-dev python3
./install.sh
```

---

## 3. Werkt het?

```bash
./build/macemu fixtures/hello.exe
```

Verwachte uitvoer:

```
macemu draait een echte PE-executable.
Deze tekst komt uit geemuleerde x86-64 code die WriteFile aanroept.
```

Die tekst komt niet uit C++ — hij komt uit geëmuleerde x86-64 machinecode die
`GetStdHandle` en `WriteFile` aanroept, precies zoals een echte Windows-applicatie.

En de grafische variant:

```bash
./build/macemu fixtures/window.exe
```

Met SDL2 opent er een venster. Zonder SDL2 (of met `--headless`):

```bash
./build/macemu fixtures/window.exe --headless --screenshot venster.ppm
open venster.ppm        # macOS opent .ppm met Voorvertoning
```

---

## 4. Je eigen `.exe` proberen

### Stap 1 — Fase 0: is het haalbaar? (nooit overslaan)

```bash
./build/macemu-peinfo /pad/naar/programma.exe
```

Je krijgt een rapport met de PE-header, de secties, alle DLL-afhankelijkheden en
een oordeel:

| Oordeel | Exit-code | Betekenis |
|---|---|---|
| `HAALBAAR` | 0 | kleine, bekende dependency-set — goed startpunt |
| `HAALBAAR MET WERK` | 1 | te doen, maar reken op weken tot maanden API-werk |
| `GROTE SPRONG` | 2 | er ontbreken hele subsystemen (widgets, sockets, GDI+) |
| `GEBLOKKEERD` | 3 | er is een complete externe runtime nodig — doodlopend pad |

**Krijg je `GEBLOKKEERD`, kies dan een ander doelbestand.** Doorbouwen heeft geen
zin; het rapport vertelt precies waarom (`.NET`, `UWP/WinRT`, `DirectX`).

Voor Minesweeper concreet: de versie uit de Microsoft Store is een UWP/XAML-app en
valt onder `GEBLOKKEERD`. De klassieke `winmine.exe` uit Windows XP of de
`MineSweeper.exe` uit Vista/7 zijn puur Win32 en dus wél een realistisch doel.

Alle geïmporteerde functies zien:

```bash
./build/macemu-peinfo programma.exe --verbose
```

### Stap 2 — Fase 2: laadt het image?

```bash
./build/macemu /pad/naar/programma.exe --dry-run
```

Dit laadt de secties, past relocations toe, koppelt de imports en stopt daarna.
Je krijgt de geheugenkaart plus een lijst met imports die macemu nog niet kent —
dat is je werklijst voor Fase 3.

### Stap 3 — draaien

```bash
./build/macemu /pad/naar/programma.exe
```

Stopt het programma met *"niet-geïmplementeerde Win32-functie: kernel32.dll!Foo"*,
dan is dat geen bug maar de werkwijze: je implementeert `Foo` en probeert opnieuw.
Zie [fase3-win32.md](fase3-win32.md) — dat is een kwestie van vijf regels code.

Snel willen weten hoe ver je komt zonder elke functie te schrijven:

```bash
./build/macemu programma.exe --stub-unknown
```

Onbekende functies geven dan `0` terug. Dat werkt lang niet altijd, maar het laat
wel zien welke functie écht belangrijk is.

---

## 5. Alle opties

```
macemu <programma.exe> [opties] [-- args voor de gast]

  --dry-run           laden + imports resolven, niet uitvoeren
  --trace             log elke uitgevoerde instructie (disassembly + registers)
  --trace-limit N     stop met loggen na N instructies
  --max-instr N       breek af na N instructies (tegen oneindige lussen)
  --headless          geen venster openen
  --headless-secs S   hoe lang een headless GUI-app draait (standaard 1.5)
  --screenshot P      schrijf het venster weg als .ppm
  --stub-unknown      onbekende imports geven 0 terug i.p.v. te stoppen
  --force             negeer het Fase 0-oordeel GEBLOKKEERD
  --quiet / -v / -vv  minder of meer logging
```

```
macemu-peinfo <bestand.exe> [-v] [-e]
  -v   alle geïmporteerde symbolen tonen
  -e   de export-tabel tonen
```

---

## 6. Debuggen als er iets misgaat

Bij een crash print macemu:

- de reden (ontbrekende instructie, geheugenfout, ontbrekende API),
- alle registers en flags,
- de bovenste stackwaarden,
- de bytes op `RIP` (zodat je precies ziet welke instructie mist),
- de complete geheugenkaart.

Wil je zien wat eraan voorafging:

```bash
./build/macemu programma.exe --trace --trace-limit 200 2>&1 | tail -60
```

Elke regel is één instructie met adres, bytes, disassembly en de belangrijkste
registers.

**Ontbrekende instructie?** De foutmelding geeft de opcode. Voeg een `case` toe in
`src/cpu/exec.cpp` (of `exec_sse.cpp` voor SSE) — zie [fase1-cpu.md](fase1-cpu.md).

**Ontbrekende API?** Voeg een regel toe in `src/win32/` — zie
[fase3-win32.md](fase3-win32.md).

---

## 7. Zelf uitbreiden — de werkwijze

De hele methode in vijf stappen, en dit is niet vrijblijvend bedoeld:

1. Draai `macemu-peinfo` en accepteer het oordeel.
2. Draai het programma. Het stopt ergens.
3. Lees de foutmelding: ontbrekende instructie of ontbrekende API?
4. Implementeer precies dat ene ding.
5. Ga terug naar stap 2.

Nooit vooraf raden welke functies nodig zijn. De executable vertelt het je.

Tests draaien na elke wijziging:

```bash
cd build && ctest --output-on-failure
```

Nieuwe test-`.exe`'s maken zonder Windows-compiler:

```bash
python3 tools/mkpe.py fixtures/
```

`tools/mkpe.py` bevat een kleine x86-64 assembler in Python; je kunt er je eigen
testprogramma's mee schrijven om een specifieke instructie of API te isoleren.

---

## 8. Veelgestelde vragen

**Waarom draait x86-64 code niet gewoon native op mijn Intel-Mac?**
Omdat macemu bewust altijd emuleert. Dat houdt het gedrag identiek op Apple
Silicon en Intel, en het is het punt van het project: je wilt de CPU begrijpen,
niet omzeilen.

**Waarom is het traag?**
Het is een interpreter: elke geëmuleerde instructie kost tientallen host-
instructies. Reken op ~10–50 miljoen instructies per seconde. Een JIT is de
logische volgende stap, maar pas als de correctheid staat.

**Kan ik een echt spel draaien?**
Een Win32-spel uit de jaren '90 met GDI: op termijn realistisch. Alles met
DirectX, .NET of UWP: nee, en `macemu-peinfo` zegt dat ook meteen.

**Wat is een `.ppm` en waarom geen PNG?**
Het simpelste onbewerkte afbeeldingsformaat dat bestaat — twintig regels code en
geen enkele dependency. macOS Voorvertoning opent het gewoon. Converteren kan met
`sips -s format png venster.ppm --out venster.png`.
