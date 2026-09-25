// Mini Geek Uninstaller - C++/Win32 原生实现
// 仿 Geek Uninstaller 风格的软件卸载工具（单文件，免环境）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <strsafe.h>
#include <windowsx.h>
#include <setupapi.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

// 非即插即用驱动设备类（设备管理器“显示隐藏设备 → 非即插即用驱动程序”）
// 对应注册表 ROOT\LEGACY_<服务名>\0000
static const GUID GUID_LegacyDriverLocal =
    { 0x8ecc055d, 0x047f, 0x11d1, { 0xa5, 0x37, 0x00, 0x00, 0xf8, 0x75, 0x3e, 0xd1 } };
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "dwmapi.lib")

// ------------------------------------------------------------------
// 数据结构
// ------------------------------------------------------------------
struct AppInfo {
    std::wstring name;
    std::wstring publisher;
    std::wstring installDate;
    std::wstring location;
    std::wstring uninstallCmd;
    std::wstring iconPath;
    std::wstring regSubkey;      // Uninstall 下相对子键
    DWORD        regView = 0;    // KEY_WOW64_64KEY / 32KEY
    HKEY         regRoot = HKEY_LOCAL_MACHINE;
    bool         systemComponent = false;
    bool         portable = false;
    bool         appx = false;
    int          iconIndex = -1;
    unsigned long long size = 0;
    bool         sizeReady = false;
};

static HINSTANCE g_hInst;
static HWND      g_hMain;
static HWND      g_hList;
static HWND      g_hStatus;
static HIMAGELIST g_hImg;
static HFONT     g_hFont;
static std::vector<AppInfo> g_apps;

#define IDC_LIST   1001
#define ID_REFRESH 2001
#define IDM_UNINSTALL 3001
#define IDM_FORCE     3002
#define IDM_OPENLOC   3003
#define IDM_OPENREG   3004
#define IDM_REFRESH   3005
#define WM_FORCE_DONE (WM_APP + 1)

// ------------------------------------------------------------------
// 工具函数
// ------------------------------------------------------------------
static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::wstring RegGetStr(HKEY root, const std::wstring& sub,
                              const std::wstring& val, DWORD view) {
    HKEY h;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024]; DWORD sz = sizeof(buf); DWORD type;
    DWORD rc = RegQueryValueExW(h, val.c_str(), nullptr, &type, (LPBYTE)buf, &sz);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS) return L"";
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        std::wstring r = buf;
        if (type == REG_EXPAND_SZ) {
            wchar_t exp[2048];
            ExpandEnvironmentStringsW(r.c_str(), exp, 2048);
            r = exp;
        }
        return r;
    }
    return L"";
}

static std::wstring RegGetDwordStr(HKEY root, const std::wstring& sub,
                                   const std::wstring& val, DWORD view) {
    HKEY h;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS)
        return L"";
    DWORD v = 0, sz = sizeof(v), type;
    DWORD rc = RegQueryValueExW(h, val.c_str(), nullptr, &type, (LPBYTE)&v, &sz);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS) return L"";
    wchar_t b[16]; wsprintfW(b, L"%u", v);
    return b;
}

// 从 "C:\dir\app.exe,0" 中解析路径与图标索引
static void ParseIconPath(const std::wstring& raw, std::wstring& path, int& idx) {
    path = raw; idx = 0;
    size_t comma = path.find_last_of(L',');
    if (comma != std::wstring::npos) {
        std::wstring tail = path.substr(comma + 1);
        bool numeric = !tail.empty();
        for (wchar_t c : tail) if (!iswdigit(c) && c != L'-') numeric = false;
        if (numeric) { idx = _wtoi(tail.c_str()); path = path.substr(0, comma); }
    }
    // 去掉首尾引号
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);
}

// 提取图标到 ImageList，返回索引；失败 -1
static int AddIconFor(const std::wstring& iconRaw, const std::wstring& location) {
    std::wstring path; int idx;
    ParseIconPath(iconRaw, path, idx);
    HICON hIcon = nullptr;
    auto validIcon = [](HICON h){ return h && h != INVALID_HANDLE_VALUE; };
    if (!path.empty()) {
        HICON hArr[1] = {nullptr}; UINT ids[1] = {0};
        UINT got = PrivateExtractIconsW(path.c_str(), idx, 16, 16,
                                       hArr, ids, 1, 0);
        if (got > 0 && validIcon(hArr[0])) hIcon = hArr[0];
        else if (hArr[0]) DestroyIcon(hArr[0]);
        if (!validIcon(hIcon)) {
            HICON e = ExtractIconW(g_hInst, path.c_str(), idx);
            if (validIcon(e)) hIcon = e;
        }
    }
    if (!validIcon(hIcon) && !location.empty()) {
        SHFILEINFOW sfi = {0};
        if (SHGetFileInfoW(location.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_SMALLICON))
            hIcon = sfi.hIcon;
    }
    if (!validIcon(hIcon)) {
        SHFILEINFOW sfi = {0};
        SHGetFileInfoW(L"*.exe", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        hIcon = sfi.hIcon;
    }
    if (!validIcon(hIcon)) return 0;
    int added = ImageList_AddIcon(g_hImg, hIcon);
    DestroyIcon(hIcon);
    return added;
}

// ------------------------------------------------------------------
// 枚举已安装软件
// ------------------------------------------------------------------
static unsigned long long CalcDirSize(const std::wstring& dir) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd;
    std::wstring pat = dir + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                total += CalcDirSize(p);
        } else {
            LARGE_INTEGER li; li.HighPart = fd.nFileSizeHigh;
            li.LowPart = fd.nFileSizeLow;
            total += (unsigned long long)li.QuadPart;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static std::wstring FmtSize(unsigned long long b) {
    wchar_t t[64];
    if (b >= 1024ULL * 1024 * 1024)
        swprintf(t, 64, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ULL * 1024)
        swprintf(t, 64, L"%.2f MB", b / (1024.0 * 1024));
    else if (b >= 1024ULL)
        swprintf(t, 64, L"%.1f KB", b / 1024.0);
    else
        swprintf(t, 64, L"%llu B", b);
    return t;
}

static std::wstring FmtInstallDate(const std::wstring& raw) {
    std::wstring s = raw;
    // 去掉可能的非数字
    std::wstring digits;
    for (wchar_t c : s) if (iswdigit(c)) digits += c;
    if (digits.size() == 8) {
        wchar_t t[24];
        swprintf(t, 24, L"%c%c%c%c-%c%c-%c%c",
            digits[0], digits[1], digits[2], digits[3],
            digits[4], digits[5], digits[6], digits[7]);
        return t;
    }
    return raw;
}

static std::wstring DisplayNameWithArch(const AppInfo& a) {
    std::wstring n = a.name;
    if (a.appx || a.portable) return n;
    if (n.find(L"(32-bit)") != std::wstring::npos ||
        n.find(L"(64-bit)") != std::wstring::npos) return n;
    if (!a.location.empty()) {
        bool x86 = Lower(a.location).find(L"program files (x86)") != std::wstring::npos;
        n += x86 ? L" (32-bit)" : L" (64-bit)";
    }
    return n;
}

static void EnumerateApps() {
    g_apps.clear();

    struct Root { HKEY hkey; const wchar_t* sub; DWORD view; };
    const wchar_t* UNINST = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    std::wstring uninst = UNINST;
    Root roots[] = {
        { HKEY_LOCAL_MACHINE, UNINST, KEY_WOW64_64KEY },
        { HKEY_LOCAL_MACHINE, UNINST, KEY_WOW64_32KEY },
        { HKEY_CURRENT_USER,  UNINST, 0 },
    };

    for (const Root& r : roots) {
        HKEY hBase;
        if (RegOpenKeyExW(r.hkey, r.sub, 0, KEY_READ | r.view, &hBase) != ERROR_SUCCESS)
            continue;
        DWORD i = 0;
        for (;;) {
            wchar_t name[512]; DWORD nlen = 512;
            if (RegEnumKeyExW(hBase, i++, name, &nlen, nullptr,
                              nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            std::wstring sub = uninst + L"\\" + name;
            std::wstring dispName = RegGetStr(r.hkey, sub, L"DisplayName", r.view);
            if (dispName.empty()) continue;
            std::wstring sysComp = RegGetDwordStr(r.hkey, sub, L"SystemComponent", r.view);
            std::wstring parent = RegGetDwordStr(r.hkey, sub, L"ParentKeyName", r.view);

            AppInfo a;
            a.name = dispName;
            a.publisher = RegGetStr(r.hkey, sub, L"Publisher", r.view);
            a.installDate = FmtInstallDate(RegGetStr(r.hkey, sub, L"InstallDate", r.view));
            a.location = RegGetStr(r.hkey, sub, L"InstallLocation", r.view);
            a.uninstallCmd = RegGetStr(r.hkey, sub, L"UninstallString", r.view);
            a.iconPath = RegGetStr(r.hkey, sub, L"DisplayIcon", r.view);
            a.regSubkey = name;
            a.regView = r.view;
            a.regRoot = r.hkey;
            a.systemComponent = (sysComp == L"1") || !parent.empty();
            if (a.location.size() == 1) a.location.clear();
            g_apps.push_back(std::move(a));
        }
        RegCloseKey(hBase);
    }

    // 排序：名称
    std::sort(g_apps.begin(), g_apps.end(),
              [](const AppInfo& x, const AppInfo& y) {
                  return Lower(x.name) < Lower(y.name);
              });
}

// ------------------------------------------------------------------
// 填充 ListView
// ------------------------------------------------------------------
static void PopulateList() {
    ListView_DeleteAllItems(g_hList);
    if (g_hImg) { ImageList_Destroy(g_hImg); }
    g_hImg = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 256, 64);
    ListView_SetImageList(g_hList, g_hImg, LVSIL_SMALL);

    // 索引 0：默认 exe 图标占位，保证任何程序至少有图标
    {
        SHFILEINFOW d = {0};
        if (SHGetFileInfoW(L"*.exe", FILE_ATTRIBUTE_NORMAL, &d, sizeof(d),
                SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
            ImageList_AddIcon(g_hImg, d.hIcon);
            DestroyIcon(d.hIcon);
        }
    }

    int row = 0;
    for (size_t i = 0; i < g_apps.size(); ++i) {
        AppInfo& a = g_apps[i];
        a.iconIndex = AddIconFor(a.iconPath, a.location);

        std::wstring disp = DisplayNameWithArch(a);
        LVITEMW lvi = {0};
        lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem = row;
        lvi.iImage = a.iconIndex < 0 ? 0 : a.iconIndex;
        lvi.pszText = (LPWSTR)disp.c_str();
        lvi.lParam = (LPARAM)i;
        int inserted = ListView_InsertItem(g_hList, &lvi);

        std::wstring sz = a.sizeReady ? FmtSize(a.size) : L"";
        ListView_SetItemText(g_hList, inserted, 1, (LPWSTR)sz.c_str());
        ListView_SetItemText(g_hList, inserted, 2, (LPWSTR)a.installDate.c_str());
        ++row;
    }

    wchar_t st[128];
    wsprintfW(st, L"共 %d 个程序", row);
    SetWindowTextW(g_hStatus, st);

    // 后台线程计算各程序占用大小
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        for (size_t i = 0; i < g_apps.size(); ++i) {
            AppInfo& a = g_apps[i];
            if (!a.location.empty()) {
                a.size = CalcDirSize(a.location);
                a.sizeReady = true;
                PostMessageW(g_hMain, WM_APP + 2, (WPARAM)i, 0);
            }
        }
        return 0;
    }, 0, 0, nullptr);
}

// ------------------------------------------------------------------
// 残留扫描
// ------------------------------------------------------------------
enum RType { R_DIR, R_FILE, R_REG, R_REGV };
struct ResidualItem {
    RType type = R_DIR;
    std::wstring path;          // 文件/目录路径
    std::wstring regSubkey;     // 注册表相对路径（HKLM/HKCU 由 regRoot 决定）
    HKEY   regRoot = HKEY_LOCAL_MACHINE;
    DWORD  regView = KEY_WOW64_64KEY;
    std::wstring regValue;      // 注册表值名（R_REGV）
    bool   allowSystem = false; // 白名单允许删除系统路径（drivers 下厂商 .sys）
    bool   isServicesKey = false; // 该项是 Services 下精确服务键
    std::wstring svc;           // 关联服务名
    std::wstring display;       // 显示名
};

// 提取厂商关键字集合
static bool IsGenericKeyword(const std::wstring& k);  // 前向声明
static std::vector<std::wstring> BuildKeywords(const AppInfo& a) {
    std::vector<std::wstring> kw;
    auto add = [&](std::wstring t) {
        t = Lower(t);
        // 只保留字母数字
        std::wstring c;
        for (wchar_t ch : t) if (iswalnum(ch)) c += ch; else { if (c.size() >= 3) kw.push_back(c); c.clear(); }
        if (c.size() >= 3) kw.push_back(c);
    };
    // 安装目录各段
    std::wstring loc = a.location;
    if (!loc.empty()) {
        size_t s = 0;
        while (s <= loc.size()) {
            size_t e = loc.find(L'\\', s);
            std::wstring seg = loc.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
            std::wstring low = Lower(seg);
            if (low != L"program files" && low != L"program files (x86)" &&
                low != L"c:" && low != L"common files" && low != L"windows" &&
                low != L"applications" && seg.size() >= 2)
                add(seg);
            if (e == std::wstring::npos) break;
            s = e + 1;
        }
    }
    add(a.publisher);
    add(a.name);
    // 剔除通用词，避免误伤同名系统键/目录
    kw.erase(std::remove_if(kw.begin(), kw.end(),
              [](const std::wstring& k){ return IsGenericKeyword(k); }), kw.end());
    // 去重
    std::sort(kw.begin(), kw.end());
    kw.erase(std::unique(kw.begin(), kw.end()), kw.end());
    return kw;
}

static bool MatchKeyword(const std::wstring& text, const std::vector<std::wstring>& kw) {
    std::wstring t = Lower(text);
    for (const std::wstring& k : kw) {
        size_t pos = t.find(k);
        while (pos != std::wstring::npos) {
            if (k.size() <= 4) {
                // 短关键字（如 360）要求前面是词边界，避免误伤 JZDS360Ly
                bool boundary = (pos == 0) || !iswalnum(t[pos - 1]);
                if (boundary) return true;
            } else {
                return true;
            }
            pos = t.find(k, pos + 1);
        }
    }
    return false;
}

// ------------------------------------------------------------------
// 注册表删除安全防护
// ------------------------------------------------------------------
// 通用词：不得作为递归删 SOFTWARE\<词> 的依据
static bool IsGenericKeyword(const std::wstring& k) {
    static const wchar_t* bad[] = {
        L"safe", L"safer", L"safely", L"security", L"secure", L"tool", L"tools",
        L"toolbar", L"desktop", L"update", L"updater", L"setup", L"install",
        L"installer", L"helper", L"assistant", L"service", L"center", L"centre",
        L"guard", L"protect", L"clean", L"cleaner", L"speed", L"fast", L"net",
        L"web", L"app", L"apps", L"software", L"system", L"driver", L"manager",
        L"master", L"box", L"zip", L"file", L"files", L"download", L"player",
        L"viewer", L"editor", L"launcher", L"plugin", L"addon", L"core", L"pro",
        L"new", L"hot", L"top", L"win", L"pc", L"user"
    };
    for (auto w : bad) if (k == w) return true;
    return false;
}

// 受保护的注册表路径：命中任意前缀即禁止删除
static bool IsProtectedRegSubkey(const std::wstring& sub) {
    std::wstring s = Lower(sub);
    static const wchar_t* deny[] = {
        L"software\\microsoft",
        L"software\\policies",
        L"software\\classes",
        L"software\\clients",
        L"software\\wow6432node",
        L"software\\registeredapplications",
        L"software\\microsoft\\windows nt",
        L"system\\currentcontrolset\\control",
        L"system\\currentcontrolset\\services\\acpi",
        L"system\\currentcontrolset\\services\\disk",
        L"system\\currentcontrolset\\services\\mountmgr",
        L"system\\currentcontrolset\\services\\null",
        L"system\\currentcontrolset\\services\\partmgr",
        L"system\\currentcontrolset\\services\\volmgr",
        L"system\\currentcontrolset\\services\\volsnap",
    };
    for (auto p : deny) {
        std::wstring pp = p;
        if (s == pp || s.find(pp + L"\\") == 0) return true;
    }
    // SOFTWARE 下只允许删除“深度>=2段”的私有键（SOFTWARE\厂商\产品）
    return false;
}

// 计算反斜杠分隔的路径段数
static int CountSegments(const std::wstring& p) {
    int n = 0; bool in = false;
    for (wchar_t ch : p) {
        if (ch == L'\\') { if (in) { n++; in = false; } }
        else in = true;
    }
    if (in) n++;
    return n;
}

// 递归校验某键下（值+一层子键）是否真的出现关键字；用于删除前二次确认
static bool KeyTreeContainsKeyword(HKEY root, const std::wstring& sub,
                                   const std::vector<std::wstring>& kw, DWORD view) {
    HKEY h;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS)
        return false;
    bool found = false;
    DWORD vi = 0;
    for (;;) {
        wchar_t vn[256]; DWORD vnl = 256;
        DWORD type; BYTE data[2048]; DWORD dlen = sizeof(data);
        LONG rc = RegEnumValueW(h, vi++, vn, &vnl, nullptr, &type, data, &dlen);
        if (rc != ERROR_SUCCESS) break;
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            std::wstring s = Lower(std::wstring((wchar_t*)data,
                dlen / sizeof(wchar_t)));
            for (auto& k : kw) if (s.find(k) != std::wstring::npos) { found = true; break; }
        }
        if (found) break;
    }
    if (!found) {
        DWORD si = 0;
        for (;;) {
            wchar_t sn[256]; DWORD snl = 256;
            if (RegEnumKeyExW(h, si++, sn, &snl, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS) break;
            if (MatchKeyword(sn, kw)) { found = true; break; }
        }
    }
    RegCloseKey(h);
    return found;
}

