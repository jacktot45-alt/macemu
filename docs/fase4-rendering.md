# Fase 4 — Vensters en rendering

Bestanden: `src/win32/user32.cpp`, `src/win32/gdi32.cpp`, `src/host/framebuffer.cpp`,
`src/host/window.cpp`.

> Dit is de fase waarin de complexiteit een sprong maakt. Tot en met Fase 3 is de
> gast passief: hij roept ons aan. Vanaf Fase 4 roepen wij hem aan — midden in een
> host-functie moet de CPU opnieuw starten om gastcode uit te voeren. Dat vraagt
> `Cpu::callGuest`, en dat moet herintreedbaar zijn.

## De omgekeerde aanroep

```
gast:  DispatchMessageW(&msg)
  → host: user32!DispatchMessageW
      → leest MSG uit gastgeheugen
      → Cpu::callGuest(wndProc, {hwnd, msg, wParam, lParam})
          → gast: WndProc(...)          ← weer geëmuleerde x86-64 code
              → host: user32!BeginPaint
              → host: gdi32!TextOutA
              → host: user32!EndPaint
          → ret naar het sentinel-adres
      → registers terug, RAX = resultaat van WndProc
```

`callGuest` bewaart alle registers, zet de argumenten neer volgens Win64, pusht een
sentinel-returnadres in het HLE-gebied en draait tot RIP daar aankomt. Het geheugen
wordt bewust niet teruggedraaid.

Dit werkt tot willekeurige diepte: een WndProc die `SendMessage` doet naar een ander
venster nest gewoon nog een `callGuest`.

## De drie lagen

```
gdi32!TextOutA ──▶ Framebuffer::drawText ──▶ HostWindow::present ──▶ SDL2 / null
   (Win32-API)        (32-bit ARGB)            (abstractie)         (macOS-venster)
```

De Win32-laag kent SDL niet en SDL kent Win32 niet. Alleen `HostWindow` zit
ertussen, en die heeft twee implementaties:

- **SDL2** — echt venster op macOS (Metal/OpenGL onder water), muis en toetsenbord.
- **null** — headless: tekent gewoon door in het geheugen, kan het resultaat als
  `.ppm` wegschrijven. Dit is wat de tests en CI gebruiken.

Zonder SDL2 gebouwd? Dan werkt alles nog, alleen zie je het niet live. Dat is een
bewuste keuze: de tekenlaag hoort testbaar te zijn zonder scherm.

## Vensters en klassen

`RegisterClass(Ex)A/W` leest de `WNDCLASS(EX)` uit gastgeheugen. Op x64 zijn
`WNDCLASSW` en `WNDCLASSEXW` identiek vanaf offset 8 — alleen het `style`-veld
schuift op omdat de EX-variant met `cbSize` begint.

`CreateWindowExW` maakt een `WindowObject` met een eigen `Framebuffer` voor de
clientarea, en stuurt daarna `WM_NCCREATE` en `WM_CREATE` naar de WndProc — met een
`CREATESTRUCT` in gastgeheugen, want daar haalt de app zijn `lpCreateParams` uit.

De non-client area (titelbalk, rand) wordt niet getekend; de clientarea is
`buitenmaat - 16` breed en `- 39` hoog, wat overeenkomt met een standaard
`WS_OVERLAPPEDWINDOW`.

## De message loop

`GetMessageW` is het hart:

```
herhaal:
    host-events ophalen (SDL → WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_KEYDOWN, ...)
    timers laten aflopen  (→ WM_TIMER)
    vensters die hertekend moeten worden → WM_PAINT
    is er een bericht?  → naar de MSG-struct in gastgeheugen, klaar
    is er quit gevraagd? → WM_QUIT, geef 0 terug
    frame presenteren, 4 ms slapen
```

In headless modus loopt die lus niet oneindig door: na `--headless-secs` (standaard
1,5 s) wordt er `WM_CLOSE` gestuurd. Zonder dat zou een GUI-app in CI voor altijd
blijven hangen.

`PeekMessage` doet hetzelfde maar keert meteen terug — spelletjes gebruiken dat voor
een render-lus in plaats van te blokkeren.

## GDI

Een `HDC` is een `DeviceContext`: een pointer naar een `Framebuffer` plus de huidige
brush, pen, font, tekstkleur, achtergrondkleur en tekenpositie. `BeginPaint` maakt er
een die naar de clientarea van het venster wijst.

Geïmplementeerd: `GetStockObject`, `CreateSolidBrush`, `CreatePen`, `CreateFont*`,
`SelectObject`, `SetTextColor`, `SetBkColor`, `SetBkMode`, `SetPixel`, `GetPixel`,
`Rectangle`, `MoveToEx`, `LineTo`, `TextOutA/W`, `ExtTextOut*`, `FillRect` (user32),
`GetTextExtentPoint32*`, `GetDeviceCaps`, `CreateCompatibleDC/Bitmap`, `DeleteDC`.

**De klassieke valkuil:** Win32 `COLORREF` is `0x00BBGGRR`, onze framebuffer is
`0x00RRGGBB`. Rood en blauw omgedraaid is verreweg de meest gemaakte fout in deze
laag. Er staan daarom twee functies `fromColorRef`/`toColorRef` in `gdi32.cpp` en die
zijn de enige plek waar de conversie gebeurt.

Tekst gaat via een ingebouwd 8×8-bitmapfont (ASCII 32–95; kleine letters worden op
hoofdletters afgebeeld). Geen fontbestanden, geen dependencies. Het `height`-veld van
een geselecteerd font bepaalt de schaalfactor 1×, 2× of 3×.

## Invoer

SDL-events worden vertaald naar Windows-berichten: muisbewegingen en -klikken naar
`WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_RBUTTONDOWN` met de coördinaten in `lParam`
(x in de lage 16 bits, y in de hoge), toetsen naar `WM_KEYDOWN`/`WM_KEYUP` met een
Windows virtual-key code, en tekstinvoer naar `WM_CHAR`.

## Wat er niet is

| Ontbreekt | Gevolg |
|---|---|
| Non-client rendering | geen titelbalk of rand in de framebuffer |
| Child windows / controls | knoppen en tekstvakken worden niet getekend |
| comctl32 | apps met common controls hebben hier niets aan |
| Bitmaps en `BitBlt` | `BitBlt` is een no-op; double buffering werkt dus niet |
| Regions en clipping | tekenen loopt alleen tegen de framebuffergrens aan |
| Echte fonts | alleen het ingebouwde 8×8-font |
| Menu's | `SetMenu` doet niets |

`BitBlt` is waarschijnlijk het eerste dat je nodig hebt bij een echte applicatie:
veel Win32-programma's tekenen in een geheugen-DC en blitten die naar het scherm.
Dat betekent `CreateCompatibleBitmap` een echte `Framebuffer` geven en `BitBlt` die
laten kopiëren — een overzichtelijke uitbreiding.

## Zelf zien

```bash
./build/macemu fixtures/window.exe                                   # met SDL2
./build/macemu fixtures/window.exe --headless --screenshot uit.ppm   # zonder
sips -s format png uit.ppm --out uit.png                             # macOS
```

De demo doorloopt de hele keten: `RegisterClassExW` → `CreateWindowExW` →
`ShowWindow` → message loop → `WM_PAINT` → WndProc → `BeginPaint`, `FillRect`,
`SetTextColor`, `TextOutA`, `EndPaint` → framebuffer → scherm. Alle gastcode in die
keten is met de hand geassembleerde x86-64 (`tools/mkpe.py`).
