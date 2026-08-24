# Bildschirmschoner-Steuerung / Screen Saver Control

A native Win32 tray applet that shows and controls the state of the default
Windows screen saver. 32-bit, no C runtime, no external dependencies —
runs on every 32-bit Windows from **Windows 95** through **Windows 11**.

`build\scrctl.exe` — **86,016 bytes**, of which ~62 KB is the two icons;
code + data is about 13 KB.

## Features

| Action | Behaviour |
|---|---|
| Tray icon | `enabled.ico` when the screen saver is on, `disabled.ico` when off. Polled every 2 s and on `WM_WININICHANGE`, so it also follows changes made in the Control Panel. |
| Double-click | Starts the current default screen saver (`<saver>.scr /s`). |
| Right-click | Opens the context menu from `Menu.rc`. |
| **Starten** | All installed screen savers, ascending; runs the chosen one with `/s`. |
| **Einstellungen** | Same list; runs the chosen one with `/c` (its settings dialog). |
| **Als Standard festlegen** | Same list, with a **checkmark on the current default**; writes `HKCU\Control Panel\Desktop\SCRNSAVE.EXE`. |
| **Ein-/Ausschalten** | `SPI_SETSCREENSAVEACTIVE` + `ScreenSaveActive`. The switch that would be a no-op is greyed out. |
| **Anzeigeeigenschaften** | `rundll32 shell32.dll,Control_RunDLL desk.cpl,,1` (the Screen Saver page on every version), with a `ShellExecute desk.cpl` fallback. |
| **Sprache / Language** | Switches German ⇄ English live — menu, About box and tooltip — and remembers the choice. |
| **Dunkles Design / Dark mode** | Dark context menu and About box; checkmarked while on. Defaults to the Windows setting, then remembers your choice. |
| **Mit Windows starten / Start with Windows** | Toggles a per-user `HKCU\…\CurrentVersion\Run` entry holding this exe's quoted full path; checkmarked while enabled. |
| **Über… / About…** | The dialog from `About.rc`, in the selected language. |

## Finding the screen savers

Scanned, in this order:

1. `%WINDIR%\Sysnative` — recorded as `%WINDIR%\System32` (see below)
2. `GetSystemDirectory()` — `\System` on 9x, `\System32` natively, `\SysWOW64` under WOW64
3. `%WINDIR%`
4. the directory of the currently configured saver, if it is somewhere else

De-duplicated by file name, so the first scan wins — which is why the native
directory is scanned first.