// 导出 .reg 备份（备份目录 C:\ProgramData\MiniGeekRegBackup）
static std::wstring ExportRegBackup(HKEY root, const std::wstring& sub) {
    wchar_t winDir[MAX_PATH]; GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring bdir = std::wstring(winDir) + L"\\..\\..\\ProgramData\\MiniGeekRegBackup";
    // 直接固定系统盘 ProgramData
    wchar_t sd[MAX_PATH]; GetSystemWindowsDirectoryW(sd, MAX_PATH);
    std::wstring sysroot(sd);
    size_t pos = Lower(sysroot).find(L"\\windows");
    std::wstring bkdir = (pos != std::wstring::npos ? sysroot.substr(0, pos) : L"C:")
                         + L"\\ProgramData\\MiniGeekRegBackup";
    CreateDirectoryW(bkdir.c_str(), nullptr);
    std::wstring safe = Lower(sub);
    for (auto& ch : safe) if (!iswalnum(ch)) ch = L'_';
    std::wstring file = bkdir + L"\\" + safe + L".reg";
    const wchar_t* rootName =
        root == HKEY_LOCAL_MACHINE ? L"HKLM" :
        root == HKEY_CURRENT_USER  ? L"HKCU" :
        root == HKEY_CLASSES_ROOT  ? L"HKCR" : L"HKU";
    std::wstring args = L"/c reg export \"", rootStr(rootName);
    args += rootStr + L"\\" + sub + L"\" \"" + file + L"\" /y >nul 2>&1";
    SHELLEXECUTEINFOW s = { sizeof(s) };
    s.lpFile = L"cmd.exe"; s.lpParameters = args.c_str();
    s.nShow = SW_HIDE; s.fMask = SEE_MASK_NOCLOSEPROCESS;
    ShellExecuteExW(&s);
    if (s.hProcess) { WaitForSingleObject(s.hProcess, 8000); CloseHandle(s.hProcess); }
    return file;
}

// 判定一个待删注册表项是否安全；安全则备份并删除，返回 true 表示已删
static bool SafeDeleteRegItem(HKEY root, const std::wstring& sub,
                              const std::vector<std::wstring>& kw, DWORD view,
                              bool isServicesKey) {
    // 服务键：路径精确到单个服务（深度足够），允许删
    if (isServicesKey) {
        if (IsProtectedRegSubkey(sub)) return false;
        ExportRegBackup(root, sub);
        return RegDeleteTreeW(root, sub.c_str()) == ERROR_SUCCESS;
    }
    if (IsProtectedRegSubkey(sub)) return false;
    // SOFTWARE 私有键：必须深度>=2（SOFTWARE\厂商\产品），杜绝删 SOFTWARE\单段
    int seg = CountSegments(sub);
    if (Lower(sub).find(L"software\\") == 0) {
        if (seg < 2) return false;                 // "software" 本身不删；SOFTWARE\X 经多重校验
    } else if (seg < 2) {
        return false;
    }
    // 末段键名若是通用词，禁止
    size_t bs = sub.find_last_of(L'\\');
    std::wstring leaf = Lower(sub.substr(bs == std::wstring::npos ? 0 : bs + 1));
    if (IsGenericKeyword(leaf)) return false;
    // 删除前内容校验：键下确有厂商关键字
    if (!KeyTreeContainsKeyword(root, sub, kw, view)) return false;
    ExportRegBackup(root, sub);
    return RegDeleteTreeW(root, sub.c_str()) == ERROR_SUCCESS;
}

static std::wstring EnvPath(const wchar_t* env) {
    wchar_t b[MAX_PATH]; GetEnvironmentVariableW(env, b, MAX_PATH);
    return b;
}

// ==================================================================
// 数字签名验证 / PE 静态依赖分析 / 随机目录识别
// ==================================================================

static int  VerifyFileSignature(const std::wstring&);
static bool IsSystemDirTree(const std::wstring&);

