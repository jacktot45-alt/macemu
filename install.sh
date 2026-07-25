#!/usr/bin/env bash
#
# macemu - installatiescript voor macOS (Apple Silicon en Intel) en Linux.
#
#   ./install.sh              bouwen en testen
#   ./install.sh --install    daarna ook naar /usr/local/bin kopiëren
#   ./install.sh --no-sdl     zonder SDL2 bouwen (alleen headless)
#
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
step()  { echo -e "${BLUE}==>${NC} $*"; }
ok()    { echo -e "${GREEN} ok${NC} $*"; }
warn()  { echo -e "${YELLOW} !!${NC} $*"; }
fail()  { echo -e "${RED}fout${NC} $*"; exit 1; }

DO_INSTALL=0
USE_SDL=ON
for arg in "$@"; do
  case "$arg" in
    --install) DO_INSTALL=1 ;;
    --no-sdl)  USE_SDL=OFF ;;
    -h|--help)
      sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) fail "onbekende optie: $arg" ;;
  esac
done

cd "$(dirname "$0")"

# ---------------------------------------------------------------------------
step "Systeem controleren"
OS="$(uname -s)"
ARCH="$(uname -m)"
echo "    besturingssysteem : $OS"
echo "    architectuur      : $ARCH"
if [ "$OS" = "Darwin" ] && [ "$ARCH" = "arm64" ]; then
  echo "    -> Apple Silicon: x86-64 wordt volledig geëmuleerd (zoals bedoeld)."
fi

# ---------------------------------------------------------------------------
step "Bouwgereedschap controleren"
if ! command -v cmake >/dev/null 2>&1; then
  if [ "$OS" = "Darwin" ]; then
    warn "cmake ontbreekt."
    if command -v brew >/dev/null 2>&1; then
      step "cmake installeren via Homebrew"
      brew install cmake
    else
      fail "Installeer eerst Homebrew (https://brew.sh) en daarna: brew install cmake"
    fi
  else
    fail "Installeer cmake (bijv. sudo apt install cmake build-essential)"
  fi
fi
ok "cmake $(cmake --version | head -1 | awk '{print $3}')"

if [ "$OS" = "Darwin" ] && ! xcode-select -p >/dev/null 2>&1; then
  warn "Xcode command line tools ontbreken - ze worden nu geïnstalleerd."
  xcode-select --install || true
  fail "Start install.sh opnieuw zodra de installatie klaar is."
fi

if command -v c++ >/dev/null 2>&1; then
  ok "compiler: $(c++ --version | head -1)"
else
  fail "geen C++-compiler gevonden"
fi

# ---------------------------------------------------------------------------
if [ "$USE_SDL" = "ON" ]; then
  step "SDL2 controleren (nodig voor een echt venster)"
  HAVE_SDL=0
  if pkg-config --exists sdl2 2>/dev/null; then HAVE_SDL=1; fi
  if [ -d /opt/homebrew/include/SDL2 ] || [ -d /usr/local/include/SDL2 ]; then HAVE_SDL=1; fi
  if [ "$HAVE_SDL" = "0" ]; then
    if [ "$OS" = "Darwin" ] && command -v brew >/dev/null 2>&1; then
      step "SDL2 installeren via Homebrew"
      brew install sdl2 || warn "SDL2 installeren mislukte - er wordt headless gebouwd"
    else
      warn "SDL2 niet gevonden. macemu bouwt door, maar opent geen venster."
      warn "  macOS : brew install sdl2"
      warn "  Ubuntu: sudo apt install libsdl2-dev"
    fi
  else
    ok "SDL2 gevonden"
  fi
fi

# ---------------------------------------------------------------------------
step "Bouwen"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMACEMU_USE_SDL2="$USE_SDL" >/dev/null
cmake --build build -j"$( (command -v sysctl >/dev/null && sysctl -n hw.ncpu) || nproc || echo 4)"
ok "build/macemu en build/macemu-peinfo gebouwd"

# ---------------------------------------------------------------------------
step "Tests draaien"
if (cd build && ctest --output-on-failure >/dev/null 2>&1); then
  ok "alle tests geslaagd"
else
  (cd build && ctest --output-on-failure) || fail "tests mislukt"
fi

# ---------------------------------------------------------------------------
step "Snelle rooktest: een echte .exe draaien"
./build/macemu fixtures/hello.exe --quiet --no-analysis
ok "de emulator draait geëmuleerde x86-64 code"

if [ "$DO_INSTALL" = "1" ]; then
  step "Installeren in /usr/local/bin (kan om je wachtwoord vragen)"
  sudo cmake --install build --prefix /usr/local
  ok "macemu en macemu-peinfo staan nu in je PATH"
fi

cat <<EOF

$(echo -e "${GREEN}Klaar.${NC}")

Volgende stappen:
  ./build/macemu-peinfo <jouw.exe>      Fase 0: is dit haalbaar?
  ./build/macemu <jouw.exe> --dry-run   Fase 2: laadt het image?
  ./build/macemu <jouw.exe> -v          draaien met logging

Lees docs/TUTORIAL.md voor de uitgebreide handleiding.
EOF
