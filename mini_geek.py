# -*- coding: utf-8 -*-
"""
Mini Geek Uninstaller  —  双击即用的 Windows 软件卸载工具
功能：
  1. 正常删除：调用软件原生卸载 → 扫描残留(文件/文件夹/注册表/快捷方式) → 用户勾选清理
  2. 强制删除：杀进程 → 停止/删除驱动服务(sys) → 解锁强删(dll/sys) → 删不掉则标记重启删除
  3. 安装监控：设置菜单中开关，对安装位置与注册表做快照对比，可一键还原
作者：Doubao
"""
import os
import sys
import re
import ctypes
import shutil
import struct
import subprocess
import threading
import json
import time
import tempfile
import winreg
from datetime import datetime
from dataclasses import dataclass, field
from typing import Optional, List, Dict

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from PIL import Image, ImageTk

# ============================================================
# 常量 / 通用工具
# ============================================================
APP_NAME = "Mini Geek Uninstaller"
APP_DIR = os.path.join(os.environ.get("APPDATA", tempfile.gettempdir()), "MiniGeekUninstaller")
MONITOR_FILE = os.path.join(APP_DIR, "monitor_snapshot.json")
MONITOR_LOG = os.path.join(APP_DIR, "monitor_records.json")
SETTINGS_FILE = os.path.join(APP_DIR, "settings.json")
ICON_CACHE = os.path.join(APP_DIR, "icon_cache")
os.makedirs(ICON_CACHE, exist_ok=True)

MOVEFILE_DELAY_UNTIL_REBOOT = 0x4
TOKEN_ADJUST_PRIVILEGES = 0x20
TOKEN_QUERY = 0x8
SE_PRIVILEGE_ENABLED = 0x2

# 受系统保护、禁止强制删除的关键字（白名单，匹配软件名）
PROTECT_KEYWORDS = [
    "microsoft visual c++", "microsoft .net", "windows software development",
    "windows driver package", "security update", "update for microsoft windows",
    "windows sdk", "directx", "microsoft edge", "windows defender",
    "intel(", "realtek", "nvidia", "amd chipset",
]

# 受保护的系统目录：这些路径本身及其上层绝不允许删除/扫描为残留
def _sys_dirs():
    win = os.environ.get("WINDIR", r"C:\Windows")
    # 32 位进程下 %ProgramFiles% 会被重定向到 x86 目录，必须用 ProgramW6432 取真实 64 位目录
    pf = os.environ.get("ProgramW6432", r"C:\Program Files")
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    if not pf86:
        pf86 = os.environ.get("ProgramFiles", r"C:\Program Files (x86)")
    pd = os.environ.get("ProgramData", r"C:\ProgramData")
    la = os.environ.get("LOCALAPPDATA", "")
    ad = os.environ.get("APPDATA", "")
    up = os.environ.get("USERPROFILE", "")
    sysdrv = os.environ.get("SystemDrive", "C:") + "\\"
    dirs = [
        win, os.path.join(win, "System32"), os.path.join(win, "SysWOW64"),
        os.path.join(win, "WinSxS"), os.path.join(win, "Installer"),
        os.path.join(win, "assembly"), os.path.join(win, "Microsoft.NET"),
        os.path.join(pf, "WindowsApps"), os.path.join(pf, "Common Files"),
        os.path.join(pf86, "Common Files"),
        pf, pf86, pd, la, ad, up, sysdrv,
        os.path.join(pd, "Microsoft"), os.path.join(la, "Microsoft"),
        os.path.join(ad, "Microsoft"),
    ]
    return [os.path.normcase(os.path.normpath(d)).rstrip("\\/") for d in dirs if d]

PROTECTED_DIRS = _sys_dirs()

# 受保护的注册表键（前缀匹配，大小写不敏感）
PROTECTED_REG_PREFIX = [
    r"software\microsoft\windows",
    r"software\microsoft\windows nt",
    r"software\classes",
    r"software\microsoft\.netframework",
    r"software\policies",
    r"software\wow6432node\microsoft\windows",
    r"software\wow6432node\classes",
]
# 卸载键根路径本身
UNINSTALL_REG_PREFIX_LOWER = r"software\microsoft\windows\currentversion\uninstall"


def is_protected_path(path: str) -> bool:
    """路径本身是系统目录/根目录，或位于 Windows 目录内 -> 禁止删除"""
    if not path:
        return True
    p = os.path.normcase(os.path.normpath(path)).rstrip("\\/")
    win = os.path.normcase(os.path.normpath(os.environ.get("WINDIR", r"C:\Windows")))
    if p == win or p.startswith(win + os.sep):
        return True
    for d in PROTECTED_DIRS:
        if p == d:          # Program Files 根本身受保护，但其下的软件目录不受保护
            return True
    return False


def is_protected_reg(subkey: str) -> bool:
    s = subkey.lower().lstrip("\\/")
    # 具体软件的卸载项允许删除；Uninstall 根本身受保护
    if s.startswith(UNINSTALL_REG_PREFIX_LOWER + "\\"):
        return False
    if s == UNINSTALL_REG_PREFIX_LOWER:
        return True
    for pre in PROTECTED_REG_PREFIX:
        if s == pre or s.startswith(pre + "\\"):
            return True
    return False

UNINSTALL_KEYS = [
    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall", winreg.KEY_WOW64_64KEY),
    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall", winreg.KEY_WOW64_32KEY),
    (winreg.HKEY_CURRENT_USER,  r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall", 0),
]

# 常见残留根目录（ProgramW6432 避免 32 位进程下漏掉 64 位 Program Files）
RESIDUAL_ROOTS = [
    os.environ.get("ProgramW6432", r"C:\Program Files"),
    os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
    os.environ.get("ProgramData", r"C:\ProgramData"),
    os.path.join(os.environ.get("LOCALAPPDATA", ""), ""),
    os.path.join(os.environ.get("APPDATA", ""), ""),
    os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs"),
]
START_MENU_DIRS = [
    os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\Start Menu\Programs"),
    r"C:\ProgramData\Microsoft\Windows\Start Menu\Programs",
]
DESKTOP_DIRS = [
    os.path.join(os.environ.get("USERPROFILE", ""), "Desktop"),
    os.path.join(os.environ.get("PUBLIC", r"C:\Users\Public"), "Desktop"),
]

# 32 位打包后默认 PowerShell 会被重定向到 32 位，Appx 模块必须用 64 位
_PS_CANDIDATES = [
    os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Sysnative",
                 r"WindowsPowerShell\v1.0\powershell.exe"),
    os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "System32",
                 r"WindowsPowerShell\v1.0\powershell.exe"),
    "powershell",
]
PS_EXE = next((p for p in _PS_CANDIDATES if p == "powershell" or os.path.exists(p)), "powershell")
NO_WINDOW = 0x08000000


def run_ps(script: str, timeout: int = 60) -> str:
    """运行 PowerShell，返回 stdout 文本（中文系统按 GBK 解码）"""
    try:
        r = subprocess.run([PS_EXE, "-NoProfile", "-NonInteractive", "-Command", script],
                           capture_output=True, text=True, encoding="gbk", errors="ignore",
                           timeout=timeout, creationflags=NO_WINDOW)
        return r.stdout or ""
    except Exception:
        return ""


# 流氓/全家桶厂商别名词典：软件名/发布者命中任一词，就把整组关键字纳入匹配
VENDOR_ALIASES = {
    "360": ["360", "qihoo", "qihu", "奇虎", "360safe", "360se", "360zip", "360drv",
            "360tpt", "360chrome", "360total", "leaddesk", "haosou", "so.com", "360kan",
            "360paper", "lucoms", "360doc", "zhudianbao", "360huabao"],
    "2345": ["2345", "2345explorer", "2345pic", "2345mp", "2345soft", "duowanluzhi"],
    "kingsoft": ["kingsoft", "金山", "duba", "xindubawukong", "cheetah", "liebao"],
    "baidu": ["baidu", "百度", "baiduprotect", "bddownloader"],
    "sogou": ["sogou", "搜狗"],
    "ludashi": ["ludashi", "鲁大师", "lu master"],
    "tencent": ["tencent", "腾讯", "qqpc", "qqpcmgr", "guanjia"],
    "haozip": ["haozip", "2345haozip"],
    "xunlei": ["xunlei", "迅雷"],
}


def vendor_keywords(app: "AppEntry") -> List[str]:
    """根据软件名/发布者/安装目录，推导厂商全家桶关键字（含随机目录同公司匹配）"""
    blob = " ".join([app.name or "", app.publisher or "", app.location or "",
                     app.company or ""]).lower()
    extra = set()
    for _key, words in VENDOR_ALIASES.items():
        if any(w.lower() in blob for w in words):
            extra.update(w.lower() for w in words if len(w) >= 3)
    # 中文公司名核心词：奇虎科技/百度在线 等，取 2 字以上中文片段
    for m in re.findall(r"[\u4e00-\u9fff]{2,5}", blob):
        if m not in ("软件", "科技", "网络", "技术", "信息", "有限", "公司", "北京", "上海",
                     "安全", "中心", "大师", "驱动"):
            extra.add(m)
    return list(extra)


def _path_in_roots(p: str, roots) -> bool:
    if not p:
        return False
    pp = os.path.normcase(os.path.normpath(p))
    for r in roots:
        if not r:
            continue
        rr = os.path.normcase(os.path.normpath(r))
        if pp == rr or pp.startswith(rr + os.sep):
            return True
    return False


def _is_system_path(p: str) -> bool:
    """进程/服务路径是否位于 Windows 系统目录（这些绝不按厂商残留处理）"""
    if not p:
        return True
    pp = os.path.normcase(os.path.normpath(p))
    win = os.path.normcase(os.path.normpath(os.environ.get("WINDIR", r"C:\Windows")))
    if pp.startswith(win + os.sep):
        # Windows\System32/drivers 等系统目录受保护；Windows 下厂商目录极少见，整体保护
        return True
    pf = os.path.normcase(os.path.normpath(os.environ.get("ProgramW6432", r"C:\Program Files")))
    for sysname in ("windowsapps", "common files", "internet explorer"):
        if os.sep + sysname + os.sep in pp + os.sep:
            return True
    return False



def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def run_as_admin():
    params = " ".join(f'"{a}"' for a in sys.argv)
    ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, params, None, 1)


def enable_debug_privilege():
    """启用 SeDebugPrivilege / SeRestorePrivilege / SeBackupPrivilege / SeTakeOwnership"""
    adv = ctypes.windll.advapi32
    k32 = ctypes.windll.kernel32

    class LUID(ctypes.Structure):
        _fields_ = [("LowPart", ctypes.c_uint32), ("HighPart", ctypes.c_int32)]

    class LUID_AND_ATTRIBUTES(ctypes.Structure):
        _fields_ = [("Luid", LUID), ("Attributes", ctypes.c_uint32)]

    class TOKEN_PRIVILEGES(ctypes.Structure):
        _fields_ = [("PrivilegeCount", ctypes.c_uint32),
                    ("Privileges", LUID_AND_ATTRIBUTES * 1)]

    hToken = ctypes.c_void_p()
    if not adv.OpenProcessToken(k32.GetCurrentProcess(),
                                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, ctypes.byref(hToken)):
        return
    for priv in (b"SeDebugPrivilege", b"SeRestorePrivilege", b"SeBackupPrivilege",
                 b"SeTakeOwnershipPrivilege", b"SeLoadDriverPrivilege", b"SeShutdownPrivilege"):
        luid = LUID()
        if adv.LookupPrivilegeValueW(None, ctypes.c_wchar_p(priv.decode()), ctypes.byref(luid)):
            tp = TOKEN_PRIVILEGES()
            tp.PrivilegeCount = 1
            tp.Privileges[0].Luid = luid
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED
            adv.AdjustTokenPrivileges(hToken, False, ctypes.byref(tp), 0, None, None)
    k32.CloseHandle(hToken)


def norm_name(s: str) -> str:
    """归一化名称：去公司后缀/版本号/空格标点，用于伪装残留的模糊匹配"""
    if not s:
        return ""
    s = s.lower()
    s = re.sub(r"\b(v?\d+(\.\d+)*)\b", "", s)
    for w in ("l.l.c.", "llc", "ltd.", "ltd", "inc.", "inc", "co.,ltd", "co.ltd",
              "corporation", "corp.", "corp", "company", "gmbh", "software",
              "(r)", "(tm)", "x64", "x86", "64-bit", "32-bit", "setup", "installer"):
        s = s.replace(w, "")
    s = re.sub(r"[^a-z0-9\u4e00-\u9fff]+", "", s)
    return s


def size_human(n) -> str:
    try:
        n = float(n)
    except Exception:
        return ""
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024


# ============================================================
# 图标提取（纯 ctypes + Pillow，从 exe/ico/dll 资源提取）
# ============================================================
class _ICONINFO(ctypes.Structure):
    _fields_ = [("fIcon", ctypes.c_uint32),
                ("xHotspot", ctypes.c_uint32), ("yHotspot", ctypes.c_uint32),
                ("hbmMask", ctypes.c_void_p), ("hbmColor", ctypes.c_void_p)]


class _BITMAP(ctypes.Structure):
    _fields_ = [("bmType", ctypes.c_long), ("bmWidth", ctypes.c_long),
                ("bmHeight", ctypes.c_long), ("bmWidthBytes", ctypes.c_long),
                ("bmPlanes", ctypes.c_uint16), ("bmBitsPixel", ctypes.c_uint16),
                ("bmBits", ctypes.c_void_p)]


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]


class _SHFILEINFO(ctypes.Structure):
    _fields_ = [("hIcon", ctypes.c_void_p), ("iIcon", ctypes.c_int),
                ("dwAttributes", ctypes.c_uint32),
                ("szDisplayName", ctypes.c_wchar * 260),
                ("szTypeName", ctypes.c_wchar * 80)]


