<p align="center">
  <img src="docs/assets/tuxoffice-logo.png" alt="TuxOffice logo" width="180">
</p>

<h1 align="center">TuxOffice</h1>

<p align="center">
  <strong>Microsoft 365 desktop apps on Linux, running natively — no virtual machine, no remote desktop.</strong>
</p>

<p align="center">
  <img alt="Platform: Linux" src="https://img.shields.io/badge/platform-Linux-informational">
  <img alt="Session: Wayland" src="https://img.shields.io/badge/session-Wayland-blue">
  <img alt="Office: Microsoft 365 Apps x64" src="https://img.shields.io/badge/Office-Microsoft%20365%20Apps%20x64-orange">
  <img alt="Status: in development" src="https://img.shields.io/badge/status-in%20development-yellow">
  <img alt="License: LGPL-2.1-or-later" src="https://img.shields.io/badge/license-LGPL--2.1--or--later-green">
</p>

<p align="center">
  <img src="docs/assets/word.png" alt="Word for Microsoft 365 running on KDE Plasma (Wayland) with TuxOffice" width="900">
</p>

TuxOffice installs and runs the real Microsoft 365 Apps (Word, Excel,
PowerPoint, Outlook, OneNote, Access and Publisher) on a Linux desktop.
Office's x86-64 code runs directly on your CPU; its Windows, COM, Direct2D and
composition calls are answered by a Wine runner built from Valve's Proton Wine,
Bottles' [Soda](https://github.com/bottlesdevs/wine) Office patches and
TuxOffice's own fixes in [`wine/`](wine). Install it from your
distribution's package manager, let it download Office from Microsoft's
servers, and sign in with the Microsoft account that holds your
subscription.

## Highlights

- **Activation by signing in.** Office activates with your own Microsoft 365
  account through a browser sign-in. No KMS tools, no product-key tricks.