// 签名归属分类：0=微软/Windows 系统签名；1=其他有效签名(第三方)；2=无签名；3=签名无效
static int ClassifyFileSignature(const std::wstring& path) {
    // 先验证签名有效性
    int v = VerifyFileSignature(path);
    if (v == 1) return 2;
    if (v == 2) return 3;
    // 有效：读取证书主体，判断是否微软 Windows 签名
    HCERTSTORE hStore = nullptr; HCRYPTMSG hMsg = nullptr;
    DWORD enc=0, cont=0, fmt=0; CMSG_SIGNER_INFO* signer=nullptr; CERT_INFO* ci=nullptr;
    BOOL q = CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
        CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0,
        &enc, &cont, &fmt, &hStore, &hMsg, nullptr);
    int cls = 1;
    if (q) {
        DWORD info=0;
        CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &info);
        std::vector<BYTE> sb(info);
        if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, sb.data(), &info)) {
            signer = (CMSG_SIGNER_INFO*)sb.data();
            CERT_INFO ci2; ZeroMemory(&ci2,sizeof(ci2));
            ci2.Issuer = signer->Issuer;
            ci2.SerialNumber = signer->SerialNumber;
            PCCERT_CONTEXT pc = CertFindCertificateInStore(hStore,
                X509_ASN_ENCODING|PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &ci2, nullptr);
            if (pc) {
                wchar_t sub[512]={0};
                CertGetNameStringW(pc, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, sub, 512);
                std::wstring s = Lower(sub);
                if (s.find(L"microsoft windows") != std::wstring::npos ||
                    s.find(L"microsoft windows hardware") != std::wstring::npos ||
                    s.find(L"microsoft corporation") != std::wstring::npos)
                    cls = 0;
                CertFreeCertificateContext(pc);
            }
        }
    }
    if (hStore) CertCloseStore(hStore,0);
    if (hMsg) CryptMsgClose(hMsg);
    return cls;
}

// 返回 0=有效可信签名; 1=无签名; 2=签名无效或不可信
static int VerifyFileSignature(const std::wstring& path) {
    WINTRUST_FILE_INFO fi; ZeroMemory(&fi, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd; ZeroMemory(&wd, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;
    LONG rc = WinVerifyTrust(nullptr, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    if (rc == 0) return 0;
    if (rc == TRUST_E_NOSIGNATURE) return 1;
    return 2;
}

static bool ReadAllBytes(const std::wstring& path, std::vector<BYTE>& buf) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 200*1024*1024) { CloseHandle(h); return false; }
    buf.resize((size_t)sz.QuadPart);
    DWORD got = 0; BOOL ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(h);
    return ok && got == buf.size();
}

static DWORD RvaToOffset(const BYTE* base, DWORD rva) {
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (rva >= sec[i].VirtualAddress &&
            rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
            return rva - sec[i].VirtualAddress + sec[i].PointerToRawData;
    return 0;
}

// 静态解析 PE 导入表，返回导入模块文件名（小写）
static std::vector<std::wstring> PeImportedModules(const std::wstring& path) {
    std::vector<std::wstring> names;
    std::vector<BYTE> buf;
    if (!ReadAllBytes(path, buf) || buf.size() < sizeof(IMAGE_DOS_HEADER)) return names;
    BYTE* base = buf.data();
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return names;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return names;
    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRva) return names;
    DWORD impOff = RvaToOffset(base, impRva);
    if (!impOff || impOff + sizeof(IMAGE_IMPORT_DESCRIPTOR) > buf.size()) return names;
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + impOff);
    for (; d->Name; ++d) {
        DWORD no = RvaToOffset(base, d->Name);
        if (!no || no >= buf.size() || no + 256 > buf.size()) continue;
        std::string s((char*)(base + no));
        size_t bs = s.find_last_of("\\/");
        if (bs != std::string::npos) s = s.substr(bs+1);
        int wn = MultiByteToWideChar(CP_ACP,0,s.c_str(),-1,nullptr,0);
        std::wstring ws(wn,0);
        MultiByteToWideChar(CP_ACP,0,s.c_str(),-1,&ws[0],wn);
        if (!ws.empty() && ws.back()==0) ws.pop_back();
        ws = Lower(ws);
        if (!ws.empty()) names.push_back(ws);
    }
    return names;
}

// 名称是否随机/哈希样式（GUID、长hex、无元音随机串、长纯数字）
static bool LooksRandomName(const std::wstring& name0) {
    std::wstring s;
    for (wchar_t ch : Lower(name0)) if (iswalnum(ch)) s += ch;
    if (s.size() < 8) return false;
    bool allHex=true, hasDigit=false;
    for (wchar_t ch : s) {
        if (iswdigit(ch)) hasDigit=true;
        else if (!(ch>=L'a' && ch<=L'f')) allHex=false;
    }
    if (allHex && hasDigit) return true;
    bool allDig=true; for (wchar_t ch:s) if(!iswdigit(ch)) allDig=false;
    if (allDig) return true;
    if (s.size()>=10) {
        int vow=0, letters=0;
        for (wchar_t ch:s) if(iswalpha(ch)){letters++;
            if(ch==L'a'||ch==L'e'||ch==L'i'||ch==L'o'||ch==L'u') vow++;}
        if (letters>=6 && (double)vow/letters < 0.18) return true;
    }
    return false;
}

// 目录内（最多两层）是否出现关键字
static bool DirTreeHasKeyword(const std::wstring& dir, const std::vector<std::wstring>& kw) {
    std::vector<std::wstring> cur = { dir };
    std::vector<std::wstring> nxt;
    for (int depth=0; depth<2; ++depth) {
        for (auto& c : cur) {
            WIN32_FIND_DATAW fd;
            HANDLE hf = FindFirstFileW((c+L"\\*").c_str(), &fd);
            if (hf==INVALID_HANDLE_VALUE) continue;
            do {
                if (wcscmp(fd.cFileName,L".")==0||wcscmp(fd.cFileName,L"..")==0) continue;
                if (MatchKeyword(fd.cFileName,kw)) { FindClose(hf); return true; }
                if (depth==0 && (fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))
                    nxt.push_back(c+L"\\"+fd.cFileName);
            } while(FindNextFileW(hf,&fd));
            FindClose(hf);
        }
        cur = nxt; nxt.clear();
    }
    return false;
}

static std::vector<ResidualItem> ScanResidual(const AppInfo& a, std::wstring& note) {
    std::vector<ResidualItem> out;
    std::vector<std::wstring> kw = BuildKeywords(a);
    auto seen = [&](const std::wstring& key)->bool {
        for (auto& i : out) {
            std::wstring k = i.type == R_REG || i.type == R_REGV ? i.regSubkey + i.regValue : i.path;
            if (k == key) return true;
        }
        return false;
    };
    auto addDir = [&](const std::wstring& p) {
        if (p.empty() || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        if (seen(p)) return;
        ResidualItem r; r.type = R_DIR; r.path = p; r.display = PathFindFileNameW(p.c_str());
        out.push_back(r);
    };
    auto addFile = [&](const std::wstring& p, bool sys) {
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        if (seen(p)) return;
        ResidualItem r; r.type = R_FILE; r.path = p; r.allowSystem = sys;
        r.display = PathFindFileNameW(p.c_str());
        out.push_back(r);
    };
    auto addReg = [&](HKEY root, const std::wstring& sub, DWORD view, bool svcKey = false) {
        HKEY h; if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS) return;
        RegCloseKey(h);
        std::wstring key = sub + std::to_wstring((long long)root) + std::to_wstring(view);
        if (seen(key)) return;
        ResidualItem r; r.type = R_REG; r.regRoot = root; r.regView = view;
        r.regSubkey = sub; r.display = PathFindFileNameW(sub.c_str());
        r.isServicesKey = svcKey;
        out.push_back(r);
    };

    // 1) 主安装目录
    addDir(a.location);

    // 2) AppData / ProgramData / Program Files 同厂商目录（一层）
    std::vector<std::wstring> bases = {
        EnvPath(L"APPDATA"), EnvPath(L"LOCALAPPDATA"),
        L"C:\\ProgramData", L"C:\\Program Files", L"C:\\Program Files (x86)",
    };
    for (const std::wstring& base : bases) {
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW((base + L"\\*").c_str(), &fd);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring nm = fd.cFileName;
            std::wstring dpath = base + L"\\" + nm;
            if (MatchKeyword(nm, kw))
                addDir(dpath);
            else if (LooksRandomName(nm) && DirTreeHasKeyword(dpath, kw))
                addDir(dpath);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }

    // 3) Services 键直扫
    std::wstring svcRoot = L"SYSTEM\\CurrentControlSet\\Services";
    HKEY hSvc;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, svcRoot.c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &hSvc) == ERROR_SUCCESS) {
        DWORD i = 0;
        for (;;) {
            wchar_t name[256]; DWORD nlen = 256;
            if (RegEnumKeyExW(hSvc, i++, name, &nlen, nullptr, nullptr,
                              nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring sub = svcRoot + L"\\" + name;
            std::wstring img = RegGetStr(HKEY_LOCAL_MACHINE, sub, L"ImagePath", KEY_WOW64_64KEY);
            std::wstring dsp = RegGetStr(HKEY_LOCAL_MACHINE, sub, L"DisplayName", KEY_WOW64_64KEY);
            bool hit = MatchKeyword(name, kw) || MatchKeyword(dsp, kw) ||
                       (!a.location.empty() && Lower(img).find(Lower(a.location)) != std::wstring::npos);
            if (!hit) continue;
            // 系统路径（System32/drivers）只删服务注册；文件由 drivers 扫描处理
            addReg(HKEY_LOCAL_MACHINE, sub, KEY_WOW64_64KEY, true);
        }
        RegCloseKey(hSvc);
    }

    // 4) drivers 目录：微软系统签名保护；第三方/无签名且名字命中才删
    wchar_t winDir[MAX_PATH]; GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring drivers = std::wstring(winDir) + L"\\System32\\drivers";
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((drivers + L"\\*.sys").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            std::wstring nm = fd.cFileName;
            std::wstring full = drivers + L"\\" + nm;
            if (ClassifyFileSignature(full) == 0) continue;  // 微软系统驱动，保护
            if (MatchKeyword(nm, kw)) addFile(full, true);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }

    // 4b) PE 静态依赖分析：找主目录内 PE 引用的、位于 drivers/System32/主目录的非系统 dll/sys
    {
        std::vector<std::wstring> peFiles;
        std::wstring locLow = Lower(a.location);
        std::vector<std::wstring> walk = { a.location };
        while (!walk.empty()) {
            std::wstring c = walk.back(); walk.pop_back();
            WIN32_FIND_DATAW fx;
            HANDLE hx = FindFirstFileW((c+L"\\*").c_str(), &fx);
            if (hx==INVALID_HANDLE_VALUE) continue;
            do {
                if (wcscmp(fx.cFileName,L".")==0||wcscmp(fx.cFileName,L"..")==0) continue;
                std::wstring cf = c+L"\\"+fx.cFileName;
                if (fx.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) walk.push_back(cf);
                else {
                    std::wstring l = Lower(fx.cFileName);
                    if (l.size()>=4 &&
                        (l.substr(l.size()-4)==L".exe"||l.substr(l.size()-4)==L".dll"||
                         l.substr(l.size()-4)==L".sys")) peFiles.push_back(cf);
                }
            } while(FindNextFileW(hx,&fx));
            FindClose(hx);
        }
        std::vector<std::wstring> searchDirs = { drivers,
            std::wstring(winDir)+L"\\System32", a.location };
        for (auto& pe : peFiles) {
            auto deps = PeImportedModules(pe);
            for (auto& dep : deps) {
                for (auto& sd : searchDirs) {
                    std::wstring cand = sd + L"\\" + dep;
                    if (GetFileAttributesW(cand.c_str())==INVALID_FILE_ATTRIBUTES) continue;
                    bool inSys = IsSystemDirTree(cand);
                    int sig = ClassifyFileSignature(cand);
                    if (sig == 0) continue;        // 微软系统签名，保护
                    if (inSys) {
                        // 系统目录默认保护：仅当文件名明确命中目标关键字才纳入
                        if (MatchKeyword(dep, kw)) addFile(cand, true);
                    } else {
                        // 非系统目录的第三方/无签名依赖，纳入（用户勾选确认）
                        addFile(cand, false);
                    }
                }
            }
        }
    }

    // 5) 盘根一层目录
    std::wstring root = EnvPath(L"SystemDrive") + L"\\";
    hf = FindFirstFileW((root + L"*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (MatchKeyword(fd.cFileName, kw)) addDir(root + fd.cFileName);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }

    // 6) Uninstall 键 + 厂商私有 SOFTWARE 键
    if (!a.regSubkey.empty())
        addReg(a.regRoot,
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + a.regSubkey,
               a.regView);
    for (const std::wstring& k : kw) {
        addReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + k, KEY_WOW64_64KEY);
        addReg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + k, KEY_WOW64_32KEY);
        addReg(HKEY_CURRENT_USER, L"SOFTWARE\\" + k, 0);
    }

    // 7) 已命中厂商目录下的随机/哈希子目录
    {
        std::vector<std::wstring> known;
        for (auto& i : out) if (i.type==R_DIR) known.push_back(i.path);
        for (auto& kd : known) {
            std::vector<std::wstring> walk={kd}, nxt;
            for (int depth=0;depth<2;++depth) {
                for (auto& c:walk) {
                    WIN32_FIND_DATAW fx;
                    HANDLE hx=FindFirstFileW((c+L"\\*").c_str(),&fx);
                    if(hx==INVALID_HANDLE_VALUE) continue;
                    do {
                        if(wcscmp(fx.cFileName,L".")==0||wcscmp(fx.cFileName,L"..")==0) continue;
                        std::wstring cf=c+L"\\"+fx.cFileName;
                        if(fx.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
                            if(LooksRandomName(fx.cFileName)) addDir(cf);
                            if(depth==0) nxt.push_back(cf);
                        }
                    } while(FindNextFileW(hx,&fx));
                    FindClose(hx);
                }
                walk=nxt;nxt.clear();
            }
        }
    }

    note = L"关键字: ";
    for (auto& k : kw) note += k + L" ";
    return out;
}

// ------------------------------------------------------------------
// 强制删除引擎
// ------------------------------------------------------------------
struct ForceCtx {
    AppInfo app;
    HWND    hNotify;
    std::wstring log;
    std::vector<ResidualItem> items;
};

static void Log(ForceCtx* c, const std::wstring& s) {
    c->log += s; c->log += L"\r\n";
}

// 前向声明：路径/服务护栏定义在后面
static bool IsCriticalService(const std::wstring&);
static bool IsSystemDirTree(const std::wstring&);
static std::wstring NormPath(const std::wstring&);
static bool IsSafeToDeletePath(const std::wstring&, bool);

// 杀掉可执行路径位于安装目录内的所有进程
static void KillProcessesIn(ForceCtx* c, const std::wstring& dir) {
    if (dir.empty()) return;
    if (!IsSafeToDeletePath(dir, true)) {
        Log(c, L"  目录过于宽泛，跳过批量结束进程: " + dir); return;
    }
    std::wstring base = Lower(dir);
    if (base.back() != L'\\') base += L'\\';

    HANDLE ss = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (ss == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(ss, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == GetCurrentProcessId())
                continue;
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                    FALSE, pe.th32ProcessID);
            if (!hp) continue;
            wchar_t path[MAX_PATH] = {0}; DWORD len = MAX_PATH;
            BOOL ok = QueryFullProcessImageNameW(hp, 0, path, &len);
            if (ok && Lower(path).find(base) == 0) {
                TerminateProcess(hp, 1);
                wchar_t b[600];
                StringCchPrintfW(b, 600, L"  结束进程 %s (PID %u)", path, pe.th32ProcessID);
                Log(c, b);
            }
            CloseHandle(hp);
        } while (Process32NextW(ss, &pe));
    }
    CloseHandle(ss);
}