def _hicon_to_png(hicon, png_path: str, size: int = 32) -> Optional[str]:
    """把 HICON 渲染成透明背景 PNG"""
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    try:
        ii = _ICONINFO()
        if not user32.GetIconInfo(hicon, ctypes.byref(ii)):
            return None
        w = h = size
        bm = _BITMAP()
        if ii.hbmColor and gdi32.GetObjectW(ii.hbmColor, ctypes.sizeof(_BITMAP), ctypes.byref(bm)):
            if bm.bmWidth > 0:
                w, h = bm.bmWidth, bm.bmHeight
        hdc = user32.GetDC(0)
        hdc_mem = gdi32.CreateCompatibleDC(hdc)
        hbmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
        gdi32.SelectObject(hdc_mem, hbmp)
        user32.DrawIconEx(hdc_mem, 0, 0, hicon, w, h, 0, None, 0x3)
        buf = ctypes.create_string_buffer(w * h * 4)
        bi = _BITMAPINFOHEADER()
        bi.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        bi.biWidth, bi.biHeight = w, -h
        bi.biPlanes, bi.biBitCount = 1, 32
        gdi32.GetDIBits(hdc_mem, hbmp, 0, h, buf, ctypes.byref(bi), 0)
        img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)
        gdi32.DeleteObject(hbmp)
        gdi32.DeleteDC(hdc_mem)
        user32.ReleaseDC(0, hdc)
        if ii.hbmMask:
            gdi32.DeleteObject(ii.hbmMask)
        if ii.hbmColor:
            gdi32.DeleteObject(ii.hbmColor)
        img.resize((size, size), Image.LANCZOS).save(png_path)
        return png_path
    except Exception:
        return None


def _extract_hicon(path: str, size: int):
    """依次尝试 PrivateExtractIcons / ExtractIconEx，返回 HICON 或 None"""
    if not path or not os.path.exists(path):
        return None
    u = ctypes.windll.user32
    shell32 = ctypes.windll.shell32
    # 1) PrivateExtractIcons(user32)：可指定尺寸，phicon 必须传数组
    try:
        ph = (ctypes.c_void_p * 1)()
        ids = (ctypes.c_uint32 * 1)()
        r = u.PrivateExtractIconsW(path, 0, size, size, ph, ids, 1, 0)
        if r and ph[0]:
            return ph[0]
    except Exception:
        pass
    # 2) ExtractIconExW 由 shell32 导出（不是 user32）
    try:
        arr = (ctypes.c_void_p * 1)()
        n = shell32.ExtractIconExW(path, 0, ctypes.byref(arr), None, 1)
        if n and arr[0]:
            return arr[0]
    except Exception:
        pass
    return None


def extract_icon(exe_path: str, cache_key: str, size: int = 32) -> Optional[str]:
    """从 exe/dll/ico 提取图标，保存为 png 并返回路径；失败返回 None。"""
    png_path = os.path.join(ICON_CACHE, cache_key + ".png")
    if os.path.exists(png_path):
        return png_path
    if not exe_path or not os.path.exists(exe_path):
        return None
    try:
        if exe_path.lower().endswith(".ico"):
            img = Image.open(exe_path)
            img.convert("RGBA").resize((size, size), Image.LANCZOS).save(png_path)
            return png_path
        hicon = _extract_hicon(exe_path, size)
        if not hicon:
            return None
        out = _hicon_to_png(hicon, png_path, size)
        ctypes.windll.user32.DestroyIcon(hicon)
        return out
    except Exception:
        return None


def default_icon_png(cache_key: str = "_default_exe", size: int = 32) -> Optional[str]:
    """系统标准图标兜底：普通软件=EXE 图标，MSI 组件=msiexec 安装包图标"""
    png_path = os.path.join(ICON_CACHE, cache_key + ".png")
    if os.path.exists(png_path):
        return png_path
    try:
        if cache_key == "_default_msi":
            win = os.environ.get("WINDIR", r"C:\Windows")
            for cand in (os.path.join(win, "Sysnative", "msiexec.exe"),
                         os.path.join(win, "System32", "msiexec.exe"),
                         os.path.join(win, "SysWOW64", "msiexec.exe")):
                if os.path.exists(cand):
                    out = extract_icon(cand, cache_key, size)
                    if out:
                        return out
        sfi = _SHFILEINFO()
        SHGFI_ICON = 0x100
        SHGFI_LARGEICON = 0x0
        SHGFI_USEFILEATTRIBUTES = 0x10
        FILE_ATTRIBUTE_NORMAL = 0x80
        ctypes.windll.shell32.SHGetFileInfoW(
            "x.exe", FILE_ATTRIBUTE_NORMAL, ctypes.byref(sfi),
            ctypes.sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)
        if not sfi.hIcon:
            return None
        return _hicon_to_png(sfi.hIcon, png_path, size)
    except Exception:
        return None


def _expand_path(p: str) -> str:
    if not isinstance(p, str):
        return ""
    p = p.strip().strip('"')
    if p.startswith("'") and p.endswith("'"):
        p = p[1:-1]
    if "%" in p:
        p = os.path.expandvars(p)
    # 处理 "path,index" / "path, -1"
    if "," in p:
        p = p.split(",")[0].strip().strip('"')
    return p


def _guess_exe_in_dir(loc: str, app_name: str) -> Optional[str]:
    """在安装目录里找最可能的主程序 exe：根目录优先，名字相似优先；限量限时扫描"""
    if not loc or not os.path.isdir(loc):
        return None
    target = norm_name(app_name)
    BAD = ("unins", "uninst", "setup", "crash", "report", "update",
           "helper", "vcredist", "crashreport")

    def score(fp: str, depth: int) -> int:
        f = os.path.basename(fp)
        s = 0
        fn = norm_name(os.path.splitext(f)[0])
        if target and (fn in target or target in fn or fn in target):
            s += 100
        if depth == 0:
            s += 20
        if any(b in f.lower() for b in BAD):
            s -= 50
        try:
            s += min(os.path.getsize(fp) // 1024 // 100, 30)
        except OSError:
            pass
        return s

    # 第一轮：只看根目录（极快）
    root_best = None
    try:
        for f in os.listdir(loc):
            if f.lower().endswith(".exe"):
                fp = os.path.join(loc, f)
                sc = score(fp, 0)
                if root_best is None or sc > root_best[0]:
                    root_best = (sc, fp)
    except OSError:
        return None
    if root_best and root_best[0] >= 100:
        return root_best[1]

    # 第二轮：最多两层子目录，限量 2000 文件 / 3 秒
    candidates = []
    if root_best:
        candidates.append(root_best)
    start = time.time()
    seen = 0
    try:
        for dp, dns, fns in os.walk(loc):
            depth = os.path.relpath(dp, loc).count(os.sep)
            if depth >= 2:
                dns[:] = []
            dns[:] = [d for d in dns if d.lower() not in
                      ("locales", "help", "docs", "node_modules", "resources")]
            for f in fns:
                if f.lower().endswith(".exe"):
                    fp = os.path.join(dp, f)
                    candidates.append((score(fp, depth + 1), fp))
            seen += len(fns)
            if seen > 2000 or time.time() - start > 3:
                break
    except OSError:
        pass
    if candidates:
        candidates.sort(reverse=True)
        return candidates[0][1]
    return None


def resolve_icon_candidates(app: "AppEntry") -> List[str]:
    """按优先级返回所有可能的图标源，供逐个尝试提取"""
    cands: List[str] = []

    def add(p):
        if p and os.path.exists(p) and p not in cands:
            cands.append(p)

    # 1) 注册的 DisplayIcon
    add(_expand_path(app.icon_path))
    # 2) MSI 产品：按 ProductCode GUID 查 C:\Windows\Installer\{GUID} 缓存图标
    m = re.search(r"(\{[0-9A-Fa-f\-]{36}\})", (app.uninstall_cmd or "") + " " + app.reg_subkey)
    if m:
        guid_dir = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Installer", m.group(1))
        if os.path.isdir(guid_dir):
            try:
                for f in sorted(os.listdir(guid_dir)):
                    if f.lower().endswith(".ico"):
                        add(os.path.join(guid_dir, f))
            except OSError:
                pass
    # 3) 安装目录主 exe / 任意 exe
    if app.location and os.path.isdir(app.location):
        exe = _guess_exe_in_dir(app.location, app.name)
        add(exe)
        try:
            for f in os.listdir(app.location):
                if f.lower().endswith((".exe", ".dll")):
                    add(os.path.join(app.location, f))
        except OSError:
            pass
    return cands



# ============================================================
# 已安装软件枚举
# ============================================================
@dataclass
class AppEntry:
    name: str
    publisher: str = ""
    install_date: str = ""
    size_kb: int = 0
    location: str = ""
    uninstall_cmd: str = ""
    quiet_cmd: str = ""
    icon_path: str = ""
    reg_root: int = 0
    reg_subkey: str = ""
    reg_view: int = 0
    system_component: bool = False
    icon_png: Optional[str] = None
    portable: bool = False        # 无注册表卸载项的绿色/便携软件
    appx: bool = False            # 微软商店 UWP/Appx 应用
    package_full_name: str = ""   # Appx 包全名（Remove-AppxPackage 用）
    company: str = ""             # 主程序版本信息中的公司名（同厂商进程反查用）
    uid: str = ""                 # 列表唯一标识（注册表项或便携路径）


def _reg_value(key, name, default=""):
    try:
        v, _ = winreg.QueryValueEx(key, name)
        return v if v not in (None, "") else default
    except OSError:
        return default


def msi_product_map() -> Dict[str, dict]:
    """遍历 Installer\\UserData\\...\\Products，给 MSI 组件补 DisplayIcon/InstallLocation"""
    out: Dict[str, dict] = {}
    base = (r"SOFTWARE\Microsoft\Windows\CurrentVersion\Installer"
            r"\UserData\S-1-5-18\Products")
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        try:
            h = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base, 0,
                               winreg.KEY_READ | view)
        except OSError:
            continue
        try:
            cnt = winreg.QueryInfoKey(h)[0]
        except OSError:
            winreg.CloseKey(h)
            continue
        for i in range(cnt):
            try:
                cn = winreg.EnumKey(h, i)
                with winreg.OpenKey(h, cn + r"\InstallProperties", 0,
                                    winreg.KEY_READ | view) as k:
                    dn = str(_reg_value(k, "DisplayName", ""))
                    if not dn:
                        continue
                    icon = _reg_value(k, "DisplayIcon", "")
                    if isinstance(icon, str) and "," in icon:
                        icon = icon.split(",")[0].strip('" ')
                    loc = str(_reg_value(k, "InstallLocation", "")).strip('" ')
                    out[norm_name(dn)] = {
                        "icon": icon if isinstance(icon, str) else "",
                        "location": loc}
            except OSError:
                continue
        winreg.CloseKey(h)
    return out


def enumerate_apps(progress=None) -> List[AppEntry]:
    apps: Dict[str, AppEntry] = {}
    for root, sub, view in UNINSTALL_KEYS:
        try:
            flags = winreg.KEY_READ | view
            base = winreg.OpenKey(root, sub, 0, flags)
        except OSError:
            continue
        try:
            count = winreg.QueryInfoKey(base)[0]
        except OSError:
            winreg.CloseKey(base)
            continue
        for i in range(count):
            try:
                child_name = winreg.EnumKey(base, i)
                with winreg.OpenKey(base, child_name, 0, flags) as k:
                    name = _reg_value(k, "DisplayName")
                    if not name:
                        continue
                    syscomp = _reg_value(k, "SystemComponent", 0)
                    if syscomp:
                        continue
                    icon = _reg_value(k, "DisplayIcon", "")
                    if isinstance(icon, str) and "," in icon:
                        icon = icon.split(",")[0].strip('" ')
                    loc = _reg_value(k, "InstallLocation", "")
                    if isinstance(loc, str):
                        loc = loc.strip('" ')
                    e = AppEntry(
                        name=name,
                        publisher=str(_reg_value(k, "Publisher", "")),
                        install_date=str(_reg_value(k, "InstallDate", "")),
                        size_kb=int(_reg_value(k, "EstimatedSize", 0) or 0),
                        location=loc if isinstance(loc, str) else "",
                        uninstall_cmd=str(_reg_value(k, "UninstallString", "")),
                        quiet_cmd=str(_reg_value(k, "QuietUninstallString", "")),
                        icon_path=icon if isinstance(icon, str) else "",
                        reg_root=root, reg_subkey=sub + "\\" + child_name, reg_view=view,
                    )
                    e.uid = "REG|" + e.reg_subkey + "|" + str(e.reg_view)
                    if e.name not in apps:
                        apps[e.name] = e
            except OSError:
                continue
        winreg.CloseKey(base)
    # MSI 产品库补全缺失的图标/安装目录
    msi = msi_product_map()
    for e in apps.values():
        if (not e.icon_path or not os.path.exists(_expand_path(e.icon_path))
                or not e.location):
            info = msi.get(norm_name(e.name))
            if info:
                if (not e.icon_path or not os.path.exists(_expand_path(e.icon_path))) and info["icon"]:
                    e.icon_path = info["icon"]
                if not e.location and info["location"]:
                    e.location = info["location"]
    # 日期格式化
    for e in apps.values():
        if re.fullmatch(r"\d{8}", e.install_date):
            e.install_date = f"{e.install_date[:4]}-{e.install_date[4:6]}-{e.install_date[6:]}"
    result = list(apps.values())
    # 补充：无注册表卸载项的绿色/便携软件
    result.extend(enumerate_portable_apps(result))
    # 补充：微软商店 UWP/Appx 应用
    try:
        result.extend(enumerate_appx_packages())
    except Exception:
        pass
    return sorted(result, key=lambda x: x.name.lower())


# ============================================================
# 绿色 / 便携软件识别（无注册表卸载项）
# ============================================================
def file_version_info(path: str) -> dict:
    """读取 exe/dll 的版本资源：ProductName / CompanyName / FileDescription"""
    try:
        size = ctypes.windll.version.GetFileVersionInfoSizeW(path, None)
        if not size:
            return {}
        buf = ctypes.create_string_buffer(size)
        if not ctypes.windll.version.GetFileVersionInfoW(path, 0, size, buf):
            return {}
        lptr = ctypes.c_void_p()
        ulen = ctypes.c_uint()
        cs = "040904b0"
        if ctypes.windll.version.VerQueryValueW(
                buf, r"\VarFileInfo\Translation", ctypes.byref(lptr), ctypes.byref(ulen)) and ulen.value >= 4:
            lang, cp = struct.unpack("<HH", ctypes.string_at(lptr.value, 4))
            cs = f"{lang:04x}{cp:04x}"

        def q(field):
            lp = ctypes.c_void_p()
            u = ctypes.c_uint()
            if ctypes.windll.version.VerQueryValueW(
                    buf, rf"\StringFileInfo\{cs}\{field}", ctypes.byref(lp), ctypes.byref(u)) and u.value:
                return ctypes.wstring_at(lp.value, u.value - 1).strip()
            return ""

        return {"product": q("ProductName"), "company": q("CompanyName"),
                "desc": q("FileDescription")}
    except Exception:
        return {}


