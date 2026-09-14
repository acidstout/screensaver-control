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
#include <tlhelp32.h>
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
    char file[MAX_PATH];            /* canonical path - what the registry gets */
    char name[128];                 /* display name shown in the menu          */
    BYTE native;                    /* lives in the native System32 (see below)*/
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
    char raw[MAX_PATH];

    if (!RegReadStr(HKEY_CURRENT_USER, REG_DESKTOP, "SCRNSAVE.EXE",
                    raw, sizeof(raw)) || !raw[0])
        return FALSE;

    /* Usually a plain path, but the value is allowed to be REG_EXPAND_SZ with
       %SystemRoot% in it and nothing downstream would expand that for us. */
    out[0] = 0;
    if (!ExpandEnvironmentStringsA(raw, out, cb) || !out[0])
        lstrcpynA(out, raw, cb);
    return out[0] != 0;
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

/* Turn a recorded "%WINDIR%\System32\x.scr" into something this 32-bit
   process can actually open, i.e. the Sysnative alias.  Only rewrites a path
   that really is in System32 and really is missing from our point of view, so
   it is a no-op everywhere except under WOW64. */
static void FixupLaunchPath(char *path, int cb)
{
    char win[MAX_PATH], cand[MAX_PATH];

    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;

    win[0] = 0;
    if (!GetWindowsDirectoryA(win, sizeof(win)) || !win[0]) return;

    lstrcpynA(cand, win, MAX_PATH);
    lstrcatA(cand, "\\System32\\");
    lstrcatA(cand, BaseName(path));
    if (lstrcmpiA(cand, path) != 0) return;         /* not a System32 path */

    lstrcpynA(cand, win, MAX_PATH);
    lstrcatA(cand, "\\Sysnative\\");
    lstrcatA(cand, BaseName(path));
    if (GetFileAttributesA(cand) != INVALID_FILE_ATTRIBUTES)
        lstrcpynA(path, cand, cb);
}

/* Launch a .scr with the given switch ("/s" = show, "/c" = configure). */
static BOOL RunSaver(const char *path, const char *sw)
{
    char                cmd[MAX_PATH + 16];
    char                dir[MAX_PATH];
    char                real[MAX_PATH];
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;

    lstrcpynA(real, path, MAX_PATH);
    FixupLaunchPath(real, MAX_PATH);

    cmd[0] = '"';
    lstrcpynA(cmd + 1, real, MAX_PATH);
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

/* The name Windows itself shows for a screen saver is *string resource 1*.
   That is the scrnsave.lib IDS_DESCRIPTION convention and the Display control
   panel has read it since Windows 95.  Version info is only a fallback: for
   the built-in savers FileDescription is a whole sentence
   ("Bildschirmschoner \"Seifenblasen\"") where string 1 is the actual name
   ("Seifenblasen").  LOAD_LIBRARY_AS_DATAFILE maps the file without running a
   line of its code, and reads resources out of a 64-bit .scr just fine. */
static BOOL SaverNameFromString(const char *path, char *out, int cb)
{
    HMODULE h;
    int     n, i;

    h = LoadLibraryExA(path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!h) return FALSE;
    out[0] = 0;
    n = LoadStringA(h, 1, out, cb);
    FreeLibrary(h);
    if (n <= 0) return FALSE;

    /* Savers pad this string surprisingly often. */
    for (i = lstrlenA(out) - 1; i >= 0 && (out[i] == ' ' || out[i] == '	'); i--)
        out[i] = 0;
    for (i = 0; out[i] == ' ' || out[i] == '	'; i++) ;
    if (i) {
        int j = 0;
        while (out[i]) out[j++] = out[i++];
        out[j] = 0;
    }
    return out[0] != 0;
}

/* Best display name for a .scr, in the order Windows would pick it. */
static void ResolveName(const char *path, const char *fileName,
                        char *out, int cb)
{
    int i;

    if (!SaverNameFromString(path, out, cb) &&
        !SaverDisplayName(path, out, cb))
        out[0] = 0;

    /* A missing name, or a FileDescription that is really a paragraph, is
       less use than the plain file name. */
    if (!out[0] || lstrlenA(out) > NAME_MAX_CHARS) {
        lstrcpynA(out, fileName, cb);
        for (i = lstrlenA(out) - 1; i > 0; i--)
            if (out[i] == '.') { out[i] = 0; break; }
    }
    if (lstrlenA(out) > NAME_MAX_CHARS) {
        out[NAME_MAX_CHARS - 3] = '.';
        out[NAME_MAX_CHARS - 2] = '.';
        out[NAME_MAX_CHARS - 1] = '.';
        out[NAME_MAX_CHARS]     = 0;
    }
}

static BOOL AlreadyHave(const char *file)
{
    int i;
    for (i = 0; i < g_nSavers; i++)
        if (lstrcmpiA(BaseName(g_savers[i].file), file) == 0)
            return TRUE;
    return FALSE;
}

/* Search searchDir for *.scr but record each hit as living in recordDir.
   The two differ only for the Sysnative alias - see EnumSavers. */
static void ScanDir(const char *searchDir, const char *recordDir, BOOL native)
{
    char             pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    int              n;

    if (!searchDir || !searchDir[0]) return;
    if (!recordDir || !recordDir[0]) recordDir = searchDir;

    lstrcpynA(pat, searchDir, MAX_PATH);
    n = lstrlenA(pat);
    if (n && pat[n - 1] != '\\') lstrcatA(pat, "\\");
    lstrcatA(pat, "*.scr");

    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        SAVER *s;
        char   probe[MAX_PATH];

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_nSavers >= MAX_SAVERS) break;
        if (AlreadyHave(fd.cFileName)) continue;

        s = &g_savers[g_nSavers];
        s->native = (BYTE)(native ? 1 : 0);

        /* Path we record (and hand to the registry). */
        lstrcpynA(s->file, recordDir, MAX_PATH);
        n = lstrlenA(s->file);
        if (n && s->file[n - 1] != '\\') lstrcatA(s->file, "\\");
        lstrcatA(s->file, fd.cFileName);

        /* Path we can actually open right now, to read the name from. */
        lstrcpynA(probe, searchDir, MAX_PATH);
        n = lstrlenA(probe);
        if (n && probe[n - 1] != '\\') lstrcatA(probe, "\\");
        lstrcatA(probe, fd.cFileName);

        ResolveName(probe, fd.cFileName, s->name, sizeof(s->name));
        if (!s->name[0]) continue;
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

/* The native System32, despite WOW64 file redirection.

   This used to go through the "Sysnative" alias alone, which turned out to be
   the wrong tool: Sysnative is reliable for *opening* a file (which is why
   launching a System32-only saver worked even when the menu could not find
   one) but enumerating a directory through it is not dependable - on some
   Windows versions FindFirstFile comes back empty and every saver that lives
   only in the native System32 silently vanishes from the list.

   Wow64DisableWow64FsRedirection is the documented mechanism and it covers
   enumeration too.  It is per-thread, and this program is single threaded.
   The alias is kept purely as a fallback. */
static void ScanNativeSystem32(const char *win)
{
    typedef BOOL (WINAPI *PFNDISABLE)(PVOID *);
    typedef BOOL (WINAPI *PFNREVERT)(PVOID);
    static PFNDISABLE disable = NULL;
    static PFNREVERT  revert  = NULL;
    static BOOL       ready   = FALSE;
    char  sys32[MAX_PATH], nat[MAX_PATH];
    PVOID state = NULL;

    if (!win || !win[0]) return;

    lstrcpynA(sys32, win, MAX_PATH);
    lstrcatA(sys32, "\\System32");

    if (!ready) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        ready = TRUE;
        disable = (PFNDISABLE)(void *)GetProcAddress(
                      k, "Wow64DisableWow64FsRedirection");
        revert  = (PFNREVERT)(void *)GetProcAddress(
                      k, "Wow64RevertWow64FsRedirection");
    }

    /* Absent on 32-bit Windows and on 9x, where there is nothing to redirect
       and the ordinary GetSystemDirectory scan already covers System32. */
    if (disable && revert && disable(&state)) {
        ScanDir(sys32, sys32, TRUE);
        revert(state);
        return;
    }

    lstrcpynA(nat, win, MAX_PATH);
    lstrcatA(nat, "\\Sysnative");
    ScanDir(nat, sys32, TRUE);
}

