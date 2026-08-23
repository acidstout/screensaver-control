# Bildschirmschoner-Steuerung / Screen Saver Control

A native Win32 tray applet that shows and controls the state of the default
Windows screen saver. 32-bit, no C runtime, no external dependencies —
runs on every 32-bit Windows from **Windows 95** through **Windows 11**.

`build\scrctl.exe` — **77,824 bytes**, of which ~62 KB is the two icons;
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
| **Mit Windows starten / Start with Windows** | Toggles a per-user `HKCU\…\CurrentVersion\Run` entry holding this exe's quoted full path; checkmarked while enabled. |
| **Über… / About…** | The dialog from `About.rc`, in the selected language. |

Screen savers are discovered by scanning `%WINDIR%\System32` (`\System` on 9x),
`%WINDIR%`, and the directory of the currently configured saver. The display
name is the `.scr`'s own **FileDescription**, falling back to the file name;
names longer than 52 characters are clamped so the menu stays narrow, and lists
longer than 28 entries wrap into a second column.

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