# 这些目录即使含 exe 也不是独立软件（系统组件/运行库/商店应用容器等）
PORTABLE_SKIP_DIRS = {
    "common files", "internet explorer", "reference assemblies", "windowsapps",
    "modifiablewindowsapps", "windows defender", "windows defender advanced threat protection",
    "windows mail", "windows media player", "windows multimedia platform", "windows nt",
    "windows photo viewer", "windows portable devices", "windows security", "windowspowershell",
    "microsoft.net", "microsoft office", "microsoft shared", "microsoft update health tools",
    "microsoft edge", "microsoft edgewebview", "package cache", "installer", "temp",
    "packages", "microsoft", "connecteddevicesplatform", "dnx", "publishers", "comms",
    "consolidated", "diagnostics", "virtualstore", "squirreltemp", "appdata",
    "application data", "local settings", "start menu", "startup", "programs",
    "intel", "nvidia corporation", "realtek", "amd", "cypress", "dotnet",
    "google", "mozilla", "common", "desktop", "public", "users", "programdata",
}
# 路径中出现这些片段时，整个子树都不算便携软件（商店容器/共享组件/系统目录）
PORTABLE_SKIP_PATH_PARTS = (
    "\\windowsapps\\", "\\modifiablewindowsapps\\", "\\common files\\",
    "\\windows nt\\", "\\windows defender", "\\windows defender advanced threat protection\\",
    "\\microsoft.net\\", "\\reference assemblies\\", "\\microsoft visual studio\\installer",
    "\\appdata\\local\\packages\\", "\\appdata\\local\\microsoft\\",
    "\\appdata\\roaming\\microsoft\\", "\\programdata\\microsoft\\",
)
# exe 文件名含这些词时，视为安装包/卸载器/运行库，不算便携主程序
PORTABLE_BAD_EXE = ("setup", "install", "unins", "uninst", "安装", "卸载", "下载",
                    "更新", "升级", "补丁", "patch", "redist", "vcredist", "dotnet",
                    "crashreport", "report", "helper", "vstools", "windows-kb", "msiexec",
                    "elevation", "service", "svc", "daemon", "winsys")
# 版本信息里出现这些公司/产品名，视为系统组件，不当作便携软件
PORTABLE_BAD_VENDOR = ("microsoft corporation", "microsoft windows",
                       "microsoft® windows® operating system")
# 这些泛化 exe 名不能直接当软件名，应回退为目录名
PORTABLE_GENERIC_EXE = {"main", "app", "start", "run", "client", "program",
                        "up", "update", "launcher", "boot", "go", "open", "win"}


def _registered_paths(reg_apps: List[AppEntry]) -> set:
    """从注册表软件的安装目录/DisplayIcon/卸载命令反推它们占用的目录，用于去重"""
    out = set()

    def cover(p):
        p = _expand_path(p)
        if p and os.path.exists(p):
            d = os.path.normcase(os.path.normpath(p if os.path.isdir(p) else os.path.dirname(p)))
            out.add(d.rstrip("\\"))

    for a in reg_apps:
        if a.location:
            cover(a.location)
        cover(a.icon_path)
        m = re.search(r'"?([A-Za-z]:\\[^"]+?\.exe)', a.uninstall_cmd or "", re.I)
        if m:
            cover(m.group(1))
    return out


def _pretty_name(folder: str, exe_path: str, ver: dict) -> str:
    for k in ("product", "desc"):
        v = ver.get(k, "")
        v = re.sub(r"[\x00-\x1f]", "", v).strip()
        if v and not re.search(r"setup|install|卸载|安装|operating system", v, re.I):
            # 去掉公司名后缀和版本尾巴
            v = re.sub(r"\s+\d+(\.\d+)+.*$", "", v).strip()
            if len(v) >= 2:
                return v
    name = os.path.splitext(os.path.basename(exe_path))[0]
    if name.lower() in PORTABLE_GENERIC_EXE:
        name = os.path.basename(folder)
    return name.strip() or folder


def enumerate_portable_apps(reg_apps: List[AppEntry]) -> List[AppEntry]:
    """扫描常见落地目录，识别没有注册表卸载项的绿色/便携软件"""
    pf = os.environ.get("ProgramW6432", r"C:\Program Files")
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    la = os.environ.get("LOCALAPPDATA", "")
    ad = os.environ.get("APPDATA", "")
    pd = os.environ.get("ProgramData", r"C:\ProgramData")
    up = os.environ.get("USERPROFILE", "")
    # (根目录, 下钻深度)：程序目录下允许 厂商/产品 两级
    roots = [
        (pf, 2), (pf86, 2),
        (os.path.join(la, "Programs"), 2),
        (la, 1), (ad, 1), (pd, 1),
        (os.path.join(up, "Desktop"), 1),
        (os.path.join(os.environ.get("PUBLIC", r"C:\Users\Public"), "Desktop"), 1),
        (os.path.join(up, "Downloads"), 1),
    ]
    covered = _registered_paths(reg_apps)
    found: Dict[str, AppEntry] = {}
    start = time.time()

    def skip_dir(d):
        n = os.path.basename(d).lower()
        if n in PORTABLE_SKIP_DIRS:
            return True
        if is_protected_path(d):
            return True
        full = os.path.normcase(os.path.normpath(d))
        if any(part in full + "\\" for part in PORTABLE_SKIP_PATH_PARTS):
            return True
        dn = full.rstrip("\\")
        # 已被注册表软件覆盖（目录本身或其父目录）
        for c in covered:
            if dn == c or dn.startswith(c + os.sep):
                return True
        return False

    def consider_dir(d):
        if skip_dir(d):
            return
        try:
            entries = os.listdir(d)
        except OSError:
            return
        exes = [f for f in entries if f.lower().endswith(".exe")]
        if not exes:
            return
        # 根目录主 exe 打分：排除安装包/卸载器，体积大的、名字与目录相似的优先
        best = None
        for f in exes:
            low = f.lower()
            if any(b in low for b in PORTABLE_BAD_EXE):
                continue
            fp = os.path.join(d, f)
            try:
                sz = os.path.getsize(fp)
            except OSError:
                continue
            if sz < 200 * 1024:
                continue
            s = min(sz // 1024 // 100, 30)
            if norm_name(os.path.splitext(f)[0]) in norm_name(os.path.basename(d)):
                s += 50
            if best is None or s > best[0]:
                best = (s, fp)
        if not best:
            return
        exe = best[1]
        ver = file_version_info(exe)
        # 系统组件：公司/产品名命中黑名单则跳过
        blob = (ver.get("company", "") + " " + ver.get("product", "") + " "
                + ver.get("desc", "")).lower()
        if any(b in blob for b in PORTABLE_BAD_VENDOR):
            return
        name = _pretty_name(d, exe, ver)
        key = os.path.normcase(os.path.normpath(d))
        if key in found or any(norm_name(a.name) == norm_name(name) for a in reg_apps) \
                or any(norm_name(a.name) == norm_name(name) for a in found.values()):
            return
        try:
            ctime = datetime.fromtimestamp(os.path.getctime(exe)).strftime("%Y-%m-%d")
        except OSError:
            ctime = ""
        e = AppEntry(
            name=name + "（绿色软件）",
            publisher=ver.get("company", "") or "未注册（无卸载信息）",
            company=ver.get("company", ""),
            install_date=ctime,
            size_kb=_dir_size_limited(d) // 1024,
            location=d,
            icon_path=exe,
            portable=True,
        )
        e.uid = "PORTABLE|" + key
        found[key] = e

    for root, depth in roots:
        if not root or not os.path.isdir(root) or time.time() - start > 20:
            continue
        try:
            level1 = [os.path.join(root, n) for n in os.listdir(root)]
        except OSError:
            continue
        for d1 in level1:
            if not os.path.isdir(d1):
                continue
            consider_dir(d1)
            if depth >= 2:
                try:
                    for n2 in os.listdir(d1):
                        d2 = os.path.join(d1, n2)
                        if os.path.isdir(d2):
                            consider_dir(d2)
                except OSError:
                    pass
    return list(found.values())


# ============================================================
# 微软商店 UWP / Appx 应用识别
# ============================================================
def enumerate_appx_packages() -> List[AppEntry]:
    """通过 PowerShell Get-AppxPackage 枚举当前用户可卸载的商店应用"""
    ps = (
        "Get-AppxPackage | Where-Object { -not $_.IsFramework "
        "-and -not $_.NonRemovable -and $_.SignatureKind -ne 'System' } | ForEach-Object { "
        "'['+$_.Name+']|'+$_.PackageFullName+'|'+$_.InstallLocation+'|'+$_.PublisherDisplayName }"
    )
    out = run_ps(ps, timeout=45)
    # 框架/运行时/语言包等非独立应用不列出
    APPX_SKIP = ("winappruntime", "windowsappruntime", "microsoft.ui.xaml", "vclibs",
                 ".net.native", "services.store.engagement", "framework", "xaml",
                 "microsoft.edgewebview", "pythonsoftwarefoundation", "app.installer",
                 "languageexperiencepack", "widgetsplatform", "gamenotification",
                 "microsoft.sechealth", "microsoft.account", "windows.capturepicker",
                 "microsoftwindows", "desktopappinstaller")
    apps: List[AppEntry] = []
    for line in out.splitlines():
        line = line.strip()
        m = re.match(r"^\[([^\]]+)\]\|([^|]*)\|([^|]*)\|(.*)$", line)
        if not m:
            continue
        name, pfn, loc, pub = m.group(1), m.group(2), m.group(3), m.group(4)
        if any(s in name.lower() for s in APPX_SKIP):
            continue
        loc = loc.strip()
        # 找应用图标：安装目录下的 Logo 图片
        icon = ""
        if loc and os.path.isdir(loc):
            try:
                cands = []
                for dp, _d, fs in os.walk(loc):
                    depth = os.path.relpath(dp, loc).count(os.sep)
                    if depth > 2:
                        continue
                    for f in fs:
                        fl = f.lower()
                        if fl.endswith((".png", ".jpg", ".jpeg")) and "logo" in fl:
                            fp = os.path.join(dp, f)
                            score = 0
                            if "scale-200" in fl or "scale-240" in fl:
                                score += 2
                            if fl.startswith("square44") or fl.startswith("square150") or fl.startswith("logo"):
                                score += 1
                            cands.append((score, os.path.getsize(fp), fp))
                    if len(cands) > 60:
                        break
                if cands:
                    cands.sort(reverse=True)
                    icon = cands[0][2]
            except OSError:
                pass
        e = AppEntry(
            name=name + "（商店应用）",
            publisher=pub or "Microsoft Store",
            location=loc,
            icon_path=icon,
            appx=True,
            package_full_name=pfn,
        )
        e.uid = "APPX|" + pfn
        apps.append(e)
    return apps


def appx_logo_to_png(app: AppEntry, cache_key: str, size: int = 32) -> Optional[str]:
    """把商店应用的 Logo 图片转成统一尺寸 png 缓存"""
    src = app.icon_path
    if not src or not os.path.exists(src):
        return None
    png_path = os.path.join(ICON_CACHE, cache_key + ".png")
    if os.path.exists(png_path):
        return png_path
    try:
        im = Image.open(src).convert("RGBA")
        # 透明底正方形画布，等比缩放居中
        im.thumbnail((size, size), Image.LANCZOS)
        canvas = Image.new("RGBA", (size, size), (255, 255, 255, 0))
        canvas.paste(im, ((size - im.width) // 2, (size - im.height) // 2), im)
        canvas.save(png_path)
        return png_path
    except Exception:
        return None


def remove_appx_package(app: AppEntry, log=print) -> bool:
    """卸载商店应用：Remove-AppxPackage"""
    if not app.package_full_name:
        return False
    log(f"卸载商店应用: {app.package_full_name}")
    ps = f"Remove-AppxPackage -Package '{app.package_full_name}' -ErrorAction Continue 2>&1"
    out = run_ps(ps, timeout=90)
    if out.strip():
        log(out.strip()[:300])
    # 成功判定：包不再可查
    chk = run_ps(
        f"if (Get-AppxPackage | Where-Object {{ $_.PackageFullName -eq '{app.package_full_name}' }}) {{'STILL'}}",
        timeout=30)
    return "STILL" not in chk


# ============================================================
# 残留扫描（含伪装识别）
# ============================================================
def _match_score(folder_name: str, tokens: List[str]) -> int:
    """目录名与软件名 token 的模糊匹配分，识别各种伪装残留"""
    n = norm_name(folder_name)
    if not n:
        return 0
    # 过短的英文名（ai/up/app 等）不参与匹配，防止子串误报
    ascii_len = len(re.sub(r"[^a-z0-9]", "", n))
    cn = re.findall(r"[\u4e00-\u9fff]", n)
    if ascii_len < 3 and len(cn) < 2:
        return 0
    score = 0
    for t in tokens:
        if not t or len(t) < 2:
            continue
        if n == t:
            score += 10
        elif t in n:
            score += 6
        elif n in t and (ascii_len >= 4 or len(cn) >= 2):
            score += 6
        else:
            # 字符集合重合度（识别缩写/乱序伪装）
            common = len(set(n) & set(t))
            if len(t) >= 4 and common / len(set(t)) > 0.8:
                score += 3
    return score


def build_tokens(app: AppEntry) -> List[str]:
    raw = [app.name]
    if app.location:
        raw.append(os.path.basename(app.location.rstrip("\\/")))
    if app.publisher:
        raw.append(app.publisher)
    tokens = set()
    for r in raw:
        tokens.add(norm_name(r))
        # 空格分词后的长词
        for w in re.split(r"[^A-Za-z0-9\u4e00-\u9fff]+", r):
            nw = norm_name(w)
            if len(nw) >= 4:
                tokens.add(nw)
    tokens.discard("")
    return [t for t in tokens if len(t) >= 3]


def app_main_exe(app: "AppEntry") -> str:
    """找软件主 exe，用于读取公司名版本信息"""
    if app.icon_path and app.icon_path.lower().endswith(".exe") and os.path.exists(app.icon_path):
        return app.icon_path
    if app.location and os.path.isdir(app.location):
        return _guess_exe_in_dir(app.location, app.name) or ""
    return ""


def app_company(app: "AppEntry") -> str:
    if app.company:
        return app.company
    exe = app_main_exe(app)
    if exe:
        app.company = file_version_info(exe).get("company", "")
    return app.company or ""


def _blob_hit(blob: str, tokens: List[str], vendor_kw: List[str]) -> bool:
    """路径/名称是否命中软件 token 或厂商别名"""
    b = norm_name(blob)
    if not b:
        return False
    for t in list(tokens) + [norm_name(w) for w in vendor_kw if len(w) >= 3]:
        if not t:
            continue
        if t in b:
            return True
        # 反向包含（短名是长 token 子串）仅对足够长的名称生效，避免 ai/up 等误报
        if b in t and (len(b) >= 4 or re.fullmatch(r"[\u4e00-\u9fff]{2,}", b)):
            return True
    return False


# ---------- 浏览器扩展 ----------
def _msg_name(raw: str, ext_dir: str) -> str:
    """解析 __MSG_xxx__ 形式的扩展名称，读 _locales 语言文件"""
    m = re.match(r"__MSG_([A-Za-z0-9_]+)__", raw or "")
    if not m:
        return raw or ""
    key = m.group(1).lower()
    locales = os.path.join(ext_dir, "_locales")
    if not os.path.isdir(locales):
        return raw
    order = ["zh_cn", "zh", "en", "en_us", "en_gb"]
    langs = order + [d for d in os.listdir(locales)
                     if os.path.isdir(os.path.join(locales, d)) and d.lower() not in order]
    for lang in langs:
        mf = os.path.join(locales, lang, "messages.json")
        if os.path.exists(mf):
            try:
                with open(mf, "r", encoding="utf-8", errors="ignore") as f:
                    data = json.load(f)
                v = data.get(key) or data.get(key.title())
                if isinstance(v, dict) and v.get("message"):
                    return v["message"]
            except (OSError, ValueError):
                pass
    return raw


def scan_browser_extensions(tokens, vendor_kw, add, log):
    """扫描 Chrome/Edge/360浏览器等 Chromium 内核浏览器的扩展与外部扩展注册表"""
    la = os.environ.get("LOCALAPPDATA", "")
    user_data_dirs = [
        os.path.join(la, r"Google\Chrome\User Data"),
        os.path.join(la, r"Microsoft\Edge\User Data"),
        os.path.join(la, r"360Chrome\Chrome\User Data"),
        os.path.join(la, r"360ChromeX\Chrome\User Data"),
        os.path.join(la, r"Chromium\User Data"),
        os.path.join(la, r"BraveSoftware\Brave-Browser\User Data"),
    ]
    for ud in user_data_dirs:
        if not os.path.isdir(ud):
            continue
        browser = os.path.basename(os.path.dirname(ud))
        try:
            profiles = os.listdir(ud)
        except OSError:
            continue
        for prof in profiles:
            ext_root = os.path.join(ud, prof, "Extensions")
            if not os.path.isdir(ext_root):
                continue
            try:
                ext_ids = os.listdir(ext_root)
            except OSError:
                continue
            for eid in ext_ids:
                idir = os.path.join(ext_root, eid)
                if not os.path.isdir(idir):
                    continue
                try:
                    vers = [v for v in os.listdir(idir)
                            if os.path.isdir(os.path.join(idir, v)) and re.match(r"[\d.]+", v)]
                except OSError:
                    continue
                if not vers:
                    continue
                vdir = os.path.join(idir, sorted(vers)[-1])
                mf = os.path.join(vdir, "manifest.json")
                name = desc = ""
                try:
                    with open(mf, "r", encoding="utf-8", errors="ignore") as f:
                        mani = json.load(f)
                    name = _msg_name(str(mani.get("name", "")), vdir)
                    desc = _msg_name(str(mani.get("description", "")), vdir)
                except (OSError, ValueError):
                    pass
                blob = f"{name} {desc} {eid} {browser}"
                if _blob_hit(blob, tokens, vendor_kw):
                    log(f"发现浏览器扩展: {name or eid}（{browser}）")
                    add("dir", idir, 9)
    # 外部扩展注册表（企业白名单/安装器写入）
    ext_reg_bases = [
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Google\Chrome\Extensions", winreg.KEY_WOW64_64KEY),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Google\Chrome\Extensions", winreg.KEY_WOW64_32KEY),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Google\Chrome\Extensions", 0),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Edge\Extensions", winreg.KEY_WOW64_64KEY),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Edge\Extensions", winreg.KEY_WOW64_32KEY),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Edge\Extensions", 0),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Google\Chrome\Extensions", winreg.KEY_WOW64_64KEY),
    ]
    for root, sub, view in ext_reg_bases:
        try:
            base = winreg.OpenKey(root, sub, 0, winreg.KEY_READ | view)
        except OSError:
            continue
        try:
            for i in range(winreg.QueryInfoKey(base)[0]):
                try:
                    cn = winreg.EnumKey(base, i)
                    full = sub + "\\" + cn
                    path_val = ""
                    with winreg.OpenKey(base, cn, 0, winreg.KEY_READ | view) as k:
                        try:
                            path_val, _ = winreg.QueryValueEx(k, "path")
                        except OSError:
                            path_val = ""
                        try:
                            url, _ = winreg.QueryValueEx(k, "update_url")
                        except OSError:
                            url = ""
                    if _blob_hit(f"{cn} {path_val} {url}", tokens, vendor_kw):
                        add("reg", "", 9, {"root": root, "subkey": full, "view": view})
                except OSError:
                    continue
        finally:
            winreg.CloseKey(base)