// 停并删除名称/ImagePath 命中目录或关键字的服务
static void RemoveMatchingServices(ForceCtx* c, const std::wstring& dir) {
    if (dir.empty()) return;
    if (!IsSafeToDeletePath(dir, true)) {
        Log(c, L"  目录过于宽泛，跳过批量服务删除: " + dir); return;
    }
    std::wstring base = Lower(dir);
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { Log(c, L"  无法打开服务管理器"); return; }

    std::wstring svcRoot = L"SYSTEM\\CurrentControlSet\\Services";
    HKEY hSvc;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, svcRoot.c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &hSvc) == ERROR_SUCCESS) {
        DWORD i = 0;
        for (;;) {
            wchar_t name[256]; DWORD nlen = 256;
            if (RegEnumKeyExW(hSvc, i++, name, &nlen, nullptr,
                              nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            std::wstring sub = svcRoot + L"\\" + name;
            std::wstring img = RegGetStr(HKEY_LOCAL_MACHINE, sub, L"ImagePath", KEY_WOW64_64KEY);
            std::wstring imgLow = Lower(img);
            // 去掉 \??\ 前缀
            if (imgLow.find(L"\\??\\") == 0) imgLow = imgLow.substr(4);
            if (imgLow.find(base) != 0) continue;
            if (IsCriticalService(name)) {
                Log(c, L"  系统关键服务，跳过 " + std::wstring(name)); continue;
            }

            SC_HANDLE h = OpenServiceW(scm, name, SERVICE_ALL_ACCESS);
            if (h) {
                SERVICE_STATUS ss2;
                ControlService(h, SERVICE_CONTROL_STOP, &ss2);
                Sleep(300);
                if (DeleteService(h)) {
                    wchar_t b[400];
                    StringCchPrintfW(b, 400, L"  停止并删除服务 %s", name);
                    Log(c, b);
                }
                CloseServiceHandle(h);
            }
            // 删除服务注册表键（即使 OpenService 失败）
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, sub.c_str());
        }
        RegCloseKey(hSvc);
    }
    CloseServiceHandle(scm);
}

