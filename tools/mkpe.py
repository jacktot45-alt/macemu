#!/usr/bin/env python3
"""mkpe.py - bouwt kleine 64-bit Windows .exe-bestanden voor het testen van macemu.

Waarom dit bestaat: om Fase 2 t/m 4 te testen heb je een echte PE-executable
nodig. Zonder Windows-compiler (en zonder Wine) is de eenvoudigste weg: het
PE-formaat en de x86-64 machinecode met de hand genereren. Dat is meteen de
beste manier om te snappen wat de loader precies moet doen.

Gebruik:
    python3 tools/mkpe.py fixtures/

Genereert:
    hello.exe   - console-app: GetStdHandle + WriteFile + ExitProcess
    window.exe  - GUI-app: window class, message loop, WndProc die tekent
    crash.exe   - roept een niet-geïmplementeerde API aan (voor foutmeldingen)
"""

import os
import struct
import sys

# --------------------------------------------------------------------------
# Registers
# --------------------------------------------------------------------------
RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI = range(8)
R8, R9, R10, R11, R12, R13, R14, R15 = range(8, 16)


class Asm:
    """Minimale x86-64 assembler met labels en RIP-relatieve fixups."""

    def __init__(self, base_rva):
        self.buf = bytearray()
        self.base_rva = base_rva
        self.labels = {}
        # (positie_van_disp32, doelnaam, aantal_bytes_na_de_disp32)
        self.rip_fixups = []
        # (positie_van_rel32, labelnaam)
        self.rel_fixups = []

    # ---- infrastructuur ---------------------------------------------------
    def label(self, name):
        self.labels[name] = len(self.buf)

    def emit(self, *bs):
        for b in bs:
            self.buf.append(b & 0xFF)

    def emit_bytes(self, data):
        self.buf.extend(data)

    def imm32(self, v):
        self.buf.extend(struct.pack("<i", v if v < 0x80000000 else v - 0x100000000))

    def imm64(self, v):
        self.buf.extend(struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))

    def _rip_disp(self, target, tail=0):
        self.rip_fixups.append((len(self.buf), target, tail))
        self.buf.extend(b"\0\0\0\0")

    def _rel32(self, label):
        self.rel_fixups.append((len(self.buf), label))
        self.buf.extend(b"\0\0\0\0")

    def _rex(self, w, reg, rm_base, index=0):
        v = 0x40 | (0x08 if w else 0) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (rm_base >> 3)
        if v != 0x40:
            self.emit(v)

    def _modrm_rsp(self, reg, disp):
        """ModRM+SIB voor [rsp+disp]."""
        if -128 <= disp <= 127:
            self.emit(0x40 | ((reg & 7) << 3) | 4, 0x24, disp & 0xFF)
        else:
            self.emit(0x80 | ((reg & 7) << 3) | 4, 0x24)
            self.imm32(disp)

    # ---- instructies ------------------------------------------------------
    def mov_r64_imm64(self, r, v):
        self._rex(True, 0, r)
        self.emit(0xB8 + (r & 7))
        self.imm64(v)

    def mov_r32_imm32(self, r, v):
        self._rex(False, 0, r)
        self.emit(0xB8 + (r & 7))
        self.imm32(v)

    def mov_r64_r64(self, dst, src):
        self._rex(True, src, dst)
        self.emit(0x89, 0xC0 | ((src & 7) << 3) | (dst & 7))

    def xor_r32_r32(self, dst, src):
        self._rex(False, src, dst)
        self.emit(0x31, 0xC0 | ((src & 7) << 3) | (dst & 7))

    def sub_rsp(self, n):
        self.emit(0x48, 0x81, 0xEC)
        self.imm32(n)

    def add_rsp(self, n):
        self.emit(0x48, 0x81, 0xC4)
        self.imm32(n)

    def lea_rip(self, r, target):
        """lea r64, [rip+target]"""
        self._rex(True, r, 0)
        self.emit(0x8D, 0x05 | ((r & 7) << 3))
        self._rip_disp(target)

    def lea_rsp(self, r, disp):
        """lea r64, [rsp+disp]"""
        self._rex(True, r, RSP)
        self.emit(0x8D)
        self._modrm_rsp(r, disp)

    def mov_rsp_r64(self, disp, r):
        """mov [rsp+disp], r64"""
        self._rex(True, r, RSP)
        self.emit(0x89)
        self._modrm_rsp(r, disp)

    def mov_r64_rsp(self, r, disp):
        """mov r64, [rsp+disp]"""
        self._rex(True, r, RSP)
        self.emit(0x8B)
        self._modrm_rsp(r, disp)

    def mov_rsp_imm32(self, disp, v, qword=True):
        if qword:
            self.emit(0x48)
        self.emit(0xC7)
        self._modrm_rsp(0, disp)
        self.imm32(v)

    def mov_rip_r64(self, target, r):
        """mov [rip+target], r64"""
        self._rex(True, r, 0)
        self.emit(0x89, 0x05 | ((r & 7) << 3))
        self._rip_disp(target)

    def call_import(self, name):
        """call qword [rip+IAT-slot]"""
        self.emit(0xFF, 0x15)
        self._rip_disp("iat:" + name)

    def cmp_r32_imm32(self, r, v):
        self._rex(False, 0, r)
        self.emit(0x81, 0xF8 | (r & 7))
        self.imm32(v)

    def test_r32_r32(self, a, b):
        self._rex(False, b, a)
        self.emit(0x85, 0xC0 | ((b & 7) << 3) | (a & 7))

    def jmp(self, label):
        self.emit(0xE9)
        self._rel32(label)

    def jcc(self, cc, label):
        self.emit(0x0F, 0x80 + cc)
        self._rel32(label)

    def je(self, label):
        self.jcc(0x4, label)

    def jne(self, label):
        self.jcc(0x5, label)

    def ret(self):
        self.emit(0xC3)

    def int3(self):
        self.emit(0xCC)

    # ---- afronden ---------------------------------------------------------
    def resolve(self, symbols):
        """symbols: dict naam -> RVA (voor niet-code-doelen zoals iat:/data:)."""
        for pos, target, tail in self.rip_fixups:
            if target in self.labels:
                target_rva = self.base_rva + self.labels[target]
            elif target in symbols:
                target_rva = symbols[target]
            else:
                raise KeyError("onbekend RIP-doel: " + target)
            next_rva = self.base_rva + pos + 4 + tail
            struct.pack_into("<i", self.buf, pos, target_rva - next_rva)
        for pos, label in self.rel_fixups:
            if label not in self.labels:
                raise KeyError("onbekend sprongdoel: " + label)
            struct.pack_into("<i", self.buf, pos, self.labels[label] - (pos + 4))
        return bytes(self.buf)