# ---------- 运行进程：按公司名/路径反查随机目录 ----------
def vendor_root_in_path(path: str, tokens, vendor_kw):
    """在路径中定位含厂商关键字的那一级目录（产品根），如 ...\\360\\360Safe\\deepscan → 360Safe"""
    parts = os.path.normpath(path).split(os.sep)
    # 最后一段若是文件名（含扩展名），不参与目录匹配
    last_dir = len(parts)
    if "." in parts[-1]:
        last_dir -= 1
    hit_root = None
    keys = list(tokens) + [norm_name(w) for w in vendor_kw if len(w) >= 3]
    for i in range(1, last_dir):
        seg = norm_name(parts[i])
        if not seg:
            continue
        for t in keys:
            if t and (t == seg or (len(t) >= 3 and t in seg)):
                hit_root = os.sep.join(parts[:i + 1])
                break
    return hit_root


def _same_company_dir(full: str, company_norm: str) -> bool:
    """目录内任一 exe 的版本公司名与给定公司名相同（随机目录识别的核心）"""
    checked = 0
    try:
        for dp, _d, fs in os.walk(full):
            for f in fs:
                if f.lower().endswith(".exe"):
                    checked += 1
                    c = norm_name(file_version_info(os.path.join(dp, f)).get("company", ""))
                    if c and c == company_norm:
                        return True
                if checked >= 12:
                    return False
    except OSError:
        return False
    return False


def add_sibling_dirs(parent: str, self_full, company_norm, tokens, vendor_kw, add, log):
    """枚举 parent 下与 self_full 同级的同厂商兄弟目录（全家桶/随机目录）"""
    if not parent or not os.path.isdir(parent) or is_protected_path(parent):
        return
    try:
        names = os.listdir(parent)
    except OSError:
        return
    for name in names:
        full = os.path.join(parent, name)
        if not os.path.isdir(full):
            continue
        if self_full and os.path.normcase(full) == os.path.normcase(os.path.normpath(self_full)):
            continue
        if is_protected_path(full):
            continue
        hit = _blob_hit(name, tokens, vendor_kw)
        if not hit and company_norm:
            hit = _same_company_dir(full, company_norm)
        if hit:
            log(f"发现同厂商兄弟目录（疑似全家桶/随机目录）: {full}")
            add("dir", full, 8)


def scan_running_processes(app, tokens, vendor_kw, add, log):
    company = norm_name(app_company(app))
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath } | "
          "ForEach-Object { \"$($_.ProcessId)|$($_.ExecutablePath)\" }")
    out = run_ps(ps, timeout=40)
    roots = [r for r in RESIDUAL_ROOTS if r] + [tempfile.gettempdir()]
    start = time.time()
    vendor_roots = set()
    for line in out.splitlines():
        if time.time() - start > 25:
            break
        parts = line.split("|", 1)
        if len(parts) != 2 or not parts[0].strip().isdigit():
            continue
        pid, exe = parts[0].strip(), parts[1].strip()
        if not exe or not os.path.exists(exe):
            continue
        if _is_system_path(exe) or not _path_in_roots(exe, roots):
            continue
        hit = _blob_hit(exe, tokens, vendor_kw)
        if not hit and company:
            proc_company = norm_name(file_version_info(exe).get("company", ""))
            if proc_company and proc_company == company:
                hit = True
        if hit:
            log(f"发现同厂商进程: {exe} (PID {pid})")
            # 上溯到含厂商关键字的产品根目录（整个产品而非单个 bin 目录）
            vroot = vendor_root_in_path(exe, tokens, vendor_kw)
            target = vroot or os.path.dirname(exe)
            add("dir", target, 9, action={"kill": [int(pid)]})
            if vroot:
                vendor_roots.add(vroot)
    # 进程产品根的同级兄弟目录（如 360Safe 旁边的 360TptMon/360DrvMgr）
    for vr in vendor_roots:
        add_sibling_dirs(os.path.dirname(vr), vr, company, tokens, vendor_kw, add, log)


# ---------- 服务 / 内核驱动 ----------
def _parse_svc_path(raw: str) -> str:
    s = (raw or "").strip()
    s = s.replace(r"\??\\", "").replace(r"\SystemRoot", os.environ.get("WINDIR", r"C:\Windows"))
    if s.lower().startswith("svchost"):
        return ""
    m = re.match(r'"([^"]+)"', s)
    if m:
        return m.group(1)
    m = re.match(r"([A-Za-z]:\\\S+?\.sys)", s, re.I)
    if m:
        return m.group(1)
    m = re.match(r"([A-Za-z]:\\\S+?\.exe)", s, re.I)
    return m.group(1) if m else ""


def scan_services(app, tokens, vendor_kw, add, log):
    ps = ("@('Win32_Service','Win32_SystemDriver') | ForEach-Object { "
          "Get-CimInstance $_ -ErrorAction SilentlyContinue } | "
          "Where-Object { $_.PathName } | ForEach-Object { \"$($_.Name)|$($_.State)|$($_.PathName)\" }")
    out = run_ps(ps, timeout=40)
    for line in out.splitlines():
        parts = line.split("|", 2)
        if len(parts) != 3:
            continue
        svc, state, rawpath = parts
        exe = _parse_svc_path(parts[2])
        if not exe:
            continue
        in_loc = app.location and os.path.normcase(exe).startswith(
            os.path.normcase(os.path.normpath(app.location)) + os.sep)
        if not in_loc and not _blob_hit(exe + " " + svc, tokens, vendor_kw):
            continue
        log(f"发现同厂商服务/驱动: {svc}（{state}）→ {exe}")
        item_kind = "file" if os.path.isfile(exe) else "dir"
        # Windows 系统目录内的 sys/dll 不删文件（受保护），只删服务
        if _is_system_path(exe):
            add("reg", "", 10, action={"svc": [svc]},
                reg={"root": winreg.HKEY_LOCAL_MACHINE,
                     "subkey": r"SYSTEM\CurrentControlSet\Services\\" + svc,
                     "view": winreg.KEY_WOW64_64KEY, "_svc_only": True})
            continue
        add(item_kind, exe if item_kind == "file" else os.path.dirname(exe), 9,
            action={"svc": [svc]})


# ---------- 计划任务 ----------
def scan_scheduled_tasks(app, tokens, vendor_kw, add, log):
    ps = ("Get-ScheduledTask -ErrorAction SilentlyContinue | ForEach-Object { "
          "$t = $_.TaskPath + $_.TaskName; foreach ($a in $_.Actions) { "
          "\"$t|$($a.Execute)|$($a.Arguments)\" } }")
    out = run_ps(ps, timeout=40)
    for line in out.splitlines():
        parts = line.split("|", 2)
        if len(parts) != 3:
            continue
        task, exe, args = parts
        blob = f"{exe} {args} {task}"
        in_loc = app.location and exe and os.path.normcase(exe).startswith(
            os.path.normcase(os.path.normpath(app.location)) + os.sep)
        if not in_loc and not _blob_hit(blob, tokens, vendor_kw):
            continue
        log(f"发现同厂商计划任务: {task}")
        if exe and os.path.exists(exe) and not _is_system_path(exe):
            add("dir", os.path.dirname(exe), 8, action={"task": [task]})
        else:
            # 无可删目录，也确保任务被删除：挂到一个占位 reg 项不合适，用特殊 file? 用 task_only
            add("task_only", task, 8, action={"task": [task]})


