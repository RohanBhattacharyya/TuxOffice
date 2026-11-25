# Running Microsoft Office natively on Linux: research notes

Goal: run current Microsoft 365 Apps (Word, Excel, PowerPoint, Outlook, ...)
on Linux at native speed and integrated into the desktop, replacing the
"Windows VM + xfreerdp3 RemoteApp" setup.

Test machine: CachyOS, Linux 7.3, i7 Raptor Lake + Intel UHD / RTX 4070
Mobile, Wayland session, 32 GB RAM. Office build 16.0.20527.20088
(Current channel, 2610), x64.

## 1. Where the overhead actually comes from

Office is x86-64 Windows code and the host CPU is x86-64. There is **no
instruction-set gap**, so running Office natively is entirely a question of
providing the Windows *API and ABI* (NT syscalls, Win32, COM, Direct2D/
DirectWrite, DirectComposition, WinRT, licensing and identity services),
not of translating machine code.

| Cost in the VM + RDP setup | Native replacement |
|---|---|
| A second kernel, 4-8 GB pinned RAM, virtual disk I/O | Office runs as ordinary Linux processes; files are read straight from ext4/btrfs |
| Each frame rendered in the guest, encoded (RemoteFX/H.264), sent, decoded, composited | Office's Direct2D draws through D3D11 → Vulkan straight on the host GPU into a Wayland surface |
| RemoteApp windows are fake (no real Wayland roles, broken DnD/IME/HiDPI) | Real Wayland toplevels/popups via winewayland.drv |
| Clipboard and file sharing go over RDP channels / SMB | Same process space for clipboard; `Z:` drive is the Linux filesystem |

## 2. Approaches evaluated

### 2.1 Static recompilation (PE → ELF)
Rejected. Recompilation helps when the *ISA* differs (e.g. x86 → ARM), but
here the ISA is identical, so a recompiler would emit roughly the same
instructions Office already contains. It would remove none of the real work,
which is implementing ~15,000 Windows API exports that Office imports across
hundreds of DLLs. A recompiled binary would still need Wine (or an
equivalent) underneath. On top of that, Office:

* ships ~1,500 PE modules (3.4 GB) that update every month, so a recompile
  would have to be redone for every build;
* has signature/integrity checks on its own modules (C2R, licensing);
* loads code at runtime (VBA p-code engine, COM add-ins, WebView2/V8 JIT for
  Office.js add-ins), which a static recompiler cannot see.

Recompilation/translation only matters on **ARM64 Linux**, where FEX-Emu
plus Wine's ARM64EC can run x64 Office. Soda already ships an aarch64 build
for that case.

### 2.2 Binary translation layers (Box64/FEX)
Not needed on x86-64, for the reason above. Wine already runs Office's code
directly on the CPU; the only "translation" is at API boundaries.

### 2.3 Running the macOS or Android builds
* macOS Office through Darling: Darling cannot run AppKit GUI apps
  reliably, and Mac Office is a different, less capable product.
* Android Office through Waydroid: deprecated by Microsoft in favour of the
  "Microsoft 365" mobile app, with phone-class features only.
Both rejected.

### 2.4 Office on the web / PWA
Native-looking windows, but not native: no VBA, no COM add-ins, missing
features, browser rendering. Not a replacement.

### 2.5 Wine / Proton (API reimplementation): **chosen**
Office's own code runs at full native speed; Win32 calls are served by
Wine's DLLs on top of Linux. This is the only approach that is both native
and keeps the real, current Office binaries. Upstream Wine historically
handles Office 2016 (32-bit MSI) but **not** modern Click-to-Run Microsoft
365 x64. That needs:

* Click-to-Run's service, AppV ISV virtual-registry/FS subsystem and package
  locker semantics;
* WinRT/DirectComposition (`Windows.UI.Composition`, `dcomp`), and a much
  more complete Direct2D (layers, effects, command lists, shadows);
* Web Account Manager / `Windows.Security.Authentication.OnlineId` for
  sign-in, WebView2 for the sign-in and Office.js surfaces;
* MSI/C2R product-code bridging, a number of kernel32/kernelbase APIs, and
  UI Automation pieces.

#### Which Wine base
| Base | Office 365 x64 status (Oct 2026) |
|---|---|
| Upstream Wine 11.20 (installed system wine) | Not tested here; lacks the WinRT composition, WAM and C2R work below |
| CrossOver 26 | Office 2016 supported; Office 2021/2024 "Will Not Install"; M365 32-bit only |
| Valve Proton Wine (bleeding edge) | Same as upstream for Office; adds gaming-oriented fixes |
| **Soda 11.0-33-experimental** (Bottles) | **Valve Proton Wine + ~100 Office patches**: Direct2D Office rendering, DirectComposition/WinRT composition, WAM sign-in bridged to the system browser + libsecret, C2R/MSI fixes, Wayland surface fixes, THP and launch-profile tuning |

"Fork Proton" is effectively what Soda already does: it rebases on Valve's
Wine experimental branch and carries the Office work as `wine-tkg-userpatches`.
TuxOffice therefore uses Soda as its runner instead of maintaining a third
fork. The patch set is plain `.mypatch` files, so carrying local patches on
top (`runner/` in this repo) costs little.

## 3. Findings from experiments

### 3.1 Installation
* Office's online bootstrapper (`OfficeSetup.exe`, the "Firefly" C2R
  bootstrapper, generic product) and ODT `setup.exe` both work under Soda.
