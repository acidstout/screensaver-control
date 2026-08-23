/* ------------------------------------------------------------------------
 * scrctl.c - Bildschirmschoner-Steuerung / Screen Saver Control
 *
 * A tray applet that shows and controls the state of the default Windows
 * screen saver.  Built freestanding (-nostdlib): no C runtime is linked,
 * every string/number helper comes from kernel32/user32, so the binary runs
 * on every 32-bit Windows from Windows 95 up to Windows 11.
 *
 * Rules that keep the Windows 95 compatibility intact - do not break them:
 *   - ANSI (*A) entry points only; Win9x has no Unicode USER/GDI.
 *   - NOTIFYICONDATA is submitted with the V1 size (88 bytes).
 *   - No API newer than shell32/comctl32 4.0 (so: no CheckMenuRadioItem,
 *     no NIM_SETVERSION, no SHGetFolderPath, no shlwapi).
 *   - version.dll is bound late so its absence degrades instead of failing.
 *   - The .ico files must hold classic BMP/DIB images only.  PNG-compressed
 *     icon entries are a Vista-and-later feature and are not icons at all to
 *     95/98/ME/2000/XP.  Run tools/depng_ico.py over any re-exported icon.
 * ---------------------------------------------------------------------- */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "resource.h"

#define CLASSNAME     "ScreensaverControlWndClass"
#define WM_TRAYICON   (WM_APP + 1)
#define IDT_REFRESH   1
#define REFRESH_MS    2000
#define PROBE_TICKS   15            /* re-verify the icon every 30 s      */
#define MAX_SAVERS    256
#define MENU_BREAK_AT 28            /* start a new menu column after N items */
#define NAME_MAX_CHARS 52           /* clamp over-long FileDescription strings   */

#define REG_DESKTOP   "Control Panel\\Desktop"
#define REG_SETTINGS  "Software\\Rekow IT\\ScreensaverControl"
#define REG_RUN       "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE     "ScreensaverControl"   /* language independent */

typedef struct {
    char file[MAX_PATH];            /* full path of the .scr                */
    char name[128];                 /* display name (FileDescription)       */
} SAVER;

static HINSTANCE g_hInst;
static HWND      g_hWnd;
static UINT      g_uTaskbarCreated;
static BOOL      g_bActive = FALSE; /* last known screen saver state        */
static BOOL      g_bIconOk = FALSE; /* tray icon currently installed        */
static BOOL      g_bEnglish = FALSE;
static SAVER    *g_savers;          /* lazily allocated, MAX_SAVERS */
static int       g_nSavers;
static int       g_iDefault;        /* index into g_savers, or -1           */

/* ======================================================================= */
/*  freestanding helpers                                                   */
/* ======================================================================= */