# ---------- 开机自启动注册表值 ----------
RUN_KEYS = [
    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Run", winreg.KEY_WOW64_64KEY),
    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Run", winreg.KEY_WOW64_32KEY),
    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce", winreg.KEY_WOW64_64KEY),
    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run", winreg.KEY_WOW64_64KEY),
    (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Run", 0),
    (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce", 0),
]


def scan_run_keys(app, tokens, vendor_kw, add, log):
    for root, sub, view in RUN_KEYS:
        try:
            k = winreg.OpenKey(root, sub, 0, winreg.KEY_READ | view)
        except OSError:
            continue
        try:
            for i in range(winreg.QueryInfoKey(k)[1]):
                try:
                    vn, vd, _ = winreg.EnumValue(k, i)
                except OSError:
                    continue
                if not isinstance(vd, str):
                    continue
                if _blob_hit(f"{vn} {vd}", tokens, vendor_kw) or (
                        app.location and app.location.lower() in vd.lower()):
                    log(f"发现开机自启动项: {sub}\\{vn} = {vd[:80]}")
                    add("regvalue", "", 9,
                        reg={"root": root, "subkey": sub, "view": view, "value": vn})
        finally:
            winreg.CloseKey(k)


# ---------- 厂商父目录下的兄弟产品（全家桶 / 随机目录） ----------
def scan_vendor_siblings(app, tokens, vendor_kw, add, log):
    if not app.location:
        return
    vroot = vendor_root_in_path(os.path.join(app.location, "x"), tokens, vendor_kw) \
        or vendor_root_in_path(app.location, tokens, vendor_kw)
    if not vroot or not os.path.isdir(vroot):
        return
    company = norm_name(app_company(app))
    # 厂商根本身（若比安装目录更靠上，如安装目录是 360SoftMgr，厂商段就是它自己）
    if os.path.normcase(os.path.normpath(vroot)) != \
            os.path.normcase(os.path.normpath(app.location)):
        add("dir", vroot, 8)
    add_sibling_dirs(os.path.dirname(vroot), vroot, company, tokens, vendor_kw, add, log)


def scan_residual(app: AppEntry, log=print) -> List[dict]:
    """返回残留项 [{type, path, reg(root,subkey[,value]), size, score, action}]"""
    results = []
    seen = set()

    def add(kind, path, score=10, reg=None, action=None):
        # 系统保护：文件/目录路径硬过滤
        if kind in ("dir", "file") and path and is_protected_path(path):
            return
        # 系统保护：注册表键硬过滤（纯服务删除项除外）
        if kind == "reg" and reg and not reg.get("_svc_only") and is_protected_reg(reg["subkey"]):
            return
        key = (kind, path.lower() if path else json.dumps(reg, default=str))
        if key in seen:
            # 已存在则合并 action
            for r in results:
                rk = (r["type"], r["path"].lower() if r["path"] else json.dumps(r.get("reg"), default=str))
                if rk == key and action:
                    if not r.get("action"):
                        r["action"] = {}
                    for k, v in action.items():
                        r["action"].setdefault(k, [])
                        r["action"][k] = list(dict.fromkeys(r["action"][k] + v))
            return
        seen.add(key)
        sz = 0
        if kind in ("dir", "file") and path and os.path.exists(path):
            if kind == "file":
                try:
                    sz = os.path.getsize(path)
                except OSError:
                    pass
            else:
                sz = _dir_size_limited(path, budget=2.0)
        results.append({"type": kind, "path": path, "size": sz, "score": score,
                        "reg": reg, "action": action})

    tokens = build_tokens(app)
    vendor_kw = vendor_keywords(app)
    log(f"匹配关键字: {tokens}")
    if vendor_kw:
        log(f"厂商全家桶关键字: {vendor_kw}")

    # 1) 安装目录（若卸载后仍存在）
    if app.location and os.path.isdir(app.location) and not app.appx:
        add("dir", app.location, 10)

    # 2) 常见根目录下模糊匹配（伪装残留）
    roots = [r for r in RESIDUAL_ROOTS if r and os.path.isdir(r)]
    for root in roots:
        try:
            for name in os.listdir(root):
                full = os.path.join(root, name)
                if os.path.isdir(full):
                    sc = _match_score(name, tokens)
                    if sc >= 6 or _blob_hit(name, [], vendor_kw):
                        add("dir", full, max(sc, 7))
        except OSError:
            pass

    # 3) 开始菜单 / 桌面快捷方式
    for d in START_MENU_DIRS + DESKTOP_DIRS:
        if not d or not os.path.isdir(d):
            continue
        for dp, _, files in os.walk(d):
            for f in files:
                if f.lower().endswith((".lnk", ".url")):
                    sc = _match_score(os.path.splitext(f)[0], tokens)
                    if sc >= 6 or _blob_hit(os.path.splitext(f)[0], [], vendor_kw):
                        add("file", os.path.join(dp, f), sc or 7)

    # 4) 注册表：卸载键残留 + Software\厂商 键
    for root, sub, view in UNINSTALL_KEYS:
        try:
            with winreg.OpenKey(root, sub, 0, winreg.KEY_READ | view) as base:
                cnt = winreg.QueryInfoKey(base)[0]
                for i in range(cnt):
                    try:
                        cn = winreg.EnumKey(base, i)
                        full = sub + "\\" + cn
                        with winreg.OpenKey(base, cn, 0, winreg.KEY_READ | view) as k:
                            dn = str(_reg_value(k, "DisplayName", cn))
                        if _match_score(dn, tokens) >= 6 or norm_name(cn) in tokens \
                                or _blob_hit(cn + " " + dn, [], vendor_kw):
                            add("reg", "", 10, {"root": root, "subkey": full, "view": view})
                    except OSError:
                        continue
        except OSError:
            pass
    # Software 下厂商/产品键
    for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        for view in (0, winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
            try:
                with winreg.OpenKey(hive, r"SOFTWARE", 0, winreg.KEY_READ | view) as base:
                    for i in range(winreg.QueryInfoKey(base)[0]):
                        try:
                            cn = winreg.EnumKey(base, i)
                        except OSError:
                            continue
                        sc = _match_score(cn, tokens)
                        if sc >= 6 or _blob_hit(cn, [], vendor_kw):
                            add("reg", "", max(sc, 7),
                                {"root": hive, "subkey": r"SOFTWARE\\" + cn, "view": view})
            except OSError:
                pass

    # 5) Temp 目录
    tmp = tempfile.gettempdir()
    try:
        for name in os.listdir(tmp):
            sc = _match_score(name, tokens)
            if sc >= 10 or _blob_hit(name, [], vendor_kw):
                full = os.path.join(tmp, name)
                add("dir" if os.path.isdir(full) else "file", full, max(sc, 7))
    except OSError:
        pass

    # 6) 浏览器扩展（Chrome / Edge / 360 等 Chromium 内核）
    if not app.appx:
        try:
            scan_browser_extensions(tokens, vendor_kw, add, log)
        except Exception as e:
            log(f"浏览器扩展扫描失败: {e}")

    # 7) 运行进程按公司名/路径反查（识别随机目录守护进程）
    if not app.appx:
        try:
            scan_running_processes(app, tokens, vendor_kw, add, log)
        except Exception as e:
            log(f"进程扫描失败: {e}")

    # 8) 服务 / 内核驱动
    if not app.appx:
        try:
            scan_services(app, tokens, vendor_kw, add, log)
        except Exception as e:
            log(f"服务扫描失败: {e}")

    # 9) 计划任务
    if not app.appx:
        try:
            scan_scheduled_tasks(app, tokens, vendor_kw, add, log)
        except Exception as e:
            log(f"计划任务扫描失败: {e}")

    # 10) 开机自启动注册表值
    if not app.appx:
        try:
            scan_run_keys(app, tokens, vendor_kw, add, log)
        except Exception as e:
            log(f"自启动项扫描失败: {e}")

    # 11) 厂商父目录下的兄弟产品（全家桶 / 随机目录）
    if not app.appx:
        try:
            scan_vendor_siblings(app, tokens, vendor_kw, add, log)
        except Exception as e:
            log(f"兄弟目录扫描失败: {e}")

    results.sort(key=lambda x: -x["score"])
    return results


def _dir_size(path: str) -> int:
    total = 0
    try:
        for dp, _, files in os.walk(path):
            for f in files:
                try:
                    total += os.path.getsize(os.path.join(dp, f))
                except OSError:
                    pass
    except OSError:
        pass
    return total


def _dir_size_limited(path: str, budget: float = 1.5, max_files: int = 60000) -> int:
    """限时/限量的目录大小统计，超时返回已统计部分（绿色软件列表用，避免卡住界面）"""
    total = 0
    n = 0
    start = time.time()
    try:
        for dp, _, files in os.walk(path):
            for f in files:
                try:
                    total += os.path.getsize(os.path.join(dp, f))
                except OSError:
                    pass
                n += 1
                if n > max_files or time.time() - start > budget:
                    return total
    except OSError:
        pass
    return total


# ============================================================
# 删除引擎
# ============================================================
def schedule_reboot_delete(path: str) -> bool:
    """MoveFileEx MOVEFILE_DELAY_UNTIL_REBOOT：重启时由系统删除（可处理占用的 dll/sys）"""
    p = os.path.abspath(path)
    # 先标记目录内文件，再标记目录
    targets = []
    if os.path.isdir(p):
        for dp, _, files in os.walk(p):
            for f in files:
                targets.append(os.path.join(dp, f))
        targets.append(p)
    else:
        targets.append(p)
    ok = True
    for t in targets:
        if not ctypes.windll.kernel32.MoveFileExW(t, None, MOVEFILE_DELAY_UNTIL_REBOOT):
            ok = False
    return ok


def force_takeown(path: str):
    try:
        subprocess.run(["takeown", "/F", path, "/R", "/D", "Y"],
                       capture_output=True, timeout=60,
                       creationflags=0x08000000)
    except Exception:
        pass
    try:
        subprocess.run(["icacls", path, "/grant", "*S-1-5-32-544:F", "/T", "/C", "/Q"],
                       capture_output=True, timeout=60,
                       creationflags=0x08000000)
    except Exception:
        pass


def _procs_under_path(loc: str):
    """WMI 查找可执行路径位于安装目录下的所有进程（含改名/随机名的守护进程）"""
    pids = []
    loc_norm = os.path.normcase(os.path.normpath(loc))
    ps = (
        "Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and "
        "$_.ExecutablePath -like '" + loc.replace("'", "''") + "*' } | "
        "Select-Object -ExpandProperty ProcessId"
    )
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, encoding="gbk", errors="ignore", timeout=30,
                             creationflags=0x08000000)
        for line in out.stdout.split():
            if line.isdigit():
                pids.append(int(line))
    except Exception:
        pass
    return pids


def kill_processes_using(app: AppEntry, log=print):
    """多轮绞杀：按 exe 名 + 按安装目录路径(WMI) 双保险，杀掉互保/改名复活的看门狗"""
    names = set()
    if app.location and os.path.isdir(app.location):
        for dp, _, files in os.walk(app.location):
            for f in files:
                if f.lower().endswith(".exe"):
                    names.add(f)
    guess = re.sub(r"[^A-Za-z0-9]+", "", app.name)
    if guess:
        names.add(guess + ".exe")

    ROUNDS = 4
    for rnd in range(1, ROUNDS + 1):
        killed = 0
        # 1) 按进程映像名杀（同名所有实例 + 进程树）
        for n in names:
            r = subprocess.run(["taskkill", "/F", "/T", "/IM", n],
                               capture_output=True, text=True, encoding="gbk", errors="ignore",
                               creationflags=0x08000000)
            if r.returncode == 0:
                killed += 1
                log(f"[第{rnd}轮] 结束进程 {n}")
        # 2) 按安装目录路径杀（守护进程改名也逃不掉）
        if app.location:
            for pid in _procs_under_path(app.location):
                r = subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                                   capture_output=True, text=True, encoding="gbk", errors="ignore",
                                   creationflags=0x08000000)
                if r.returncode == 0:
                    killed += 1
                    log(f"[第{rnd}轮] 按路径结束 PID {pid}")
        if killed == 0 and rnd > 1:
            break
        time.sleep(0.8)   # 等守护进程复活，下一轮再杀，连续两轮无生还即清场


def stop_driver_services(path: str, log=print):
    """sys 驱动文件：先停止并删除对应服务/驱动"""
    try:
        out = subprocess.run(["sc", "query", "type=", "driver", "state=", "all"],
                             capture_output=True, text=True, encoding="gbk", errors="ignore", timeout=30,
                             creationflags=0x08000000).stdout
    except Exception:
        return
    names = re.findall(r"SERVICE_NAME:\s*(\S+)", out)
    for svc in names:
        try:
            cfg = subprocess.run(["sc", "qc", svc], capture_output=True, text=True, encoding="gbk", errors="ignore",
                                 timeout=10, creationflags=0x08000000).stdout
            m = re.search(r"BINARY_PATH_NAME\s*:\s*(.+)", cfg)
            if m and os.path.basename(path).lower() in m.group(1).lower():
                log(f"停止并删除驱动服务: {svc}")
                subprocess.run(["sc", "stop", svc], capture_output=True,
                               creationflags=0x08000000)
                time.sleep(1)
                subprocess.run(["sc", "delete", svc], capture_output=True,
                               creationflags=0x08000000)
        except Exception:
            continue


def force_delete(path: str, log=print, reboot_list: Optional[list] = None) -> bool:
    """强删文件/目录，含占用处理；失败则标记重启删除"""
    if reboot_list is None:
        reboot_list = []
    if not os.path.exists(path):
        return True
    if is_protected_path(path):
        log(f"[拦截] 系统保护路径，拒绝删除: {path}")
        return False
    log(f"强制删除: {path}")
    if os.path.isfile(path):
        if path.lower().endswith(".sys"):
            stop_driver_services(path, log)
        force_takeown(path)
        # 清除只读/系统/隐藏属性
        ctypes.windll.kernel32.SetFileAttributesW(path, 0x80)
        try:
            os.chmod(path, 0o777)
            os.remove(path)
            return True
        except OSError:
            pass
        # 短路径再试
        try:
            buf = ctypes.create_unicode_buffer(260)
            n = ctypes.windll.kernel32.GetShortPathNameW(path, buf, 260)
            if n:
                os.remove(buf.value)
                return True
        except OSError:
            pass
        if schedule_reboot_delete(path):
            log("  → 已被占用，标记为重启时删除")
            reboot_list.append(path)
            return True
        return False

    # 目录：先递归强删内部
    force_takeown(path)
    ok = True
    for dp, dns, fns in os.walk(path, topdown=False):
        for f in fns:
            fp = os.path.join(dp, f)
            if not force_delete(fp, log, reboot_list):
                ok = False
    try:
        shutil.rmtree(path, ignore_errors=False)
        return True
    except OSError:
        try:
            os.rmdir(path)
            return True
        except OSError:
            if os.path.exists(path):
                if schedule_reboot_delete(path):
                    log("  → 目录非空/被占用，标记为重启时删除")
                    reboot_list.append(path)
                    return True
                return False
            return True


def delete_reg(reg: dict, log=print) -> bool:
    try:
        root, sub, view = reg["root"], reg["subkey"], reg.get("view", 0)
        if is_protected_reg(sub):
            log(f"[拦截] 系统保护注册表项，拒绝删除: {sub}")
            return False
        # 递归删除注册表键
        def del_recursive(hive, path, flag):
            with winreg.OpenKey(hive, path, 0, winreg.KEY_ALL_ACCESS | flag) as k:
                while True:
                    try:
                        child = winreg.EnumKey(k, 0)
                    except OSError:
                        break
                    del_recursive(hive, path + "\\" + child, flag)
            winreg.DeleteKey(hive if False else root, sub) if False else None
        # 用 reg delete 更稳；32 位进程下 reg.exe 默认走 32 位视图，必须显式指定 /reg:64
        rootname = "HKLM" if root == winreg.HKEY_LOCAL_MACHINE else "HKCU"
        view_flag = "/reg:64" if view == winreg.KEY_WOW64_64KEY else (
            "/reg:32" if view == winreg.KEY_WOW64_32KEY else "")
        cmd = ["reg", "delete", rootname + "\\" + sub, "/f"] + ([view_flag] if view_flag else [])
        r = subprocess.run(cmd, capture_output=True, creationflags=0x08000000)
        if r.returncode == 0:
            log(f"删除注册表项: {sub}")
            return True
        return False
    except Exception as e:
        log(f"注册表删除失败 {sub}: {e}")
        return False


def delete_reg_value(reg: dict, log=print) -> bool:
    """删除注册表中的某个值（Run 自启动项）"""
    try:
        root, sub, view, value = reg["root"], reg["subkey"], reg.get("view", 0), reg.get("value")
        rootname = "HKLM" if root == winreg.HKEY_LOCAL_MACHINE else "HKCU"
        view_flag = "/reg:64" if view == winreg.KEY_WOW64_64KEY else (
            "/reg:32" if view == winreg.KEY_WOW64_32KEY else "")
        cmd = ["reg", "delete", rootname + "\\" + sub, "/v", value, "/f"] + \
              ([view_flag] if view_flag else [])
        r = subprocess.run(cmd, capture_output=True, creationflags=NO_WINDOW)
        if r.returncode == 0:
            log(f"删除自启动值: {sub}\\{value}")
            return True
        return False
    except Exception as e:
        log(f"自启动值删除失败: {e}")
        return False


def perform_actions(item: dict, log=print):
    """删除残留前先执行关联动作：杀进程、停删服务/驱动、删计划任务"""
    act = item.get("action") or {}
    for pid in act.get("kill", []):
        r = subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                           capture_output=True, text=True, encoding="gbk", errors="ignore",
                           creationflags=NO_WINDOW)
        if r.returncode == 0:
            log(f"结束同厂商进程 PID {pid}")
    for svc in act.get("svc", []):
        log(f"停止并删除服务/驱动: {svc}")
        subprocess.run(["sc", "stop", svc], capture_output=True, creationflags=NO_WINDOW)
        time.sleep(0.8)
        r = subprocess.run(["sc", "delete", svc], capture_output=True,
                           text=True, encoding="gbk", errors="ignore", creationflags=NO_WINDOW)
        if r.returncode != 0:
            delete_reg({"root": winreg.HKEY_LOCAL_MACHINE,
                        "subkey": r"SYSTEM\CurrentControlSet\Services\\" + svc,
                        "view": winreg.KEY_WOW64_64KEY}, log)
    for task in act.get("task", []):
        r = subprocess.run(["schtasks", "/delete", "/tn", task, "/f"],
                           capture_output=True, text=True, encoding="gbk", errors="ignore",
                           creationflags=NO_WINDOW)
        if r.returncode == 0:
            log(f"删除计划任务: {task}")