**The WOW64 trap.** On 64-bit Windows a 32-bit process is file-redirected:
everything it asks for in `...\System32\` is actually served out of
`SysWOW64`. So `GetSystemDirectory()` hands us the 32-bit savers and the
*native* `System32` — where Bubbles, Mystify, Ribbons and 3D Text live — stays
completely invisible. The `Sysnative` alias is the way out; it exists only for
32-bit processes under WOW64, so on 32-bit Windows and on 9x the directory
simply isn't there and that scan finds nothing. No version check needed.

`Sysnative` is meaningless to the 64-bit shell, so those entries are *recorded*
under their real `System32` name — that is what has to end up in
`SCRNSAVE.EXE` — and mapped back to `Sysnative` only when we open or launch
them (`FixupLaunchPath`). Without that mapping, a saver present only in the
native `System32` would be listed but refuse to start.

## Screen saver names

In the order Windows itself picks them:

1. **String resource 1** of the `.scr`. This is the `scrnsave.lib`
   `IDS_DESCRIPTION` convention, and the Display control panel has read it
   since Windows 95. It is the *right* answer and it is why the menu now says
   `Seifenblasen`, `Schleifen`, `Fotos`, `3D-Text`, `Leer`, `Field Lines`,
   `Solar Winds`.
2. **`FileDescription`** from the version info — only a fallback. For the
   built-in savers it is a sentence (`Bildschirmschoner "Seifenblasen"`) where
   string 1 is the actual name (`Seifenblasen`).
3. **File name** without its extension, when neither resource is present, or
   when the name found is longer than 52 characters — a file name beats an
   ellipsised paragraph. (`Blaze.scr` has no string 1 and a 68-character
   `FileDescription`, so it shows as `Blaze`.)

Resources are read with `LoadLibraryEx(..., LOAD_LIBRARY_AS_DATAFILE)`, which
maps the file without running a line of its code and reads resources out of a
**64-bit** `.scr` from this 32-bit process without complaint.

The whole list is rebuilt on every right-click, so newly installed savers show
up immediately and names are never stale. Measured cost of a full rebuild of 27
savers: ~15 ms. Lists longer than 28 entries wrap into a second column.

## Dark mode

Windows only grew a way to darken menus in Windows 10 1809, and even there it
is undocumented `uxtheme` ordinals — useless for a program that has to look
right back to Windows 95. Owner-drawn menu items behave identically the whole
way back, so that is what this uses.

Items are converted to `MF_OWNERDRAW` **only while dark mode is on**. In light
mode the menu is left exactly as the system draws it, so the native look is
never second-guessed and all the risk sits on the dark path.

Two traps worth knowing, both of which produced visible bugs here:

- **`GetMenuState` lies about popups.** For an item that opens a submenu it
  returns the submenu's *item count* in the high byte and only the flags in the
  low byte. `MF_SEPARATOR` is `0x800` — inside that high byte — so a popup with
  8 or more entries reads back as a separator and gets drawn as a bare line.
  Never trust high-byte flags on a popup; derive the state and rebuild the
  flags rather than masking the raw value.
- **`GetMenuStringA` truncates by character count, not buffer size.** Harmless
  on a normal single-byte codepage, but on a machine with Windows 10's *"Beta:
  Use Unicode UTF-8 for worldwide language support"* enabled `GetACP()` is
  65001 and every non-ASCII character costs one byte off the end —
  `Anzeigeeigenschaften öffnen` came back as `Anzeigeeigenschaften öffne`. The
  fix is `GetMenuStringW` where it exists (every NT) with the conversion done
  here, sized in bytes; Windows 9x has no `W` entry point but its ANSI codepage
  can never be UTF-8, so the `A` path is exact there.

### Repainting what Windows draws after us

Owner-drawing the items is only half of it. Windows paints the submenu arrows
and the popup's frame *after* `WM_DRAWITEM`, so anything drawn there for those
two gets stamped over — an arrow drawn in the item handler ends up underneath
the system's black one. Three pieces make it work:

- **The light edge is two separate things.** A 1px window border, and 2px of
  the menu's *own client background* that the item rects never cover — the
  background is what actually reads as a thick light frame. `SetMenuInfo` with
  `MIM_BACKGROUND | MIM_APPLYTOSUBMENUS` replaces it outright (Windows 2000+,
  bound late; 9x menus have no such padding). The 1px border is then painted
  over through a window DC.
- **The arrows are redrawn after the fact.** `WM_DRAWITEM` records each popup
  item's rect and posts `WM_FIXMENU` to ourselves. `TrackPopupMenu` runs a
  modal loop that still dispatches to our window, so it is handled the moment
  the paint finishes — no subclassing of the system menu window needed. Every
  item updates the tracked window, not just popups: a saver list contains no
  popups at all and still needs its border fixed.
- **`TPM_NOANIMATION` is mandatory.** Windows 10 fades popup menus in, and the
  fade composites over the post-paint — with the animation left on, both the
  border and the arrow work silently did nothing.

### Glyph geometry

The check and the chevron are drawn by hand, so their size and placement were
measured off a screenshot of the themed light menu and matched exactly rather
than eyeballed:

| | check | chevron |
|---|---|---|
| size | 10 x 7 px | 4 x 7 px |
| stroke | 2 px | 1 px |
| placement | 9 px in from the left | tip `ARROW_INSET` (10 px) in from the item's right edge |

Verify a change here by sampling pixels, not by looking: an ink bounding box
straight off a screen capture is the only way to tell a 1px drift from a
correct glyph. (The light theme anti-aliases and this does not, so ink pixel
*counts* differ slightly; the bounding boxes are identical.)

GDI leaves the final point of a `LineTo` undrawn, which is why the chevron's
last segment deliberately overshoots by one step.

The About box is done the ordinary way — `WM_CTLCOLORDLG` / `WM_CTLCOLORSTATIC`
for the background and text, plus `DWMWA_USE_IMMERSIVE_DARK_MODE` (bound late,
absent and harmless before Windows 10 2004) for the title bar. The OK button
has to be `BS_OWNERDRAW`, because a themed button ignores `WM_CTLCOLORBTN` for
its face and would stay light against a dark dialog.

Stored in `HKCU\Software\Rekow IT\ScreensaverControl\DarkMode` (`1`/`0`).
With nothing stored it follows Windows' own `AppsUseLightTheme`.

## Language handling

The two languages are **separate resource IDs**, not resource *locales*:
`IDR_MENU_DE`/`IDR_MENU_EN`, `IDD_ABOUT_DE`/`IDD_ABOUT_EN`, and one string
table with a German block at `IDS_BASE_DE` and an English one at `IDS_BASE_EN`.
Everything is tagged `LANG_NEUTRAL`.

This is deliberate: picking resources by locale relies on the loader's language
fallback, which differs between Win9x and NT. Selecting by ID is identical on
every version and is what makes the runtime switch work at all.

The choice lives in `HKCU\Software\Rekow IT\ScreensaverControl\Language`
(`de`/`en`). With nothing stored, a German UI language gets German, everything
else English.

## Run at logon

`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value
`ScreensaverControl` (the name is fixed, not translated). It is honoured by
every shell from Windows 95 on, needs no elevation, and is per-user like the
rest of the settings — a Startup-folder shortcut would have dragged in
`IShellLink`/COM for nothing.

The menu item is only checkmarked when the stored path actually resolves to
**this** executable, so an entry left behind by a copy that has since been
moved or replaced reads as off; switching it on then overwrites it with the
current path.

## Building

```bash
build.bat
```

(or `bash build.sh`). Requires MinGW-w64 `gcc`, `windres`, `dlltool` on `PATH`.