// 递归删除；失败文件登记重启删除
static void DeleteRecursive(ForceCtx* c, const std::wstring& path, bool isDir) {
    if (isDir) {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return;
        if (attr & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);

        std::wstring pattern = path + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW(pattern.c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;
                std::wstring child = path + L"\\" + fd.cFileName;
                bool childDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                DeleteRecursive(c, child, childDir);
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        if (!RemoveDirectoryW(path.c_str())) {
            MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            Log(c, L"  目录将在重启后删除: " + path);
        }
    } else {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return;
        if (attr & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
        if (!DeleteFileW(path.c_str())) {
            if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
                Log(c, L"  文件将在重启后删除: " + path);
        }
    }
}

// takeown + icacls 夺权
static void TakeOwnership(ForceCtx* c, const std::wstring& dir) {
    std::wstring cmd = L"takeown.exe /f \"" + dir + L"\" /r /d y >nul 2>&1 & "
                       L"icacls.exe \"" + dir + L"\" /grant administrators:F /t >nul 2>&1";
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    sei.lpVerb = L"open";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = (L"/c " + cmd).c_str();
    sei.nShow = SW_HIDE;
    ShellExecuteExW(&sei);
    if (sei.hProcess) {
        DWORD wr = WaitForSingleObject(sei.hProcess, 45000);
        if (wr == WAIT_TIMEOUT) {
            TerminateProcess(sei.hProcess, 1);
            Log(c, L"  夺权超时，已跳过 " + dir);
        } else {
            Log(c, L"  已夺取所有权并授权");
        }
        CloseHandle(sei.hProcess);
    }
}

static bool IsSystemDirTree(const std::wstring& p) {
    wchar_t winDir[MAX_PATH]; GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring wl = Lower(winDir);
    std::wstring pl = Lower(p);
    return pl == wl || pl.find(wl + L"\\") == 0;
}

// 系统关键服务/驱动黑名单：任何情况下都不允许禁用或删除，杜绝死机
static bool IsCriticalService(const std::wstring& n0) {
    std::wstring n = Lower(n0);
    static const wchar_t* crit[] = {
        // 启动 / 总线 / 电源
        L"acpi", L"pci", L"plugplay", L"power", L"pdc", L"watchdog", L"pcw", L"pshed",
        // 存储 / 卷 / 文件系统
        L"disk", L"partmgr", L"volmgr", L"volmgrx", L"volsnap", L"mountmgr", L"ntfs",
        L"fvevol", L"storahci", L"stornvme", L"pciide", L"intelide", L"iastor", L"iastorv",
        L"iastordatamgr", L"iastoravc", L"stexstor", L"spaceport", L"sdstor", L"vdrvroot",
        L"fileinfo", L"filetrace", L"fltmgr", L"gpt_loader", L"swenum", L"errdev",
        // 网络栈
        L"ndis", L"tcpip", L"netio", L"afd", L"http", L"dnscache", L"nsi", L"netbt",
        L"mslldp", L"iphlpsvc", L"dhcp", L"lanmanserver", L"lanmanworkstation",
        // 内核安全 / 代码完整性 / 密码学
        L"ci", L"cng", L"ksecdd", L"ksecpkg", L"bcrypt", L"ncrypt", L"clfs", L"tm",
        L"tpm", L"tpmwmi", L"win32k", L"win32kbase", L"win32kfull", L"dxgkrnl", L"dxgmms2",
        // 驱动框架
        L"wdf01000", L"wdfldr", L"wudfpf", L"wudfrd", L"vmbus", L"winhypervisor",
        // Windows Defender / 防火墙（不由本工具处理）
        L"wdboot", L"wdfilter", L"wdnisdrv", L"wdenable", L"windefend", L"mpssvc",
        L"mpsdrv", L"bfe", L"nissrv", L"securityhealthservice",
        // 关键系统服务
        L"rpcss", L"rpceptmapper", L"dcomlaunch", L"eventlog", L"winmgmt", L"schedule",
        L"profsvc", L"userprofile", L"appinfo", L"appidsvc", L"cryptsvc", L"msiserver",
        L"wininit", L"user manager", L"local session manager",
    };
    for (auto c : crit) if (n == c) return true;
    return false;
}

// 规范化路径：转绝对、小写、去尾部多余反斜杠（保留盘符根 c:\）
static std::wstring NormPath(const std::wstring& in) {
    wchar_t full[MAX_PATH] = {0};
    GetFullPathNameW(in.c_str(), MAX_PATH, full, nullptr);
    std::wstring p = Lower(full);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

// 文件/目录删除前的硬护栏。isDir=true 时对 Windows 树整体禁止递归。
static bool IsSafeToDeletePath(const std::wstring& in, bool isDir) {
    std::wstring p = NormPath(in);
    if (p.size() <= 3) return false;                       // 盘符根目录
    wchar_t winDir[MAX_PATH]; GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring win = NormPath(winDir);
    std::wstring sd = win.substr(0, 3);                   // 系统盘根
    // Windows 目录树：目录一律不递归；具体文件交由 allowSystem 单文件流程
    if (p == win || p.find(win + L"\\") == 0)
        return !isDir;
    // 受保护的精确目录
    std::vector<std::wstring> deny = {
        sd + L"program files", sd + L"program files (x86)", sd + L"programdata",
        sd + L"users", sd + L"perflogs", sd + L"recovery", sd + L"boot",
        sd + L"$recycle.bin", sd + L"system volume information", sd + L"msocache",
        win + L"\\system32", win + L"\\syswow64", win + L"\\winsxs",
        win + L"\\servicing", win + L"\\assembly", win + L"\\installer",
        win + L"\\boot", win + L"\\system32\\drivers",
    };
    for (auto& d : deny) if (p == d) return false;
    // 用户配置关键目录
    wchar_t prof[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, prof);
    std::wstring up = NormPath(prof);
    std::vector<std::wstring> denyProf = {
        up, up + L"\\appdata", up + L"\\appdata\\roaming", up + L"\\appdata\\local",
        up + L"\\appdata\\locallow", up + L"\\documents", up + L"\\desktop",
        up + L"\\downloads", up + L"\\start menu", up + L"\\start menu\\programs",
        up + L"\\favorites", up + L"\\pictures", up + L"\\videos", up + L"\\music",
    };
    for (auto& d : denyProf) if (p == d) return false;
    // 深度：盘符后至少两段（C:\Program Files\Xxx），防止删关键父目录
    if (CountSegments(p) < 3) return false;
    return true;
}

static void StopDeleteServiceByName(ForceCtx* c, const std::wstring& svc) {
    if (IsCriticalService(svc)) {
        Log(c, L"  系统关键服务，跳过 " + svc); return;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    SC_HANDLE h = OpenServiceW(scm, svc.c_str(), SERVICE_ALL_ACCESS);
    if (h) {
        SERVICE_STATUS st; ControlService(h, SERVICE_CONTROL_STOP, &st);
        Sleep(200);
        if (DeleteService(h)) Log(c, L"  停止并删除服务 " + svc);
        CloseServiceHandle(h);
    }
    CloseServiceHandle(scm);
}

static void ExecuteItems(ForceCtx* c) {
    const std::wstring SP = L"SYSTEM\\CurrentControlSet\\Services\\";
    Log(c, L"[1/4] 结束占用进程...");
    KillProcessesIn(c, c->app.location);
    Log(c, L"[2/4] 停止并删除服务/驱动...");
    for (auto& it : c->items) {
        if (it.type == R_REG && it.regSubkey.find(SP) == 0)
            StopDeleteServiceByName(c, it.regSubkey.substr(SP.size()));
    }
    Log(c, L"[3/4] 夺权并删除文件...");
    for (auto& it : c->items) {
        if (it.type == R_DIR) {
            if (!IsSafeToDeletePath(it.path, true)) {
                Log(c, L"  受保护路径，跳过 " + it.path); continue;
            }
            TakeOwnership(c, it.path);
        } else if (it.type == R_FILE) {
            if (!IsSafeToDeletePath(it.path, false)) {
                Log(c, L"  受保护路径，跳过 " + it.path); continue;
            }
            if (IsSystemDirTree(it.path) && !it.allowSystem) {
                Log(c, L"  受保护，跳过 " + it.path); continue;
            }
            if (it.allowSystem) {
                // 单文件夺权
                std::wstring cmd = L"takeown.exe /f \"" + it.path + L"\" >nul 2>&1 & icacls.exe \""
                                 + it.path + L"\" /grant administrators:F >nul 2>&1";
                SHELLEXECUTEINFOW s = { sizeof(s) };
                s.lpFile = L"cmd.exe"; s.lpParameters = (L"/c " + cmd).c_str();
                s.nShow = SW_HIDE; s.fMask = SEE_MASK_NOCLOSEPROCESS;
                ShellExecuteExW(&s);
                if (s.hProcess) { WaitForSingleObject(s.hProcess, 10000); CloseHandle(s.hProcess); }
            }
        }
    }
    KillProcessesIn(c, c->app.location);
    for (auto& it : c->items) {
        if (it.type == R_DIR) {
            if (!IsSafeToDeletePath(it.path, true)) continue;
            DeleteRecursive(c, it.path, true);
        } else if (it.type == R_FILE) {
            if (!IsSafeToDeletePath(it.path, false)) continue;
            if (IsSystemDirTree(it.path) && !it.allowSystem) continue;
            DeleteRecursive(c, it.path, false);
        }
    }
    Log(c, L"[4/4] 删除注册表项...");
    std::vector<std::wstring> safeKw = BuildKeywords(c->app);
    for (auto& it : c->items) {
        if (it.type == R_REG) {
            bool ok = SafeDeleteRegItem(it.regRoot, it.regSubkey, safeKw,
                                        it.regView, it.isServicesKey);
            if (!ok) Log(c, L"  受保护，未删除注册表 " + it.regSubkey);
            // 32/64 双视图兜底同样走安全校验
        } else if (it.type == R_REGV) {
            // 删单个值：仅允许 Uninstall / 厂商私有路径，系统键黑名单拦截
            if (!IsProtectedRegSubkey(it.regSubkey)) {
                HKEY h;
                if (RegOpenKeyExW(it.regRoot, it.regSubkey.c_str(), 0,
                                  KEY_SET_VALUE | it.regView, &h) == ERROR_SUCCESS) {
                    RegDeleteValueW(h, it.regValue.c_str());
                    RegCloseKey(h);
                }
            }
        }
    }
    Log(c, L"完成。");
}

static DWORD WINAPI ForceThread(LPVOID p) {
    ForceCtx* c = (ForceCtx*)p;
    ExecuteItems(c);
    PostMessageW(c->hNotify, WM_FORCE_DONE, 0, (LPARAM)c);
    return 0;
}

// ------------------------------------------------------------------
// 残留勾选对话框 + 安全模式
// ------------------------------------------------------------------
#define DL_DELETE   4001
#define DL_SAFE     4002
#define DL_ALL      4003
#define DL_NONE     4004
#define DL_CANCEL   4005
#define IDC_DLLIST  4006

static const wchar_t* SAFEDIR = L"C:\\ProgramData\\MiniGeekSafeClean";
static const wchar_t* SAFEEXE = L"C:\\ProgramData\\MiniGeekSafeClean\\clean.exe";
static const wchar_t* SAFEPLAN = L"C:\\ProgramData\\MiniGeekSafeClean\\pending.dat";
static const wchar_t* SAFETASK = L"MiniGeekSafeClean";

// 两段式强制删除（重启续删）
static const wchar_t* RESUMEDIR = L"C:\\ProgramData\\MiniGeekResume";
static const wchar_t* RESUMEPLAN = L"C:\\ProgramData\\MiniGeekResume\\plan.dat";
static const wchar_t* RUNONCE_SUB =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce";

struct DlgState {
    AppInfo app;
    std::vector<ResidualItem> items;
    HWND hTree;
    bool forceMode = false;
    std::vector<ResidualItem> pool;   // 树节点 lParam 存此 vector 的下标
};
static DlgState g_dlg;

// 两段式阶段二：重启续删（需在主窗过程之前声明）
static bool g_resumeWanted = false;
static AppInfo g_resumeApp;
static std::vector<ResidualItem> g_resumeItems;

static int RunHidden(const std::wstring& cmd) {
    SHELLEXECUTEINFOW s = { sizeof(s) };
    s.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    s.lpFile = L"cmd.exe";
    s.lpParameters = (L"/c " + cmd).c_str();
    s.nShow = SW_HIDE;
    ShellExecuteExW(&s);
    DWORD code = 1;
    if (s.hProcess) { WaitForSingleObject(s.hProcess, 20000); GetExitCodeProcess(s.hProcess, &code); CloseHandle(s.hProcess); }
    return (int)code;
}

static std::wstring EncodeRoot(HKEY r) { return r == HKEY_CURRENT_USER ? L"1" : L"0"; }
static HKEY DecodeRoot(const std::wstring& s) { return s == L"1" ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }

static void WritePlan(const wchar_t* path, const AppInfo& app,
                      std::vector<ResidualItem>& items) {
    HANDLE hf = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto wl = [&](const std::wstring& line) {
        std::wstring l = line + L"\n";
        DWORD wr; WriteFile(hf, l.c_str(), l.size() * 2, &wr, nullptr);
    };
    // 写 UTF-16LE BOM
    WORD bom = 0xFEFF; DWORD wr; WriteFile(hf, &bom, 2, &wr, nullptr);
    wl(L"A\t" + app.name + L"\t" + app.location + L"\t" + app.regSubkey + L"\t"
       + EncodeRoot(app.regRoot) + L"\t" + std::to_wstring(app.regView));
    for (auto& i : items) {
        wl(L"I\t" + std::to_wstring(i.type) + L"\t" + i.path + L"\t" + i.regSubkey + L"\t"
           + EncodeRoot(i.regRoot) + L"\t" + std::to_wstring(i.regView) + L"\t"
           + (i.allowSystem ? L"1" : L"0") + L"\t" + i.display);
    }
    CloseHandle(hf);
}

static std::vector<std::wstring> SplitTab(const std::wstring& s) {
    std::vector<std::wstring> out; std::wstring cur;
    for (wchar_t c : s) { if (c == L'\t') { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur);
    return out;
}

static bool ReadPlan(const wchar_t* path, AppInfo& app,
                     std::vector<ResidualItem>& items) {
    HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(hf, nullptr);
    std::wstring data(sz / 2, L'\0');
    DWORD rd; ReadFile(hf, &data[0], sz, &rd, nullptr);
    CloseHandle(hf);
    if (!data.empty() && data[0] == 0xFEFF) data = data.substr(1);
    size_t s = 0;
    while (s <= data.size()) {
        size_t e = data.find(L'\n', s);
        std::wstring line = data.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        auto f = SplitTab(line);
        if (f.size() >= 6 && f[0] == L"A") {
            app.name = f[1]; app.location = f[2]; app.regSubkey = f[3];
            app.regRoot = DecodeRoot(f[4]); app.regView = (DWORD)_wtoi(f[5].c_str());
        } else if (f.size() >= 8 && f[0] == L"I") {
            ResidualItem i; i.type = (RType)_wtoi(f[1].c_str()); i.path = f[2];
            i.regSubkey = f[3]; i.regRoot = DecodeRoot(f[4]);
            i.regView = (DWORD)_wtoi(f[5].c_str());
            i.allowSystem = f[6] == L"1"; i.display = f[7];
            items.push_back(i);
        }
        if (e == std::wstring::npos) break;
        s = e + 1;
    }
    return true;
}

// 部署安全模式清理
static const wchar_t* WINLOGON_SUB =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

static bool SetRegSz(HKEY root, const std::wstring& sub, const std::wstring& val,
                     const std::wstring& data, DWORD view) {
    HKEY h;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE | view, &h) != ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExW(h, val.c_str(), 0, REG_SZ,
                  (const BYTE*)data.c_str(),
                  (DWORD)((data.size() + 1) * 2)) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok;
}

static bool PrepareSafeMode(const AppInfo& app, std::vector<ResidualItem>& items,
                            std::wstring& detail) {
    CreateDirectoryW(SAFEDIR, nullptr);
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    CopyFileW(self, SAFEEXE, FALSE);
    WritePlan(SAFEPLAN, app, items);

    // 1) 备份并改写 Winlogon Userinit（安全模式必经，不依赖任务计划服务）
    std::wstring orig = RegGetStr(HKEY_LOCAL_MACHINE, WINLOGON_SUB,
                                  L"Userinit", KEY_WOW64_64KEY);
    if (orig.empty()) orig = L"C:\\Windows\\system32\\userinit.exe,";
    // 备份原值到文件（UTF-16）
    HANDLE hb = CreateFileW(L"C:\\ProgramData\\MiniGeekSafeClean\\userinit.bak",
                            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr;
    WriteFile(hb, orig.c_str(), (DWORD)(orig.size() * 2), &wr, nullptr);
    CloseHandle(hb);

    std::wstring base = orig;
    if (!base.empty() && base.back() == L',') base.pop_back();
    std::wstring newInit = base + L"," + SAFEEXE;
    bool u1 = SetRegSz(HKEY_LOCAL_MACHINE, WINLOGON_SUB, L"Userinit",
                       newInit, KEY_WOW64_64KEY);

    // 2) Schedule 白名单（双保险，失败不阻断）
    RunHidden(L"reg.exe add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Minimal\\Schedule\" /ve /t REG_SZ /d Service /f");

    // 3) 安全模式引导
    int r3 = RunHidden(L"bcdedit.exe /set {current} safeboot minimal");
    detail = L"Userinit=" + std::wstring(u1 ? L"OK" : L"FAIL")
           + L" 引导=" + std::to_wstring(r3);
    if (!u1 || r3 != 0) return false;
    return true;
}

// 安全模式清理主流程
static int RunSafeClean() {
    std::wstring logPath = std::wstring(SAFEDIR) + L"\\clean.log";
    FILE* lg = _wfopen(logPath.c_str(), L"a, ccs=UTF-8");
    auto wlog = [&](const std::wstring& s) { if (lg) { fwprintf(lg, L"%s\n", s.c_str()); fflush(lg); } };
    wlog(L"==== 安全模式清理开始 ====");

    // 最先恢复正常引导（兜底，防止困死）
    RunHidden(L"bcdedit.exe /deletevalue {current} safeboot");
    wlog(L"已恢复正常引导");
    // 立刻恢复 Userinit 原值（兜底，即使后续崩溃也不残留）
    HANDLE hb = CreateFileW(L"C:\\ProgramData\\MiniGeekSafeClean\\userinit.bak",
                            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hb != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(hb, nullptr);
        std::wstring orig(sz / 2, L'\0');
        DWORD rd; ReadFile(hb, &orig[0], sz, &rd, nullptr);
        CloseHandle(hb);
        SetRegSz(HKEY_LOCAL_MACHINE, WINLOGON_SUB, L"Userinit", orig, KEY_WOW64_64KEY);
        wlog(L"已恢复 Userinit");
    }

    AppInfo app; std::vector<ResidualItem> items;
    if (ReadPlan(SAFEPLAN, app, items)) {
        wlog(L"读取计划: " + std::to_wstring(items.size()) + L" 项, app=" + app.name);
        std::wstring note;
        std::vector<ResidualItem> fresh = ScanResidual(app, note);
        for (auto& f : fresh) {
            bool exists = false;
            for (auto& o : items) {
                if (o.type == f.type &&
                    (f.type == R_REG ? o.regSubkey == f.regSubkey : o.path == f.path)) {
                    if (f.allowSystem) o.allowSystem = true;
                    exists = true; break;
                }
            }
            if (!exists) items.push_back(f);
        }
        ForceCtx c; c.app = app; c.items = items; c.hNotify = nullptr;
        ExecuteItems(&c);
        wlog(c.log);
    } else {
        wlog(L"找不到清理计划");
    }

    // 收尾：删任务、白名单、备份文件
    RunHidden(std::wstring(L"schtasks.exe /delete /tn \"") + SAFETASK + L"\" /f");
    RunHidden(L"reg.exe delete \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Minimal\\Schedule\" /f");
    DeleteFileW(L"C:\\ProgramData\\MiniGeekSafeClean\\userinit.bak");
    wlog(L"已清理收尾，准备重启");
    if (lg) fclose(lg);
    Sleep(1000);
    RunHidden(L"shutdown.exe /r /t 2");
    return 0;
}

static void StartDelete(std::vector<ResidualItem>& chosen, const AppInfo& app) {
    ForceCtx* c = new ForceCtx;
    c->app = app; c->items = chosen; c->hNotify = g_hMain;
    EnableWindow(g_hList, FALSE);
    SetWindowTextW(g_hStatus, L"正在删除...");
    CreateThread(nullptr, 0, ForceThread, c, 0, nullptr);
}

// ================= TreeView 残留窗辅助 =================
static HTREEITEM TVInsert(HWND ht, HTREEITEM parent, const std::wstring& text, int idx) {
    TVINSERTSTRUCTW ins; ZeroMemory(&ins, sizeof(ins));
    ins.hParent = parent ? parent : TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = (LPWSTR)text.c_str();
    ins.item.lParam = idx;
    return TreeView_InsertItem(ht, &ins);
}
static void TVSetCheck(HWND ht, HTREEITEM node, bool chk) {
    TVITEMW it; ZeroMemory(&it, sizeof(it));
    it.hItem = node; it.mask = TVIF_STATE; it.stateMask = TVIS_STATEIMAGEMASK;
    it.state = INDEXTOSTATEIMAGEMASK(chk ? 2 : 1);
    TreeView_SetItem(ht, &it);
}
static void TVCascade(HWND ht, HTREEITEM node, bool chk) {
    TVSetCheck(ht, node, chk);
    HTREEITEM c = TreeView_GetChild(ht, node);
    while (c) { TVCascade(ht, c, chk); c = TreeView_GetNextSibling(ht, c); }
}
// 递归枚举文件系统，把子目录/文件加入树（深度限制 3 层，防止卡顿）
static void AddFsChildren(HWND ht, HTREEITEM parent, const std::wstring& dir, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring child = dir + L"\\" + fd.cFileName;
        ResidualItem r; r.display = fd.cFileName;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        r.type = isDir ? R_DIR : R_FILE; r.path = child;
        int idx = (int)g_dlg.pool.size(); g_dlg.pool.push_back(r);
        HTREEITEM node = TVInsert(ht, parent, fd.cFileName, idx);
        if (isDir && depth < 3) AddFsChildren(ht, node, child, depth + 1);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}
// 收集：节点勾选且无勾选祖先 → 加入 chosen（勾整目录只加目录项）
static void CollectWalk(HWND ht, HTREEITEM node, bool ancestorChk,
                        std::vector<ResidualItem>& chosen) {
    bool self = TreeView_GetCheckState(ht, node) == 1;
    if (self && !ancestorChk) {
        TVITEMW q; ZeroMemory(&q, sizeof(q));
        q.hItem = node; q.mask = TVIF_PARAM;
        TreeView_GetItem(ht, &q);
        chosen.push_back(g_dlg.pool[(int)q.lParam]);
    }
    HTREEITEM c = TreeView_GetChild(ht, node);
    while (c) { CollectWalk(ht, c, self || ancestorChk, chosen);
                c = TreeView_GetNextSibling(ht, c); }
}

static void StartForceTwoStage(HWND h, const AppInfo& app,
                               std::vector<ResidualItem>& chosen);

static LRESULT CALLBACK WndResidual(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            10, 10, 690, 20, h, (HMENU)4100, g_hInst, nullptr);
        g_dlg.pool.clear();
        g_dlg.hTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
            TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_CHECKBOXES | TVS_FULLROWSELECT,
            10, 38, 690, 380, h, (HMENU)(INT_PTR)IDC_DLLIST, g_hInst, nullptr);
        int topN = 0;
        for (auto& it : g_dlg.items) {
            int idx = (int)g_dlg.pool.size(); g_dlg.pool.push_back(it);
            HTREEITEM node = TVInsert(g_dlg.hTree, nullptr, it.display, idx);
            if (it.type == R_DIR) AddFsChildren(g_dlg.hTree, node, it.path, 0);
            ++topN;
        }
        // 默认全部勾选
        HTREEITEM rr = TreeView_GetRoot(g_dlg.hTree);
        while (rr) { TVCascade(g_dlg.hTree, rr, true);
                     rr = TreeView_GetNextSibling(g_dlg.hTree, rr); }
        wchar_t t[128];
        wsprintfW(t, L"发现 %d 项残留，可展开目录查看并勾选要清理的项目", topN);
        SetWindowTextW(GetDlgItem(h, 4100), t);

        const wchar_t* bn[] = { L"全选", L"全不选", L"删除选中", L"取消" };
        int bid[] = { DL_ALL, DL_NONE, DL_DELETE, DL_CANCEL };
        int bx[] = { 10, 120, 520, 628 };
        int bw[] = { 100, 100, 100, 72 };
        for (int i = 0; i < 4; ++i)
            CreateWindowW(L"BUTTON", bn[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                bx[i], 430, bw[i], 30, h, (HMENU)(INT_PTR)bid[i], g_hInst, nullptr);

        if (!g_hFont) {
            HDC hdc0 = GetDC(nullptr);
            int fh = -MulDiv(9, GetDeviceCaps(hdc0, LOGPIXELSY), 72);
            ReleaseDC(nullptr, hdc0);
            g_hFont = CreateFontW(fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        EnumChildWindows(h, [](HWND c, LPARAM x) -> BOOL {
            SendMessageW(c, WM_SETFONT, (WPARAM)x, TRUE); return TRUE;
        }, (LPARAM)g_hFont);
        break;
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_DLLIST && nm->code == TVN_ITEMCHANGEDW) {
            NMTVITEMCHANGE* tc = (NMTVITEMCHANGE*)nm;
            if (tc->uChanged & TVIF_STATE) {
                int si = (tc->uStateNew & TVIS_STATEIMAGEMASK) >> 12;
                TVCascade(g_dlg.hTree, tc->hItem, si == 2);
            }
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == DL_ALL || id == DL_NONE) {
            bool chk = id == DL_ALL;
            HTREEITEM rr = TreeView_GetRoot(g_dlg.hTree);
            while (rr) { TVCascade(g_dlg.hTree, rr, chk);
                         rr = TreeView_GetNextSibling(g_dlg.hTree, rr); }
        } else if (id == DL_CANCEL) {
            DestroyWindow(h);
        } else if (id == DL_DELETE) {
            std::vector<ResidualItem> chosen;
            HTREEITEM rr = TreeView_GetRoot(g_dlg.hTree);
            while (rr) { CollectWalk(g_dlg.hTree, rr, false, chosen);
                         rr = TreeView_GetNextSibling(g_dlg.hTree, rr); }
            if (chosen.empty()) {
                MessageBoxW(h, L"没有勾选任何项目。", L"提示", MB_OK); break;
            }
            if (g_dlg.forceMode) {
                StartForceTwoStage(h, g_dlg.app, chosen);
            } else {
                DestroyWindow(h);
                StartDelete(chosen, g_dlg.app);
            }
        }
        break;
    }
    case WM_CLOSE: DestroyWindow(h); break;
    case WM_DESTROY:
        EnableWindow(g_hMain, TRUE);
        SetForegroundWindow(g_hMain);
        break;
    default: return DefWindowProcW(h, msg, wp, lp);
    }
    return 0;
}

// ================= 删除结果日志对话框（带竖向滚动条） =================
static std::wstring g_logText;
static bool g_logNeed = false, g_logRebootChosen = false;
static LRESULT CALLBACK WndLog(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HWND ed = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            12, 12, 676, 360, h, (HMENU)4200, g_hInst, nullptr);
        SetWindowTextW(ed, g_logText.c_str());
        SendMessageW(ed, EM_SETSEL, 0x7FFFFFFF, 0x7FFFFFFF);
        SendMessageW(ed, EM_SCROLLCARET, 0, 0);
        if (g_logNeed) {
            CreateWindowW(L"BUTTON", L"立即重启", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                488, 384, 100, 32, h, (HMENU)4201, g_hInst, nullptr);
            CreateWindowW(L"BUTTON", L"暂不重启", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                588, 384, 100, 32, h, (HMENU)4202, g_hInst, nullptr);
        } else {
            CreateWindowW(L"BUTTON", L"完成", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                588, 384, 100, 32, h, (HMENU)4201, g_hInst, nullptr);
        }
        if (!g_hFont) {
            HDC hdc0 = GetDC(nullptr);
            int fh = -MulDiv(9, GetDeviceCaps(hdc0, LOGPIXELSY), 72);
            ReleaseDC(nullptr, hdc0);
            g_hFont = CreateFontW(fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        EnumChildWindows(h, [](HWND c, LPARAM x) -> BOOL {
            SendMessageW(c, WM_SETFONT, (WPARAM)x, TRUE); return TRUE;
        }, (LPARAM)g_hFont);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 4201) { g_logRebootChosen = g_logNeed; DestroyWindow(h); }
        else if (id == 4202) { g_logRebootChosen = false; DestroyWindow(h); }
        break;
    }
    case WM_CLOSE: g_logRebootChosen = false; DestroyWindow(h); break;
    default: return DefWindowProcW(h, msg, wp, lp);
    }
    return 0;
}
static bool ShowLogDialog(const std::wstring& log, bool needReboot) {
    g_logText = log; g_logNeed = needReboot; g_logRebootChosen = false;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndLog; wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MiniGeekLogDlg";
    RegisterClassW(&wc);
    RECT r; GetWindowRect(g_hMain, &r);
    int W = 700, H = 460;
    int x = r.left + ((r.right - r.left) - W) / 2;
    int y = r.top + 40;
    EnableWindow(g_hMain, FALSE);
    HWND h = CreateWindowW(L"MiniGeekLogDlg",
        needReboot ? L"需要重启" : L"强制删除完成",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, W, H, g_hMain, nullptr, g_hInst, nullptr);
    ShowWindow(h, SW_SHOW); UpdateWindow(h);
    MSG msg;
    while (IsWindow(h) && GetMessageW(&msg, 0, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(g_hMain, TRUE); SetForegroundWindow(g_hMain);
    return g_logRebootChosen;
}

static void ShowResidualDialog(const AppInfo& app, bool forceMode) {
    std::wstring note;
    g_dlg.app = app;
    g_dlg.forceMode = forceMode;
    g_dlg.items = ScanResidual(app, note);
    if (g_dlg.items.empty()) {
        MessageBoxW(g_hMain, L"没有发现残留。", L"提示", MB_OK);
        return;
    }
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndResidual; wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MiniGeekResidualDlg";
    RegisterClassW(&wc);
    RECT r; GetWindowRect(g_hMain, &r);
    int W = 720, H = 510;
    int x = r.left + ((r.right - r.left) - W) / 2;
    int y = r.top + 40;
    HWND h = CreateWindowW(L"MiniGeekResidualDlg",
        (L"残留清理 — " + app.name).c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, W, H, g_hMain, nullptr, g_hInst, nullptr);
    EnableWindow(g_hMain, FALSE);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
}

// ------------------------------------------------------------------
// 操作
// ------------------------------------------------------------------
static int SelectedAppIndex() {
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0) return -1;
    LVITEMW lvi = {0};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = sel;
    ListView_GetItem(g_hList, &lvi);
    return (int)lvi.lParam;
}

static void DoNormalUninstall(const AppInfo& a) {
    if (a.uninstallCmd.empty()) {
        MessageBoxW(g_hMain, L"该程序没有可用的卸载程序，建议使用“强制删除”。",
                    L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring cmd = a.uninstallCmd;
    HINSTANCE r;
    size_t sp = cmd.find(L".exe");
    if (cmd[0] == L'"' || sp == std::wstring::npos) {
        // 整串交给 cmd / 或 ShellExecute 解析
        r = ShellExecuteW(g_hMain, L"runas", L"cmd.exe",
                          (L"/c " + cmd).c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        std::wstring file = cmd.substr(0, sp + 4);
        std::wstring args = sp + 4 < cmd.size() ? cmd.substr(sp + 4) : L"";
        r = ShellExecuteW(g_hMain, L"runas", file.c_str(), args.c_str(),
                          nullptr, SW_SHOWNORMAL);
    }
    if ((INT_PTR)r <= 32)
        MessageBoxW(g_hMain, L"无法启动卸载程序，请尝试强制删除。", L"错误",
                    MB_OK | MB_ICONERROR);
    else if (MessageBoxW(g_hMain,
                 L"卸载程序已启动。请在软件自带卸载完成后，点“是”扫描并清理残留。",
                 L"扫描残留", MB_YESNO | MB_ICONQUESTION) == IDYES)
        ShowResidualDialog(a, false);
}

static void DoForceDelete(const AppInfo& a) {
    // 强制删除：扫描进入勾选窗；确认后走两段式（禁用服务→重启→续删）
    ShowResidualDialog(a, true);
}

static void OpenLocation(const AppInfo& a) {
    if (a.location.empty()) {
        MessageBoxW(g_hMain, L"没有记录安装位置。", L"提示", MB_OK);
        return;
    }
    PIDLIST_ABSOLUTE pidl;
    if (SUCCEEDED(SHParseDisplayName(a.location.c_str(), 0, &pidl, 0, 0))) {
        SHOpenFolderAndSelectItems(pidl, 0, 0, 0);
        CoTaskMemFree(pidl);
    }
}

static void OpenRegKey(const AppInfo& a) {
    std::wstring sub = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                     + a.regSubkey;
    std::wstring cmd = L"regedit.exe";
    ShellExecuteW(g_hMain, L"open", cmd.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ------------------------------------------------------------------
// 菜单
// ------------------------------------------------------------------
static HMENU BuildMenuBar() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_REFRESH, L"刷新\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 100, L"退出");
    HMENU set = CreatePopupMenu();
    AppendMenuW(set, MF_STRING | MF_GRAYED, 0, L"安装监控（开发中）");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING | MF_GRAYED, 0, L"关于");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"文件(&F)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)set, L"设置(&S)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"帮助(&H)");
    return bar;
}

static void ShowContextMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_UNINSTALL, L"正常卸载");
    AppendMenuW(menu, MF_STRING, IDM_FORCE, L"强制删除");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPENLOC, L"打开安装位置");
    AppendMenuW(menu, MF_STRING, IDM_OPENREG, L"打开注册表项");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"刷新");
    POINT pt = { x, y };
    ClientToScreen(g_hList, &pt);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                   g_hMain, nullptr);
    DestroyMenu(menu);
}