* **Click-to-Run's built-in downloader is the bottleneck under Wine:**
  ~100 KiB/s inside Wine vs **28 MB/s** for curl against the same CDN on the
  same connection (≈280× slower). It also spent ~20 min in an
  `Experimentation.EcsFetch` timeout before downloading anything. TuxOffice
  avoids this by downloading the CDN payload natively
  (`lib/c2rfetch.py`, 3.4 GB in 2 min 19 s) and pointing ODT at it with
  `SourcePath`. Install then runs entirely offline.
* Catalog signature validation (`CabManager::ValidateCatalogFile`, error
  188) fails under Wine but is non-fatal.
* The C2R service (`ClickToRunSvc`) must be registered before setup, and
  `AppvIsvSubsystems64.dll` / `C2R64.dll` must be copied next to the apps.

### 3.2 Activation and licensing
Office activates through subscription ("vNext") licensing: after sign-in,
Office fetches a licence from Microsoft's licensing service (OLS). Soda
implements the Web Account Manager and OnlineId APIs, bridged to the system
browser (device code) with tokens in the desktop keyring (Secret Service).
TuxOffice sets `LicensingNext\<SKU>=2` for subscription SKUs.

**Bug found and fixed (runner patch 0002).** With a work/school account,
sign-in "succeeded" but Office stayed unactivated. Tracing (patch 0001)
showed why: the sign-in prompt stores tokens under the `common` authority,
Office lists accounts under `organizations` and requests tokens at
`https://login.microsoftonline.com/<tenant>`, and Soda's keyring lookup
matched the authority string exactly. Every silent token request failed with
`STATUS_OBJECT_NAME_NOT_FOUND`. AAD refresh tokens redeem at all three
authorities, so the patch falls back to the generic records (tenant-checked,
never consumer accounts for `organizations`) and persists refreshed tokens
under the requested authority. Result: `LicenseFetcher::FetchModelFromOls -
Successfully retrieved model from OLS`, licence stored, Office activated.
Remaining gap: Soda rejects Microsoft's 16-hex-digit consumer client id, so
personal-account (MSA) token requests still fail.

### 3.3 Fonts
Office's UI expects Segoe UI (and Segoe Fluent Icons/MDL2 Assets on
Windows 11), and documents expect Calibri, Cambria, Consolas and Aptos. None
are in a Wine prefix, and they cannot be redistributed. Findings:

* Click-to-Run copies its 207 bundled fonts into `C:\Windows\Fonts` and
  registers them; those were fine.
* Office's **cloud font service** works under Wine. Office downloads a font
  when a document uses it, but only loads it into its own process. TuxOffice
  (`fonts cloud`) opens a generated document that references the needed
  families in an invisible Word over COM, waits for the cache to settle
  (~30 s), then copies and registers the 53 cached fonts prefix-wide: full
  Segoe UI family, Segoe UI Symbol/Emoji, Calibri, Cambria, Consolas,
  Candara, Corbel and all Aptos families. No redistribution: Office fetched
  them under the user's licence.
* Microsoft's public downloads for Segoe UI Variable, Segoe Fluent Icons and
  MDL2 Assets carry an EULA limited to "design, develop and test your
  programs"; `fonts segoe` shows it and requires explicit acceptance.
* `fonts import` copies fonts from a Windows install the user owns.

### 3.4 Rendering (open problem)
Display scale is now matched (Office renders at physical resolution; DPI
120 at 1.25x), text antialiasing is on, D3D11/DXGI go through DXVK pinned to
the GPU that drives the display (on this laptop Office had been rendering on
the RTX 4070 and copying every frame across PRIME to the Intel iGPU).

What remains is in Soda's **Windows.UI.Composition / DirectComposition**
implementation, which current Office requires (removing the WinRT
compositor registration makes Word hang at startup; disabling `dcomp.dll`
deadlocks the loader because Wine's `windows.ui.dll` links it):

* **Native Wayland:** whole-window flicker to white. Traced to the Wayland
  driver's overlay compositing: Office's opaque GDI host windows sit above
  the DirectComposition surface of the document; the driver mirrors their
  GDI pixels as overlay subsurfaces, and Office's repeated z-order-only
  `SetWindowPos` calls make those overlays get destroyed and recreated
  (80x in 25 s for one window), each time showing an empty buffer. A
  transient wrong z-order is also committed on every child reconfigure.
* **XWayland:** no window flicker, but popups (spelling suggestions) render
  white, the caret is invisible, the UI is slow, and Office's hang watchdog
  eventually raises `0xe0000002` on the UI thread.

Fixing these is compositor work inside the runner (patch series to come);
the Wayland overlay lifetime/stacking fix is the first target.

## 4. Performance design
* **ntsync** (`/dev/ntsync`, Linux 6.14+): NT sync primitives in the kernel
  instead of round trips to wineserver. Detected and used automatically.
* **Native Wayland driver**: no XWayland hop; correct fractional scaling.
  `TUXOFFICE_X11=1` falls back to XWayland.
* **Direct2D → D3D11 → Vulkan**: Soda's d2d1 with wined3d's Vulkan
  backend; Office's GPU-accelerated canvas runs on the host GPU.
* **No CPU translation**: Excel recalculation, VBA, and file parsing run
  as native x86-64 code.
* Cold start: Soda records the modules each launch maps
  (`SODA_ADAPTIVE_TRACE_DIR`); TuxOffice replays that list as page-cache
  readahead on the next launch (`lib/readahead.py`). Not yet benchmarked.
* Soda also offers `SODA_USE_THP=1` (transparent huge pages for large
  allocations); not enabled until measured.