**The toolchain may be x86_64-only.** Nothing here links against a 32-bit CRT —
only against the import libraries in `lib32\`. Regenerate those with:

```bash
bash tools/genimportlibs.sh
```

which compiles a probe object, reads its undefined `__imp__Foo@N` symbols,
resolves each to its DLL via the toolchain's own 64-bit import libraries, and
emits `lib32\*.def` + `lib32\lib*.a` through `dlltool -m i386 -k`. Only needed
when the set of Win32 APIs used by `scrctl.c` changes.

## Tray icon robustness

Getting an icon into the notification area is not a one-shot operation, and
two separate mistakes here made the program run invisibly:

1. **Never call `Shell_NotifyIcon` from `WM_CREATE`.** The window is not fully
   created yet, and the shell may accept the icon and then quietly drop it.
   The icon is added once `CreateWindowEx` has returned.
2. **`NIM_ADD` returning TRUE is not proof the icon exists**, and a lost icon
   used to be permanent — the only retry path ran when the screen saver's
   on/off state changed, which might be never.

So `TrayEnsure()` runs on every 2 s timer tick and: re-adds the icon whenever
`g_bIconOk` is clear; otherwise probes with `NIM_MODIFY` on *every* tick until
one probe succeeds, and only then settles down to one check every 30 s. A
failed probe means the icon is gone, so it is added again. `TaskbarCreated`
still handles the ordinary Explorer-restart case immediately.

## Files

```
scrctl.c              the whole program
resource.h            shared IDs, version number, About-box URL
scrctl.rc             master resource script (the one handed to windres)
  Menu.rc             tray context menu, DE (132) + EN (133)
  About.rc            About box, DE (101) + EN (102)
  Strings.rc          runtime strings, both languages
  Version_Info.rc     VERSIONINFO, DE + EN string blocks
icons/                enabled.ico, disabled.ico (supplied, unmodified)
lib32/                generated 32-bit import libraries
tools/                genimportlibs.sh, depng_ico.py
```

## Windows 95 compatibility rules

Breaking any of these silently drops Win9x support:

- **ANSI (`*A`) entry points only** — Win9x has no Unicode USER/GDI.
- `NOTIFYICONDATA` is submitted with **`cbSize = 88`** (the V1 ANSI size).
  The modern size fails on 9x/NT4.
- **No API newer than shell32 4.0**: no `CheckMenuRadioItem` (hence
  `CheckMenuItem` for the default marker), no `NIM_SETVERSION`, no `shlwapi`,
  no `HWND_MESSAGE` (the window is a plain hidden `WS_POPUP` + `WS_EX_TOOLWINDOW`).
- Dialogs are plain **`DIALOG`, not `DIALOGEX`**, with font `MS Shell Dlg` —
  `DIALOGEX` templates and `Segoe UI` are not safe on 95.
- `version.dll` is bound with `LoadLibrary`, so its absence degrades to file
  names instead of failing.
- `LOAD_LIBRARY_AS_DATAFILE` (used to read screen saver names) dates back to
  Win95; the newer `LOAD_LIBRARY_AS_IMAGE_RESOURCE` must not be used.
- Dark mode is owner-drawn rather than themed, so it works on every version;
  see **Dark mode**. `SPI_GETKEYBOARDCUES` and `DT_HIDEPREFIX` are quietly
  ignored on 9x, which leaves the mnemonic underlines always visible — which
  is exactly 9x behaviour anyway.
- **The `.ico` files must contain classic BMP/DIB images only.** Icon editors
  routinely store images inside an `.ico` as PNG streams; Windows only learned
  to decode those in Vista, so on 95/98/ME/2000/XP such an entry is not an
  icon at all. After re-exporting an icon, always run:
  `python tools/depng_ico.py icons/*.ico` — it decodes any PNG entry and
  re-encodes it as a 32-bpp DIB with an alpha-derived AND mask, leaving the
  artwork unchanged. It is a no-op on files that are already clean.
- The tray icon is added **after** `CreateWindowEx` returns, never from
  `WM_CREATE`, and a timer re-adds it if it is ever missing. See below.
- Linked `--subsystem windows:4.0` with OS version 4.0 so the 95/NT4 PE loader
  accepts the image; section alignment stays 4096 (9x will not map 512).
- Built `-nostdlib`: no CRT, so no msvcrt/UCRT dependency to worry about.
  `memset`/`memcpy` are provided locally; everything else is `lstr*` from
  kernel32 and `wsprintfA` from user32.

## Notes

- **Background.** This is a clean-room reimplementation of *Stardust
  Bildschirmschoner-Steuerung 2003*, a ~1 MB tool that failed to start
  reliably on modern versions of Windows. Stardust Software is defunct and their
  site is gone. None of the original code was used — this binary is ~66 KB and
  shares nothing with it but the idea. Intended for release as open source.
- Shrinking the exe further is almost entirely a matter of the icons — they
  are ~62 KB of the 78 KB. Dropping unused sizes or colour depths from the two
  `.ico` files is where any remaining savings are. Note that DIB entries are
  bigger than the PNG ones an icon editor produces; that trade is mandatory,
  see below.
