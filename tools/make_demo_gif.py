"""Genere docs/demo.gif : la vraie fenetre ScanFolder sur une arborescence factice.

Aucune capture de l'ecran : l'app tourne hors de l'ecran visible, chaque image
vient de PrintWindow sur sa seule fenetre. L'arborescence est creee dans %TEMP%
(fichiers creux, aucun espace disque consomme) et montee sur un lecteur subst
pour que le chemin affiche ne revele rien de la machine.

Lancer depuis la racine, apres build.cmd : python tools/make_demo_gif.py
"""

from __future__ import annotations

import ctypes
import msvcrt
import os
import shutil
import string
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "ScanFolder.exe"
OUTPUT = ROOT / "docs" / "demo.gif"
CLIENT_W, CLIENT_H = 1290, 760
OUT_WIDTH = 860
GIB = 1024**3
MIB = 1024**2

# (chemin relatif, taille en octets, date "AAAA-MM-JJ")
FAKE_FILES = [
    ("Videos/Vacances 2025/plage.mp4", 18 * GIB, "2025-08-14"),
    ("Videos/Vacances 2025/randonnee.mp4", 9 * GIB, "2025-08-16"),
    ("Videos/Vacances 2025/coucher de soleil.mov", 4 * GIB, "2025-08-17"),
    ("Videos/Montage/projet final.mkv", 23 * GIB, "2026-03-02"),
    ("Videos/Montage/rushes/prise 01.mov", 6 * GIB, "2026-02-27"),
    ("Videos/Montage/rushes/prise 02.mov", 5 * GIB, "2026-02-27"),
    ("Jeux/Aventure/data/pack0.bin", 31 * GIB, "2026-05-10"),
    ("Jeux/Aventure/data/pack1.bin", 12 * GIB, "2026-05-10"),
    ("Jeux/Course/course.pak", 14 * GIB, "2025-12-01"),
    ("Archives/sauvegarde 2024.zip", 26 * GIB, "2024-12-31"),
    ("Archives/ancien portable.7z", 11 * GIB, "2023-06-20"),
    ("Photos/2025/IMG_0001.raw", 48 * MIB, "2025-07-01"),
    ("Photos/2025/IMG_0002.raw", 51 * MIB, "2025-07-01"),
    ("Photos/2026/IMG_0103.raw", 47 * MIB, "2026-04-12"),
    ("Photos/2026/panorama.tif", 2 * GIB, "2026-04-13"),
    ("Musique/albums/live.flac", 700 * MIB, "2024-03-09"),
    ("Musique/albums/studio.flac", 520 * MIB, "2024-03-09"),
    ("Projets/site web/node_modules/cache.bin", 1800 * MIB, "2026-09-21"),
    ("Projets/site web/index.html", 24 * 1024, "2026-09-21"),
    ("Projets/maquette.psd", 3 * GIB, "2026-06-03"),
    ("Documents/factures.pdf", 3 * MIB, "2026-01-15"),
    ("Documents/notes.txt", 12 * 1024, "2026-10-02"),
    ("Documents/rapport.docx", 900 * 1024, "2026-09-30"),
    ("installeur.iso", 7 * GIB, "2026-04-01"),
]

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# Handles 64 bits : sans ces declarations ctypes les tronquerait en int.
HANDLE = ctypes.c_void_p
for func, args, res in (
    (user32.PostMessageW, [HANDLE, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM], wintypes.BOOL),
    (user32.SendMessageW, [HANDLE, wintypes.UINT, wintypes.WPARAM, ctypes.c_wchar_p], ctypes.c_ssize_t),
    (user32.FindWindowExW, [HANDLE, HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR], HANDLE),
    (user32.GetDpiForWindow, [HANDLE], wintypes.UINT),
    (user32.GetWindowRect, [HANDLE, ctypes.c_void_p], wintypes.BOOL),
    (user32.GetClientRect, [HANDLE, ctypes.c_void_p], wintypes.BOOL),
    (user32.SetWindowPos, [HANDLE, HANDLE] + [ctypes.c_int] * 4 + [wintypes.UINT], wintypes.BOOL),
    (user32.PrintWindow, [HANDLE, HANDLE, wintypes.UINT], wintypes.BOOL),
    (user32.IsWindowVisible, [HANDLE], wintypes.BOOL),
    (user32.GetWindowThreadProcessId, [HANDLE, ctypes.c_void_p], wintypes.DWORD),
    (user32.GetClassNameW, [HANDLE, wintypes.LPWSTR, ctypes.c_int], ctypes.c_int),
    (user32.GetDC, [HANDLE], HANDLE),
    (user32.ReleaseDC, [HANDLE, HANDLE], ctypes.c_int),
    (gdi32.CreateCompatibleDC, [HANDLE], HANDLE),
    (gdi32.CreateCompatibleBitmap, [HANDLE, ctypes.c_int, ctypes.c_int], HANDLE),
    (gdi32.SelectObject, [HANDLE, HANDLE], HANDLE),
    (gdi32.GetDIBits, [HANDLE, HANDLE, wintypes.UINT, wintypes.UINT, ctypes.c_void_p,
                       ctypes.c_void_p, wintypes.UINT], ctypes.c_int),
    (gdi32.DeleteObject, [HANDLE], wintypes.BOOL),
    (gdi32.DeleteDC, [HANDLE], wintypes.BOOL),
    (kernel32.WaitForSingleObject, [HANDLE, wintypes.DWORD], wintypes.DWORD),
    (kernel32.TerminateProcess, [HANDLE, wintypes.UINT], wintypes.BOOL),
    (kernel32.CloseHandle, [HANDLE], wintypes.BOOL),
    (kernel32.SetFilePointerEx, [HANDLE, ctypes.c_int64, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
    (kernel32.SetEndOfFile, [HANDLE], wintypes.BOOL),
):
    func.argtypes, func.restype = args, res

WM_CLOSE, WM_SETTEXT, WM_KEYDOWN = 0x0010, 0x000C, 0x0100
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK = 0x0200, 0x0201, 0x0202, 0x0203
VK_BACK = 0x08
SWP_NOZORDER, SWP_NOACTIVATE = 0x0004, 0x0010
PW_CLIENTONLY_FULLCONTENT = 0x1 | 0x2


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR), ("lpDesktop", wintypes.LPWSTR),
        ("lpTitle", wintypes.LPWSTR), ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
        ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD),
        ("dwXCountChars", wintypes.DWORD), ("dwYCountChars", wintypes.DWORD),
        ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
        ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
        ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
        ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
        ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD),
    ]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG), ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD),
    ]