def reboot_now(log=print) -> bool:
    """立即重启电脑（调用前应已写好重启删除标记）"""
    EWX_REBOOT = 0x2
    EWX_FORCEIFHUNG = 0x10
    SHTDN_REASON_MAJOR_SOFTWARE = 0x2
    SHTDN_REASON_MINOR_INSTALLATION = 0x3
    SHTDN_REASON_FLAG_PLANNED = 0x80000000
    reason = SHTDN_REASON_FLAG_PLANNED | (SHTDN_REASON_MAJOR_SOFTWARE << 16) | \
        (SHTDN_REASON_MINOR_INSTALLATION << 2)
    try:
        if ctypes.windll.user32.ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, reason):
            return True
    except Exception:
        pass
    try:
        subprocess.run(["shutdown", "/r", "/t", "5", "/c",
                        "Mini Geek Uninstaller 完成重启删除"], creationflags=NO_WINDOW)
        return True
    except Exception as e:
        log(f"重启失败: {e}")
        return False


def ask_reboot_if_needed(reboot_list, parent=None):
    """有重启删除项时询问用户是否立即重启；点“是”自动重启"""
    if not reboot_list:
        return
    ans = messagebox.askyesno(
        "需要重启电脑",
        f"有 {len(reboot_list)} 个文件/目录正在被占用，已标记为重启后自动删除。\n\n"
        "点“是”：电脑将在 5 秒后自动重启，重启后这些项目会被系统删除；\n"
        "点“否”：保留标记，你下次手动重启时同样会删除。",
        parent=parent)
    if ans:
        reboot_now()


# ============================================================
# 安全模式离线清理（对付杀毒软件内核级自我保护）
# ============================================================
SAFEMODE_DIR = r"C:\ProgramData\MiniGeekSafeClean"
SAFEMODE_EXE = os.path.join(SAFEMODE_DIR, "clean.exe")
SAFEMODE_PLAN = os.path.join(SAFEMODE_DIR, "pending.json")
SAFEMODE_LOG = os.path.join(SAFEMODE_DIR, "clean.log")
SAFEMODE_TASK = "MiniGeekSafeClean"


def prepare_safemode_cleanup(items: list, log=print) -> bool:
    """落盘删除清单 + 复制自身 + 注册 SYSTEM 开机任务 + 配置安全模式引导"""
    try:
        os.makedirs(SAFEMODE_DIR, exist_ok=True)
        # 1) 序列化删除清单
        plan = []
        for it in items:
            plan.append({
                "type": it["type"], "path": it.get("path") or "",
                "reg": it.get("reg"), "action": it.get("action"),
            })
        with open(SAFEMODE_PLAN, "w", encoding="utf-8") as f:
            json.dump(plan, f, ensure_ascii=False, indent=1)
        # 2) 复制自身到 ProgramData（不依赖外部盘）
        src = sys.executable
        if os.path.exists(src):
            shutil.copy2(src, SAFEMODE_EXE)
        else:
            log("找不到自身 exe，无法部署安全模式清理程序")
            return False
        # 3) 注册 SYSTEM 权限、开机即运行的计划任务（安全模式登录前执行，无需 UAC）
        subprocess.run(["schtasks", "/delete", "/tn", SAFEMODE_TASK, "/f"],
                       capture_output=True, creationflags=NO_WINDOW)
        tr = f'"{SAFEMODE_EXE}" --safemode-clean'
        r = subprocess.run(["schtasks", "/create", "/tn", SAFEMODE_TASK, "/tr", tr,
                            "/sc", "onstart", "/ru", "SYSTEM", "/rl", "HIGHEST", "/f"],
                           capture_output=True, text=True, encoding="gbk", errors="ignore",
                           creationflags=NO_WINDOW)
        if r.returncode != 0:
            log("计划任务创建失败: " + (r.stderr or r.stdout or ""))
            return False
        # 4) 配置安全模式引导
        r1 = subprocess.run(["bcdedit", "/set", "{current}", "safeboot", "minimal"],
                            capture_output=True, text=True, encoding="gbk", errors="ignore",
                            creationflags=NO_WINDOW)
        if r1.returncode != 0:
            log("bcdedit 配置安全模式失败: " + (r1.stderr or r1.stdout or ""))
            return False
        log("安全模式清理已部署：重启后将自动进入安全模式执行删除，完成后自动回到正常模式。")
        return True
    except Exception as e:
        log(f"部署安全模式清理失败: {e}")
        return False


def reboot_to_safemode():
    subprocess.run(["shutdown", "/r", "/t", "6", "/c",
                    "Mini Geek Uninstaller：即将进入安全模式彻底清理"], creationflags=NO_WINDOW)


def run_safemode_cleanup():
    """安全模式下由计划任务以 SYSTEM 身份调用：执行清单、恢复引导、重启回正常模式"""
    try:
        os.makedirs(SAFEMODE_DIR, exist_ok=True)
        logf = open(SAFEMODE_LOG, "a", encoding="utf-8")

        def wlog(s):
            try:
                logf.write(f"[{datetime.now():%H:%M:%S}] {s}\n"); logf.flush()
            except Exception:
                pass
    except Exception:
        def wlog(s):
            pass

    wlog("==== 安全模式清理开始 ====")
    try:
        enable_debug_privilege()
        reboot_list = []
        try:
            with open(SAFEMODE_PLAN, "r", encoding="utf-8") as f:
                items = json.load(f)
        except Exception as e:
            wlog(f"读取清单失败: {e}")
            items = []
        # 先执行所有动作（杀进程/停删服务/删任务），再删文件注册表
        for it in items:
            try:
                perform_actions(it, wlog)
            except Exception as e:
                wlog(f"动作失败: {e}")
        for it in items:
            t = it.get("type")
            try:
                if t in ("dir", "file") and it.get("path"):
                    force_delete(it["path"], wlog, reboot_list)
                elif t == "reg" and it.get("reg"):
                    if it["reg"].get("_svc_only"):
                        continue
                    delete_reg(it["reg"], wlog)
                elif t == "regvalue" and it.get("reg"):
                    delete_reg_value(it["reg"], wlog)
            except Exception as e:
                wlog(f"删除失败 {it.get('path') or it.get('reg')}: {e}")
        wlog(f"重启删除标记 {len(reboot_list)} 项")
    finally:
        # 无论成败都恢复正常引导并删除自身任务，防止卡死在安全模式
        try:
            r = subprocess.run(["bcdedit", "/deletevalue", "{current}", "safeboot"],
                               capture_output=True, text=True, encoding="gbk", errors="ignore",
                               creationflags=NO_WINDOW)
            wlog("清除 safeboot 引导: " + ("成功" if r.returncode == 0 else (r.stderr or r.stdout or "")))
        except Exception as e:
            wlog(f"清除 safeboot 引导异常: {e}")
        try:
            subprocess.run(["schtasks", "/delete", "/tn", SAFEMODE_TASK, "/f"],
                           capture_output=True, creationflags=NO_WINDOW)
        except Exception:
            pass
        wlog("==== 清理结束，10 秒后重启回正常模式 ====")
        try:
            logf.close()
        except Exception:
            pass
        time.sleep(10)
        subprocess.run(["shutdown", "/r", "/t", "3", "/c", "安全模式清理完成，返回正常系统"],
                       creationflags=NO_WINDOW)


def native_uninstall(app: AppEntry, log=print) -> bool:
    """调用软件原生卸载程序"""
    cmd = app.uninstall_cmd
    if not cmd:
        log("该软件没有注册卸载命令")
        return False
    log(f"运行原生卸载: {cmd}")
    try:
        # 直接交给 ShellExecute，兼容 MsiExec.exe /I{GUID} 等
        exe, *args = None, None
        if cmd.startswith('"'):
            m = re.match(r'"([^"]+)"(.*)', cmd)
            if m:
                exe, args = m.group(1), m.group(2).strip()
            else:
                exe, args = cmd, ""
        else:
            parts = cmd.split(" ", 1)
            exe, args = parts[0], parts[1] if len(parts) > 1 else ""
        if exe and os.path.exists(exe):
            subprocess.Popen([exe] + (args.split() if args else []),
                             creationflags=0x08000000)
        else:
            ctypes.windll.shell32.ShellExecuteW(None, "open", exe or cmd, args or "", None, 1)
        return True
    except Exception as e:
        log(f"启动卸载程序失败: {e}")
        return False


def is_protected(app: AppEntry) -> bool:
    n = app.name.lower()
    return any(k in n for k in PROTECT_KEYWORDS)


# ============================================================
# 安装监控（快照对比 + 还原）
# ============================================================
MONITOR_PATHS = [
    os.environ.get("ProgramFiles", r"C:\Program Files"),
    os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
    os.environ.get("LOCALAPPDATA", ""),
    os.environ.get("APPDATA", ""),
    os.environ.get("ProgramData", ""),
]


def _snapshot_dirs():
    snap = {}
    for root in MONITOR_PATHS:
        if not root or not os.path.isdir(root):
            continue
        try:
            for name in os.listdir(root):
                full = os.path.join(root, name)
                snap[full.lower()] = os.path.isdir(full)
        except OSError:
            pass
    return snap


def _snapshot_reg():
    snap = set()
    for root, sub, view in UNINSTALL_KEYS:
        try:
            with winreg.OpenKey(root, sub, 0, winreg.KEY_READ | view) as base:
                for i in range(winreg.QueryInfoKey(base)[0]):
                    try:
                        cn = winreg.EnumKey(base, i)
                        snap.add(f"{root}|{view}|{sub}\\{cn}")
                    except OSError:
                        continue
        except OSError:
            pass
    return sorted(snap)


def make_snapshot():
    return {"time": datetime.now().isoformat(),
            "dirs": _snapshot_dirs(), "reg": _snapshot_reg()}


def diff_snapshot(old: dict):
    """返回监控期间新增的目录与注册表项"""
    new_dirs = _snapshot_dirs()
    new_reg = set(_snapshot_reg())
    added_dirs = [p for p in new_dirs if p not in old.get("dirs", {})]
    added_reg = sorted(new_reg - set(old.get("reg", [])))
    return {"time": datetime.now().isoformat(),
            "added_dirs": added_dirs, "added_reg": added_reg}


