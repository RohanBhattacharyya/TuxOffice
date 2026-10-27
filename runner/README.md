# Runner and graphics builds

`./runner/build.sh` rebuilds changed Wine modules and overlays them on the
pinned Soda runner. Build and compiler caches live under
`~/.cache/tuxoffice-build` and `~/.cache/tuxoffice-dxvk`, outside `wine/`.

## Wine source

`wine/` is part of this repository. Its history starts with the commit
`soda: Import Soda 11.0-33 experimental Wine source`: Valve's Proton Wine
(`ValveSoftware/wine` 0509a0b64a48) with the Office patch set of Bottles' Soda
11.0-33 experimental release (`bottlesdevs/wine` de388a6d05a2) and Wine's
generated sources. Each TuxOffice change to Wine is a commit after it, so
`git log -- wine` lists them and `git diff <soda commit> -- wine` shows them
all. The overlay build finds that commit by its subject, rebuilds the DLLs
and server changed since then, and keeps the Soda release's binaries for
everything else. `--full` configures and builds the whole tree instead.

## Graphics

When the server's shared-memory protocol changes, the overlay also rebuilds
ntdll and Soda's legacy 32-bit Unix ntdll client. This needs a working
`gcc -m32` toolchain and 32-bit libc development files. The 32-bit client cache
is `~/.cache/tuxoffice-build/build32`. A matching protocol version must be
installed on both clients and the server before restarting the prefix.

The build also bundles a matched, locally compiled DXVK family for both
architectures. `build-dxvk.sh` pins DXVK 3.1.1 to commit
`b1a1c99ab52b687cf950d62c88bc2fa316b41663` and verifies the downloaded
LLVM-MinGW 20261020 compiler archive. The patch in
`patches/dxvk-context-state-lock.patch` protects immediate-context state swaps.
`patches/dxvk-resource-import-lock.patch` serializes external image and buffer
imports with allocation and release. They share a resource-allocation pool;
importing a swapchain image while another thread creates drawing resources
previously corrupted that pool.
Wine also protects complete Direct2D draw transactions and enables protection
when sharing an application's device with composition.

Do not mix the official GCC-built DXVK libraries with these LLVM-built
libraries. The bundle installs `d3d11`, `dxgi` and `d3d10core` together.
Stripped runtime files and the DXVK license are packaged in the runner;
unstripped debug files remain in the build cache.

After building, `./tuxoffice graphics auto` selects the bundled DXVK only
when both the Wine fixes and complete DXVK bundle are present, including the
`resource-pool-lock-v1` marker from the newer allocator fix.
`./tuxoffice graphics builtin` selects Wine's built-in libraries instead.
Changes apply to newly started Office processes. `TUXOFFICE_GRAPHICS=builtin`
also provides a temporary override for one launch. Set
`TUXOFFICE_SKIP_DXVK_BUILD=1` to build a runner without the bundled backend.

Desktop regression coverage and measurement limits are recorded in
[`docs/STATUS.md`](../docs/STATUS.md). Stock DXVK 3.1.1 faulted during Word
resize tests. The public-API `tests/dxvk-resource-import.cpp` regression
reproduces the allocator fault with the earlier bundle on both architectures.
Three patched trials per architecture pass 480,000 imports and 480,000 ordinary
buffer allocations in total. Visible spelling-card resize checks also pass.

The runner bundles `office-ui.exe`; setup uses the Windows preference API to
enable window shadows after installing the updated composition and Wayland
modules. Its `fonts` command sets Segoe UI 9-point system menu and message
fonts after verifying that the real font is available. Both commands preserve
other preferences and run during setup/font installation. They avoid broadcasting
to Office's non-pumping helper threads.

Office's unowned frame-shadow strips are matched to the managed frame's
cached edges. Unmatched strips wait for a parent surface instead of becoming
independent desktop windows. This preserves their pixels and avoids focus
being taken by a thin shadow when a document is restored.

Child-window rectangles arrive in parent-client coordinates. The driver caches
their mapping to the real root before acquiring its window-data lock, then
converts them into the root's screen coordinate system for Wayland placement.
This covers nested children, window decorations, monitor origins and DPI,
including the misplaced ribbon shadow reported in Word.

`tests/scroll-benchmark.py` disables detailed rendering traces by default and
checks for caught graphics faults. Set `WINEDEBUG=-all,err+all,trace+uiperf`
explicitly for profiling; compare runs using the same diagnostic settings.

A shared window-order cache and batching of CommitNeeded notifications were
benchmarked and rejected for deployment. Their API-level speedup did not
produce a material scrolling improvement in the matched desktop tests without
detailed profiling. The installed build retains server protocol 932 and the
existing notification behavior.