def make_tree(root: Path) -> None:
    """Fichiers creux : la taille logique est la bonne, rien n'est ecrit sur le disque."""
    for rel, size, day in FAKE_FILES:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        subprocess.run(["fsutil", "sparse", "setflag", str(path)], check=True, capture_output=True)
        # truncate() du CRT remplit de zeros : on passe par SetEndOfFile.
        with path.open("r+b") as handle:
            native = msvcrt.get_osfhandle(handle.fileno())
            if not (kernel32.SetFilePointerEx(native, size, None, 0) and kernel32.SetEndOfFile(native)):
                raise ctypes.WinError(ctypes.get_last_error())
        stamp = time.mktime(time.strptime(day + " 10:30", "%Y-%m-%d %H:%M"))
        os.utime(path, (stamp, stamp))
    # Sinon les dossiers afficheraient la date du jour de generation.
    folder_stamp = time.mktime(time.strptime("2026-10-20 18:00", "%Y-%m-%d %H:%M"))
    for folder in [p for p in root.rglob("*") if p.is_dir()]:
        os.utime(folder, (folder_stamp, folder_stamp))


def free_drive_letter() -> str:
    used = kernel32.GetLogicalDrives()
    for letter in "SZYXWVUTR":
        if not used & (1 << string.ascii_uppercase.index(letter)):
            return f"{letter}:"
    raise RuntimeError("Aucune lettre de lecteur libre pour subst")