static void EnumSavers(void)
{
    char dir[MAX_PATH];
    char win[MAX_PATH];
    char cur[MAX_PATH];
    int  i;

    g_nSavers = 0;
    g_iDefault = -1;

    /* Kept off the static data so the array does not bloat the image. */
    if (!g_savers) {
        g_savers = (SAVER *)LocalAlloc(LPTR, MAX_SAVERS * sizeof(SAVER));
        if (!g_savers) return;
    }

    /* On 64-bit Windows a 32-bit process is file-redirected: everything we
       ask for in "...\System32\\" is actually served out of SysWOW64.  So
       GetSystemDirectory gives us the 32-bit savers and the *native* System32
       - where Bubbles, Mystify, Ribbons and 3D Text live - stays invisible.
       The "Sysnative" alias is the way out; it exists only for 32-bit
       processes under WOW64, so on a 32-bit Windows (or on 9x) the directory
       simply is not there and the scan finds nothing.  No version check
       needed.

       Sysnative is meaningless to the 64-bit shell, so these entries are
       *recorded* under their real System32 name - that is what has to end up
       in SCRNSAVE.EXE - and mapped back to Sysnative only when we open or
       launch them.  Native first, so it wins the de-duplication against the
       SysWOW64 copy of the same saver, exactly as the Control Panel does. */
    win[0] = 0;
    GetWindowsDirectoryA(win, sizeof(win));
    ScanNativeSystem32(win);

    dir[0] = 0; GetSystemDirectoryA(dir, sizeof(dir));  ScanDir(dir, dir, FALSE);
    if (win[0]) ScanDir(win, win, FALSE);

    /* A saver configured from somewhere else must still show up.  Search the
       directory in the form this process can actually reach, but record the
       entries under the name the registry uses - those two differ under
       WOW64. */
    if (GetDefaultSaver(cur, sizeof(cur)) && !AlreadyHave(BaseName(cur))) {
        char        probe[MAX_PATH];
        const char *bp, *bc;

        lstrcpynA(probe, cur, MAX_PATH);
        FixupLaunchPath(probe, MAX_PATH);
        bp = BaseName(probe);
        bc = BaseName(cur);
        if (bp > probe && bc > cur) {
            char rec[MAX_PATH];
            lstrcpynA(dir, probe, (int)(bp - probe));  /* strips the backslash */
            lstrcpynA(rec, cur,   (int)(bc - cur));
            ScanDir(dir, rec, FALSE);
        }
    }

    /* Whatever the scans did or did not turn up, the configured saver has to
       appear - showing which one is current is the whole point of the list.
       Add it by hand rather than trusting any directory walk. */
    if (GetDefaultSaver(cur, sizeof(cur)) && !AlreadyHave(BaseName(cur)) &&
        g_nSavers < MAX_SAVERS) {
        char probe[MAX_PATH];

        lstrcpynA(probe, cur, MAX_PATH);
        FixupLaunchPath(probe, MAX_PATH);
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            SAVER *s = &g_savers[g_nSavers];
            lstrcpynA(s->file, cur, MAX_PATH);
            s->native = 0;
            ResolveName(probe, BaseName(cur), s->name, sizeof(s->name));
            if (s->name[0]) g_nSavers++;
        }
    }

    SortSavers();

    /* Exact path first; fall back to the file name, since the registry and our
       own spelling of the same file need not match character for character. */
    if (GetDefaultSaver(cur, sizeof(cur))) {
        for (i = 0; i < g_nSavers; i++)
            if (lstrcmpiA(g_savers[i].file, cur) == 0) { g_iDefault = i; break; }
        if (g_iDefault < 0)
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

    /* NIM_ADD fails whenever the shell still holds our icon - and that is
       precisely the state one transient NIM_MODIFY failure leaves behind
       (Explorer slow to answer, e.g. while a game is running).  With NIM_ADD
       alone that recovery can never succeed: g_bIconOk stayed FALSE, every
       update became another rejected add, and the icon and tooltip froze for
       the rest of the session.  Falling back to NIM_MODIFY adopts the icon
       that is already there. */
    g_bIconOk = Shell_NotifyIconA(NIM_ADD, &nid) ||
                Shell_NotifyIconA(NIM_MODIFY, &nid);
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

/* ======================================================================= */
/*  guard: who turned the screen saver off, and putting it back            */
/* ======================================================================= */

/* Games routinely switch the screen saver off so a cut scene is not
   interrupted, and some of them never switch it back - GTA V is the reason
   this exists.  Note that a game holding SetThreadExecutionState cannot cause
   that: the request dies with the process.  Only the persistent route,
   SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, FALSE), leaves the setting off
   afterwards, and that is exactly the case this can repair.

   The rule: remember what the *user* wants.  If the setting goes off while
   some other program owns the foreground, that program is a suspect; it is
   only treated as one once it is seen running full screen, which is what
   separates a game from somebody unticking the box in the control panel.  The
   setting is left alone for as long as the suspect runs - the whole point is
   not to interrupt the game - and restored once its process is gone. */

#define SUSPECT_GRACE  15           /* ticks (~30 s) to show itself full screen */
#define ZOMBIE_TICKS    5           /* ticks (~10 s) with no window of its own  */

static BOOL  g_bGuard;              /* feature enabled, persisted            */
static BOOL  g_bIntended;           /* the state the user actually wants     */
static BOOL  g_guardLast;           /* last state the GUARD saw - see below  */
static DWORD g_suspectPid;          /* turned it off; 0 = nobody             */
static char  g_suspectName[64];
static BOOL  g_suspectFull;         /* has been seen running full screen     */
static UINT  g_suspectAge;          /* ticks since it was noticed            */
static UINT  g_suspectGone;         /* consecutive ticks showing nothing     */
static char  g_culprit[64];         /* last confirmed one, for the menu      */

/* ToolHelp32 is on Windows 95 and on Windows 2000 and later, but not NT4, so
   it is bound late and everything here degrades to "unknown" without it. */
typedef HANDLE (WINAPI *PFNSNAP)(DWORD, DWORD);
typedef BOOL   (WINAPI *PFNPROC32)(HANDLE, LPPROCESSENTRY32);

static PFNSNAP   g_pSnap;
static PFNPROC32 g_pFirst, g_pNext;

static void LoadToolHelp(void)
{
    static BOOL ready = FALSE;
    HMODULE k;

    if (ready) return;
    ready = TRUE;
    k = GetModuleHandleA("kernel32.dll");
    g_pSnap  = (PFNSNAP)  (void *)GetProcAddress(k, "CreateToolhelp32Snapshot");
    g_pFirst = (PFNPROC32)(void *)GetProcAddress(k, "Process32First");
    g_pNext  = (PFNPROC32)(void *)GetProcAddress(k, "Process32Next");
}

/* Exe name for a pid.  Empty when it cannot be determined or has exited. */
static void ProcessName(DWORD pid, char *out, int cb)
{
    HANDLE         snap;
    PROCESSENTRY32 pe;

    out[0] = 0;
    LoadToolHelp();
    if (!g_pSnap || !g_pFirst || !g_pNext || !pid) return;

    snap = g_pSnap(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    Zero(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (g_pFirst(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                lstrcpynA(out, BaseName(pe.szExeFile), cb);
                break;
            }
        } while (g_pNext(snap, &pe));
    }
    CloseHandle(snap);
}

/* TRUE while a process with this pid *and* this exe name still exists - the
   name guards against the pid being handed to something else meanwhile. */
static BOOL ProcessAlive(DWORD pid, const char *name)
{
    char now[64];

    if (!pid) return FALSE;
    ProcessName(pid, now, sizeof(now));
    if (!now[0]) return FALSE;
    return name[0] ? (lstrcmpiA(now, name) == 0) : TRUE;
}

/* Does this process still put anything on the screen?

   Waiting for the process to exit is not enough on its own: GTA V regularly
   fails to shut down and sits in the task list until it is killed by hand, and
   the screen saver would stay off all that time.  A process whose windows are
   all gone is finished as far as the user is concerned, whatever the task list
   says.  A minimised window still counts as visible, so this does not fire
   just because the game was minimised. */
typedef struct { DWORD pid; BOOL found; } VISWND;

static BOOL CALLBACK VisWndProc(HWND h, LPARAM lp)
{
    VISWND *v = (VISWND *)lp;
    DWORD   p = 0;

    GetWindowThreadProcessId(h, &p);
    if (p == v->pid && IsWindowVisible(h)) {
        v->found = TRUE;
        return FALSE;                        /* stop enumerating */
    }
    return TRUE;
}

static BOOL ProcessHasVisibleWindow(DWORD pid)
{
    VISWND v;

    if (!pid) return FALSE;
    v.pid   = pid;
    v.found = FALSE;
    EnumWindows(VisWndProc, (LPARAM)&v);
    return v.found;
}

/* The foreground window's owner, and whether it covers a whole monitor. */
static BOOL ForegroundApp(DWORD *pid, char *name, int cb, BOOL *fullscreen)
{
    typedef HMONITOR (WINAPI *PFNMFW)(HWND, DWORD);
    typedef BOOL     (WINAPI *PFNGMI)(HMONITOR, LPMONITORINFO);
    static PFNMFW monFrom = NULL;
    static PFNGMI monInfo = NULL;
    static BOOL   ready   = FALSE;

    HWND  fg = GetForegroundWindow();
    RECT  wr, mr;
    DWORD p = 0;

    *pid = 0; name[0] = 0; *fullscreen = FALSE;
    if (!fg || fg == GetDesktopWindow()) return FALSE;

    /* The visible desktop is Progman/WorkerW, owned by Explorer and exactly
       the size of the monitor - so without this, Explorer read as a
       full-screen app whenever the desktop had the focus, and was tracked as
       a suspect forever because it never exits.  The taskbar is where the
       focus lands after using our own tray menu. */
    {
        char cls[32];
        cls[0] = 0;
        GetClassNameA(fg, cls, sizeof(cls));
        if (!lstrcmpiA(cls, "Progman")       || !lstrcmpiA(cls, "WorkerW") ||
            !lstrcmpiA(cls, "Shell_TrayWnd") ||
            !lstrcmpiA(cls, "Shell_SecondaryTrayWnd"))
            return FALSE;
    }

    GetWindowThreadProcessId(fg, &p);
    if (!p || p == GetCurrentProcessId()) return FALSE;
    *pid = p;
    ProcessName(p, name, cb);

    if (!GetWindowRect(fg, &wr)) return TRUE;

    if (!ready) {
        HMODULE u = GetModuleHandleA("user32.dll");
        ready = TRUE;
        monFrom = (PFNMFW)(void *)GetProcAddress(u, "MonitorFromWindow");
        monInfo = (PFNGMI)(void *)GetProcAddress(u, "GetMonitorInfoA");
    }

    mr.left = mr.top = 0;
    mr.right  = GetSystemMetrics(SM_CXSCREEN);
    mr.bottom = GetSystemMetrics(SM_CYSCREEN);
    if (monFrom && monInfo) {                 /* multi-monitor: Windows 98+ */
        MONITORINFO mi;
        HMONITOR    hm = monFrom(fg, MONITOR_DEFAULTTONEAREST);
        Zero(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (hm && monInfo(hm, &mi)) mr = mi.rcMonitor;
    }

    /* Exclusive full screen and borderless both look like this. */
    *fullscreen = (wr.left  <= mr.left  && wr.top    <= mr.top &&
                   wr.right >= mr.right && wr.bottom >= mr.bottom);
    return TRUE;
}

static void ForgetSuspect(void)
{
    g_suspectPid    = 0;
    g_suspectName[0] = 0;
    g_suspectFull   = FALSE;
    g_suspectAge    = 0;
    g_suspectGone   = 0;
}

/* The user said what they want - stop second-guessing it. */
static void SetIntended(BOOL on)
{
    g_bIntended = on;
    ForgetSuspect();
}

static void LoadGuard(void)
{
    char v[16];
    g_bGuard = TRUE;                 /* on unless switched off */
    if (RegReadStr(HKEY_CURRENT_USER, REG_SETTINGS, "Guard", v, sizeof(v))
        && v[0])
        g_bGuard = (v[0] != '0');
    g_culprit[0] = 0;
    RegReadStr(HKEY_CURRENT_USER, REG_SETTINGS, "LastDisabledBy",
               g_culprit, sizeof(g_culprit));
}

static void SetGuard(BOOL on)
{
    g_bGuard = on;
    RegWriteStr(HKEY_CURRENT_USER, REG_SETTINGS, "Guard", on ? "1" : "0");
    if (!on) ForgetSuspect();
}

/* Called once per timer tick, after the on/off state has been read. */
/* The guard keeps its own "last seen" state rather than sharing g_bActive
   with the tray icon.  Sharing it was a bug: anything that refreshed the icon
   first - the context menu reading the state, or one of our own menu commands -
   swallowed the transition, so the guard either missed a game switching the
   saver off or mistook our own change for someone else's. */
static void GuardTick(BOOL now)
{
    DWORD pid = 0;
    char  name[64];
    BOOL  full = FALSE;
    BOOL  changed = (now != g_guardLast);

    g_guardLast = now;

    if (changed) {
        if (now) {
            /* Back on - whoever did it, that is now the wanted state. */
            SetIntended(TRUE);
            return;
        }
        /* Switched off by something that is not us. */
        ForegroundApp(&pid, name, sizeof(name), &full);
        if (!pid) { SetIntended(FALSE); return; }

        g_suspectPid  = pid;
        g_suspectFull = full;
        g_suspectAge  = 0;
        g_suspectGone = 0;
        lstrcpynA(g_suspectName, name, sizeof(g_suspectName));
        return;
    }

    if (!g_suspectPid) return;

    /* It may not have gone full screen yet - games switch the saver off while
       still loading in a window - so keep watching for a while. */
    if (!g_suspectFull) {
        DWORD fpid = 0;
        char  fname[64];
        BOOL  ffull = FALSE;
        ForegroundApp(&fpid, fname, sizeof(fname), &ffull);
        if (ffull && fpid == g_suspectPid) g_suspectFull = TRUE;
    }

    if (g_suspectFull) {
        /* Confirmed.  Remember it, and leave the setting alone while it runs. */
        if (lstrcmpiA(g_culprit, g_suspectName) != 0) {
            lstrcpynA(g_culprit, g_suspectName, sizeof(g_culprit));
            RegWriteStr(HKEY_CURRENT_USER, REG_SETTINGS, "LastDisabledBy",
                        g_culprit);
        }
        /* Finished = the process is gone, or it has stopped showing anything
           for a while (a hung game still listed in the task manager). */
        if (!ProcessAlive(g_suspectPid, g_suspectName)) {
            g_suspectGone = ZOMBIE_TICKS;
        } else if (!ProcessHasVisibleWindow(g_suspectPid)) {
            g_suspectGone++;
        } else {
            g_suspectGone = 0;
        }

        if (g_suspectGone >= ZOMBIE_TICKS) {
            ForgetSuspect();
            if (g_bGuard && g_bIntended && !SaverIsActive()) {
                SaverSetActive(TRUE);       /* put it back */
                g_bActive   = TRUE;
                g_guardLast = TRUE;
                TrayUpdate();
            }
        }
        return;
    }

    /* Never went full screen, and it is gone or has had long enough: this was
       somebody changing the setting on purpose, so adopt it. */
    if (++g_suspectAge >= SUSPECT_GRACE ||
        !ProcessAlive(g_suspectPid, g_suspectName))
        SetIntended(FALSE);
}

/* Keep the icon in step with the setting.  Touches nothing the guard uses. */
static void SyncIcon(void)
{
    BOOL now = SaverIsActive();
    if (now != g_bActive) {
        g_bActive = now;
        TrayUpdate();
    }
}

static void RefreshState(BOOL force)
{
    BOOL now = SaverIsActive();

    if (force || now != g_bActive) {
        g_bActive = now;
        TrayUpdate();
    }
    GuardTick(now);
}

/* A change we make ourselves.  The icon follows it, and the guard is told the
   new state up front so it cannot mistake it for another program switching
   the screen saver off - it used to, attributing it to whatever window had
   the focus after the menu closed. */
static void ApplyOwnChange(BOOL on)
{
    SetIntended(on);
    SaverSetActive(on);
    g_guardLast = SaverIsActive();
    g_bActive   = g_guardLast;
    TrayUpdate();
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
/*  dark mode                                                              */
/* ======================================================================= */

/* Windows only grew a way to darken menus in Windows 10 1809, and even there
   it is undocumented uxtheme ordinals.  Owner-drawn menu items, by contrast,
   behave identically from Windows 95 onwards, so that is what this uses.
   Items are converted to MF_OWNERDRAW only while dark mode is on - in light
   mode the menu is left exactly as the system draws it, so the native look is
   never second-guessed. */

#define DARK_BG      RGB( 43,  43,  43)
#define DARK_BGSEL   RGB( 72,  72,  72)
#define DARK_TEXT    RGB(240, 240, 240)
#define DARK_DIS     RGB(120, 120, 120)
#define DARK_SEP     RGB( 78,  78,  78)
#define DARK_DLGBG   RGB( 32,  32,  32)
#define DARK_LINK    RGB(105, 170, 255)

#define ITEM_CHECKW  22             /* width of the check column, in pixels */
#define ITEM_ARROWW  14             /* room kept for a submenu arrow        */
#define ARROW_INSET  10             /* chevron tip, in from the item edge   */
#define ITEM_PADX     8
#define ITEM_PADY     6
#define ITEM_SEPH     7
#define DARK_BORDER  RGB( 80,  80,  80)
#define BORDER_W      2             /* matches the frame Windows draws      */
#define MAX_ARROWS   48

/* Windows paints the submenu arrows and the popup's border *after* it has
   asked us to draw the items, so neither can be touched from WM_DRAWITEM -
   the system arrow simply lands on top of anything we put there.  The way in
   is to repaint once the whole pass is over: WM_DRAWITEM records what needs
   fixing and posts WM_FIXMENU to ourselves.  TrackPopupMenu runs a modal loop
   that still dispatches to our window, so the message is handled the moment
   the paint finishes.  No subclassing of the system menu window required. */
#define WM_FIXMENU   (WM_APP + 2)

typedef struct {
    char text[100];
    BYTE isSep;
    BYTE isPopup;
} MITEM;

#define MAX_MITEMS   (3 * MAX_SAVERS + 40)

static BOOL    g_bDark;
static MITEM  *g_items;             /* owner-draw item pool, lazily made   */
static int     g_nMItems;

/* Arrow zones of the popup window currently being painted. */
static HWND    g_menuWnd;
static int     g_nArrows;
static BOOL    g_fixPending;
static struct { RECT r; COLORREF bg; } g_arrows[MAX_ARROWS];

/* The real menu font, so owner-drawn items match everything else on screen.
   NONCLIENTMETRICS grew a field in Vista; try the current size, then the
   pre-Vista one, before falling back to the stock GUI font. */
static HFONT MenuFont(void)
{
    static HFONT      f = NULL;
    static BOOL       tried = FALSE;
    NONCLIENTMETRICSA ncm;

    if (!tried) {
        tried = TRUE;
        Zero(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (!SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            Zero(&ncm, sizeof(ncm));
            ncm.cbSize = sizeof(ncm) - sizeof(int);
            if (!SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0))
                ncm.cbSize = 0;
        }
        if (ncm.cbSize) f = CreateFontIndirectA(&ncm.lfMenuFont);
    }
    return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* Windows 10 2004 and later can darken the title bar too.  Bound late, so it
   is simply absent - and harmless - everywhere else. */
static void DarkenCaption(HWND hWnd, BOOL dark)
{
    typedef LONG (WINAPI *PFNDWMSET)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE   dwm;
    PFNDWMSET set;
    BOOL      on = dark ? TRUE : FALSE;

    dwm = LoadLibraryA("dwmapi.dll");
    if (!dwm) return;
    set = (PFNDWMSET)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (set) {
        /* 20 is DWMWA_USE_IMMERSIVE_DARK_MODE; it was 19 on the first builds. */
        if (set(hWnd, 20, &on, sizeof(on)) != 0)
            set(hWnd, 19, &on, sizeof(on));
    }
    FreeLibrary(dwm);
}

/* GetMenuStringA truncates by *character* count rather than by buffer size.
   On a system whose ANSI codepage is UTF-8 - Windows 10's "Beta: Use Unicode
   UTF-8 for worldwide language support" - every non-ASCII character therefore
   costs one byte off the end: "Anzeigeeigenschaften oeffnen" comes back one
   character short.  Where GetMenuStringW exists (every NT) take the wide
   string and do the conversion here, sized in bytes.  Windows 9x has no W
   entry point, but its ANSI codepage can never be UTF-8, so the A path is
   exact there. */
static void MenuItemText(HMENU hMenu, int pos, char *out, int cb)
{
    typedef int (WINAPI *PFNGMSW)(HMENU, UINT, LPWSTR, int, UINT);
    static PFNGMSW getW  = NULL;
    static BOOL    ready = FALSE;
    WCHAR w[160];

    out[0] = 0;
    if (!ready) {
        ready = TRUE;
        getW = (PFNGMSW)(void *)GetProcAddress(GetModuleHandleA("user32.dll"),
                                               "GetMenuStringW");
    }
    if (getW) {
        if (getW(hMenu, (UINT)pos, w, 160, MF_BYPOSITION) > 0 &&
            WideCharToMultiByte(CP_ACP, 0, w, -1, out, cb, NULL, NULL) > 0)
            return;
        out[0] = 0;
    }
    GetMenuStringA(hMenu, (UINT)pos, out, cb, MF_BYPOSITION);
}

/* The light frame around a dark popup is two different things: a 1px window
   border, and 2px of the menu's own client background that the item rects do
   not cover.  The background is what actually reads as a thick light edge,
   and MIM_BACKGROUND replaces it outright.  SetMenuInfo arrived in Windows
   2000, so it is bound late; Windows 9x menus have no such padding to fix. */
static void MenuDarkBackground(HMENU hMenu)
{
    typedef BOOL (WINAPI *PFNSETMENUINFO)(HMENU, LPCMENUINFO);
    static PFNSETMENUINFO setInfo = NULL;
    static BOOL           ready   = FALSE;
    static HBRUSH         brBack  = NULL;
    MENUINFO mi;

    if (!ready) {
        ready = TRUE;
        setInfo = (PFNSETMENUINFO)(void *)GetProcAddress(
                      GetModuleHandleA("user32.dll"), "SetMenuInfo");
        brBack = CreateSolidBrush(DARK_BG);
    }
    if (!setInfo || !brBack) return;

    Zero(&mi, sizeof(mi));
    mi.cbSize  = sizeof(mi);
    mi.fMask   = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = brBack;
    setInfo(hMenu, &mi);
}

/* Convert one popup, and everything below it, to owner-drawn items. */
static void MenuGoDark(HMENU hMenu)
{
    int i, n;

    if (!g_items) {
        g_items = (MITEM *)LocalAlloc(LPTR, MAX_MITEMS * sizeof(MITEM));
        if (!g_items) return;
    }

    n = GetMenuItemCount(hMenu);
    for (i = 0; i < n; i++) {
        UINT     st = GetMenuState(hMenu, i, MF_BYPOSITION);
        MITEM   *it;
        UINT     flags;
        UINT_PTR target;

        if (g_nMItems >= MAX_MITEMS) return;
        it = &g_items[g_nMItems++];

        /* For an item that opens a submenu, GetMenuState returns the submenu's
           item count in the high byte and only the flags in the low byte.
           MF_SEPARATOR is 0x800 - inside that high byte - so a popup with 8 or
           more entries reads back as a separator and would be drawn as a bare
           line.  Never trust the high-byte flags on a popup. */
        it->isPopup = (BYTE)((st & MF_POPUP) ? 1 : 0);
        it->isSep   = (BYTE)((!it->isPopup && (st & MF_SEPARATOR)) ? 1 : 0);
        it->text[0] = 0;
        if (!it->isSep)
            MenuItemText(hMenu, i, it->text, sizeof(it->text));

        /* Rebuild the flags from what we established rather than masking the
           raw value, for the same reason. */
        flags = MF_BYPOSITION | MF_OWNERDRAW;
        if (st & MF_GRAYED)       flags |= MF_GRAYED;
        if (st & MF_DISABLED)     flags |= MF_DISABLED;
        if (st & MF_CHECKED)      flags |= MF_CHECKED;
        if (st & MF_MENUBARBREAK) flags |= MF_MENUBARBREAK;
        if (st & MF_MENUBREAK)    flags |= MF_MENUBREAK;
        if (it->isPopup)     flags |= MF_POPUP;
        else if (it->isSep)  flags |= MF_SEPARATOR;

        if (it->isPopup) {
            HMENU sub = GetSubMenu(hMenu, i);
            MenuGoDark(sub);                       /* depth first */
            target = (UINT_PTR)sub;
        } else {
            target = GetMenuItemID(hMenu, i);
        }
        ModifyMenuA(hMenu, i, flags, target, (LPCSTR)it);
    }
}

/* Both glyphs are matched to what the themed light menu draws, measured off a
   screenshot: the check is 10x7 at a 2px stroke, the chevron 4x7 at 1px and
   ARROW_INSET in from the right edge of the item.  Keeping the two modes
   geometrically identical matters more than picking prettier numbers. */
static void DrawCheck(HDC dc, const RECT *r, COLORREF col)
{
    HPEN pen, old;
    int  cx = (r->left + r->right) / 2;
    int  cy = (r->top + r->bottom) / 2;

    pen = CreatePen(PS_SOLID, 2, col);
    if (!pen) return;
    old = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, cx - 5, cy, NULL);
    LineTo(dc, cx - 2, cy + 3);
    LineTo(dc, cx + 4, cy - 3);
    SelectObject(dc, old);
    DeleteObject(pen);
}

/* A chevron the same size and in the same place as the themed light menu's:
   4 wide, 7 tall, 1px stroke, tip ARROW_INSET in from the item's right edge.
   GDI leaves the final point of a LineTo undrawn, so the last segment
   deliberately overshoots by one step to make the bottom pixel appear. */
static void DrawArrow(HDC dc, const RECT *r, COLORREF col)
{
    HPEN pen;
    HGDIOBJ old;
    int  x = r->right - ARROW_INSET;          /* tip of the chevron */
    int  y = (r->top + r->bottom) / 2;

    pen = CreatePen(PS_SOLID, 1, col);
    if (!pen) return;
    old = SelectObject(dc, pen);
    MoveToEx(dc, x - 3, y - 3, NULL);
    LineTo(dc, x, y);
    LineTo(dc, x - 4, y + 4);                 /* overshoot: draws (x-3, y+3) */
    SelectObject(dc, old);
    DeleteObject(pen);
}

/* Repaint the bits Windows draws after us: the popup's light frame, and the
   black submenu arrows it stamps over whatever the item draw left behind. */
static void FixMenuChrome(HWND hMenuWnd)
{
    HDC    dc;
    HBRUSH br;
    RECT   rc;
    int    i;

    if (!hMenuWnd || !IsWindow(hMenuWnd)) return;

    /* Border: the frame lives outside the client area, so it needs a window DC. */
    dc = GetWindowDC(hMenuWnd);
    if (dc) {
        GetWindowRect(hMenuWnd, &rc);
        rc.right  -= rc.left;
        rc.bottom -= rc.top;
        rc.left = rc.top = 0;
        br = CreateSolidBrush(DARK_BORDER);
        if (br) {
            for (i = 0; i < BORDER_W; i++) {
                FrameRect(dc, &rc, br);
                rc.left++; rc.top++; rc.right--; rc.bottom--;
            }
            DeleteObject(br);
        }
        ReleaseDC(hMenuWnd, dc);
    }

    /* Arrows: client coordinates, same space the item rects came in. */
    if (!g_nArrows) return;
    dc = GetDC(hMenuWnd);
    if (!dc) return;
    for (i = 0; i < g_nArrows; i++) {
        RECT a = g_arrows[i].r;
        a.left = a.right - ITEM_ARROWW;
        br = CreateSolidBrush(g_arrows[i].bg);
        if (br) { FillRect(dc, &a, br); DeleteObject(br); }
        DrawArrow(dc, &g_arrows[i].r, DARK_TEXT);
    }
    ReleaseDC(hMenuWnd, dc);
}

static void OnMeasureItem(MEASUREITEMSTRUCT *mis)
{
    MITEM  *it = (MITEM *)mis->itemData;
    HDC     dc;
    HGDIOBJ old;
    SIZE    sz;

    if (mis->CtlType != ODT_MENU || !it) return;

    if (it->isSep) {
        mis->itemWidth  = 0;
        mis->itemHeight = ITEM_SEPH;
        return;
    }

    dc = GetDC(NULL);
    if (!dc) { mis->itemWidth = 120; mis->itemHeight = 20; return; }
    old = SelectObject(dc, MenuFont());
    sz.cx = 0; sz.cy = 0;
    GetTextExtentPoint32A(dc, it->text, lstrlenA(it->text), &sz);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);

    mis->itemWidth  = sz.cx + ITEM_CHECKW + ITEM_ARROWW + ITEM_PADX;
    mis->itemHeight = sz.cy + ITEM_PADY;
    if (mis->itemHeight < 18) mis->itemHeight = 18;
}

static void OnDrawItem(DRAWITEMSTRUCT *dis)
{
    MITEM   *it = (MITEM *)dis->itemData;
    HBRUSH   br;
    HGDIOBJ  old;
    RECT     r, tr;
    COLORREF fg;

    if (dis->CtlType != ODT_MENU || !it) return;

    r = dis->rcItem;

    br = CreateSolidBrush((dis->itemState & ODS_SELECTED) ? DARK_BGSEL : DARK_BG);
    if (br) { FillRect(dis->hDC, &r, br); DeleteObject(br); }

    if (it->isSep) {
        RECT s;
        s.left   = r.left + 4;
        s.right  = r.right - 4;
        s.top    = (r.top + r.bottom) / 2;
        s.bottom = s.top + 1;
        br = CreateSolidBrush(DARK_SEP);
        if (br) { FillRect(dis->hDC, &s, br); DeleteObject(br); }
        return;
    }

    fg = (dis->itemState & (ODS_GRAYED | ODS_DISABLED)) ? DARK_DIS : DARK_TEXT;

    if (dis->itemState & ODS_CHECKED) {
        RECT c = r;
        c.right = c.left + ITEM_CHECKW;
        DrawCheck(dis->hDC, &c, fg);
    }

    /* Track which popup window is being painted, for every item and not just
       the ones with submenus: a saver list has no popups at all, and it still
       needs its border repainted. */
    {
        HWND mw = WindowFromDC(dis->hDC);
        if (!mw) mw = FindWindowA("#32768", NULL);   /* menu window class */
        if (mw != g_menuWnd) { g_menuWnd = mw; g_nArrows = 0; }
    }

    /* Do not draw the arrow here - Windows would stamp its own black one on
       top.  Record it instead and redraw after the whole pass; see WM_FIXMENU. */
    if (it->isPopup && g_nArrows < MAX_ARROWS) {
        g_arrows[g_nArrows].r  = r;
        g_arrows[g_nArrows].bg = (dis->itemState & ODS_SELECTED)
                                 ? DARK_BGSEL : DARK_BG;
        g_nArrows++;
    }

    if (!g_fixPending) {
        g_fixPending = TRUE;
        PostMessageA(g_hWnd, WM_FIXMENU, 0, 0);
    }

    old = SelectObject(dis->hDC, MenuFont());
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    tr = r;
    tr.left  += ITEM_CHECKW;
    tr.right -= ITEM_ARROWW;

    /* Match the system: from Windows 2000 on, the "&" underlines stay hidden
       until the menu is reached by keyboard.  SPI_GETKEYBOARDCUES does not
       exist on 9x, where underlines are always shown - which is also what
       happens here, since the call fails and cues stays TRUE. */
    {
        BOOL cues = TRUE;
        UINT fmt  = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_EXPANDTABS;
        SystemParametersInfoA(SPI_GETKEYBOARDCUES, 0, &cues, 0);
        if (!cues) fmt |= DT_HIDEPREFIX;
        DrawTextA(dis->hDC, it->text, -1, &tr, fmt);
    }
    SelectObject(dis->hDC, old);
}

/* Windows keeps its own preference as a DWORD, so this needs its own read. */
static BOOL SystemPrefersDark(void)
{
    HKEY  hk;
    DWORD type = 0, val = 1, cb = sizeof(val);
    BOOL  dark = FALSE;

    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExA(hk, "AppsUseLightTheme", 0, &type,
                         (LPBYTE)&val, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD)
        dark = (val == 0);
    RegCloseKey(hk);
    return dark;
}

static void LoadDarkMode(void)
{
    char v[16];
    if (RegReadStr(HKEY_CURRENT_USER, REG_SETTINGS, "DarkMode", v, sizeof(v))
        && v[0]) {
        g_bDark = (v[0] == '1');
        return;
    }
    g_bDark = SystemPrefersDark();    /* follow Windows until told otherwise */
}

static void SetDarkMode(BOOL dark)
{
    g_bDark = dark;
    RegWriteStr(HKEY_CURRENT_USER, REG_SETTINGS, "DarkMode", dark ? "1" : "0");
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
    SyncIcon();

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
    if (g_bDark)
        CheckMenuItem(hPop, IDM_DARKMODE, MF_BYCOMMAND | MF_CHECKED);
    if (g_bGuard)
        CheckMenuItem(hPop, IDM_GUARD, MF_BYCOMMAND | MF_CHECKED);

    /* Purely informational, and the answer to "which program was it?". */
    {
        char line[160];
        if (g_culprit[0])
            wsprintfA(line, Str(IDS_DISABLEDBY), g_culprit);
        else
            lstrcpynA(line, Str(IDS_NOCULPRIT), sizeof(line));
        ModifyMenuA(hPop, IDM_CULPRIT,
                    MF_BYCOMMAND | MF_STRING | MF_GRAYED, IDM_CULPRIT, line);
    }

    /* Last of all: every check and grey state must already be set, because
       this reads them back off each item as it converts it. */
    if (g_bDark) {
        g_nMItems = 0;
        MenuDarkBackground(hPop);
        MenuGoDark(hPop);
    }

    GetCursorPos(&pt);
    /* The classic dance that lets the menu close when focus is lost. */
    SetForegroundWindow(g_hWnd);
    /* Dark mode repaints the popup's chrome after Windows has drawn it, so the
       fade-in must not composite over that work.  TPM_NOANIMATION is a no-op
       on anything that does not animate menus anyway. */
    TrackPopupMenu(hPop, TPM_LEFTALIGN | TPM_RIGHTBUTTON |
                         (g_bDark ? TPM_NOANIMATION : 0),
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

/* The OK button is owner-drawn only in dark mode: a themed button ignores
   WM_CTLCOLORBTN for its face, so it would stay light against a dark dialog. */
static void DrawDarkButton(DRAWITEMSTRUCT *dis)
{
    HBRUSH  br;
    HGDIOBJ old;
    RECT    r = dis->rcItem;
    char    txt[64];

    br = CreateSolidBrush((dis->itemState & ODS_SELECTED)
                          ? RGB(80, 80, 80) : RGB(58, 58, 58));
    if (br) { FillRect(dis->hDC, &r, br); DeleteObject(br); }

    br = CreateSolidBrush((dis->itemState & (ODS_FOCUS | ODS_DEFAULT))
                          ? RGB(140, 140, 140) : RGB(95, 95, 95));
    if (br) { FrameRect(dis->hDC, &r, br); DeleteObject(br); }

    txt[0] = 0;
    GetWindowTextA(dis->hwndItem, txt, sizeof(txt));
    old = SelectObject(dis->hDC, MenuFont());
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, DARK_TEXT);
    DrawTextA(dis->hDC, txt, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dis->hDC, old);
}

static INT_PTR CALLBACK AboutProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static HBRUSH hbrBg;

    switch (msg) {
    case WM_INITDIALOG:
        SendMessageA(hDlg, WM_SETICON, ICON_BIG,
                     (LPARAM)LoadIconA(g_hInst, MAKEINTRESOURCEA(IDI_ENABLED)));
        hbrBg = NULL;
        if (g_bDark) {
            HWND ok = GetDlgItem(hDlg, IDOK);
            hbrBg = CreateSolidBrush(DARK_DLGBG);
            if (ok)
                SetWindowLongA(ok, GWL_STYLE,
                               GetWindowLongA(ok, GWL_STYLE) | BS_OWNERDRAW);
            DarkenCaption(hDlg, TRUE);
        }
        SetForegroundWindow(hDlg);
        return TRUE;

    case WM_DRAWITEM:
        if (g_bDark && wp == IDOK) {
            DrawDarkButton((DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        return FALSE;

    case WM_CTLCOLORDLG:
        if (g_bDark && hbrBg) return (INT_PTR)hbrBg;
        return FALSE;

    case WM_CTLCOLORBTN:
        if (g_bDark && hbrBg) {
            SetBkColor((HDC)wp, DARK_DLGBG);
            return (INT_PTR)hbrBg;
        }
        return FALSE;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == GetDlgItem(hDlg, IDC_ABOUT_LINK)) {
            SetTextColor((HDC)wp, g_bDark ? DARK_LINK : RGB(0, 0, 224));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (INT_PTR)(g_bDark && hbrBg ? hbrBg
                                              : GetSysColorBrush(COLOR_3DFACE));
        }
        if (g_bDark && hbrBg) {
            SetTextColor((HDC)wp, DARK_TEXT);
            SetBkMode((HDC)wp, TRANSPARENT);
            return (INT_PTR)hbrBg;
        }
        return FALSE;

    case WM_DESTROY:
        if (hbrBg) { DeleteObject(hbrBg); hbrBg = NULL; }
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
    case IDM_ENABLE:        ApplyOwnChange(TRUE);           break;
    case IDM_DISABLE:       ApplyOwnChange(FALSE);          break;
    case IDM_DISPLAY:       OpenDisplayProperties();        break;
    case IDM_AUTOSTART:     AutostartSet(!AutostartEnabled()); break;
    case IDM_ABOUT:         ShowAbout();                    break;
    case IDM_DARKMODE:      SetDarkMode(!g_bDark);          break;
    case IDM_GUARD:         SetGuard(!g_bGuard);            break;
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

    case WM_FIXMENU:
        g_fixPending = FALSE;
        if (g_bDark) FixMenuChrome(g_menuWnd);
        return 0;

    case WM_MEASUREITEM:
        OnMeasureItem((MEASUREITEMSTRUCT *)lp);
        return TRUE;

    case WM_DRAWITEM:
        OnDrawItem((DRAWITEMSTRUCT *)lp);
        return TRUE;

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
    LoadDarkMode();
    LoadGuard();
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

    g_bActive   = SaverIsActive();
    g_bIntended = g_bActive;   /* whatever it is at startup is what is wanted */
    g_guardLast = g_bActive;
    TrayAdd();          /* if this is dropped, the timer notices and retries */

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (g_hVersion) FreeLibrary(g_hVersion);
    ExitProcess((UINT)msg.wParam);
}
