// minesweeper.c - klein Win32-Minesweeper voor macemu
//
// Compileren op macOS (met Homebrew mingw-w64):
//   brew install mingw-w64
//   x86_64-w64-mingw32-gcc -O2 -mwindows -o fixtures/minesweeper.exe fixtures/minesweeper.c
//
// Draaien:
//   ./build/macemu fixtures/minesweeper.exe
//
// Bewust simpel gehouden: vierkant grid, linksklik onthult, rechtsklik
// vlagt, flood-fill voor lege vakjes, R = reset.
//
// UNICODE staat aan omdat macemu's Win32-laag de W-varianten van
// DefWindowProc/GetMessage/DispatchMessage implementeert (zie
// src/win32/user32.cpp) en de ANSI-varianten daarvan niet.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdlib.h>
#include <time.h>

#define COLS 9
#define ROWS 9
#define MINES 10
#define CELL 24
#define TOPBAR 40

typedef struct {
    int mine;
    int revealed;
    int flagged;
    int count;
} Cell;

static Cell g_board[ROWS][COLS];
static int g_gameOver = 0;
static int g_win = 0;
static int g_firstClick = 1;

static void placeMines(int avoidX, int avoidY) {
    int placed = 0;
    while (placed < MINES) {
        int x = rand() % COLS;
        int y = rand() % ROWS;
        if (g_board[y][x].mine) continue;
        if (x == avoidX && y == avoidY) continue;
        g_board[y][x].mine = 1;
        placed++;
    }
    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            if (g_board[y][x].mine) continue;
            int c = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) continue;
                    if (g_board[ny][nx].mine) c++;
                }
            g_board[y][x].count = c;
        }
    }
}

static void resetGame(void) {
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            g_board[y][x].mine = 0;
            g_board[y][x].revealed = 0;
            g_board[y][x].flagged = 0;
            g_board[y][x].count = 0;
        }
    g_gameOver = 0;
    g_win = 0;
    g_firstClick = 1;
}

static void reveal(int x, int y) {
    if (x < 0 || y < 0 || x >= COLS || y >= ROWS) return;
    if (g_board[y][x].revealed || g_board[y][x].flagged) return;
    g_board[y][x].revealed = 1;
    if (g_board[y][x].mine) {
        g_gameOver = 1;
        return;
    }
    if (g_board[y][x].count == 0) {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (dx || dy) reveal(x + dx, y + dy);
    }
}

static void checkWin(void) {
    int hidden = 0;
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            if (!g_board[y][x].revealed && !g_board[y][x].mine) hidden++;
    if (hidden == 0) g_win = 1;
}

static COLORREF numberColor(int n) {
    switch (n) {
        case 1: return RGB(0, 0, 255);
        case 2: return RGB(0, 128, 0);
        case 3: return RGB(255, 0, 0);
        case 4: return RGB(0, 0, 128);
        default: return RGB(64, 0, 0);
    }
}

static void paintBoard(HDC hdc) {
    HBRUSH hidden = CreateSolidBrush(RGB(192, 192, 192));
    HBRUSH shown = CreateSolidBrush(RGB(224, 224, 224));
    HBRUSH mineBrush = CreateSolidBrush(RGB(255, 0, 0));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(96, 96, 96));
    HPEN oldPen = SelectObject(hdc, border);
    SetBkMode(hdc, TRANSPARENT);

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            RECT r;
            r.left = x * CELL;
            r.top = TOPBAR + y * CELL;
            r.right = r.left + CELL;
            r.bottom = r.top + CELL;
            Cell* c = &g_board[y][x];

            HBRUSH b = c->revealed ? shown : hidden;
            HGDIOBJ oldBrush = SelectObject(hdc, b);
            Rectangle(hdc, r.left, r.top, r.right, r.bottom);
            SelectObject(hdc, oldBrush);

            if (c->revealed && c->mine) {
                SelectObject(hdc, mineBrush);
                Ellipse(hdc, r.left + 5, r.top + 5, r.right - 5, r.bottom - 5);
            } else if (c->revealed && c->count > 0) {
                char buf[2] = { (char)('0' + c->count), 0 };
                SetTextColor(hdc, numberColor(c->count));
                TextOutA(hdc, r.left + 8, r.top + 4, buf, 1);
            } else if (!c->revealed && c->flagged) {
                SetTextColor(hdc, RGB(200, 0, 0));
                TextOutA(hdc, r.left + 7, r.top + 4, "F", 1);
            }
        }
    }
    SelectObject(hdc, oldPen);
    DeleteObject(hidden);
    DeleteObject(shown);
    DeleteObject(mineBrush);
    DeleteObject(border);

    SetTextColor(hdc, RGB(0, 0, 0));
    if (g_gameOver) TextOutA(hdc, 8, 10, "Geraakt! Klik op Reset.", 23);
    else if (g_win) TextOutA(hdc, 8, 10, "Gewonnen!", 9);
    else TextOutA(hdc, 8, 10, "macemu Minesweeper - R = reset", 30);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paintBoard(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == 'R') {
                resetGame();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (g_gameOver || g_win) return 0;
            int x = LOWORD(lParam) / CELL;
            int y = (HIWORD(lParam) - TOPBAR) / CELL;
            if (y < 0 || y >= ROWS || x < 0 || x >= COLS) return 0;
            if (g_firstClick) {
                placeMines(x, y);
                g_firstClick = 0;
            }
            reveal(x, y);
            checkWin();
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (g_gameOver || g_win) return 0;
            int x = LOWORD(lParam) / CELL;
            int y = (HIWORD(lParam) - TOPBAR) / CELL;
            if (y < 0 || y >= ROWS || x < 0 || x >= COLS) return 0;
            if (!g_board[y][x].revealed) g_board[y][x].flagged = !g_board[y][x].flagged;
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR cmdLine, int nCmdShow) {
    srand((unsigned)time(NULL));
    resetGame();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
    wc.lpszClassName = L"MacemuMinesweeper";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"MacemuMinesweeper", L"macemu - Minesweeper",
                                WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                                100, 100, COLS * CELL + 16, ROWS * CELL + TOPBAR + 39,
                                NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