def launch(drive: str, cache: Path) -> tuple[PROCESS_INFORMATION, int]:
    """Demarre l'app hors de l'ecran visible, sans lui donner le focus."""
    info = STARTUPINFOW()
    info.cb = ctypes.sizeof(info)
    info.dwFlags = 0x1 | 0x4  # STARTF_USESHOWWINDOW | STARTF_USEPOSITION
    info.wShowWindow = 4  # SW_SHOWNOACTIVATE
    info.dwX = info.dwY = ctypes.c_uint32(-6000).value
    process = PROCESS_INFORMATION()
    env = dict(os.environ, SCANFOLDER_CACHE_DIR=str(cache))
    block = "".join(f"{k}={v}\0" for k, v in env.items()) + "\0"
    command = f'"{EXE}" --open {drive}\\'
    if not kernel32.CreateProcessW(
        None, ctypes.create_unicode_buffer(command), None, None, False,
        0x400, ctypes.create_unicode_buffer(block), None,  # CREATE_UNICODE_ENVIRONMENT
        ctypes.byref(info), ctypes.byref(process),
    ):
        raise ctypes.WinError(ctypes.get_last_error())
    return process, process.dwProcessId


def find_window(pid: int, timeout: float = 10) -> int:
    found: list[int] = []
    proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def check(hwnd: int, _: int) -> bool:
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, name, 64)
        if owner.value == pid and name.value == "ScanFolderApp":
            found.append(hwnd)
        return True

    deadline = time.time() + timeout
    while time.time() < deadline:
        user32.EnumWindows(proto(check), 0)
        if found and user32.IsWindowVisible(found[0]):
            return found[0]
        time.sleep(0.1)
    raise RuntimeError("Fenetre ScanFolder introuvable")


class Window:
    def __init__(self, hwnd: int) -> None:
        self.hwnd = hwnd
        self.scale = user32.GetDpiForWindow(hwnd) / 96
        self.cursor: tuple[float, float] | None = None
        self.resize()

    def resize(self) -> None:
        window, client = wintypes.RECT(), wintypes.RECT()
        user32.GetWindowRect(self.hwnd, ctypes.byref(window))
        user32.GetClientRect(self.hwnd, ctypes.byref(client))
        extra_w = (window.right - window.left) - client.right
        extra_h = (window.bottom - window.top) - client.bottom
        user32.SetWindowPos(
            self.hwnd, 0, window.left, window.top,
            round(CLIENT_W * self.scale) + extra_w, round(CLIENT_H * self.scale) + extra_h,
            SWP_NOZORDER | SWP_NOACTIVATE,
        )
        time.sleep(0.4)

    def _lparam(self, x: float, y: float) -> int:
        return (round(y * self.scale) << 16) | round(x * self.scale)

    def move(self, x: float, y: float) -> None:
        self.cursor = (x, y)
        user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 0, self._lparam(x, y))

    def click(self, x: float, y: float, double: bool = False) -> None:
        self.move(x, y)
        lparam = self._lparam(x, y)
        user32.PostMessageW(self.hwnd, WM_LBUTTONDOWN, 1, lparam)
        user32.PostMessageW(self.hwnd, WM_LBUTTONUP, 0, lparam)
        if double:
            user32.PostMessageW(self.hwnd, WM_LBUTTONDBLCLK, 1, lparam)
            user32.PostMessageW(self.hwnd, WM_LBUTTONUP, 0, lparam)

    def key(self, vk: int) -> None:
        user32.PostMessageW(self.hwnd, WM_KEYDOWN, vk, 0)

    def search(self, text: str) -> None:
        edit = user32.FindWindowExW(self.hwnd, None, "EDIT", None)
        edit = user32.FindWindowExW(self.hwnd, edit, "EDIT", None)  # le second EDIT est la recherche
        user32.SendMessageW(edit, WM_SETTEXT, 0, ctypes.c_wchar_p(text))

    def grab(self) -> Image.Image:
        width, height = round(CLIENT_W * self.scale), round(CLIENT_H * self.scale)
        screen = user32.GetDC(None)
        dc = gdi32.CreateCompatibleDC(screen)
        bitmap = gdi32.CreateCompatibleBitmap(screen, width, height)
        gdi32.SelectObject(dc, bitmap)
        user32.PrintWindow(self.hwnd, dc, PW_CLIENTONLY_FULLCONTENT)
        header = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), width, -height, 1, 32, 0)
        buffer = ctypes.create_string_buffer(width * height * 4)
        gdi32.GetDIBits(dc, bitmap, 0, height, buffer, ctypes.byref(header), 0)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(dc)
        user32.ReleaseDC(None, screen)
        image = Image.frombuffer("RGB", (width, height), buffer, "raw", "BGRX", 0, 1)
        image = image.resize((CLIENT_W, CLIENT_H), Image.LANCZOS)
        if self.cursor:
            draw_cursor(image, *self.cursor)
        return image.resize((OUT_WIDTH, round(CLIENT_H * OUT_WIDTH / CLIENT_W)), Image.LANCZOS)