// ------------------------------------------------------------------
// 窗口过程
// ------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS,
            0, 0, 100, 100, h, (HMENU)(INT_PTR)IDC_LIST, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);

        HDC hdc0 = GetDC(nullptr);
        int fh = -MulDiv(9, GetDeviceCaps(hdc0, LOGPIXELSY), 72);
        ReleaseDC(nullptr, hdc0);
        g_hFont = CreateFontW(fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        struct Col { const wchar_t* t; int w; };
        Col cols[] = {
            { L"程序名称", 380 }, { L"大小", 110 },
            { L"安装日期", 150 },
        };
        LVCOLUMNW lc = {0};
        lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        for (int i = 0; i < 3; ++i) {
            lc.iSubItem = i; lc.cx = cols[i].w; lc.pszText = (LPWSTR)cols[i].t;
            ListView_InsertColumn(g_hList, i, &lc);
        }

        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, h, nullptr, g_hInst, nullptr);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        SetMenu(h, BuildMenuBar());

        // Win11 圆角（DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2）
        typedef LONG(WINAPI* PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
        if (HMODULE dm = LoadLibraryW(L"dwmapi.dll")) {
            auto pfn = (PFN_DwmSetWindowAttribute)GetProcAddress(dm, "DwmSetWindowAttribute");
            if (pfn) { DWORD c = 2; pfn(h, 33, &c, sizeof(c)); }
        }

        EnumerateApps();
        PopulateList();

        // 重启续删：主窗就绪后自动继续删除（此时目标防护未加载）
        if (g_resumeWanted) {
            g_resumeWanted = false;
            if (ReadPlan(RESUMEPLAN, g_resumeApp, g_resumeItems) &&
                !g_resumeItems.empty()) {
                SetWindowTextW(g_hStatus, L"重启后续清理：正在删除...");
                StartDelete(g_resumeItems, g_resumeApp);
            }
        }
        break;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(h, &rc);
        SendMessageW(g_hStatus, WM_SIZE, 0, 0);
        RECT sr; GetWindowRect(g_hStatus, &sr);
        int hh = sr.bottom - sr.top;
        MoveWindow(g_hList, 0, 0, rc.right, rc.bottom - hh, TRUE);
        break;
    }
    case WM_APP + 2: {
        int target = (int)wp;
        int n = ListView_GetItemCount(g_hList);
        for (int r = 0; r < n; ++r) {
            LVITEMW li = {0}; li.mask = LVIF_PARAM; li.iItem = r;
            ListView_GetItem(g_hList, &li);
            if ((int)li.lParam == target) {
                std::wstring s = FmtSize(g_apps[target].size);
                ListView_SetItemText(g_hList, r, 1, (LPWSTR)s.c_str());
                break;
            }
        }
        break;
    }
    case WM_NOTIFY: {
        LPNMHDR pn = (LPNMHDR)lp;
        if (pn->idFrom == IDC_LIST) {
            if (pn->code == NM_RCLICK) {
                DWORD pos = GetMessagePos();
                int x = GET_X_LPARAM(pos), y = GET_Y_LPARAM(pos);
                POINT pt = { x, y };
                ScreenToClient(g_hList, &pt);
                LVHITTESTINFO ht = {0};
                ht.pt = pt;
                int row = ListView_HitTest(g_hList, &ht);
                if (row >= 0) {
                    ListView_SetItemState(g_hList, row, 0xFFFF, LVIS_SELECTED);
                    ShowContextMenu(pt.x, pt.y);
                }
            } else if (pn->code == NM_DBLCLK) {
                int idx = SelectedAppIndex();
                if (idx >= 0) DoNormalUninstall(g_apps[idx]);
            }
        }
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int idx = SelectedAppIndex();
        switch (id) {
        case IDM_UNINSTALL: if (idx >= 0) DoNormalUninstall(g_apps[idx]); break;
        case IDM_FORCE:     if (idx >= 0) DoForceDelete(g_apps[idx]); break;
        case IDM_OPENLOC:   if (idx >= 0) OpenLocation(g_apps[idx]); break;
        case IDM_OPENREG:   if (idx >= 0) OpenRegKey(g_apps[idx]); break;
        case IDM_REFRESH:
        case ID_REFRESH:
            EnumerateApps(); PopulateList(); break;
        case 100: PostQuitMessage(0); break;
        }
        break;
    }
    case WM_FORCE_DONE: {
        ForceCtx* c = (ForceCtx*)lp;
        EnableWindow(g_hList, TRUE);
        EnumerateApps(); PopulateList();
        bool needReboot = c->log.find(L"重启后删除") != std::wstring::npos;
        bool reboot = ShowLogDialog(c->log, needReboot);
        if (needReboot && reboot) {
            HANDLE tp;
            TOKEN_PRIVILEGES priv = {0};
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tp)) {
                LookupPrivilegeValue(0, SE_SHUTDOWN_NAME, &priv.Privileges[0].Luid);
                priv.PrivilegeCount = 1;
                priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(tp, 0, &priv, 0, 0, 0);
                CloseHandle(tp);
            }
            ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM);
        }
        delete c;
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
    return 0;
}