# --------------------------------------------------------------------------
# PE-bouwer
# --------------------------------------------------------------------------
SECTION_ALIGN = 0x1000
FILE_ALIGN = 0x200
IMAGE_BASE = 0x140000000

IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_SCN_MEM_WRITE = 0x80000000


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def build_imports(imports, base_rva):
    """Bouwt de complete import-sectie.

    imports: lijst van (dllnaam, [functienamen]).
    Geeft (bytes, iat_rvas, iat_dir_rva, iat_dir_size, import_dir_rva) terug.
    """
    n_dlls = len(imports)
    desc_size = (n_dlls + 1) * 20

    # Layout: descriptors | ILTs | IATs | hint/name | dll-namen
    off = desc_size
    ilt_off = {}
    for dll, funcs in imports:
        ilt_off[dll] = off
        off += (len(funcs) + 1) * 8

    iat_start = off
    iat_off = {}
    for dll, funcs in imports:
        iat_off[dll] = off
        off += (len(funcs) + 1) * 8
    iat_size = off - iat_start

    name_off = {}
    for dll, funcs in imports:
        for f in funcs:
            name_off[(dll, f)] = off
            off += 2 + len(f) + 1
            off = align_up(off, 2)

    dll_name_off = {}
    for dll, _ in imports:
        dll_name_off[dll] = off
        off += len(dll) + 1
        off = align_up(off, 2)

    buf = bytearray(align_up(off, 16))

    # descriptors
    for i, (dll, funcs) in enumerate(imports):
        struct.pack_into("<IIIII", buf, i * 20,
                         base_rva + ilt_off[dll],   # OriginalFirstThunk
                         0, 0,
                         base_rva + dll_name_off[dll],
                         base_rva + iat_off[dll])   # FirstThunk

    iat_rvas = {}
    for dll, funcs in imports:
        for k, f in enumerate(funcs):
            entry = base_rva + name_off[(dll, f)]
            struct.pack_into("<Q", buf, ilt_off[dll] + k * 8, entry)
            struct.pack_into("<Q", buf, iat_off[dll] + k * 8, entry)
            iat_rvas[f] = base_rva + iat_off[dll] + k * 8
            struct.pack_into("<H", buf, name_off[(dll, f)], 0)
            buf[name_off[(dll, f)] + 2:name_off[(dll, f)] + 2 + len(f)] = f.encode()
        buf[dll_name_off[dll]:dll_name_off[dll] + len(dll)] = dll.encode()

    return bytes(buf), iat_rvas, base_rva + iat_start, iat_size, base_rva