def draw_cursor(image: Image.Image, x: float, y: float) -> None:
    points = [(0, 0), (0, 18), (4.5, 14), (8, 21), (11, 19.5), (7.5, 13), (13, 13)]
    shape = [(x + dx * 1.3, y + dy * 1.3) for dx, dy in points]
    ImageDraw.Draw(image).polygon(shape, fill=(255, 255, 255), outline=(0, 0, 0), width=2)


def record(win: Window) -> list[tuple[Image.Image, int]]:
    frames: list[tuple[Image.Image, int]] = []

    def shot(ms: int, wait: float = 0.35) -> None:
        time.sleep(wait)
        frames.append((win.grab(), ms))

    def glide(x: float, y: float, steps: int = 6) -> None:
        sx, sy = win.cursor or (x, y)
        for i in range(1, steps + 1):
            win.move(sx + (x - sx) * i / steps, sy + (y - sy) * i / steps)
            shot(60, 0.05)

    analyse, first_row, second_row = (967, 110), (520, 380), (520, 432)
    tab_big, tab_types = (387, 302), (496, 302)

    win.move(700, 600)
    shot(1400)
    glide(*analyse)
    win.click(*analyse)
    shot(1800, 2.0)
    glide(*first_row)
    win.click(*first_row)
    shot(1500)
    win.click(*first_row, double=True)
    shot(1600, 0.8)
    glide(*second_row)
    win.click(*second_row)
    shot(1300)
    win.key(VK_BACK)
    shot(900, 0.6)
    glide(*tab_big)
    win.click(*tab_big)
    shot(1800, 0.8)
    win.move(1150, 302)
    for typed in ("m", "mo", "mov"):
        win.search(typed)
        shot(300, 0.5)
    shot(1600, 0.4)
    win.search("")
    glide(*tab_types)
    win.click(*tab_types)
    shot(3000, 0.8)
    return frames


def main() -> int:
    if not EXE.is_file():
        print("ScanFolder.exe manquant : lance build.cmd", file=sys.stderr)
        return 1
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    work = Path(tempfile.mkdtemp(prefix="scanfolder-demo-"))
    tree, cache = work / "tree", work / "cache"
    tree.mkdir()
    cache.mkdir()
    drive = free_drive_letter()
    process = None
    try:
        make_tree(tree)
        subprocess.run(["subst", drive, str(tree)], check=True)
        process, pid = launch(drive, cache)
        win = Window(find_window(pid))
        frames = record(win)
        user32.PostMessageW(win.hwnd, WM_CLOSE, 0, 0)
        kernel32.WaitForSingleObject(process.hProcess, 5000)
    finally:
        if process is not None:
            kernel32.TerminateProcess(process.hProcess, 0)
            kernel32.CloseHandle(process.hProcess)
            kernel32.CloseHandle(process.hThread)
        subprocess.run(["subst", drive, "/d"], check=False, capture_output=True)
        shutil.rmtree(work, ignore_errors=True)

    images = [image for image, _ in frames]
    OUTPUT.parent.mkdir(exist_ok=True)
    images[0].save(
        OUTPUT,
        save_all=True,
        append_images=images[1:],
        duration=[ms for _, ms in frames],
        loop=0,
        optimize=True,
    )
    print(f"{OUTPUT.relative_to(ROOT)} : {len(images)} images, {OUTPUT.stat().st_size / 1024:.0f} Ko")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