// 禁用 ROOT\LEGACY_<服务名> 非即插即用驱动（DIF_PROPERTYCHANGE / DICS_DISABLE）
// 成功后该驱动 Start=4，普通重启一次即不加载，随后可删除其文件。
// 返回: 0 成功；1 未找到设备；2 禁用失败（可能被杀软自保护拦截）
static int DisableLegacyDriver(const std::wstring& svc, std::wstring& detail) {
    if (IsCriticalService(svc)) {
        detail = L"系统关键服务，禁止禁用: " + svc; return 2;
    }
    // 新版 Win11 已移除“非即插即用驱动程序”设备类，直接改服务 Start=4
    std::wstring sub = L"SYSTEM\\CurrentControlSet\\Services\\" + svc;
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &h)
        != ERROR_SUCCESS) {
        wchar_t b[160];
        StringCchPrintfW(b, 160, L"打开服务键失败(可能不存在或被拦截) err=%lu",
                         GetLastError());
        detail = b; return 2;
    }
    DWORD type = 0, cb = sizeof(type);
    RegQueryValueExW(h, L"Type", nullptr, nullptr, (BYTE*)&type, &cb);
    // Type: 1=内核驱动 2=文件系统驱动；其余(如 Win32 服务)也允许但提示
    DWORD start = 4, s4 = 4, cbs = sizeof(s4);
    LONG ok = RegSetValueExW(h, L"Start", 0, REG_DWORD, (BYTE*)&s4, sizeof(s4));
    RegCloseKey(h);
    if (ok != ERROR_SUCCESS) {
        wchar_t b[160];
        StringCchPrintfW(b, 160, L"写入 Start=4 被拒绝 err=%lu（大概率杀软自保护拦截）", ok);
        detail = b; return 2;
    }
    wchar_t b[160];
    StringCchPrintfW(b, 160, L"已将服务 Start 设为4（Type=%lu），普通重启一次后驱动不再加载", type);
    detail = b;
    return 0;
}