- **A Linux desktop citizen.** Open and Save As use your desktop's file
  chooser (Dolphin's dialog on Plasma) and save straight into your Linux
  folders. Text, HTML and images copy and paste with native apps. You can
  drag files from your file manager into Office. Office follows the desktop's
  light or dark theme, and its apps get desktop entries, icons and file
  associations.
- **Native Wayland.** Office uses Wine's Wayland driver, with Windows 11 caption
  buttons, frame shadows, crisp text at fractional scales, and modal dialogs
  that stay above their window.
- **GPU rendering.** Direct3D goes through a bundled, patched DXVK on Vulkan;
  Office's composition layers are drawn as Wayland subsurfaces and clipped to
  their windows.
- **Tested in a sandbox.** An automated suite drives the apps in a private,
  nested KWin session and checks documents, PDF export, charts, spelling,
  ribbon rendering, scrolling performance, frame shadows, dialogs and the
  file chooser.

## Status

TuxOffice is under active development and used daily on KDE Plasma (Wayland)
at 1.25× scaling. Tested with Office 16.0.20527 (Current Channel, x64):

| App | State |
| --- | --- |
| Word, Excel, PowerPoint | Open, edit, save, print to PDF, charts, slide shows, spelling, VBA macros |
| Outlook | Mail, compose, calendar in every view, contacts, tasks and folders on a local data file; a real account is untested |
| OneNote | Local notebooks, pages, search, PDF export |
| Access | Databases, tables, queries, forms, reports, VBA, ADO, export; XML export fails, and closing a database after an Excel import can rarely abort |
| Publisher | Create, edit, save, PDF export; shows "Unlicensed Product", likely because Microsoft retired it in October 2026 |

Known limitations, measurements and the full change history are in
[`docs/STATUS.md`](docs/STATUS.md). In short: dragging *out of* Office into Linux
apps is not implemented, sign-in with personal Microsoft accounts is untested, tray icons
are not shown, Office's embedded sign-in page has minor rendering glitches, and
there is no matched Windows baseline, so performance parity with Windows is not
claimed.

## Install

One command installs TuxOffice with your distribution's package manager and
then offers to install Microsoft 365 right away:

```sh
curl -fsSL https://raw.githubusercontent.com/RohanBhattacharyya/TuxOffice/main/install.sh | bash
```

The installer checks your system, picks the package for your distribution,
installs it with `apt`, `dnf`, `zypper` or `pacman` (or into your home folder
if you prefer, or on immutable systems), verifies every download against the
release checksums, and then asks what to do next:

```
What next?
  1) Set up TuxOffice and install Microsoft 365 now (downloads about 4 GB, 15-30 minutes)
  2) Set up TuxOffice now, install Microsoft 365 later
  3) Nothing for now
```

It never runs Office as root. `bash install.sh --help` lists the options for
unattended installs (`--system`, `--user`, `--office`, `--setup-only`,
`--yes`) and `--uninstall`.

### Packages

Every [release](https://github.com/RohanBhattacharyya/TuxOffice/releases)
provides native packages that declare the right dependencies for each
distribution family:

| Distribution | Package | Install |
| --- | --- | --- |
| Ubuntu 24.04+, Debian 13+, Linux Mint 22+, Pop!_OS 24.04+ | `.deb` | `sudo apt install ./tuxoffice_VERSION-1_amd64.deb` |
| Fedora 40+ | `.rpm` | `sudo dnf install ./tuxoffice-VERSION-1.x86_64.rpm` |
| openSUSE Tumbleweed | `.rpm` | `sudo zypper install --allow-unsigned-rpm ./tuxoffice-VERSION-1.x86_64.rpm` |
| Arch Linux, CachyOS, EndeavourOS, Manjaro | `.pkg.tar.zst`, AUR `tuxoffice-bin` | `sudo pacman -U ./tuxoffice-VERSION-1-x86_64.pkg.tar.zst` |
| Anything else, Fedora Atomic, Bazzite | `.tar.xz` | `bash install.sh --user` |

The packages install the launcher to `/usr/bin/tuxoffice` and a ready-built
runner to `/usr/lib/tuxoffice`; nothing is compiled on your machine. Then, as
your normal user:

```sh
tuxoffice setup      # Office environment, fonts and Windows components
tuxoffice install    # downloads and installs Microsoft 365 Apps (about 3.5 GB)
tuxoffice run word   # sign in when Office asks
tuxoffice fonts cloud   # after signing in: fetch Segoe UI, Aptos, Calibri, …
tuxoffice set-default   # optional: open Office files with TuxOffice
```

`install` also creates the desktop entries, so the apps appear in your
application menu. Check your environment at any time with `tuxoffice doctor`.

### Requirements

- A 64-bit x86 Linux distribution with glibc 2.38 or newer (the releases
  above or newer). Wayland is recommended and KDE Plasma is the tested
  desktop; X11 sessions use Wine's X11 driver and are less tested.
- A Vulkan-capable GPU and driver.
- Linux 6.14 or later for `ntsync` (optional; older kernels fall back to the
  wineserver).
- `xdg-desktop-portal` with a FileChooser backend, for the native file dialogs.
- A Microsoft 365 subscription that includes the desktop apps.

### From source

```sh
git clone https://github.com/RohanBhattacharyya/TuxOffice.git
cd TuxOffice
./tuxoffice setup && ./tuxoffice install
```

From a checkout, `setup` builds the runner from [`wine/`](wine) on top of the
pinned Soda release (an overlay build takes a few minutes). It needs `clang`,
`lld`, `bison`, `flex`, `meson`, `ninja`, `glslangValidator`, Wine's usual
development libraries (Wayland, xkbcommon, Vulkan, FreeType, Fontconfig,
GnuTLS, libsecret, …) and the `libsystemd` headers.
[`packaging/`](packaging) describes how the release packages are built.

## Commands

| Command | Purpose |
| --- | --- |
| `setup` | Download the base runner, build the TuxOffice runner and create the Office environment |
| `install [--config FILE]` | Install Office with Click-to-Run (default: [`config/m365-apps.xml`](config/m365-apps.xml)) |
| `install-status` | Show Click-to-Run progress |
| `run APP [FILES…]` | Launch `word`, `excel`, `powerpoint`, `outlook`, `onenote`, `access` or `publisher` |
| `integrate` / `unintegrate` | Add or remove desktop entries and icons |
| `set-default` | Make TuxOffice the default handler for Office documents |
| `fonts status\|office\|cloud\|import PATH\|segoe` | Inspect and install the fonts Office expects |
| `graphics auto\|builtin\|dxvk` | Choose the Direct3D backend for Office |
| `ui` | Re-apply Windows UI fonts, colours and window effects |
| `repair` | Re-register Office's chart proxies, Windows Installer components and virtual registry |
| `doctor` | Check the runner, environment, kernel, Vulkan and installed apps |
| `cmd [SCRIPT [ARGS…]]` | Open a Windows command prompt in the Office environment, or run a `.bat`/`.cmd` script in it |
| `wine ARGS…` | Run a command in the Office environment (`winecfg`, `regedit`, …) |
| `kill` | Stop every process in the Office environment |

### Configuration

| Variable | Effect |
| --- | --- |
| `TUXOFFICE_HOME` | Data directory (default `~/.local/share/tuxoffice`) |
| `TUXOFFICE_PREFIX` | Office environment (default `$TUXOFFICE_HOME/prefixes/office`) |
| `TUXOFFICE_RUNNER` | Use a specific runner directory |
| `TUXOFFICE_GRAPHICS=builtin` | Use Wine's built-in Direct3D for one launch |
| `TUXOFFICE_FILE_DIALOG=wine` | Use Wine's own Open/Save dialogs instead of the desktop's |
| `TUXOFFICE_THEME=light\|dark` | Override the desktop's colour scheme for Office |
| `TUXOFFICE_SCALE` | Override the detected display scale |
| `TUXOFFICE_X11=1` | Run through XWayland instead of the native Wayland driver |

## How it works

```
tuxoffice (launcher)
  ├─ runner      Soda base runner + modules rebuilt from wine/ (runner/build.sh)
  │                └─ patched DXVK bundle (runner/build-dxvk.sh, runner/patches/)
  ├─ prefix      Office's Windows environment: fonts, Windows components,
  │              Click-to-Run service, chart proxies, Installer components
  ├─ install     Office's payload is fetched natively from Microsoft's CDN
  │              (lib/c2rfetch.py), then installed offline by Click-to-Run
  └─ desktop     desktop entries, MIME types, theme and scale detection,
                 tuxoffice-file-dialog (xdg-desktop-portal file chooser)
```

[`wine/`](wine) holds the complete Wine source. The commit whose subject
starts with `soda` imports the Soda 11.0-33 base (Valve's Proton Wine with
Bottles' Office patch set). Every TuxOffice change to Wine is a separate commit
after it. `runner/build.sh` rebuilds only the modules changed since that base
and lays them over the pinned Soda release, so a rebuild takes minutes, not
hours.

## Repository layout

| Path | Contents |
| --- | --- |
| [`tuxoffice`](tuxoffice) | The launcher: setup, install, run, fonts, integration |
| [`wine/`](wine) | Wine source: Soda base plus TuxOffice's Wine commits |
| [`runner/`](runner) | Runner and DXVK build scripts and DXVK patches |
| [`packaging/`](packaging), [`install.sh`](install.sh) | Release packages and the one-line installer |
| [`lib/`](lib) | Helpers: CDN fetcher, file chooser, font tools, registry repair |
| [`config/`](config) | Click-to-Run configuration for Microsoft 365 Apps |
| [`tests/`](tests) | Automated tests, probes and the isolated test session |
| [`bench/`](bench) | Recorded measurements |
| [`docs/`](docs) | Status and change history, research notes |

## Development and testing

Build the runner after changing anything in `wine/` or `lib/`:

```sh
runner/build.sh                                  # overlay build into ~/.local/share/tuxoffice
TUXOFFICE_HOME=/tmp/candidate runner/build.sh    # or into a separate home to test first
```

GUI tests run in a private nested KWin session, so they never take over your
desktop. The session works on a copy of your Office environment (a reflink
copy, so it is quick and small) and builds the small Windows test programs:

```sh
tests/isolated-session.sh SESSION &              # start the private session
python3 tests/isolated-run.py SESSION python3 tests/office-suite.py RESULTS
```

Each test script documents its arguments at the top; see
[`docs/STATUS.md`](docs/STATUS.md) for the current results and how they were
measured.

## Troubleshooting

- **An app does not start:** run `./tuxoffice doctor`, then `./tuxoffice kill`
  and try again. Office asks to start in safe mode after a crash; answering
  *No* is usually fine.
- **Graphics problems:** try `TUXOFFICE_GRAPHICS=builtin ./tuxoffice run word`,
  or switch permanently with `./tuxoffice graphics builtin`.
- **Open/Save dialogs:** `TUXOFFICE_FILE_DIALOG=wine` falls back to Wine's dialog.
- **Fonts look wrong:** `./tuxoffice fonts status`, then `./tuxoffice fonts
  cloud` after signing in.

## Credits

TuxOffice stands on the work of [Wine](https://www.winehq.org/),
[Valve's Proton Wine](https://github.com/ValveSoftware/wine),
[Bottles' Soda](https://github.com/bottlesdevs/wine) and
[DXVK](https://github.com/doitsujin/dxvk).

The logo is built on Tux, the classic Linux penguin, by Larry Ewing
(lewing@isc.tamu.edu), created with [The GIMP](https://www.gimp.org/).

## License and trademarks

TuxOffice is licensed under the GNU Lesser General Public License, version
2.1 or (at your option) any later version; see [`LICENSE`](LICENSE). This
covers the launcher, helpers, build scripts and tests as well as the Wine
source in [`wine/`](wine), which carries Wine's own copyright notices (see
[`wine/COPYING.LIB`](wine/COPYING.LIB) and [`wine/AUTHORS`](wine/AUTHORS)).
Source files are marked with `SPDX-License-Identifier: LGPL-2.1-or-later`.

TuxOffice is not affiliated with or endorsed by Microsoft. Microsoft, Microsoft
365, Word, Excel, PowerPoint, Outlook, OneNote, Access and Publisher are
trademarks of the Microsoft group of companies. TuxOffice does not include or
distribute any Microsoft software: Office and its fonts are downloaded from
Microsoft, and using them requires your own licence.