/* gcc may synthesise calls to these even with -nostdlib. */
void *memset(void *d, int c, unsigned n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

static void Zero(void *p, unsigned n) { memset(p, 0, n); }

/* Last path component of "C:\\Windows\\System32\\Foo.scr" -> "Foo.scr". */
static const char *BaseName(const char *p)
{
    const char *b = p;
    while (*p) {
        if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
        p++;
    }
    return b;
}

/* Load a UI string in the currently selected language. */
static const char *Str(UINT id)
{
    static char buf[4][256];
    static int  slot = 0;
    char *p = buf[slot];
    slot = (slot + 1) & 3;
    p[0] = 0;
    LoadStringA(g_hInst, (g_bEnglish ? IDS_BASE_EN : IDS_BASE_DE) + id, p, 256);
    return p;
}

static void Say(UINT id, UINT icon)
{
    MessageBoxA(g_hWnd, Str(id), Str(IDS_APPTITLE), MB_OK | icon | MB_SETFOREGROUND);
}

/* ======================================================================= */
/*  registry                                                               */
/* ======================================================================= */

static BOOL RegReadStr(HKEY root, const char *subkey, const char *value,
                       char *out, DWORD cb)
{
    HKEY  hk;
    DWORD type = 0, n = cb;
    BOOL  ok = FALSE;

    out[0] = 0;
    if (RegOpenKeyExA(root, subkey, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExA(hk, value, 0, &type, (LPBYTE)out, &n) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        if (n >= cb) n = cb - 1;
        out[n] = 0;
        ok = TRUE;
    } else {
        out[0] = 0;
    }
    RegCloseKey(hk);
    return ok;
}

static BOOL RegWriteStr(HKEY root, const char *subkey, const char *value,
                        const char *data)
{
    HKEY  hk;
    DWORD disp;
    LONG  r;

    if (RegCreateKeyExA(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, NULL, &hk, &disp) != ERROR_SUCCESS)
        return FALSE;
    r = RegSetValueExA(hk, value, 0, REG_SZ, (const BYTE *)data,
                       lstrlenA(data) + 1);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

/* ======================================================================= */
/*  screen saver state                                                     */
/* ======================================================================= */

static BOOL SaverIsActive(void)
{
    BOOL b = FALSE;
    if (!SystemParametersInfoA(SPI_GETSCREENSAVEACTIVE, 0, &b, 0)) {
        char v[16];
        if (RegReadStr(HKEY_CURRENT_USER, REG_DESKTOP, "ScreenSaveActive",
                       v, sizeof(v)))
            b = (v[0] == '1');
    }
    return b ? TRUE : FALSE;
}

static void SaverSetActive(BOOL on)
{
    /* SPI does the live change and the .INI/registry writeback; the explicit
       registry write keeps NT-family shells in sync even when SPI is a no-op. */
    SystemParametersInfoA(SPI_SETSCREENSAVEACTIVE, on ? 1 : 0, NULL,
                          SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
    RegWriteStr(HKEY_CURRENT_USER, REG_DESKTOP, "ScreenSaveActive",
                on ? "1" : "0");
}

/* Full path of the configured default saver; FALSE when none is set. */
static BOOL GetDefaultSaver(char *out, DWORD cb)
{
    return RegReadStr(HKEY_CURRENT_USER, REG_DESKTOP, "SCRNSAVE.EXE", out, cb)
           && out[0];
}

static void SetDefaultSaver(const char *path)
{
    DWORD_PTR res = 0;

    RegWriteStr(HKEY_CURRENT_USER, REG_DESKTOP, "SCRNSAVE.EXE", path);
    /* Poke the shell so an open Display control panel picks the change up. */
    SystemParametersInfoA(SPI_SETSCREENSAVEACTIVE, SaverIsActive() ? 1 : 0,
                          NULL, SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
    SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETSCREENSAVEACTIVE,
                        (LPARAM)"WindowsMetrics", SMTO_ABORTIFHUNG, 1000, &res);
}

/* Launch a .scr with the given switch ("/s" = show, "/c" = configure). */
static BOOL RunSaver(const char *path, const char *sw)
{
    char                cmd[MAX_PATH + 16];
    char                dir[MAX_PATH];
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;

    cmd[0] = '"';
    lstrcpynA(cmd + 1, path, MAX_PATH);
    lstrcatA(cmd, "\" ");
    lstrcatA(cmd, sw);

    dir[0] = 0;
    GetSystemDirectoryA(dir, sizeof(dir));

    Zero(&si, sizeof(si));
    si.cb = sizeof(si);
    Zero(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_DEFAULT_ERROR_MODE,
                        NULL, dir[0] ? dir : NULL, &si, &pi))
        return FALSE;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

static void StartDefaultSaver(void)
{
    char path[MAX_PATH];

    if (!GetDefaultSaver(path, sizeof(path))) {
        Say(IDS_NODEFAULT, MB_ICONINFORMATION);
        return;
    }
    if (!RunSaver(path, "/s"))
        Say(IDS_STARTFAIL, MB_ICONEXCLAMATION);
}

/* ======================================================================= */
/*  enumerating installed screen savers                                    */
/* ======================================================================= */

typedef DWORD (WINAPI *PFNGETSIZE)(LPCSTR, LPDWORD);
typedef BOOL  (WINAPI *PFNGETINFO)(LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL  (WINAPI *PFNQUERY)(const LPVOID, LPSTR, LPVOID *, PUINT);

static HMODULE    g_hVersion;
static PFNGETSIZE g_pGetSize;
static PFNGETINFO g_pGetInfo;
static PFNQUERY   g_pQuery;

static void LoadVersionDll(void)
{
    if (g_hVersion) return;
    g_hVersion = LoadLibraryA("version.dll");
    if (!g_hVersion) return;
    g_pGetSize = (PFNGETSIZE)GetProcAddress(g_hVersion, "GetFileVersionInfoSizeA");
    g_pGetInfo = (PFNGETINFO)GetProcAddress(g_hVersion, "GetFileVersionInfoA");
    g_pQuery   = (PFNQUERY)  GetProcAddress(g_hVersion, "VerQueryValueA");
}

/* Friendly name from the .scr's FileDescription; FALSE if unavailable. */
static BOOL SaverDisplayName(const char *path, char *out, int cb)
{
    DWORD  dummy = 0, size;
    void  *buf;
    BOOL   ok = FALSE;
    UINT   len = 0;
    char   sub[64];
    WORD  *xlat = NULL;
    char  *desc = NULL;
    int    i;

    LoadVersionDll();
    if (!g_pGetSize || !g_pGetInfo || !g_pQuery) return FALSE;

    size = g_pGetSize((LPSTR)path, &dummy);
    if (!size || size > 0x20000) return FALSE;

    buf = (void *)LocalAlloc(LPTR, size);
    if (!buf) return FALSE;

    if (g_pGetInfo((LPSTR)path, 0, size, buf)) {
        /* Prefer the file's own translation, then the usual suspects. */
        WORD tries[4][2] = { { 0, 0 }, { 0x0409, 0x04B0 },
                             { 0x0407, 0x04B0 }, { 0x0409, 0x04E4 } };
        if (g_pQuery(buf, "\\VarFileInfo\\Translation", (void **)&xlat, &len) &&
            xlat && len >= 4) {
            tries[0][0] = xlat[0];
            tries[0][1] = xlat[1];
        }
        for (i = 0; i < 4 && !ok; i++) {
            if (!tries[i][0] && !tries[i][1]) continue;
            wsprintfA(sub, "\\StringFileInfo\\%04X%04X\\FileDescription",
                      tries[i][0], tries[i][1]);
            len = 0;
            if (g_pQuery(buf, sub, (void **)&desc, &len) && desc && desc[0]) {
                lstrcpynA(out, desc, cb);
                ok = TRUE;
            }
        }
    }
    LocalFree((HLOCAL)buf);
    return ok;
}

static BOOL AlreadyHave(const char *file)
{
    int i;
    for (i = 0; i < g_nSavers; i++)
        if (lstrcmpiA(BaseName(g_savers[i].file), file) == 0)
            return TRUE;
    return FALSE;
}

static void ScanDir(const char *dir)
{
    char             pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              n;

    if (!dir || !dir[0]) return;
    lstrcpynA(pat, dir, MAX_PATH);
    n = lstrlenA(pat);
    if (n && pat[n - 1] != '\\') lstrcatA(pat, "\\");
    lstrcatA(pat, "*.scr");

    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        SAVER *s;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_nSavers >= MAX_SAVERS) break;
        if (AlreadyHave(fd.cFileName)) continue;

        s = &g_savers[g_nSavers];
        lstrcpynA(s->file, dir, MAX_PATH);
        n = lstrlenA(s->file);
        if (n && s->file[n - 1] != '\\') lstrcatA(s->file, "\\");
        lstrcatA(s->file, fd.cFileName);

        if (!SaverDisplayName(s->file, s->name, sizeof(s->name))) {
            /* Fall back to the file name without its extension. */
            int i;
            lstrcpynA(s->name, fd.cFileName, sizeof(s->name));
            for (i = lstrlenA(s->name) - 1; i > 0; i--)
                if (s->name[i] == '.') { s->name[i] = 0; break; }
        }
        if (!s->name[0]) continue;
        if (lstrlenA(s->name) > NAME_MAX_CHARS) {
            /* Some savers put a whole sentence in FileDescription. */
            s->name[NAME_MAX_CHARS - 3] = '.';
            s->name[NAME_MAX_CHARS - 2] = '.';
            s->name[NAME_MAX_CHARS - 1] = '.';
            s->name[NAME_MAX_CHARS]     = 0;
        }
        g_nSavers++;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

/* Insertion sort, ascending, case-insensitive by display name. */
static void SortSavers(void)
{
    int i, j;
    for (i = 1; i < g_nSavers; i++) {
        SAVER tmp = g_savers[i];
        for (j = i - 1; j >= 0 && lstrcmpiA(g_savers[j].name, tmp.name) > 0; j--)
            g_savers[j + 1] = g_savers[j];
        g_savers[j + 1] = tmp;
    }
}

static void EnumSavers(void)
{
    char dir[MAX_PATH];
    char cur[MAX_PATH];
    int  i;

    g_nSavers = 0;
    g_iDefault = -1;

    /* Kept off the static data so the array does not bloat the image. */
    if (!g_savers) {
        g_savers = (SAVER *)LocalAlloc(LPTR, MAX_SAVERS * sizeof(SAVER));
        if (!g_savers) return;
    }

    dir[0] = 0; GetSystemDirectoryA(dir, sizeof(dir));  ScanDir(dir);
    dir[0] = 0; GetWindowsDirectoryA(dir, sizeof(dir)); ScanDir(dir);

    /* A saver configured from somewhere else must still show up. */
    if (GetDefaultSaver(cur, sizeof(cur)) && !AlreadyHave(BaseName(cur))) {
        const char *b = BaseName(cur);
        if (b > cur) {
            lstrcpynA(dir, cur, (int)(b - cur));   /* strips the backslash */
            ScanDir(dir);
        }
    }

    SortSavers();

    if (GetDefaultSaver(cur, sizeof(cur))) {
        for (i = 0; i < g_nSavers; i++)
            if (lstrcmpiA(BaseName(g_savers[i].file), BaseName(cur)) == 0) {
                g_iDefault = i;
                break;
            }
    }
}

/* ======================================================================= */
/*  tray icon                                                              */
/* ======================================================================= */

/* ANSI NOTIFYICONDATA as Windows 95 knows it: 4 DWORDs + HICON + 64 chars. */
#define NID_V1_SIZE 88

static void TrayFill(NOTIFYICONDATAA *nid, BOOL withData)
{
    Zero(nid, sizeof(*nid));
    nid->cbSize = NID_V1_SIZE;
    nid->hWnd   = g_hWnd;
    nid->uID    = 1;
    if (withData) {
        nid->uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid->uCallbackMessage = WM_TRAYICON;
        nid->hIcon = LoadIconA(g_hInst, MAKEINTRESOURCEA(
                         g_bActive ? IDI_ENABLED : IDI_DISABLED));
        /* Shell_NotifyIcon rejects the whole call for a NULL hIcon, which
           would cost us the icon entirely; a stock icon is far better. */
        if (!nid->hIcon)
            nid->hIcon = LoadIconA(NULL, IDI_APPLICATION);
        lstrcpynA(nid->szTip, Str(g_bActive ? IDS_TIP_ON : IDS_TIP_OFF), 64);
    }
}

static void TrayAdd(void)
{
    NOTIFYICONDATAA nid;
    TrayFill(&nid, TRUE);
    g_bIconOk = Shell_NotifyIconA(NIM_ADD, &nid);
}

static void TrayUpdate(void)
{
    NOTIFYICONDATAA nid;
    if (!g_bIconOk) { TrayAdd(); return; }
    TrayFill(&nid, TRUE);
    if (!Shell_NotifyIconA(NIM_MODIFY, &nid)) { g_bIconOk = FALSE; TrayAdd(); }
}

static void TrayRemove(void)
{
    NOTIFYICONDATAA nid;
    if (!g_bIconOk) return;
    TrayFill(&nid, FALSE);
    Shell_NotifyIconA(NIM_DELETE, &nid);
    g_bIconOk = FALSE;
}

/* Re-read the state and repaint the icon only when it actually changed. */
/* Keep the icon present.  Cheap enough to call on every timer tick.

   This exists because a single lost NIM_ADD used to be permanent: the only
   other caller of TrayAdd was TrayUpdate, which ran only when the screen
   saver's on/off state actually changed, so the program would sit there
   running with no icon at all.  NIM_ADD does fail in practice - most easily
   when we are started from the Run key at logon and the shell has not created
   the notification area yet.  Retrying every tick costs nothing and makes the
   icon appear as soon as the tray is ready.

   The shell can also drop an icon silently (a tray rebuild whose
   TaskbarCreated broadcast we missed).  There is no way to ask whether our
   icon is still there, so probe with a NIM_MODIFY now and then: if it fails,
   the icon is gone and has to be added again. */
static void TrayEnsure(void)
{
    static UINT probe   = 0;
    static BOOL settled = FALSE;
    NOTIFYICONDATAA nid;

    if (!g_bIconOk) {
        TrayAdd();
        probe = 0;
        settled = FALSE;
        return;
    }

    /* Probe on every tick until one probe has actually succeeded, and only
       then settle into an occasional check.  NIM_ADD can report success while
       the shell quietly drops the icon - which is what happens when we start
       before the notification area is ready - so a success from NIM_ADD is
       not proof that the icon exists.  A successful NIM_MODIFY is. */
    if (settled && ++probe < PROBE_TICKS) return;
    probe = 0;

    TrayFill(&nid, TRUE);
    if (Shell_NotifyIconA(NIM_MODIFY, &nid)) {
        settled = TRUE;
    } else {
        settled = FALSE;
        g_bIconOk = FALSE;
        TrayAdd();
    }
}

static void RefreshState(BOOL force)
{
    BOOL now = SaverIsActive();
    if (force || now != g_bActive) {
        g_bActive = now;
        TrayUpdate();
    }
}

/* ======================================================================= */
/*  run at logon                                                           */
/* ======================================================================= */

/* HKCU\...\Run is honoured by every shell from Windows 95 on, needs no
   elevation, and is per-user - which matches where the rest of our settings
   live.  A Startup-folder shortcut would drag in IShellLink/COM for nothing. */

static void StripQuotes(char *s)
{
    int n;
    if (s[0] != '"') return;
    for (n = 0; s[n + 1]; n++) s[n] = s[n + 1];
    s[n] = 0;
    n = lstrlenA(s);
    if (n && s[n - 1] == '"') s[n - 1] = 0;
}

static BOOL AutostartEnabled(void)
{
    char v[MAX_PATH + 4], me[MAX_PATH + 4];

    if (!RegReadStr(HKEY_CURRENT_USER, REG_RUN, RUN_VALUE, v, sizeof(v)) || !v[0])
        return FALSE;
    StripQuotes(v);

    /* An entry left behind by a copy that has since been moved or replaced is
       not "this program starts with Windows", so report it as off. */
    me[0] = 0;
    GetModuleFileNameA(NULL, me, MAX_PATH);
    return me[0] && lstrcmpiA(v, me) == 0;
}

static void AutostartSet(BOOL on)
{
    if (on) {
        char me[MAX_PATH + 4], q[MAX_PATH + 4];

        me[0] = 0;
        if (!GetModuleFileNameA(NULL, me, MAX_PATH) || !me[0]) return;
        q[0] = '"';                      /* quoted: the path may have spaces */
        lstrcpynA(q + 1, me, MAX_PATH);
        lstrcatA(q, "\"");
        RegWriteStr(HKEY_CURRENT_USER, REG_RUN, RUN_VALUE, q);
    } else {
        HKEY hk;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_RUN, 0, KEY_SET_VALUE, &hk)
            == ERROR_SUCCESS) {
            RegDeleteValueA(hk, RUN_VALUE);
            RegCloseKey(hk);
        }
    }
}

/* ======================================================================= */
/*  language                                                               */
/* ======================================================================= */

static void LoadLanguage(void)
{
    char v[16];
    if (RegReadStr(HKEY_CURRENT_USER, REG_SETTINGS, "Language", v, sizeof(v))
        && v[0]) {
        g_bEnglish = (v[0] == 'e' || v[0] == 'E');
        return;
    }
    /* No preference stored yet: German on a German UI, English elsewhere. */
    g_bEnglish = (PRIMARYLANGID(GetUserDefaultLangID()) != LANG_GERMAN);
}

static void SetLanguage(BOOL english)
{
    if (english == g_bEnglish) return;
    g_bEnglish = english;
    RegWriteStr(HKEY_CURRENT_USER, REG_SETTINGS, "Language",
                english ? "en" : "de");
    TrayUpdate();                    /* tooltip is language dependent */
}

/* ======================================================================= */
/*  context menu                                                           */
/* ======================================================================= */

/* Replace a placeholder popup's contents with the sorted saver list. */
static void FillSaverMenu(HMENU sub, UINT base, UINT noneId, BOOL checkDefault)
{
    int i;

    if (!sub) return;
    while (DeleteMenu(sub, 0, MF_BYPOSITION)) ;

    if (g_nSavers == 0) {
        AppendMenuA(sub, MF_STRING | MF_GRAYED, noneId, Str(IDS_NONE));
        return;
    }
    for (i = 0; i < g_nSavers; i++) {
        UINT flags = MF_STRING;
        if (i && (i % MENU_BREAK_AT) == 0) flags |= MF_MENUBARBREAK;
        AppendMenuA(sub, flags, base + i, g_savers[i].name);
    }
    if (checkDefault && g_iDefault >= 0)
        CheckMenuItem(sub, base + g_iDefault, MF_BYCOMMAND | MF_CHECKED);
}

static void ShowContextMenu(void)
{
    HMENU  hMenu, hPop;
    POINT  pt;

    EnumSavers();
    g_bActive = SaverIsActive();

    hMenu = LoadMenuA(g_hInst, MAKEINTRESOURCEA(
                g_bEnglish ? IDR_MENU_EN : IDR_MENU_DE));
    if (!hMenu) return;
    hPop = GetSubMenu(hMenu, 0);
    if (!hPop) { DestroyMenu(hMenu); return; }

    FillSaverMenu(GetSubMenu(hPop, POS_RUN),        IDR_RUN_BASE, IDM_NONE_RUN, FALSE);
    FillSaverMenu(GetSubMenu(hPop, POS_CONFIG),     IDR_CFG_BASE, IDM_NONE_CFG, FALSE);
    FillSaverMenu(GetSubMenu(hPop, POS_SETDEFAULT), IDR_SET_BASE, IDM_NONE_SET, TRUE);

    /* Only one of the two switches can ever do something. */
    EnableMenuItem(hPop, IDM_ENABLE,
                   MF_BYCOMMAND | (g_bActive ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(hPop, IDM_DISABLE,
                   MF_BYCOMMAND | (g_bActive ? MF_ENABLED : MF_GRAYED));
    if (g_iDefault < 0)
        EnableMenuItem(hPop, IDM_START_DEFAULT, MF_BYCOMMAND | MF_GRAYED);

    CheckMenuItem(hPop, g_bEnglish ? IDM_LANG_EN : IDM_LANG_DE,
                  MF_BYCOMMAND | MF_CHECKED);
    if (AutostartEnabled())
        CheckMenuItem(hPop, IDM_AUTOSTART, MF_BYCOMMAND | MF_CHECKED);

    GetCursorPos(&pt);
    /* The classic dance that lets the menu close when focus is lost. */
    SetForegroundWindow(g_hWnd);
    TrackPopupMenu(hPop, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, g_hWnd, NULL);
    PostMessageA(g_hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

static void OpenDisplayProperties(void)
{
    char                sys[MAX_PATH];
    char                cmd[MAX_PATH + 64];
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;

    sys[0] = 0;
    GetSystemDirectoryA(sys, sizeof(sys));
    lstrcpynA(cmd, sys, MAX_PATH);
    if (cmd[0]) lstrcatA(cmd, "\\");
    lstrcatA(cmd, "rundll32.exe shell32.dll,Control_RunDLL desk.cpl,,1");

    Zero(&si, sizeof(si));
    si.cb = sizeof(si);
    Zero(&pi, sizeof(pi));

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL,
                       sys[0] ? sys : NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        ShellExecuteA(g_hWnd, "open", "desk.cpl", NULL, NULL, SW_SHOWNORMAL);
    }
}

/* ======================================================================= */
/*  about box                                                              */
/* ======================================================================= */

static INT_PTR CALLBACK AboutProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        SendMessageA(hDlg, WM_SETICON, ICON_BIG,
                     (LPARAM)LoadIconA(g_hInst, MAKEINTRESOURCEA(IDI_ENABLED)));
        SetForegroundWindow(hDlg);
        return TRUE;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == GetDlgItem(hDlg, IDC_ABOUT_LINK)) {
            SetTextColor((HDC)wp, RGB(0, 0, 224));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
        }
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        case IDC_ABOUT_LINK:
            if (HIWORD(wp) == STN_CLICKED)
                ShellExecuteA(hDlg, "open", APP_URL, NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void ShowAbout(void)
{
    DialogBoxParamA(g_hInst,
                    MAKEINTRESOURCEA(g_bEnglish ? IDD_ABOUT_EN : IDD_ABOUT_DE),
                    g_hWnd, AboutProc, 0);
}

/* ======================================================================= */
/*  window procedure                                                       */
/* ======================================================================= */

static void OnCommand(UINT id)
{
    /* Dynamic ranges first - they are the only ids that carry an index. */
    if (id >= IDR_RUN_BASE && id < IDR_RUN_BASE + IDR_RANGE) {
        int i = id - IDR_RUN_BASE;
        if (i < g_nSavers && !RunSaver(g_savers[i].file, "/s"))
            Say(IDS_STARTFAIL, MB_ICONEXCLAMATION);
        return;
    }
    if (id >= IDR_CFG_BASE && id < IDR_CFG_BASE + IDR_RANGE) {
        int i = id - IDR_CFG_BASE;
        if (i < g_nSavers) RunSaver(g_savers[i].file, "/c");
        return;
    }
    if (id >= IDR_SET_BASE && id < IDR_SET_BASE + IDR_RANGE) {
        int i = id - IDR_SET_BASE;
        if (i < g_nSavers) {
            SetDefaultSaver(g_savers[i].file);
            g_iDefault = i;
            RefreshState(TRUE);
        }
        return;
    }

    switch (id) {
    case IDM_START_DEFAULT: StartDefaultSaver();            break;
    case IDM_ENABLE:        SaverSetActive(TRUE);
                            RefreshState(TRUE);             break;
    case IDM_DISABLE:       SaverSetActive(FALSE);
                            RefreshState(TRUE);             break;
    case IDM_DISPLAY:       OpenDisplayProperties();        break;
    case IDM_AUTOSTART:     AutostartSet(!AutostartEnabled()); break;
    case IDM_ABOUT:         ShowAbout();                    break;
    case IDM_LANG_DE:       SetLanguage(FALSE);             break;
    case IDM_LANG_EN:       SetLanguage(TRUE);              break;
    case IDM_EXIT:          DestroyWindow(g_hWnd);          break;
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_uTaskbarCreated && g_uTaskbarCreated) {
        /* Explorer restarted - our icon went with it. */
        g_bIconOk = FALSE;
        TrayAdd();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        /* No Shell_NotifyIcon here: the window is not fully created yet while
           WM_CREATE runs, and the shell can accept the icon and then drop it.
           The icon is added once CreateWindowEx has returned. */
        g_bActive = SaverIsActive();
        SetTimer(hWnd, IDT_REFRESH, REFRESH_MS, NULL);
        return 0;

    case WM_TIMER:
        if (wp == IDT_REFRESH) {
            TrayEnsure();          /* re-add a lost or never-added icon */
            RefreshState(FALSE);
        }
        return 0;

    case WM_WININICHANGE:               /* == WM_SETTINGCHANGE on NT */
        RefreshState(FALSE);
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK)      StartDefaultSaver();
        else if (lp == WM_RBUTTONUP ||
                 lp == WM_CONTEXTMENU)   ShowContextMenu();
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;

    case WM_ENDSESSION:
        if (wp) TrayRemove();
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_REFRESH);
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wp, lp);
}

/* ======================================================================= */
/*  entry point (no CRT startup)                                           */
/* ======================================================================= */

void __cdecl WinMainCRTStartup(void)
{
    WNDCLASSA wc;
    MSG       msg;
    HWND      prev;

    g_hInst = GetModuleHandleA(NULL);

    /* One tray icon is enough: hand focus to the instance already running. */
    prev = FindWindowA(CLASSNAME, NULL);
    if (prev) {
        SetForegroundWindow(prev);
        ExitProcess(0);
    }

    LoadLanguage();
    g_uTaskbarCreated = RegisterWindowMessageA("TaskbarCreated");

    Zero(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g_hInst;
    wc.hIcon         = LoadIconA(g_hInst, MAKEINTRESOURCEA(IDI_ENABLED));
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = CLASSNAME;
    if (!RegisterClassA(&wc)) ExitProcess(1);

    /* A plain hidden overlapped window - HWND_MESSAGE does not exist on 9x. */
    g_hWnd = CreateWindowExA(WS_EX_TOOLWINDOW, CLASSNAME, Str(IDS_APPTITLE), WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, g_hInst, NULL);
    if (!g_hWnd) ExitProcess(1);

    g_bActive = SaverIsActive();
    TrayAdd();          /* if this is dropped, the timer notices and retries */

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (g_hVersion) FreeLibrary(g_hVersion);
    ExitProcess((UINT)msg.wParam);
}