def load_json(p, default):
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def save_json(p, data):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)


# ============================================================
# GUI
# ============================================================
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_NAME)
        self.geometry("1000x620")
        self.minsize(820, 500)
        self.configure(bg="white")
        self.apps: List[AppEntry] = []
        self.icons = {}
        self.monitor_thread = None
        self.monitor_stop = threading.Event()
        self.settings = load_json(SETTINGS_FILE, {"monitor": False})

        self._build_menu()
        self._build_ui()
        self.refresh_apps()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        if self.settings.get("monitor"):
            self.after(300, self.start_monitor)

    # ---------- 菜单 ----------
    def _build_menu(self):
        menubar = tk.Menu(self, bg="white", activebackground="#e8f1fb", bd=0)
        m_file = tk.Menu(menubar, tearoff=0)
        m_file.add_command(label="刷新 (F5)", command=self.refresh_apps)
        m_file.add_separator()
        m_file.add_command(label="退出", command=self.on_close)
        menubar.add_cascade(label="文件", menu=m_file)

        m_set = tk.Menu(menubar, tearoff=0)
        self.var_monitor = tk.BooleanVar(value=self.settings.get("monitor", False))
        m_set.add_checkbutton(label="启用安装监控", variable=self.var_monitor,
                              command=self.toggle_monitor)
        m_set.add_command(label="还原监控到的安装...", command=self.restore_monitor)
        m_set.add_separator()
        m_set.add_command(label="以管理员身份重启", command=self.restart_admin)
        menubar.add_cascade(label="设置", menu=m_set)

        m_help = tk.Menu(menubar, tearoff=0)
        m_help.add_command(label="关于", command=lambda: messagebox.showinfo(
            "关于", f"{APP_NAME}\n\n正常卸载 / 强制删除 / 安装监控\n建议以管理员身份运行。"))
        menubar.add_cascade(label="帮助", menu=m_help)
        self.config(menu=menubar)

    # ---------- 主界面 ----------
    def _build_ui(self):
        UI_FONT = ("Microsoft YaHei UI", 9)
        top = tk.Frame(self, bg="white")
        top.pack(fill="x", padx=12, pady=(10, 6))
        tk.Label(top, text="搜索:", bg="white", font=UI_FONT, fg="#333").pack(side="left")
        self.var_search = tk.StringVar()
        self.var_search.trace_add("write", lambda *a: self.fill_tree())
        ent = tk.Entry(top, textvariable=self.var_search, relief="solid", bd=1,
                       font=UI_FONT, highlightthickness=1,
                       highlightcolor="#316ac5", highlightbackground="#cfcfcf")
        ent.pack(side="left", fill="x", expand=True, padx=8, ipady=3)
        self.lbl_status = tk.Label(top, text="", bg="white", fg="#777", font=UI_FONT)
        self.lbl_status.pack(side="right")

        container = tk.Frame(self, bg="#e3e3e3")
        container.pack(fill="both", expand=True, padx=12, pady=(0, 10))
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Treeview", background="white", fieldbackground="white",
                        rowheight=28, font=UI_FONT)
        style.configure("Treeview.Heading", font=("Microsoft YaHei UI", 9, "bold"),
                        background="#f5f6f8", foreground="#333", relief="flat")
        style.map("Treeview.Heading", background=[("active", "#e9ebef")])
        style.map("Treeview", background=[("selected", "#316ac5")],
                  foreground=[("selected", "#ffffff")])
        style.configure("Vertical.TScrollbar", background="#f0f0f0", troughcolor="#ffffff",
                        arrowcolor="#666")
        cols = ("name", "publisher", "date", "size")
        self.tree = ttk.Treeview(container, columns=cols, show="tree headings",
                                 selectmode="browse")
        self.tree.heading("#0", text="图标")
        self.tree.heading("name", text="名称")
        self.tree.heading("publisher", text="发布者")
        self.tree.heading("date", text="安装日期")
        self.tree.heading("size", text="大小")
        self.tree.column("#0", width=40, anchor="center", stretch=False, minwidth=40)
        self.tree.column("name", width=330, minwidth=160)
        self.tree.column("publisher", width=230, minwidth=120)
        self.tree.column("date", width=110, anchor="center", stretch=False)
        self.tree.column("size", width=100, anchor="e", stretch=False)
        vs = ttk.Scrollbar(container, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vs.set)
        self.tree.pack(side="left", fill="both", expand=True, padx=(0, 1), pady=1)
        vs.pack(side="right", fill="y", pady=1)
        self.tree.bind("<Button-3>", self.on_right_click)
        self.tree.bind("<Double-1>", lambda e: self.normal_uninstall())
        self.tree.bind("<F5>", lambda e: self.refresh_apps())
        self.tree.bind("<Delete>", lambda e: self.force_delete())
        self.tree.tag_configure("odd", background="#f7f9fc")
        self.tree.tag_configure("even", background="white")

        self.ctx = tk.Menu(self, tearoff=0, font=UI_FONT, bd=1, relief="solid")
        self.ctx.add_command(label="正常卸载...", command=self.normal_uninstall)
        self.ctx.add_command(label="强制删除...", command=self.force_delete)
        self.ctx.add_separator()
        self.ctx.add_command(label="打开安装位置", command=self.open_location)
        self.ctx.add_command(label="打开注册表项", command=self.open_regedit)
        self.ctx.add_separator()
        self.ctx.add_command(label="刷新", command=self.refresh_apps)

    # ---------- 数据 ----------
    def refresh_apps(self):
        self.lbl_status.config(text="正在读取已安装程序...")
        self.update_idletasks()
        self.apps = enumerate_apps()
        # 预提取图标（后台线程）
        threading.Thread(target=self._load_icons_async, daemon=True).start()
        self.fill_tree()
        self.lbl_status.config(text=f"共 {len(self.apps)} 个程序")

    def _load_icons_async(self):
        default_exe = default_icon_png("_default_exe")
        default_msi = default_icon_png("_default_msi")
        for idx, app in enumerate(self.apps):
            if app.icon_png:
                continue
            key = "ic_" + str(abs(hash(app.uid)))
            png = None
            try:
                if app.appx:
                    png = appx_logo_to_png(app, key)
                else:
                    for src in resolve_icon_candidates(app):
                        png = extract_icon(src, key)
                        if png:
                            break
            except Exception:
                png = None
            if png:
                app.icon_png = png
            elif app.appx:
                app.icon_png = default_exe
            elif "msiexec" in (app.uninstall_cmd or "").lower():
                app.icon_png = default_msi or default_exe
            else:
                app.icon_png = default_exe
            if idx % 10 == 0:
                self.after(0, self.fill_tree)
        self.after(0, self.fill_tree)
        self.after(0, lambda: self.lbl_status.config(text=f"共 {len(self.apps)} 个程序"))

    def fill_tree(self):
        kw = self.var_search.get().lower().strip()
        selected = self.tree.selection()
        self.tree.delete(*self.tree.get_children())
        self.icons.clear()
        shown = 0
        for app in self.apps:
            if kw and kw not in app.name.lower() and kw not in app.publisher.lower():
                continue
            img = ""
            if app.icon_png and os.path.exists(app.icon_png):
                try:
                    im = Image.open(app.icon_png)
                    img = ImageTk.PhotoImage(im)
                    self.icons[app.uid] = img
                except Exception:
                    img = ""
            iid = app.uid
            self.tree.insert(
                "", "end", iid=iid,
                text="", image=img if img else "",
                values=(app.name, app.publisher, app.install_date, size_human(app.size_kb * 1024)),
                tags=("odd" if shown % 2 else "even",))
            shown += 1
        if selected:
            for s in selected:
                if self.tree.exists(s):
                    self.tree.selection_set(s)

    def current_app(self) -> Optional[AppEntry]:
        sel = self.tree.selection()
        if not sel:
            return None
        uid = sel[0]
        for a in self.apps:
            if a.uid == uid:
                return a
        return None

    # ---------- 右键菜单 ----------
    def on_right_click(self, event):
        iid = self.tree.identify_row(event.y)
        if iid:
            self.tree.selection_set(iid)
            self.ctx.tk_popup(event.x_root, event.y_root)

    def open_location(self):
        app = self.current_app()
        if app and app.location and os.path.isdir(app.location):
            os.startfile(app.location)
        else:
            messagebox.showinfo("提示", "没有记录安装位置")

    def open_regedit(self):
        app = self.current_app()
        if not app:
            return
        if app.appx:
            messagebox.showinfo("提示", "微软商店应用没有传统注册表卸载项。")
            return
        if app.portable or not app.reg_subkey:
            messagebox.showinfo("提示", "该软件是绿色/便携软件，没有注册表卸载项。")
            return
        rootname = "HKEY_LOCAL_MACHINE" if app.reg_root == winreg.HKEY_LOCAL_MACHINE else "HKEY_CURRENT_USER"
        key = rootname + "\\" + app.reg_subkey
        try:
            subprocess.run(["reg", "add",
                            r"HKCU\Software\Microsoft\Windows\CurrentVersion\Applets\Regedit",
                            "/v", "LastKey", "/t", "REG_SZ", "/d", key, "/f"],
                           capture_output=True)
            os.startfile("regedit.exe")
        except Exception:
            os.startfile("regedit.exe")

    # ---------- 正常卸载 ----------
    def normal_uninstall(self):
        app = self.current_app()
        if not app:
            return
        if app.appx:
            if not messagebox.askyesno("卸载商店应用",
                                       f"确定通过微软商店机制卸载：\n\n{app.name} ？"):
                return
        elif app.portable or not app.uninstall_cmd:
            tip = ("该软件没有注册卸载程序（绿色/便携软件），\n"
                   "将直接扫描它的文件、快捷方式和注册表残留，\n"
                   "扫描完成后由你勾选清理。\n\n确定继续？")
            if not messagebox.askyesno("卸载绿色软件", tip):
                return
        elif not messagebox.askyesno("正常卸载", f"确定通过原生卸载程序卸载：\n\n{app.name} ？"):
            return
        self._run_dialog(
            title=f"卸载 {app.name}",
            work=lambda log: self._normal_uninstall_task(app, log),
            after_ok=None, allow_skip=not app.appx)

    def _normal_uninstall_task(self, app, log):
        self._skip_wait = False
        if app.appx:
            log("商店应用：调用 Remove-AppxPackage 卸载…")
            remove_appx_package(app, log)
            time.sleep(1)
            log("开始扫描残留…")
            items = scan_residual(app, log)
            self.after(0, lambda: self._show_residual_dialog(app, items, "normal"))
            return
        if app.portable or not app.uninstall_cmd:
            log("该软件没有注册卸载程序（绿色/便携软件），直接扫描其文件与残留…")
            time.sleep(1)
            items = scan_residual(app, log)
            self.after(0, lambda: self._show_residual_dialog(app, items, "normal"))
            return
        if not native_uninstall(app, log):
            log("无法启动原生卸载，可改用“强制删除”。")
            return
        log("请在弹出的卸载向导中完成卸载…")
        log("等待卸载程序退出（最多 10 分钟，可提前点“立即扫描残留”）")
        # 后台等待由对话框按钮控制；这里先等待一段时间
        for _ in range(600):
            time.sleep(1)
            if getattr(self, "_skip_wait", False):
                break
            cmd = app.uninstall_cmd.lower()
            exe = re.match(r'"?([^"]+\.exe)', cmd)
            if exe:
                name = os.path.basename(exe.group(1))
                r = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}"],
                                   capture_output=True, text=True, encoding="gbk", errors="ignore",
                                   creationflags=NO_WINDOW)
                if name.lower() not in r.stdout.lower():
                    time.sleep(2)
                    break
        log("开始扫描残留…")
        items = scan_residual(app, log)
        self.after(0, lambda: self._show_residual_dialog(app, items, "normal"))

    def _show_residual_dialog(self, app, items, mode="normal"):
        ResidualDialog(self, app, items, mode=mode, on_done=self.refresh_apps)

    # ---------- 强制删除 ----------
    def force_delete(self):
        app = self.current_app()
        if not app:
            return
        if is_protected(app):
            messagebox.showwarning(
                "受保护的系统组件",
                f"{app.name} 看起来是系统运行库/驱动组件，强制删除可能导致系统或其他软件异常，\n"
                "已为你拦截。如确需删除，请先确认它不是系统组件。")
            return
        if not messagebox.askyesno(
                "强制删除",
                f"将对 {app.name} 执行强制删除。\n\n"
                "程序会先扫描它的文件、注册表、服务/驱动、计划任务、开机自启动、\n"
                "浏览器扩展、同厂商随机目录等全部项目，然后列出来由你勾选删除；\n"
                "被占用的 dll/sys 会在重启时删除（可选择自动重启）。\n\n确定继续？",
                icon="warning"):
            return
        self._run_dialog(title=f"强制删除扫描 - {app.name}",
                         work=lambda log: self._force_scan_task(app, log))

    def _force_scan_task(self, app, log):
        if not is_admin():
            log("警告：当前不是管理员，部分文件/服务可能无法处理，建议右键以管理员运行。")
        enable_debug_privilege()
        log("正在扫描：文件目录 / 注册表 / 进程 / 服务与驱动 / 计划任务 / "
            "自启动 / 浏览器扩展 / 同厂商随机目录…")
        if app.appx:
            log("商店应用：先调用 Remove-AppxPackage…")
            remove_appx_package(app, log)
        items = scan_residual(app, log)
        log(f"扫描完成，共发现 {len(items)} 个项目，请在弹出的窗口中勾选后删除。")
        self.after(300, lambda: self._show_residual_dialog(app, items, "force"))

    # ---------- 通用任务对话框 ----------
    def _run_dialog(self, title, work, after_ok=None, allow_skip=False):
        TaskDialog(self, title, work, after_ok=after_ok, allow_skip=allow_skip)

    # ---------- 安装监控 ----------
    def toggle_monitor(self):
        on = self.var_monitor.get()
        self.settings["monitor"] = on
        save_json(SETTINGS_FILE, self.settings)
        if on:
            self.start_monitor()
        else:
            self.stop_monitor()

    def start_monitor(self):
        if self.monitor_thread and self.monitor_thread.is_alive():
            return
        if not os.path.exists(MONITOR_FILE):
            save_json(MONITOR_FILE, make_snapshot())
            messagebox.showinfo("安装监控", "已建立当前系统快照。\n现在去安装软件，完成后关闭监控即可查看并还原。")
        self.monitor_stop.clear()
        self.monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self.monitor_thread.start()
        self.lbl_status.config(text="● 安装监控运行中", fg="#c0392b")

    def stop_monitor(self):
        self.monitor_stop.set()
        self.lbl_status.config(text="", fg="#666")

    def _monitor_loop(self):
        while not self.monitor_stop.is_set():
            time.sleep(3)
        old = load_json(MONITOR_FILE, None)
        if old:
            rec = diff_snapshot(old)
            records = load_json(MONITOR_LOG, [])
            records.append(rec)
            save_json(MONITOR_LOG, records)
            self.after(0, lambda: messagebox.showinfo(
                "安装监控",
                f"监控结束。\n新增目录 {len(rec['added_dirs'])} 个，新增注册表项 {len(rec['added_reg'])} 个。\n"
                "可在 设置 → 还原监控到的安装 中查看。"))

    def restore_monitor(self):
        records = load_json(MONITOR_LOG, [])
        if not records:
            messagebox.showinfo("安装监控", "暂无监控记录。请先启用监控并安装软件。")
            return
        RestoreDialog(self, records, on_done=lambda: None)

    def restart_admin(self):
        if is_admin():
            messagebox.showinfo("提示", "当前已经是管理员权限。")
            return
        run_as_admin()

    def on_close(self):
        self.monitor_stop.set()
        self.destroy()