def build_exe(path, subsystem, imports, code_fn, rdata_extra=b"", data_size=0x400):
    """Bouwt een .exe. code_fn(asm, sym) vult de code; sym wordt later gevuld."""
    text_rva = 0x1000
    asm = Asm(text_rva)
    code_fn(asm)

    code_size = len(asm.buf)
    rdata_rva = align_up(text_rva + code_size, SECTION_ALIGN)

    import_blob, iat_rvas, iat_dir_rva, iat_dir_size, import_dir_rva = build_imports(
        imports, rdata_rva)

    extra_rva = rdata_rva + len(import_blob)
    rdata = import_blob + rdata_extra
    data_rva = align_up(rdata_rva + len(rdata), SECTION_ALIGN)

    symbols = {}
    for name, rva in iat_rvas.items():
        symbols["iat:" + name] = rva
    symbols["rdata"] = extra_rva
    symbols["data"] = data_rva
    # Handige alias: rdata:<offset> en data:<offset>
    for i in range(0, max(len(rdata_extra), 1) + 512, 1):
        symbols["rdata+%d" % i] = extra_rva + i
    for i in range(0, data_size + 1):
        symbols["data+%d" % i] = data_rva + i

    code = asm.resolve(symbols)

    sections = [
        (".text", text_rva, code, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ),
        (".rdata", rdata_rva, rdata, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ),
        (".data", data_rva, b"\0" * data_size,
         IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE),
    ]

    n_sections = len(sections)
    headers_size = align_up(0x80 + 24 + 240 + n_sections * 40, FILE_ALIGN)

    out = bytearray(headers_size)
    # --- DOS header ---
    struct.pack_into("<H", out, 0, 0x5A4D)
    struct.pack_into("<I", out, 0x3C, 0x80)
    out[0x40:0x4E] = b"macemu test stub\0\0"

    # --- NT headers ---
    p = 0x80
    struct.pack_into("<I", out, p, 0x00004550)
    # COFF
    struct.pack_into("<HHIIIHH", out, p + 4,
                     0x8664,        # Machine = AMD64
                     n_sections,
                     0,             # TimeDateStamp
                     0, 0,          # symbol table
                     240,           # SizeOfOptionalHeader
                     0x0022)        # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
    opt = p + 24
    size_of_image = align_up(sections[-1][1] + len(sections[-1][2]), SECTION_ALIGN)
    size_of_code = align_up(len(code), FILE_ALIGN)

    struct.pack_into("<HBBIIIIII", out, opt,
                     0x20B,   # PE32+
                     14, 0,   # linker version
                     size_of_code, 0, 0,
                     text_rva,  # AddressOfEntryPoint
                     text_rva,  # BaseOfCode
                     0)
    struct.pack_into("<Q", out, opt + 24, IMAGE_BASE)
    struct.pack_into("<II", out, opt + 32, SECTION_ALIGN, FILE_ALIGN)
    struct.pack_into("<HHHHHH", out, opt + 40, 6, 0, 0, 0, 6, 0)
    struct.pack_into("<I", out, opt + 52, 0)  # Win32VersionValue
    struct.pack_into("<II", out, opt + 56, size_of_image, headers_size)
    struct.pack_into("<I", out, opt + 64, 0)  # CheckSum
    struct.pack_into("<HH", out, opt + 68, subsystem, 0x0000)  # geen ASLR/NX-vlaggen
    struct.pack_into("<QQQQ", out, opt + 72, 0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into("<II", out, opt + 104, 0, 16)  # LoaderFlags, NumberOfRvaAndSizes

    dirs = opt + 112
    struct.pack_into("<II", out, dirs + 1 * 8, import_dir_rva, len(import_blob))
    struct.pack_into("<II", out, dirs + 12 * 8, iat_dir_rva, iat_dir_size)

    # --- sectietabel ---
    sec_off = opt + 240
    file_off = headers_size
    blobs = []
    for i, (name, rva, blob, chars) in enumerate(sections):
        raw = align_up(len(blob), FILE_ALIGN)
        e = sec_off + i * 40
        out[e:e + 8] = name.encode().ljust(8, b"\0")
        struct.pack_into("<IIII", out, e + 8, max(len(blob), 1), rva, raw, file_off)
        struct.pack_into("<IIHHI", out, e + 24, 0, 0, 0, 0, chars)
        blobs.append((file_off, blob.ljust(raw, b"\0")))
        file_off += raw

    for off, blob in blobs:
        if len(out) < off:
            out.extend(b"\0" * (off - len(out)))
        out[off:off + len(blob)] = blob

    with open(path, "wb") as f:
        f.write(bytes(out))
    print("  %-14s %6d bytes  (%s)" % (os.path.basename(path), len(out),
                                       "GUI" if subsystem == 2 else "console"))


# --------------------------------------------------------------------------
# hello.exe - console
# --------------------------------------------------------------------------
HELLO_MSG = (b"macemu draait een echte PE-executable.\r\n"
             b"Deze tekst komt uit geemuleerde x86-64 code die WriteFile aanroept.\r\n")


def build_hello(out_dir):
    def code(a):
        a.sub_rsp(0x38)
        a.mov_r32_imm32(RCX, -11 & 0xFFFFFFFF)     # STD_OUTPUT_HANDLE
        a.call_import("GetStdHandle")
        a.mov_r64_r64(RCX, RAX)                    # hFile
        a.lea_rip(RDX, "rdata")                    # lpBuffer
        a.mov_r32_imm32(R8, len(HELLO_MSG))        # nNumberOfBytesToWrite
        a.lea_rip(R9, "data")                      # lpNumberOfBytesWritten
        a.mov_rsp_imm32(0x20, 0)                   # lpOverlapped
        a.call_import("WriteFile")
        a.mov_r32_imm32(RCX, 0)
        a.call_import("ExitProcess")
        a.int3()

    build_exe(os.path.join(out_dir, "hello.exe"), 3,
              [("kernel32.dll", ["GetStdHandle", "WriteFile", "ExitProcess"])],
              code, rdata_extra=HELLO_MSG)


# --------------------------------------------------------------------------
# window.exe - GUI met echte WndProc-callback
# --------------------------------------------------------------------------
CLASS_NAME = "MacemuDemoClass"
WINDOW_TITLE = "macemu - Fase 4"
PAINT_TEXT = b"HALLO VANUIT DE EMULATOR"
PAINT_TEXT2 = b"WNDPROC DRAAIT ALS X86-64 CODE"


def build_window(out_dir):
    # .rdata-inhoud na de import-tabel
    cls_w = CLASS_NAME.encode("utf-16-le") + b"\0\0"
    title_w = WINDOW_TITLE.encode("utf-16-le") + b"\0\0"
    rdata = bytearray()
    off_cls = len(rdata); rdata += cls_w
    off_title = len(rdata); rdata += title_w
    off_text = len(rdata); rdata += PAINT_TEXT + b"\0"
    off_text2 = len(rdata); rdata += PAINT_TEXT2 + b"\0"

    # .data-layout
    D_MSG = 0        # MSG-struct (48 bytes)
    D_HWND = 48      # HWND

    def code(a):
        # ---------------- WinMain ----------------
        a.sub_rsp(0xB8)

        # WNDCLASSEXW op [rsp+0x60]
        a.mov_rsp_imm32(0x60, 80, qword=False)      # cbSize
        a.mov_rsp_imm32(0x64, 3, qword=False)       # style = CS_HREDRAW|CS_VREDRAW
        a.lea_rip(RAX, "WndProc")
        a.mov_rsp_r64(0x68, RAX)                    # lpfnWndProc
        a.mov_rsp_imm32(0x70, 0)                    # cbClsExtra/cbWndExtra
        a.mov_rsp_imm32(0x78, 0)                    # hInstance
        a.mov_rsp_imm32(0x80, 0)                    # hIcon
        a.mov_rsp_imm32(0x88, 0)                    # hCursor
        a.mov_r32_imm32(RCX, 1)                     # LTGRAY_BRUSH
        a.call_import("GetStockObject")
        a.mov_rsp_r64(0x90, RAX)                    # hbrBackground
        a.mov_rsp_imm32(0x98, 0)                    # lpszMenuName
        a.lea_rip(RAX, "rdata+%d" % off_cls)
        a.mov_rsp_r64(0xA0, RAX)                    # lpszClassName
        a.mov_rsp_imm32(0xA8, 0)                    # hIconSm
        a.lea_rsp(RCX, 0x60)
        a.call_import("RegisterClassExW")

        # CreateWindowExW(0, cls, title, WS_OVERLAPPEDWINDOW, 100,100,320,240, 0,0,0,0)
        a.xor_r32_r32(RCX, RCX)
        a.lea_rip(RDX, "rdata+%d" % off_cls)
        a.lea_rip(R8, "rdata+%d" % off_title)
        a.mov_r32_imm32(R9, 0x00CF0000)
        a.mov_rsp_imm32(0x20, 100)
        a.mov_rsp_imm32(0x28, 100)
        a.mov_rsp_imm32(0x30, 320)
        a.mov_rsp_imm32(0x38, 240)
        a.mov_rsp_imm32(0x40, 0)
        a.mov_rsp_imm32(0x48, 0)
        a.mov_rsp_imm32(0x50, 0)
        a.mov_rsp_imm32(0x58, 0)
        a.call_import("CreateWindowExW")
        a.mov_rip_r64("data+%d" % D_HWND, RAX)

        # ShowWindow(hwnd, SW_SHOWNORMAL)
        a.mov_r64_r64(RCX, RAX)
        a.mov_r32_imm32(RDX, 1)
        a.call_import("ShowWindow")

        # message loop
        a.label("loop")
        a.lea_rip(RCX, "data+%d" % D_MSG)
        a.xor_r32_r32(RDX, RDX)
        a.xor_r32_r32(R8, R8)
        a.xor_r32_r32(R9, R9)
        a.call_import("GetMessageW")
        a.test_r32_r32(RAX, RAX)
        a.je("done")
        a.lea_rip(RCX, "data+%d" % D_MSG)
        a.call_import("TranslateMessage")
        a.lea_rip(RCX, "data+%d" % D_MSG)
        a.call_import("DispatchMessageW")
        a.jmp("loop")

        a.label("done")
        a.xor_r32_r32(RCX, RCX)
        a.call_import("ExitProcess")
        a.int3()

        # ---------------- WndProc(hwnd, msg, wParam, lParam) ----------------
        # frame: 0x00-0x2F uitgaande args | 0x30 hwnd | 0x38 msg | 0x40 wParam
        #        0x48 lParam | 0x50 hdc | 0x60 PAINTSTRUCT(72) | 0xA8 RECT(16)
        a.label("WndProc")
        a.sub_rsp(0xB8)
        a.mov_rsp_r64(0x30, RCX)
        a.mov_rsp_r64(0x38, RDX)
        a.mov_rsp_r64(0x40, R8)
        a.mov_rsp_r64(0x48, R9)

        a.cmp_r32_imm32(RDX, 0x0002)   # WM_DESTROY
        a.jne("not_destroy")
        a.xor_r32_r32(RCX, RCX)
        a.call_import("PostQuitMessage")
        a.xor_r32_r32(RAX, RAX)
        a.jmp("wndproc_end")

        a.label("not_destroy")
        a.cmp_r32_imm32(RDX, 0x000F)   # WM_PAINT
        a.jne("not_paint")

        a.mov_r64_rsp(RCX, 0x30)
        a.lea_rsp(RDX, 0x60)
        a.call_import("BeginPaint")
        a.mov_rsp_r64(0x50, RAX)       # hdc

        a.mov_r64_rsp(RCX, 0x30)
        a.lea_rsp(RDX, 0xA8)
        a.call_import("GetClientRect")

        # FillRect(hdc, &rect, WHITE_BRUSH)
        a.mov_r32_imm32(RCX, 0)
        a.call_import("GetStockObject")
        a.mov_r64_r64(R8, RAX)
        a.mov_r64_rsp(RCX, 0x50)
        a.lea_rsp(RDX, 0xA8)
        a.call_import("FillRect")

        # SetBkMode(hdc, TRANSPARENT)
        a.mov_r64_rsp(RCX, 0x50)
        a.mov_r32_imm32(RDX, 1)
        a.call_import("SetBkMode")

        # SetTextColor(hdc, 0x00C00000)  (COLORREF = 0x00BBGGRR -> blauw)
        a.mov_r64_rsp(RCX, 0x50)
        a.mov_r32_imm32(RDX, 0x00C00000)
        a.call_import("SetTextColor")

        # TextOutA(hdc, 20, 40, text, len)
        a.mov_r64_rsp(RCX, 0x50)
        a.mov_r32_imm32(RDX, 20)
        a.mov_r32_imm32(R8, 40)
        a.lea_rip(R9, "rdata+%d" % off_text)
        a.mov_rsp_imm32(0x20, len(PAINT_TEXT))
        a.call_import("TextOutA")

        a.mov_r64_rsp(RCX, 0x50)
        a.mov_r32_imm32(RDX, 20)
        a.mov_r32_imm32(R8, 60)
        a.lea_rip(R9, "rdata+%d" % off_text2)
        a.mov_rsp_imm32(0x20, len(PAINT_TEXT2))
        a.call_import("TextOutA")

        # Een blokje kleur zodat je in de screenshot ziet dat GDI werkt.
        a.mov_r32_imm32(RCX, 4)        # BLACK_BRUSH
        a.call_import("GetStockObject")
        a.mov_r64_r64(R8, RAX)
        a.mov_rsp_imm32(0xA8, 20, qword=False)
        a.mov_rsp_imm32(0xAC, 90, qword=False)
        a.mov_rsp_imm32(0xB0, 120, qword=False)
        a.mov_rsp_imm32(0xB4, 140, qword=False)
        a.mov_r64_rsp(RCX, 0x50)
        a.lea_rsp(RDX, 0xA8)
        a.call_import("FillRect")

        a.mov_r64_rsp(RCX, 0x30)
        a.lea_rsp(RDX, 0x60)
        a.call_import("EndPaint")
        a.xor_r32_r32(RAX, RAX)
        a.jmp("wndproc_end")

        a.label("not_paint")
        a.mov_r64_rsp(RCX, 0x30)
        a.mov_r64_rsp(RDX, 0x38)
        a.mov_r64_rsp(R8, 0x40)
        a.mov_r64_rsp(R9, 0x48)
        a.call_import("DefWindowProcW")

        a.label("wndproc_end")
        a.add_rsp(0xB8)
        a.ret()

    build_exe(os.path.join(out_dir, "window.exe"), 2,
              [("kernel32.dll", ["ExitProcess"]),
               ("user32.dll", ["RegisterClassExW", "CreateWindowExW", "ShowWindow",
                               "GetMessageW", "TranslateMessage", "DispatchMessageW",
                               "DefWindowProcW", "PostQuitMessage", "BeginPaint", "EndPaint",
                               "GetClientRect", "FillRect"]),
               ("gdi32.dll", ["GetStockObject", "TextOutA", "SetTextColor", "SetBkMode"])],
              code, rdata_extra=bytes(rdata))


# --------------------------------------------------------------------------
# crash.exe - roept iets aan wat macemu (nog) niet kent
# --------------------------------------------------------------------------
def build_crash(out_dir):
    def code(a):
        a.sub_rsp(0x28)
        a.xor_r32_r32(RCX, RCX)
        a.call_import("CreateHardLinkW")   # bewust niet geïmplementeerd
        a.mov_r32_imm32(RCX, 0)
        a.call_import("ExitProcess")
        a.int3()

    build_exe(os.path.join(out_dir, "crash.exe"), 3,
              [("kernel32.dll", ["CreateHardLinkW", "ExitProcess"])], code)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "fixtures"
    os.makedirs(out_dir, exist_ok=True)
    print("Test-executables genereren in %s/" % out_dir)
    build_hello(out_dir)
    build_window(out_dir)
    build_crash(out_dir)
    print("Klaar.")


if __name__ == "__main__":
    main()