// 枚举并禁用所有匹配目标的服务/驱动（Start=4），返回禁用数量
static int DisableMatchingServices(const std::wstring& baseLow,
                                   const std::vector<std::wstring>& kw) {
    int cnt = 0;
    HKEY hSvc;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services", 0,
            KEY_READ | KEY_WOW64_64KEY, &hSvc) != ERROR_SUCCESS) return 0;
    DWORD i = 0;
    for (;;) {
        wchar_t name[256]; DWORD nl = 256;
        if (RegEnumKeyExW(hSvc, i++, name, &nl, 0, 0, 0, 0) != ERROR_SUCCESS) break;
        std::wstring sname = name;
        std::wstring svc = L"SYSTEM\\CurrentControlSet\\Services\\" + sname;
        std::wstring img = RegGetStr(HKEY_LOCAL_MACHINE, svc, L"ImagePath", KEY_WOW64_64KEY);
        std::wstring imgLow = Lower(img);
        if (imgLow.find(L"\\??\\") == 0) imgLow = imgLow.substr(4);
        bool match = (!baseLow.empty() && imgLow.find(baseLow) == 0)
                     || MatchKeyword(sname, kw);
        if (!match || IsCriticalService(sname)) continue;
        std::wstring detail;
        if (DisableLegacyDriver(sname, detail) == 0) ++cnt;
    }
    RegCloseKey(hSvc);
    return cnt;
}

static void DoReboot() {
    HANDLE tp; TOKEN_PRIVILEGES priv = {0};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tp)) {
        LookupPrivilegeValueW(0, SE_SHUTDOWN_NAME, &priv.Privileges[0].Luid);
        priv.PrivilegeCount = 1;
        priv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tp, 0, &priv, 0, 0, 0);
        CloseHandle(tp);
    }
    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM);
}

// 两段式阶段一：禁用服务/驱动 → 写计划 → 注册 RunOnce → 提示重启
static void StartForceTwoStage(HWND h, const AppInfo& app,
                               std::vector<ResidualItem>& chosen) {
    std::vector<std::wstring> kw = BuildKeywords(app);
    int disabled = DisableMatchingServices(Lower(app.location), kw);

    if (disabled == 0) {
        MessageBoxW(g_hMain,
            L"未能禁用目标的任何服务/驱动（被其自我保护拦截）。\n请先在该安全软件中把本工具加入信任名单，或等待代码签名通过后再试。\n本次不会重启。",
            L"无法继续", MB_OK | MB_ICONWARNING);
        return;
    }

    CreateDirectoryW(RESUMEDIR, nullptr);
    WritePlan(RESUMEPLAN, app, chosen);

    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + self + L"\" --resume-delete";
    SetRegSz(HKEY_LOCAL_MACHINE, RUNONCE_SUB, L"MiniGeekResume",
             cmd, KEY_WOW64_64KEY);

    DestroyWindow(h);
    wchar_t msg[300];
    StringCchPrintfW(msg, 300,
        L"已禁用 %d 个相关服务/驱动。\n需要重启一次以解除自我保护；重启登录后本工具会自动继续删除（会再弹一次 UAC，请点“是”）。\n\n是否立即重启？",
        disabled);
    if (MessageBoxW(g_hMain, msg, L"需要重启",
                    MB_YESNO | MB_ICONQUESTION) == IDYES)
        DoReboot();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    g_hInst = hInst;

    // 安全模式清理分支：命令行参数，或自身位于 SAFEDIR 且存在 pending.dat
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool safeMode = false;
    for (int i = 0; i < argc; ++i)
        if (wcscmp(argv[i], L"--safemode-clean") == 0) safeMode = true;
    if (argv) LocalFree(argv);
    if (!safeMode) {
        wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH);
        std::wstring ml = Lower(me);
        if (ml.find(L"minigeeksafeclean\\clean.exe") != std::wstring::npos &&
            GetFileAttributesW(SAFEPLAN) != INVALID_FILE_ATTRIBUTES)
            safeMode = true;
    }
    // 单实例保护：clean / 主程序分别互斥，防止并发删除卡死
    {
        const wchar_t* mn = safeMode ? L"Local\\MiniGeekSafeCleanMutex"
                                     : L"Local\\MiniGeekMainMutex";
        HANDLE hm = CreateMutexW(nullptr, FALSE, mn);
        if (hm && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(hm);
            return 0;
        }
    }
    if (safeMode) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        return RunSafeClean();
    }

    // 重启续删：--resume-delete
    {
        int rc2 = 0;
        LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &rc2);
        for (int i = 0; i < rc2; ++i)
            if (wcscmp(av[i], L"--resume-delete") == 0) g_resumeWanted = true;
        if (av) LocalFree(av);
    }

    // 临时扫描自测：--test-scan
    bool testScan = false;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 0; i < argc; ++i)
        if (wcscmp(argv[i], L"--test-scan") == 0) testScan = true;
    if (argv) LocalFree(argv);
    if (testScan) {
        FILE* f = _wfopen(L"C:\\testscan_out.txt", L"w, ccs=UTF-8");
        AppInfo a;
        a.name = L"360安全卫士"; a.publisher = L"360安全中心";
        a.location = L"C:\\Program Files (x86)\\360\\360Safe";
        std::wstring note;
        auto items = ScanResidual(a, note);
        fwprintf(f, L"%s\n共 %d 项\n", note.c_str(), (int)items.size());
        const wchar_t* tn[] = { L"DIR", L"FILE", L"REG", L"REGV" };
        for (auto& i : items)
            fwprintf(f, L"[%s] sys=%d %s | %s\n", tn[i.type], i.allowSystem,
                    i.display.c_str(),
                    (i.type == R_REG ? i.regSubkey : i.path).c_str());
        fclose(f);
        return 0;
    }

    // ROOT\LEGACY 单驱动禁用：--disable-driver <服务名>
    {
        int ac = 0;
        LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &ac);
        std::wstring target;
        for (int i = 0; i < ac; ++i)
            if (wcscmp(av[i], L"--disable-driver") == 0 && i + 1 < ac)
                target = av[i + 1];
        if (av) LocalFree(av);
        if (!target.empty()) {
            std::wstring detail;
            int r = DisableLegacyDriver(target, detail);
            FILE* f = _wfopen(L"C:\\disable_out.txt", L"w, ccs=UTF-8");
            if (f) { fwprintf(f, L"服务=%s\n结果码=%d\n%s\n",
                              target.c_str(), r, detail.c_str()); fclose(f); }
            return 0;
        }
    }

    // 路径护栏自测：--test-path
    {
        int ac = 0;
        LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &ac);
        bool tp = false;
        for (int i = 0; i < ac; ++i)
            if (wcscmp(av[i], L"--test-path") == 0) tp = true;
        if (av) LocalFree(av);
        if (tp) {
            struct TC { const wchar_t* p; bool isDir; bool expect; };
            TC list[] = {
                { L"C:\\", true, false },
                { L"C:\\Windows", true, false },
                { L"C:\\Windows\\System32", true, false },
                { L"C:\\Windows\\System32\\drivers", true, false },
                { L"C:\\Program Files", true, false },
                { L"C:\\Program Files (x86)", true, false },
                { L"C:\\ProgramData", true, false },
                { L"C:\\Users", true, false },
                { L"C:\\Program Files (x86)\\360\\360Safe", true, true },
                { L"C:\\Program Files\\MyApp", true, true },
                { L"C:\\ProgramData\\Vendor\\App", true, true },
                { L"C:\\Users\\me\\AppData\\Roaming\\Vendor\\App", true, true },
                { L"C:\\Windows\\System32\\drivers\\360x.sys", false, true },
            };
            FILE* f = _wfopen(L"C:\\pathcheck.txt", L"w, ccs=UTF-8");
            int pass = 0;
            for (auto& t : list) {
                bool r = IsSafeToDeletePath(t.p, t.isDir);
                bool ok = (r == t.expect);
                pass += ok ? 1 : 0;
                fwprintf(f, L"%s  expect=%d got=%d  %s\n",
                         ok ? L"PASS" : L"FAIL", t.expect, r, t.p);
            }
            fwprintf(f, L"通过 %d/%d\n", pass, (int)(sizeof(list)/sizeof(list[0])));
            fclose(f);
            return 0;
        }
    }

    // 签名分类自测：--test-sig <path>
    {
        int ac = 0;
        LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &ac);
        std::wstring tp;
        for (int i = 0; i < ac; ++i)
            if (wcscmp(av[i], L"--test-sig") == 0 && i+1<ac) tp = av[i+1];
        if (av) LocalFree(av);
        if (!tp.empty()) {
            int c = ClassifyFileSignature(tp);
            FILE* f = _wfopen(L"C:\\sigtest.txt", L"w, ccs=UTF-8");
            fwprintf(f, L"%s -> class=%d (0系统/1第三方/2无签名/3无效)\n", tp.c_str(), c);
            fclose(f); return 0;
        }
    }

    // 若已经是管理员则正常启动；manifest 已要求管理员
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MiniGeekMainWnd";
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    RegisterClassW(&wc);

    int W = 920, H = 600;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;
    g_hMain = CreateWindowW(L"MiniGeekMainWnd", L"Mini Geek Uninstaller",
        WS_OVERLAPPEDWINDOW, sx, sy, W, H, nullptr, nullptr, hInst, nullptr);
    HICON hIcoBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON hIcoSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (hIcoBig) SendMessageW(g_hMain, WM_SETICON, ICON_BIG, (LPARAM)hIcoBig);
    if (hIcoSmall) SendMessageW(g_hMain, WM_SETICON, ICON_SMALL, (LPARAM)hIcoSmall);
    ShowWindow(g_hMain, nShow);
    UpdateWindow(g_hMain);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_hMain, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    return 0;
}