# ---------- 任务执行对话框 ----------
class TaskDialog(tk.Toplevel):
    def __init__(self, master, title, work, after_ok=None, allow_skip=False):
        super().__init__(master)
        self.title(title)
        self.geometry("640x420")
        self.configure(bg="white")
        self.transient(master)
        self.txt = tk.Text(self, wrap="word", relief="flat", bg="#fafafa",
                           font=("Consolas", 9))
        self.txt.pack(fill="both", expand=True, padx=10, pady=10)
        btns = tk.Frame(self, bg="white")
        btns.pack(fill="x", padx=10, pady=(0, 10))
        self.btn_scan = tk.Button(btns, text="立即扫描残留",
                                  state="normal" if allow_skip else "disabled",
                                  command=lambda: setattr(master, "_skip_wait", True))
        self.btn_scan.pack(side="left")
        tk.Button(btns, text="关闭", command=self.destroy).pack(side="right")
        self.after_ok = after_ok
        threading.Thread(target=self._run, args=(work,), daemon=True).start()

    def log(self, s):
        self.after(0, lambda: (self.txt.insert("end", s + "\n"), self.txt.see("end")))

    def _run(self, work):
        try:
            work(self.log)
        except Exception as e:
            self.log(f"[错误] {e}")
        self.log("—— 结束 ——")
        if self.after_ok:
            self.after(500, self.after_ok)


# ---------- 残留勾选对话框 ----------
class ResidualDialog(tk.Toplevel):
    def __init__(self, master, app: AppEntry, items, on_done=None, mode="normal"):
        super().__init__(master)
        self.master_app = master
        self.mode = mode
        force = mode == "force"
        self.title(("强制删除 - " if force else "发现的残留 - ") + app.name)
        self.geometry("780x520")
        self.configure(bg="white")
        self.transient(master)
        self.app = app
        self.items = items
        self.on_done = on_done
        self.vars = []
        self.reboot = []
        head = (f"以下是扫描到的 {len(items)} 个项目，勾选后将被"
                + ("强制删除" if force else "清理")
                + "（默认全选，模糊匹配项已标注，请确认）：")
        tk.Label(self, bg="white", anchor="w", text=head, font=("Microsoft YaHei UI", 9),
                 justify="left").pack(fill="x", padx=14, pady=(12, 6))
        # 全选/反选
        selbar = tk.Frame(self, bg="white")
        selbar.pack(fill="x", padx=14)
        tk.Button(selbar, text="全选", font=("Microsoft YaHei UI", 8),
                  command=lambda: self._set_all(True)).pack(side="left")
        tk.Button(selbar, text="全不选", font=("Microsoft YaHei UI", 8),
                  command=lambda: self._set_all(False)).pack(side="left", padx=6)

        container = tk.Frame(self, bg="white")
        container.pack(fill="both", expand=True, padx=14, pady=6)
        canvas = tk.Canvas(container, bg="white", highlightthickness=0)
        sb = ttk.Scrollbar(container, orient="vertical", command=canvas.yview)
        frame = tk.Frame(canvas, bg="white")
        frame.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        win_id = canvas.create_window((0, 0), window=frame, anchor="nw")
        canvas.bind("<Configure>", lambda e: canvas.itemconfig(win_id, width=e.width))
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        if not items:
            tk.Label(frame, text="未发现残留，软件已卸载干净。", bg="white", fg="#888",
                     font=("Microsoft YaHei UI", 9)).pack(padx=4, pady=10, anchor="w")
        total = 0
        type_label = {"dir": "[文件夹]", "file": "[文件]", "reg": "[注册表]",
                      "regvalue": "[自启动]", "task_only": "[计划任务]"}
        for idx, it in enumerate(items):
            v = tk.BooleanVar(value=True)
            self.vars.append(v)
            if it["type"] == "reg":
                if it.get("reg", {}).get("_svc_only"):
                    desc = "[服务/驱动] " + (it["action"]["svc"][0] if it.get("action") else "")
                else:
                    desc = "[注册表] " + it["reg"]["subkey"]
            elif it["type"] == "regvalue":
                desc = "[自启动] " + it["reg"]["subkey"] + "  →  " + str(it["reg"].get("value"))
            elif it["type"] == "task_only":
                desc = "[计划任务] " + it["path"]
            else:
                tag = type_label.get(it["type"], "[?]")
                desc = f"{tag} {it['path']}"
                if it["size"]:
                    desc += f"   ({size_human(it['size'])})"
                    total += it["size"]
            act = it.get("action") or {}
            extras = []
            if act.get("kill"):
                extras.append(f"结束进程 {len(act['kill'])} 个")
            if act.get("svc"):
                extras.append("停删服务 " + ",".join(act["svc"]))
            if act.get("task"):
                extras.append("删任务")
            if extras:
                desc += "   〔" + "；".join(extras) + "〕"
            if it["score"] < 10 and it["type"] in ("dir", "file"):
                desc += "   （模糊匹配，请确认）"
            cb = tk.Checkbutton(frame, variable=v, text=desc, bg="white", anchor="w",
                                justify="left", wraplength=700, activebackground="white",
                                font=("Microsoft YaHei UI", 9))
            cb.pack(fill="x", anchor="w", pady=1)
        self.lbl_total = tk.Label(self, bg="white", fg="#555", font=("Microsoft YaHei UI", 9),
                                  text=f"可释放约 {size_human(total)}")
        self.lbl_total.pack(fill="x", padx=14, pady=(4, 0))
        bar = tk.Frame(self, bg="white")
        bar.pack(fill="x", padx=14, pady=10)
        tk.Button(bar, text="取消", font=("Microsoft YaHei UI", 9),
                  command=self.destroy).pack(side="right")
        if force:
            tk.Button(bar, text="安全模式彻底删除（杀软自保护时用）",
                      bg="#7d3c98", fg="white", activebackground="#6c3483",
                      font=("Microsoft YaHei UI", 9, "bold"),
                      command=self.safemode_cleanup).pack(side="left")
            tk.Button(bar, text="强制删除所选项目", bg="#c0392b", fg="white",
                      activebackground="#a93226", font=("Microsoft YaHei UI", 9, "bold"),
                      command=lambda: self.do_clean(force=True)).pack(side="right", padx=8)
        else:
            tk.Button(bar, text="清理所选项目", bg="#2d7d46", fg="white",
                      activebackground="#27693c", font=("Microsoft YaHei UI", 9, "bold"),
                      command=lambda: self.do_clean(force=False)).pack(side="right", padx=8)

    def safemode_cleanup(self):
        chosen = [it for it, v in zip(self.items, self.vars) if v.get()]
        if not chosen:
            return
        if not messagebox.askyesno(
                "安全模式彻底删除",
                "适用于杀毒软件等带内核自我保护、正常模式无法删除的软件。\n\n"
                "点击“是”后将：\n"
                "1. 把删除清单和清理程序部署到本机；\n"
                "2. 电脑自动重启进入安全模式（此时 360 自保护驱动不会加载）；\n"
                "3. 以 SYSTEM 权限无人值守删除所选全部项目；\n"
                "4. 删除完成后自动重启，回到正常系统。\n\n"
                f"本次将清理 {len(chosen)} 个项目。过程中电脑会自动重启两次，\n"
                "请先保存其他工作。确定继续？", parent=self, icon="warning"):
            return
        if not prepare_safemode_cleanup(chosen, print):
            messagebox.showerror("失败",
                                 "部署安全模式清理失败（需要管理员权限）。", parent=self)
            return
        try:
            self.destroy()
        except Exception:
            pass
        reboot_to_safemode()

    def _set_all(self, val: bool):
        for v in self.vars:
            v.set(val)

    def do_clean(self, force: bool = False):
        chosen = [it for it, v in zip(self.items, self.vars) if v.get()]
        if not chosen:
            return
        word = "强制删除" if force else "清理"
        if not messagebox.askyesno("确认",
                                   f"确定{word}选中的 {len(chosen)} 项？此操作不可撤销。",
                                   parent=self):
            return
        app, self.reboot = self.app, []
        fail = [0]

        def work(log):
            enable_debug_privilege()
            if force:
                log("第 1 步：多轮绞杀相关进程（含互保看门狗）…")
                kill_processes_using(app, log)
            log("第 2 步：停止服务/驱动、结束关联进程、删除计划任务…")
            for it in chosen:
                try:
                    perform_actions(it, log)
                except Exception as e:
                    log(f"动作执行失败: {e}")
            log(f"第 3 步：{word}文件、目录与注册表…")
            for it in chosen:
                try:
                    t = it["type"]
                    if t in ("dir", "file") and it["path"]:
                        if not force_delete(it["path"], log, self.reboot):
                            fail[0] += 1
                    elif t == "reg":
                        reg = it["reg"]
                        if reg.get("_svc_only"):
                            continue  # 服务已由 perform_actions 删除
                        if not delete_reg(reg, log):
                            fail[0] += 1
                    elif t == "regvalue":
                        if not delete_reg_value(it["reg"], log):
                            fail[0] += 1
                    elif t == "task_only":
                        pass
                except Exception as e:
                    log(f"删除失败 {it.get('path') or it.get('reg')}: {e}")
                    fail[0] += 1
            if self.reboot:
                log(f"\n有 {len(self.reboot)} 项将在重启电脑后完成删除。")
            log(f"{word}完成。" + (f" 失败 {fail[0]} 项。" if fail[0] else ""))

        def after():
            if self.on_done:
                self.on_done()
            try:
                self.destroy()
            except Exception:
                pass
            ask_reboot_if_needed(self.reboot)

        TaskDialog(self.master_app, f"{word} - {app.name}", work, after_ok=after)


# ---------- 监控还原对话框 ----------
class RestoreDialog(tk.Toplevel):
    def __init__(self, master, records, on_done=None):
        super().__init__(master)
        self.title("还原监控到的安装")
        self.geometry("720x460")
        self.configure(bg="white")
        self.records = records
        self.on_done = on_done
        tk.Label(self, bg="white", anchor="w",
                 text="选择一条监控记录，将删除监控期间新增的目录与注册表项（即还原该安装）："
                ).pack(fill="x", padx=12, pady=10)
        lb = tk.Listbox(self, font=("Consolas", 9))
        lb.pack(fill="x", padx=12)
        for i, r in enumerate(records):
            lb.insert("end", f"{i+1}. {r['time']}  目录 {len(r['added_dirs'])} / 注册表 {len(r['added_reg'])}")
        detail = tk.Text(self, height=12, relief="solid", font=("Consolas", 8))
        detail.pack(fill="both", expand=True, padx=12, pady=8)

        def show(evt):
            sel = lb.curselection()
            if not sel:
                return
            r = records[sel[0]]
            detail.delete("1.0", "end")
            detail.insert("end", "\n".join(r["added_dirs"]))
            detail.insert("end", "\n\n# 注册表\n")
            detail.insert("end", "\n".join(x.replace("|", " 视图") for x in r["added_reg"]))
        lb.bind("<<ListboxSelect>>", show)

        bar = tk.Frame(self, bg="white")
        bar.pack(fill="x", padx=12, pady=10)
        tk.Button(bar, text="关闭", command=self.destroy).pack(side="right")

        def do_restore():
            sel = lb.curselection()
            if not sel:
                return
            r = records[sel[0]]
            if not messagebox.askyesno("确认还原",
                                       f"将删除 {len(r['added_dirs'])} 个目录和 {len(r['added_reg'])} 个注册表项，继续？"):
                return
            enable_debug_privilege()
            reboot, fail = [], 0
            for d in r["added_dirs"]:
                if os.path.exists(d) and not force_delete(d, print, reboot):
                    fail += 1
            for key in r["added_reg"]:
                root_s, view_s, sub = key.split("|", 2)
                reg = {"root": int(root_s), "view": int(view_s), "subkey": sub}
                if not delete_reg(reg, print):
                    fail += 1
            messagebox.showinfo("完成", f"还原完成。失败 {fail} 项，重启删除 {len(reboot)} 项。")
        tk.Button(bar, text="还原所选记录", bg="#c0392b", fg="white",
                  command=do_restore).pack(side="right", padx=8)


def main():
    if "--safemode-clean" in sys.argv:
        # 安全模式下由计划任务以 SYSTEM 身份调用，无界面
        run_safemode_cleanup()
        return
    if not is_admin():
        # 不强制退出，仅提示（部分操作在非管理员下仍可用）
        pass
    enable_debug_privilege()
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()


