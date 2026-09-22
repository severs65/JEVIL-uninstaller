# TidyUninstaller

A lightweight, portable Windows uninstaller with a clean, classic desktop UI.
It lists installed desktop applications, Microsoft Store (Appx/UWP) packages and
portable apps, runs the original uninstaller, scans for leftover files, folders,
registry keys, services, scheduled tasks and autostart entries, and lets you
review every item before removal.

Single executable, no runtime required (built with PyInstaller), no background
service, no telemetry.

## Features

- **Installed apps inventory**
  - Classic Win32 desktop programs from the registry uninstall keys
    (64-bit and 32-bit views, machine and user hives)
  - Microsoft Store / Appx / UWP packages for the current user
  - Portable applications discovered in common locations
  - Real application icons extracted from executables and package assets
- **Normal uninstall**
  - Runs the program's original/uninstall string first
  - Then scans and presents leftovers for optional, item-by-item cleanup
- **Force removal**
  - For broken installations and programs without an uninstaller
  - Stops related processes, Windows services and kernel drivers registered
    by the same vendor, removes scheduled tasks and Run/RunOnce autostart entries
  - Takes ownership of protected files where the current user is allowed to
  - Files that are locked by the running system are scheduled for deletion on
    next reboot using the standard Windows `MoveFileEx` mechanism, with an
    optional automatic restart prompt
  - **Safe Mode cleanup**: when a security product's self-protection blocks
    removal in normal mode, the tool can deploy a one-time cleanup task, reboot
    into Windows Safe Mode (where third-party self-protection drivers are not
    loaded by design), perform the deletion under the SYSTEM account, restore
    normal boot and reboot again. The boot configuration is always restored,
    even if cleanup fails.
- **Leftover detection**
  - Install directories and sibling product folders from the same vendor
  - Program data and per-user application data folders
  - Uninstall and vendor registry keys, Run/RunOnce autostart values
  - Windows services and kernel drivers whose image path belongs to the product
  - Scheduled tasks
  - Browser extensions (Chromium-based browsers) installed by the software
  - Running processes are correlated by install path and by the executable's
    embedded company name, which also catches randomly named folders
- **Install monitor** (optional, from the Settings menu) records file and
  registry changes made by an installer and can restore them
- Built-in allowlists protect Windows system directories and core registry
  hives from accidental deletion

## Safety

TidyUninstaller never hides what it removes: every detected leftover is shown
in a checklist with its category, size and the action that will be taken.
Nothing is deleted without explicit confirmation. The tool requests an
administrator token because uninstallation inherently requires one.

It does **not** attempt to bypass operating-system or security-software
protection. Security products with kernel self-protection cannot (and should
not) be removed while running; the supported path is the vendor's own
uninstaller or the Safe Mode cleanup workflow described above.

## Download

See [Releases](../../releases) for the signed single-file executable.
Windows 10/11, 64-bit. Portable: just run it.

## Build from source

Requires Python 3.11.

```bat
pip install -r requirements.txt
pyinstaller --onefile --windowed --uac-admin --name TidyUninstaller --noconfirm mini_geek.py
```

The executable is written to `dist\TidyUninstaller.exe`.

## License

MIT, see [LICENSE](LICENSE).

## Code signing

Free code signing for open-source projects is provided by
[SignPath.io](https://signpath.io/), certificate by the
[SignPath Foundation](https://signpath.org/). Release artifacts are built and
signed by the public GitHub Actions workflow in this repository.
