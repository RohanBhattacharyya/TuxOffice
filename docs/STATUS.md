# TuxOffice status and change history

Office build tested: 16.0.20527.20088, x64. Wine source is in `wine/` (see
[`runner/README.md`](../runner/README.md)); `runner/build.sh` builds the
overlay runner. This log runs from 25 October 2026; newer entries follow the
summary below, newest first.

The rebuilt runner and bundled DXVK are installed and selected in the regular
Office prefix. Prefix registration completed, all six installed graphics DLLs
match the bundle, and the existing default printer was preserved.

The regular prefix passes the Word suggestion-card and resize checks with
window effects enabled. Chart proxy registration is repaired and included in
setup. Expanded tests pass 36 assertions across Word, Excel and PowerPoint,
plus a full Excel chart-editor UI round trip.

The shared window-order cache and CommitNeeded batching experiment was rejected:
its API benchmark speedup did not improve matched desktop scrolling materially.
Source and installed builds retain server protocol 932 and the existing event
behavior. Quiet measurements are in `bench/window-order-2026-10-25.json`.

The 09:33 upgrade adds a separately reproduced DXVK allocation-pool race fix,
correct child-surface coordinates, deferred orphan frame shadows, and Segoe UI
9-point system menu metrics. A newer GPU-child attachment change eliminates
measured ribbon flashes in the test runner. It is **not installed yet**: longer
repeated-tab tests also exposed an intermittent native Office background-layout
crash in `PrefetchAxis::Add`, and cross-app stress testing is in progress.

## Changes in this session

- Fixed the left-click spelling card fail-fast. The failing Office routine
  was `AirSpace::BackEnd::LayerWinComp::ApplyRoundedRectangleGeometryClip`.
  Wine now implements rounded rectangle geometry, composition paths and
  geometric clips, including their Direct2D masks. The speech COM activation
  error immediately before the crash was a separate issue.
- Restored the card's SVG icons and ellipsis menus. Office's
  `NetUI::React::SvgViewRenderer::GraphicCallback` serialized SVG into a shell
  memory stream, then abandoned rendering because `IStream::Commit` returned
  `E_NOTIMPL`. Memory streams now commit successfully; writes already update
  their backing buffer. Other stream implementations retain their behavior.
- Implemented WinRT DropShadow properties, sprite shadow attachment, masks,
  source policy and rendering. This fixes a second fail-fast in
  `AirSpace::BackEnd::LayerWinComp::InitializeDropShadow`, exposed by regular
  prefix validation. Five screen-pixel cases cover visible shadows, transparent
  masks, inherited alpha and a mask drawn on an application-owned device.
- Restored Office's border-effect surfaces, which Soda explicitly discarded.
  Child surfaces now use their actual root window rather than guessing it
  from a nearby screen point; a ribbon shadow no longer becomes a standalone
  desktop window. Setup enables the Windows drop-shadow preference through a
  bundled helper, preserving the other preference bits.
- Fixed a captured Wayland deadlock: window-region updates held Wine's user
  lock while waiting for a surface lock, while a surface flush and the
  Wayland driver waited on the opposite lock chain. Surface updates now
  retain a reference and run after releasing the window lock, including the
  outer caller's reference.
- Released the Direct2D render-target binding before saving its private D3D
  context state. Resize tests no longer report a retained swapchain backbuffer.
- Replaced stock DXVK installation with a locally compiled, matched bundle
  gated on the required Wine fixes. Setup falls back to built-in D3D when
  that bundle is unavailable. Setup also preserves real Segoe UI fonts and
  runs prefix registration after building the runner.
- Fixed Excel's Recommended Charts → Scatter fail-fast. Office expects
  `ID2D1DrawTransform::SetDrawInfo` while its node is added to the transform
  graph during effect initialization. Wine called it after initialization.
  The graph now initializes each node's rendering information immediately.
- Fixed a Vulkan present crash after a swapchain's window was destroyed:
  failed recreation no longer reuses freed semaphore arrays or a stale handle.
- Fixed signed cross-process DPI coordinate scaling in wineserver. Negative
  positions previously overflowed to coordinates around 35 million.
- Visual property setters now request composition commits. Missing requests
  left visual changes waiting for unrelated redraws. Matched desktop tests
  show a repeatable reduction in scrolling stalls after the initial update.
- Removed an unused full-window composition bitmap copy and skipped rendering
  targets whose parent window is invisible. These separate optimizations did
  not eliminate the measured scrolling stalls on their own.
- Fixed PowerPoint's shutdown assertion. When an object disconnected itself
  during a COM call, Wine released the final stub references on an RPC worker.
  Those releases now happen in the server apartment before completing the call.
  PowerPoint enforces main-thread ownership for those interface releases.
- GPU selection now runs before any Wine client starts background services,
  so COM-launched and embedded Office applications inherit the display GPU.
  A default, unforced COM Excel launch now selects Intel, and its independent
  calculation/readback checks pass.
- Protected complete Direct2D draw transactions on shared D3D devices and
  enabled multithread protection when composition shares an application's
  rendering device. The bundled DXVK also locks context-state swaps.
- Added a prefix-local PostScript layout fallback for installations with no
  default printer. It uses Wine's generic PPD and preserves an existing default.
  The runner bundles the helper; setup provisions it without host CUPS changes.
- Registered Office's chart proxy interfaces from its installed CHART.DLL
  NDR metadata. This fixes PowerPoint chart insertion and embedded worksheet
  access. The extractor validates the proxy tables without fixed binary
  offsets or executing Office's registration code. Setup and post-install
  apply the repair; it is also installed in the regular prefix.
- Removed tracked `wine/autom4te.cache` output and ignored that directory.

## 4 November, night: placeholders replaced

The DLLs added for Office's imports on 3 November answered "nothing here". They
now do the real work where Office uses them:

- **Extended Linguistic Services** (`elscore`). Office's RichEdit and Outlook's
  MAPIPH ask for one service by GUID, Microsoft Script Detection
  (`{2D64B439-6CAF-4F6B-B688-E5D0F4FAA7D7}`). It is now installed: the text is split
  into runs of one Unicode script, named by ISO 15924 code. Common and inherited
  characters join the run before them, or the first run when they lead. Surrogate
  pairs count as one character. Service enumeration filters by GUID, category and
  content type. Recognition runs synchronously or, given a callback, on the thread
  pool. The property bag frees what the service allocated. `MappingDoAction` now
  has the documented signature (bag, range index, action id). The spec had it wrong.
  The public `elscore.h` and `elssrvc.h` headers are added.
- **One Unicode script table.** `tools/make_unicode_scripts` generates
  `include/wine/unicode_scripts.h` from Unicode 17 `Scripts.txt` and
  `PropertyValueAliases.txt`. `GetStringScripts` used a hand-written table of about
  60 ranges. It now uses the generated table, so every script is covered, and it
  reads surrogate pairs. `winnls.h` declares it and `VerifyScripts`.
- **`SrpIsTokenService`** reads the token. It reports a service for LocalSystem,
  LocalService and NetworkService, or for a token holding the enabled SERVICE group.
  Office uses only the enterprise ID and policy queries. They report no enterprise
  and no policy, which is the true state of an unmanaged machine.
- **`WscRegisterForChanges`** returns a real registration that
  `WscUnRegisterChanges` removes exactly once. Health stays "not monitored",
  because there is no Security Center service.
- **`MFCreateSensorActivityMonitor`** (PowerPoint) returns a monitor that holds its
  callback and starts and stops. The `IMFSensor*Activit*` interfaces are added to
  `mfidl.idl`, with IIDs checked against PowerPoint and windows-rs. Wine's Media
  Foundation offers no video capture devices, so no sensor activity is reported
  (see the camera item under open items).
- Tests: `tests/els-script-detection.c` (23 checks) and
  `tests/security-sensor-apis.c` (14 checks) pass.

## 4 November, later: PowerPoint exports video

- **File > Export > Create a Video failed every time** (status "failed" after a few
  seconds, no file). PowerPoint renders frames as ARGB32 Direct3D surfaces and hands
  them to Media Foundation's sink writer with an H.264 MP4 as the target. Wine's sink
  writer could not convert or encode (`SetInputMediaType` was `E_NOTIMPL`) and could
  not finalize a file (`Finalize` was `E_NOTIMPL`). It now puts a video processor, an
  encoder or both in front of the sink, found through `MFTEnumEx`, feeds samples through
  them, drains them on `Finalize`, waits for the sink to take everything and finalizes
  it, asynchronously when the application gave a callback. Getting PowerPoint's video
  out also needed fixes in winegstreamer's MP4 sink and muxer:
  - The sink took no presentation clock (`SetPresentationClock` was a stub), so it never
    started, and it never asked for samples (`MEStreamSinkRequestSample`).
  - Two reference leaks, one in the sink writer's `AddStream` and one in the sink's
    `BeginFinalize`, kept the MP4 file open after finalizing, so nothing could read it.
  - Samples reached the muxer with no decode time, so MP4 timing came out as zero.
    PowerPoint also gives some frames the same time as the one before; GStreamer's
    parser then drops their time and the muxer stops. Times are now made to increase.
  - The encoder's input type kept winegstreamer's default 1920×1080 frame size while
    PowerPoint renders at 852×480; it now takes the frame format of what it encodes to,
    and NV12 is preferred, since I420 at widths like 852 has a different plane layout in
    Media Foundation and GStreamer (a winegstreamer bug, not fixed here).
  - A missing timestamp on a drained encoder sample is filled in.
  A 3-slide deck now exports as an 852×480 H.264 MP4 of 3.08 s with 77 frames, and its
  frames show the slides. `tests/mf-sink-writer-encode.c` writes and reads back an MP4
  through the sink writer, with and without a callback; `tests/functional/media.vbs`
  exports a deck, inserts the video and saves (10 of 10).
- **Reading video crashed decoding on machines with GStreamer's NVIDIA plugins:**
  `nvh264dec` gave frames in CUDA memory, which Wine's parser accepted and its videoflip
  then refused. The parser now asks for system memory. Inserted videos now get their
  length and poster frame.
- **Open: saving as a Single File Web Page (MHT) is refused** in Word and Outlook ("You
  need to have Microsoft Outlook Express or Windows Mail installed"). Office looks up
  the MIME class `{FD853CED-7F86-11d0-8252-00C04FD85AB4}`, which Wine's `inetcomm` does
  not provide or register, and gives up before loading it. Saving as HTML, MSG, text,
  iCalendar and vCard works.
- Also checked by hand, without faults: Outlook recurring appointments, flags, a filtered
  `Restrict`, `AdvancedSearch`, a second data file, and saving items as MSG, HTML, text,
  iCalendar and vCard. Rules report "This store does not support rules", as they do on
  Windows for a data file with no account. Word footnotes, a table of contents, tracked
  changes, comparing documents, equations, SmartArt, embedded charts, mail merge from an
  Excel file and XPS export; Excel tables, pivot tables with a slicer, conditional
  formats, sparklines, goal seek, dynamic array and LAMBDA formulas, the newer chart types
  and chart image export; PowerPoint SmartArt, Morph, sections, slide image export and
  reusing slides from another file.
- **Excel's Power Query (Data > Get Data) did not start.** Its engine is .NET, and on
  wine-mono `PluggableProtocolManager.RegisterProtocol` failed while the add-in started,
  so every query failed. Setup now installs Microsoft's .NET Framework 4.8 (winetricks
  `dotnet48`, about 120 MB), and existing installs get it once at their next start; a
  failed download only warns. The .NET installers copy Windows 7's `uiautomationcore`,
  `dxva2` and `evr` over Wine's, and the old `uiautomationcore` has no class factory, which
  broke UI Automation; setup keeps Wine's three. `tests/functional/powerquery.vbs` imports
  a CSV, filters and types it, loads it into a table and saves (8 of 8).
- **Playing a video in a slide show ended PowerPoint.** It asks the media engine for
  its timed text (captions) service, `MF_MEDIA_ENGINE_TIMEDTEXT`, and stops itself with
  an assertion (tag `0x1d7815d5`) when there is none. The media engine now implements
  timed text (`dlls/mfmediaengine/timedtext.c`, interfaces added to `mfmediaengine.idl`
  with the Windows SDK's layout): tracks from WebVTT (cue settings, REGION blocks,
  markup and entities), SRT (with `<font color>`) and TTML (styles, regions, nested
  timing, spans and line breaks, read with xmllite), from byte streams or URLs, or built
  by the application with text and data cues; regions, styles and per-run formatting;
  and notifications, delivered in order on a worker, for tracks added, loaded, selected
  and removed, errors, and cues becoming active and inactive as the playback position,
  shifted by the cue time offset, enters and leaves them. `tests/mf-timed-text.c` covers
  all of it, with cues firing within one 40 ms poll of their times during playback.
  Videos play in slide shows (`media.vbs`, 13 of 13).
- **Open: PowerPoint shows no captions button for a video with a caption track.** A deck
  whose video carries a WebVTT track (`p173:tracksInfo`, MS-PPTX 2.13) opens and keeps it
  on saving, and in a slide show PowerPoint asks the engine for timed text, its in-band
  text tracks and notifications, but never hands it the caption file, and the video's
  control bar has no CC button. PowerPoint parses WebVTT itself; its strings name a
  feature gate, `ReconcileOOBCaptionTracks`, around out-of-band caption tracks. Not
  resolved.

## 4 November: Word could not open PDFs, and the Excel chart crash explained

Tested in the isolated session; Outlook, OneNote and the format test without a network.

- **Word failed to open any PDF** ("The last time you opened it, it caused a serious
  error"). Word converts a PDF through Office's `IConverter`, a custom interface
  marshalled from Office's type library, which is a 32-bit one (`SYS_WIN32`). Since
  `1c89057` (2 November) oleaut32 reports such a library's vtable size in this
  process's pointer size, but rpcrt4 still divided it by the library's 4 bytes, so
  every proxy and stub it built from a 32-bit library had twice the slots. Each
  method became a forwarder to a base proxy that does not exist, and the first call
  read through NULL. This affected every early-bound call into Office across
  processes or apartments; late-bound `IDispatch` calls, which all the other tests
  make, go through a different proxy. rpcrt4 now takes the slot size from the
  interface's own function offsets. The same commit also made `LayOut` of a library
  built in memory start from its parent's size in the parent's unit, so a 32-bit
  library built by a 64-bit process got a wrong size too; it is converted now.
  `tests/typelib-marshal-win32.c` builds, registers and marshals a 32-bit library
  (6 of 6; the old rpcrt4 crashes at the first call). Word opens and converts its
  own PDF, and an Excel VBA macro that drives Word with early binding now returns
  (`tests/functional/vba.vbs`, 5 of 5; with the old rpcrt4 it never did).
- **New test, `tests/functional/formats.vbs`:** Word saves and reopens doc, rtf, htm,
  odt, pdf and txt; Excel xls, csv, ods, xml, htm and PDF export; PowerPoint ppt, odp
  and pdf. 44 of 44. Copying an Excel range and chart into Word (OLE object, RTF, HTML,
  metafile, EMF, bitmap, text) and PowerPoint, and Word content back into Excel, also
  ran without a fault (by hand, not a committed test).
- **The Excel crash after an embedded chart edit was the test**, see its entry below:
  0 in 80 PowerPoint rounds since. Two combase bugs found on the way are fixed.
- Regression after all of it: chart UI 8/8, suite 10/10 (Word, Excel, PowerPoint),
  Access 41/43 (the known XML export and compact), Publisher 17/17 (the known hang
  after `Quit`), Outlook 27/27, OneNote 9/10 (Search), formats 44/44.

## 3 November: running every ribbon command, and missing imports

To find crashes rather than wait for them, `tests/mso-crash-hunt.py` runs every command
an app offers, one at a time, in the isolated prefix with no network. Commands come from
`CommandBars.FindControl` over control ids 1 to 45,000, or `DoCmd.RunCommand` 1 to 999 in
Access. `tests/dialog-closer.c` closes each dialog after it has painted. A crash or a hang
of 40 s is recorded with its command and the app is started again at the next one.

| App | Commands run | Crashes | Hangs |
|---|---|---|---|
| Word | 1,149 | 0 | 7, all the desktop file chooser |
| Excel | 1,409 | 0 | 1, Open |
| PowerPoint | 1,012 | 1, New Comment (fixed) | 1, Open Favorites |
| Access | 992 | 1, RunCommand 301 (fixed) | several, inside ACE |
| Publisher | 696 | 0 | Print, Open |

Under Wine, calling a function a DLL does not have ends the process: a missing export
is an "unimplemented function" stub, and a delay-loaded DLL that cannot be found raises
`0xC06D007F`. Both only fire when the code runs. `tests/import-gaps.py` reads the
static and delay imports of every Office binary, by name and by ordinal, and lists what
the runner and the prefix do not provide.

- **Access crashed on RunCommand 301 (Create Shortcut).** `HlinkCreateShortcutFromString`
  and the rest of hlink's shortcut family were missing. They now write and read
  Internet Shortcut files, as on Windows.
- **PowerPoint crashed on New Comment.** `elscore.dll` (extended linguistic services) did
  not exist. It now exists. Script detection was added on 4 November.
- **Other imports that would have ended the process** are now present: `srpapi` and
  `wscapi` (enterprise data protection and security center, both reporting nothing
  configured, never "protected"), `mfsensorgroup`, kernelbase memory offers,
  `GetStringScripts` and `VerifyScripts`, `GetDurationFormat(Ex)`, the package
  functions (`OpenPackageInfoByFullName`, `GetPackageInfo`, `ClosePackageInfo`,
  `PackageFamilyNameFromFullName`, `PackageFamilyNameFromId`,
  `FormatApplicationUserModelId`), `SetThreadpoolWaitEx`, crypt32 chain selection
  and time stamps, `CoCancelCall`, `RegDisablePredefinedCacheEx`, user32's pointer
  frame functions, `InheritWindowMonitor` and `IsImmersiveProcess`, uxtheme panning
  feedback and theme animation queries, `UiaDisconnectAllProviders`,
  `UiaIAccessibleFromProvider`, `DeleteAppContainerProfile` and
  `DeriveAppContainerSidFromAppContainerName`. The AppContainer SID and the publisher id
  use Windows' derivations and match known values (Edge's `S-1-15-2-3624051433-...` and
  `8wekyb3d8bbwe`).
- `runner/build.sh` now builds new, untracked modules. When a module is new to the
  installed runner, it gives `wine.inf` a new time, so existing prefixes update and get the
  DLL. Otherwise it keeps the installed runner's time, so a rebuild does not update them.
- **A runner upgrade broke printing and dropped Office's registrations.** Wine
  updates a prefix by itself when `wine.inf`'s time changes. That update dropped the
  fallback layout printer (Excel: "No printers are installed", so no PDF export) and the
  registrations TuxOffice makes at setup. The launcher now runs the update itself before
  starting Wine (`run`, `wine`, `cmd`), then sets up the printer, chart proxies,
  Installer components, shell settings and virtual registry again. Wine writes the
  prefix's stamp with a CRLF ending, which the comparison has to allow for, or every start
  would update.

Left alone on purpose: `slc.dll` and `slwga.dll` (Windows genuineness checks). Also
not fixed: DXVK's `d3d11` lacks `CreateDirect3D11DeviceFromDXGIDevice`. Gaps that are
only in components Office rarely loads are also left: ALPC and WNF in `coremessagingxp`,
`inkobjcore`, and some ninput and mscms ordinals.

Harness notes: since the host dropped `lib32-freetype2`, the 32-bit `start.exe` that
launches each `wine` command prints a FreeType warning on stderr. Helpers' answers are
now read by line. OneNote's "How do you want to start OneNote?" prompt follows from
`OneNote\General\ConsecutiveEarlyCrashes`, which the harness's kills raise. Its dialog
exposes no accessibility tree, so the regression clears the counter instead.

Still open from the hunt: Publisher's Print raised a first-chance access violation in
oleacc's `client_get_state`, reached from an RPC stub with a bad out pointer. The RPC
layer catches it and the call fails. A cross-apartment test with ten child id types did
not reproduce it.

Regression on the final build: suite 10 rounds (Word and Excel 10/10, PowerPoint 7/10, all three the
Excel crash in "Open: Excel crash after an embedded chart's workbook closes"), chart UI 8/8, Access 41/43, Publisher 17/17, Outlook 27/27, OneNote 9/10 (Search).

## 2 November: the rest of the suite (Outlook, OneNote, Access, Publisher)

Tested in the isolated session on a clean copy of the prefix, with a real
sign-in (device code, tokens in `tests/memory-secret-service.py`, memory only).
Outlook and OneNote ran with no network at all (`unshare --net`), on a local
data file and a local notebook, so no real mailbox or notebook was touched.

### What was wrong

- **Office's virtual registry was empty, in every app.** Click-to-Run merges
  per-package hives (`root\vreg\*.vreg.dat`) into `VirtualRegistry.dat`, which
  App-V overlays on the registry of Office's processes. Its own database records
  these as up to 2 MB each; under Wine all of them end up as 8 KB hives holding
  only `ROOT\REGISTRY`, so about 27,000 keys were missing. The merge reports
  success. Symptoms included VBA never loading (`Vbe71DllPath`). `lib/c2rvreg.py`
  reads the originals from the payload through Click-to-Run's database, checks
  each SHA-256, parses the hive format and imports the result where App-V reads
  it. Setup, install and `repair` do this; existing installs get it on their next
  start (`update_prefix`). VBA macros now run in Word, Excel and Access, which
  they did not before. Why the merge empties the hives is still not known.
- **Explorer died during about 1 in 3 starts of any app** (7 of 21; 0 of 20 after
  the fix). wineserver compared the number of desktop users with the manager's
  thread count, which also counts threads without a desktop, so it could close
  the desktop under running apps. Every later process then got its own desktop
  window and could not see the others' windows. It now checks whether any other
  process is on the desktop. Polling with a Wine process hides the race.
- **Access crashed opening any database:** Windows' OID table names the CNG
  algorithm of each hash and Wine's did not, so Access built a string from NULL.
  crypt32 now has them for hash, public key and signature OIDs (and SHA-512
  ECDSA). Conformance: `oid` 175 tests, 0 failures on both runners.
- **Outlook crashed closing a message and opening the Calendar:** Office treats a
  missing `ICompositionSurfaceBrush2` and `InteractionTrackerInertiaRestingValue`
  as fatal. Both are implemented, the brush with its transform. Resting values are
  stored, but inertia modifiers are not applied yet: scrolling works without snapping.
- **Outlook's Month view was blank.** Its week rows sit near y=262,000 in a large
  scrolling layer, and Outlook moves there with `TryUpdatePositionWithAnimation`,
  which Wine accepted and ignored. Animated tracker updates now go to the final
  key frame, as animations on visuals already do.
- **Access's one-click Form and Report crashed.** Three causes in `oleaut32`: a
  coclass's default dual interface must resolve to its dispatch view (Access
  computes a vtable offset from it, which overflowed, error 0x717D);
  `SetFuncCustData` and its siblings, `DeleteTypeInfo` and the member deletions
  were stubs (error 0x717E); custom data rejected small integer and boolean types.
- **ADO on Access's own connection failed.** `WrapDSOandSession` was a stub, and
  `Recordset.Open` refused VBA's by-reference variants. Queries now get their cursor
  and lock properties, and `RecordCount` counts through `IRowsetLocate`, because
  Access's provider reports no count of its own.
- **Virtual registry paths ignored App-V's folder merge.** A path that only the
  package supplies, such as `MSCDM.DLL`, now names its place in `root\VFS`.
- **Word, Excel and PowerPoint could hang for good on exit.** When the server frees
  a shared object it zeroes its id. `get_shared_class` and four similar functions
  read "id 0" as "not locked yet" and retried forever, here with `user_lock` held,
  so the thread that would unlink a class being destroyed queued behind the spinner
  and every other thread followed. `get_shared_input` already guarded this. Found
  with gdb attached from outside a user namespace, because `winedbg` injects a thread
  that needs the stuck loader lock. Permanent hangs: 4 of 60 suite runs before, 0 of
  84 after.
- **Excel import in Access could fault or misread values.** `ACEEXCL.DLL` builds a
  `VT_BSTR` variant from a plain string with no length prefix and converts it to
  `VT_R8`. Wine's `VariantChangeTypeEx` copied the source first, trusting the bytes
  before the string as a length (607 MB in the case captured). Conversions from
  strings now borrow the source. `ERROR_NOACCESS` during import: 8 of 119 runs with the
  failure type recorded before, 0 of 108 after.
- **OneNote's offline first-run dialog showed no way forward.** "Continue Anyway" is
  a `BS_PUSHBOX`, a button with no face or frame, only text. Wine's button classes had
  no painter for that type and drew nothing. Both `user32` and `comctl32_v6` now draw
  it. Checked by screenshot and `BM_CLICK`; a physical pointer click was not tested.
- **PowerPoint flashed white on entering a slide show.** Wine fills a new window
  surface with white, and the slide show window shows that first, faded in by KWin, for
  about 150 ms at 56 fps capture. A full screen captionless popup now starts in its class
  brush's colour, which is black here, or black with no brush. Other windows keep the
  white default. Peak brightness on entry went from 240 to 58, the editor's own level.
- **Outlook crashed on opening People, every time after visiting Calendar.** Two causes,
  both reported as fatal: `Windows.Graphics.Capture.GraphicsCaptureItem` did not exist
  (Office takes a missing class for a lost device, while a failed `CreateFromVisual` only
  makes it go without), and `CombineWithGeometry` was a stub, needed for round avatars.
  The class now exists and reports capture as not implemented. Geometries now combine by
  flattening, splitting edges at crossings and keeping those that bound the result, as
  polylines. 39 switches across Calendar views, People and Mail ran without a fault.
- **Direct2D drew arcs as straight lines.** `AddArc` now makes Bezier curves.
- **Every composition target had its own Direct3D device,** each with DXVK's worker
  threads. They now share one per rendering thread. Five rounds of switching in Outlook:
  4 devices instead of about 49 for eight rounds, 283 threads after round one instead of
  395, and growth of about 11 threads a round instead of 86.
- **PowerPoint slide thumbnails stayed blank in any deck without a chart.** Each new
  thumbnail is faded in through an arithmetic composite effect, and Office animates its
  amounts with `StartAnimation` on the effect brush, which returned `E_NOTIMPL`. The fade
  stayed on its first source, a blank surface, with the rendered slide unseen behind it.
  A chart hid this by causing a second rendering pass that does not fade. Scalar
  animations now store their final value, and expressions are evaluated when drawing.
- **Office could freeze for good on a message for a dead window.** Another thread looks up
  a window's owner and posts `WM_WINE_UPDATEWINDOWSTATE` just as the window is destroyed, so
  the message outlives it. Under `PeekMessage` with `PM_NOREMOVE`, win32u handles an internal
  message through a nested peek filtered on that window. The server rejects the dead handle
  without removing the message, and the outer loop finds the same message again. The UI
  thread never returned from `PeekMessage`, using about 44% of a core with no window showing.
  It was seen as Excel never exiting after `Quit`, but nothing ties it to `Quit`. Read from
  the stuck process: nested peek returning -1, window `IsWindow` false, no handler or post
  reached in 60 s. The nested peek now matches on the message alone. Excel lingered in 2 of
  4 rounds before, 0 of 70 after.
- **Long sessions leaked memory and threads.** Two separate leaks, seen while switching between
  Mail, Calendar and People in Outlook. Over ten rounds, before: threads 284 to 376, and about
  24 MB a round. After: threads flat, and about 5 MB a round, falling.
  - `wayland_restack_client_surfaces` never freed two buffers. A tracking `malloc` shim put
    175 MB over 12 rounds on those two lines. Restacking happens in every app, so this was
    not particular to Outlook. Native memory is now flat, 39 MB to 41 MB over 12 rounds.
  - The compositor's target list kept its own reference to every composition target for as long
    as the compositor lived, so one Office had released, or whose window was gone, kept its
    swapchain and was still visited by every commit. Each swapchain brings a `dxvk-frame` thread,
    and these grew by 9 a round. Commits now drop such targets.
- **A workbook with a modern chart could not be saved.** Adding a Waterfall, Treemap, Sunburst,
  Histogram, Funnel or Box chart made every later save fail with "Document not saved". These
  charts are written by `chart.dll` through XmlLite, which calls
  `WriteAttributeString("xmlns", "a", "http://www.w3.org/2000/xmlns/", ...)`. Wine rejected any
  URI on that prefix, where only one other than its own reserved URI is wrong. Six such charts
  now save and reopen looking the same, in Excel; PowerPoint saves a Waterfall chart. Word's
  object model refused to create one in the test, so Word's save of them is untested, though
  it uses the same `chart.dll`.
- **Scatter charts drew no points.** Excel draws their markers with `DrawSpriteBatch`, which did
  nothing on bitmap targets. It now draws each sprite, one at a time, so a chart with a very
  large number of points may be slow; that has not been measured.
- **Shape effects blanked whole slides.** On screen, any slide with a glow, soft edge or
  reflection drew blank in the editor and thumbnails, and a slide show stayed on the previous
  slide while reporting that it had moved on. PDF export was fine. Shadows came out hard and
  black. There were several causes, in order of discovery:
  - Office's custom Direct2D effect ("Alpha Threshold", in `GFX.DLL`) calls
    `ID2D1DrawInfo::SetInputDescription`, a stub returning `E_NOTIMPL`, so creating the effect
    failed and Office dropped the drawing.
  - Nothing could run a custom effect's own pixel shader. A generic pass now does it, giving
    the shader Direct2D's standard inputs. This covers single-transform per-pixel effects.
    Alpha Threshold, BiLevel (black and white) and Find And Replace (Set Transparent Color) have
    been seen to run. Find And Replace only chooses between its two shaders when about to draw,
    so effects now get to do that before being classified.
  - Any effect not otherwise handled was treated as `ArithmeticComposite`, which failed and
    put the context in error, losing everything drawn. Gaussian blur, directional blur and
    composite are now implemented, and an unknown effect draws its input with a `FIXME`.
    `Shadow` was rebuilt on the same passes.
  - A shader loaded on one device could be drawn on another, because a command list can be
    played back on another thread with its own device. The bytecode is now kept for this.
  - `d2d_affine_matrix` scaled translations by DPI/96 in pixel mode, moving effects by about
    a quarter of their offset. The logged offsets add up to the shape's true position only
    without the scaling. This is inferred from Office's behaviour, not from Windows.
- **On-screen strokes ignored their style.** `DrawGeometry` drew every dashed and dotted
  line solid, with flat ends and mitred corners, while PDF export was right. It affected
  shapes in Word, Excel and PowerPoint, thumbnails and slide shows. Dashes, start, end and
  dash caps, and round and bevel joins are now built from the flattened geometry. A
  translucent stroke at least 2 px wide merges into one fill, which also removes the dark
  blobs where it used to cover itself at corners. Hollow figures no longer go through the
  fill's quadratic crossing search, which hung PowerPoint once a figure was made per dash.
  `FIXED` strokes divided by `m11`, which is zero for a quarter turn; they now use the
  transform's scale, and `HAIRLINE` is honoured. Cost: 49.5 ms per slide switch against
  36.7 ms on a slide of 200 shapes, about 64 microseconds a shape.
- **Shrunken pictures showed false patterns.** WIC's `BitmapScaler` implemented only nearest
  neighbour and fell back to it for every other mode. Linear, Cubic, Fant and
  HighQualityCubic are now filtered, widening the filter when shrinking, and premultiplying
  straight alpha so that transparent edges do not darken. This drew the slide thumbnails.
  The main canvas samples through Direct2D with no mip chain and `MaxLOD` 0, so static
  bitmaps drawn below 80% now get a mip chain, made when first needed. The sampler cache was
  indexed by the mode modulo 2, so cubic and anisotropic shared a slot with nearest
  neighbour and whichever came first set the filter for both. A zone plate now fades to grey
  as it should, with only very faint rings left. Bitmaps used as render targets keep the
  old sampling, because they change with every draw into them.
- **A result nobody asked for leaked across processes.** `IDispatch_Invoke_Proxy`
  never released its local result when the caller passed none, keeping server objects
  alive. Calling `db.CreateQueryDef "Q", "..."` as a statement held the database open.
- **Type libraries mixed pointer sizes.** 64-bit Office ships 32-bit-syskind
  libraries such as MSACC.OLB. Wine resolved their imports through the `win32`
  key, loading 32-bit stdole2 into 64-bit processes, and scaled `oVft` but not
  `cbSizeVft`. Base interfaces reported 28-byte vtables.
- **COM started `dllhost` for any class with an AppID,** even without a
  `DllSurrogate` value. With the real registry in place this made PowerPoint's
  chart insertion fail; such classes now report `REGDB_E_CLASSNOTREG`.
- **A stray box floated over Outlook and OneNote:** Wine's standalone tray window.
  It is hidden (`ShowSystray=0`). Tray icons are therefore not shown at all.
  `winemenubuilder` is disabled so Office cannot add Wine entries to the
  application menu.

### Results on this build

| Test | Result |
| --- | --- |
| Office suite (Word, Excel, PowerPoint), latest 10 runs | 10 clean; 21 of 174 earlier runs hit the Excel crash below, with all checks passing |
| Excel functional test with network, `Quit` then exit, after the peek fix | 70 of 70 exited; 69 within 2 s, one in 11 s |
| Dashes, caps and joins compared with PDF export, PowerPoint | match, as do thumbnails and a slide show |
| Dashed shapes and shrunken pictures, Word and Excel | draw correctly |
| Effects deck (fills, shadow, glow, soft edge, reflection, bevel, 3D, WordArt, text effects, table, SmartArt, chart) in a slide show, compared with PDF export | match by eye, apart from the items under Direct2D below |
| Shadow, glow, soft edge and reflection in Word and Excel | draw correctly |
| Effects, dashed outlines and pictures in Publisher, compared with its PDF export | match; a bevel set by script did not take in either |
| JPEG, BMP, GIF, TIFF, PNG with alpha and SVG pictures, compared with PDF export | match by eye |
| Excel: data bars, colour scales, icon sets, sparklines, cell patterns and borders, 16 chart types | draw correctly by eye |
| Word: text formatting, lists, table styles and borders, rotated cell text, equation, pictures, shapes, WordArt, compared with PDF export | match by eye |
| 41 slide transitions with entrance animations in a live slide show | no fault or hang, show reached the end |
| 7 of those transitions sampled at about 49 fps, including Cube, Origami, Vortex and Morph | 30 to 60 distinct in-between frames each, so they animate |
| Picture formatting in a slide show compared with PDF export: grayscale, black and white, washout, brightness, contrast, crop, and 16 artistic effects; Set Transparent Color on a coloured background | match by eye, no faults |
| Ribbon tabs clicked, Word and Excel | 8 tabs seen drawn correctly in each, no faults; Help was not reached in either. The ribbon first changes about 0.12 s after the click |
| VBA macro in Excel and Word | runs and returns the right value |
| Outlook object model (`tests/functional/outlook.vbs`) | 27 of 27 |
| OneNote API (`tests/onenote-api.c`) | 9 of 10; `FindPages` fails, though search in the app finds and highlights text |
| Publisher object model | 17 of 17; the client then hangs, see below |
| Access object model (`tests/functional/access.vbs`) | 41 of 43; XML export and compact after the full run fail, see below |
| Access Create: Form, Report, Blank Form, Blank Report, Form Design, Report Design, Table, Query Design, Macro, Module | all open |
| Outlook Calendar: Week, Month with scrolling, Schedule View | draw correctly |
| Slide show entry and exit, 56 fps capture on a dark deck | no white frame |
| Start to a responsive window, warm, isolated session | 3.4 s for each app tried; 3.7-4.9 s before |

Not run: Wine's `oleaut32` conformance tests (they need generated test libraries
that the overlay build disables). No Windows baseline exists, so performance
parity with Windows remains unmeasured.

### Still open

- **Access:**
  - `ExportXML` fails (0x7B2C). Access reaches it through `MSCDM.DLL`, which needs
    Windows's MSDataShape provider and asks it for a private, undocumented
    interface, `{e6b53f52-c260-11d0-ad71-00c04fc29863}`. Wine has neither. Other
    uses of `MSCDM.DLL` probably fail the same way; only `ExportXML` was tested.
  - Intermittent abort at `CloseCurrentDatabase` after a `TransferSpreadsheet` import:
    3 of the last 128 scripted runs failed, 1 of them seen as this abort and 2 with the
    failure type not recorded. A lookup in a global registry used from 446 call sites
    (`MSACCESS.EXE+0x36e0`) finds no entry, Office asserts with tag `0x30303030`, and
    Access terminates. The cause is not known. Not tried in the UI.
  - `DBEngine.CompactDatabase` reports the file in use once `CurrentProject.Connection`
    has been used in that session, even after closing the connection and the database
    and waiting 20 s. `RunCommand acCmdCompactDatabase` reports no error either way, but
    the file did not shrink, so it is not shown to work. Merely reading
    `CurrentProject.Connection` is enough to cause it; without that, compact works. `MSACCESS.EXE`
    still has the database open after `CloseCurrentDatabase`. The wrapper is not the holder: the
    ADO connection reaches a reference count of zero and is destroyed, and having it uninitialize
    the data source too made no difference. So the hold is inside the database engine Access
    created. Whether Windows behaves the same is unknown.
  - Split Form, Multiple Items and Labels have not been opened: they are in menus
    the test helper did not open.
- **Direct2D:** shadows are missing from PowerPoint's slide thumbnails, though the canvas and
  slide shows draw them, and Publisher's thumbnails show them. Office issues no Shadow effect
  for thumbnail content, so it is Office's own choice, and whether Windows does the same is
  not known. A gradient with transparency looks lighter on screen than in PDF export, and
  which is right has not been established. Effects were only tried in PowerPoint, Word and
  Excel. Office's Recolor, Tint, HLS, RGBtoHLS and LegacyOverlayBlend effects have not been
  seen to run. `GFX.DLL` also refers to built-in effects not implemented here, including Blend,
  Border, Crop, Flood, Morphology, Tile, Turbulence and the transfer and lighting effects. None
  was reached in these tests; one that is would draw its input unchanged with a `FIXME`.
  Cubic modes on the main canvas are trilinear over box filtered mips, which
  is softer than Windows's high quality cubic and has not been compared with it. A closed
  dashed figure does not join its last dash to its first. `DrawSpriteBatch` is unimplemented
  for bitmap targets, and `CreateSvgDocument` does not parse streams; the effect of neither
  has been seen.
- **Excel stuck in `user_lock` at exit, seen once:** in one of the 4 rounds before the peek
  fix, Excel's main thread was blocked on win32u's user mutex in `user_destroy_window`, under
  the loader lock, with its other Wine threads gone. Wine ends other threads before
  `DllMain` detach, so a thread ended while holding that mutex would explain it, but its
  owner was not read and this is unproven. It did not recur in 70 rounds, which does not
  show it is fixed.
- **Outlook:** memory still grows by about 5 MB a round while switching modules, in Wine's own
  heap and slowing over time, which may be caching. File descriptors grow by about 9 a round,
  mostly `ntsync` handles; what holds them was not traced. Its `iterate` time in the functional
  test rose from 8.7 s to 15.5 s across recent runs, probably because each run adds 200 items
  to the same test mailbox, though that was not checked separately. The People peek card opens at the top of the
  window, over the ribbon, which may or may not match Windows. Scripted access shows
  security prompts because no antivirus is reported. A real account has not been tried,
  by choice.
- **Sign-in:** the code for personal accounts (`login.live.com`) is written but has
  never run end to end: it needs a personal account. The embedded page runs on
  Gecko 2.47 and draws its placeholder twice. Installing WebView2 (works, 69 s) did
  not change this page, so setup does not install it.
- **Publisher** titles itself "Unlicensed Product" and asks to sign in although
  Word is licensed for the same account. The cached licence is the same for both
  (`O365ProPlusRetail`, `Provisioned`), Publisher does not reach `sppc` for this, and
  no retirement marker was found in `MSPUB.EXE`, the cached rules or the registry, so
  the decision is made inside Office or by its service. Microsoft's support pages say Microsoft
  365 subscribers lose Publisher after October 2026, which fits, though nothing
  here proves it is the cause. It still created, edited and saved in the tests. It
  is not worked around. After `Quit` by automation `MSPUB.EXE` never exits: its main
  thread waits on a lock while a worker sits in an infinite wait on an event inside `mso98`,
  and the client waits for both. Nothing shows what should set that event, so the cause is not
  known. Closing its window exits in about 5 s.
- **OneNote:** `{9e175b8b-f52a-11d8-b9a5-505054503030}`, the class `FindPages` fails to create,
  is from memory the OLE DB provider of Windows Search, which Wine does not have. That is not
  confirmed here. If so, fixing it means implementing a search index, not a small stub. The
  offline first-run dialog's "Continue Anyway" button exists and
  works when activated, but did not appear in one screenshot; not checked further.
  `FindPages` returns `hrInvalidQuery` (0x80042019) after failing to create the
  unregistered class `{9e175b8b-f52a-11d8-b9a5-505054503030}`. That looks like Windows
  Search's provider, but nothing here confirms the name. Search in the app works.
  Killing OneNote can make its next start show "We couldn't start OneNote", and
  `tests/functional/run.py` ends with a kill, so it may need "Start Normally" first.
- **Word, Excel and PowerPoint:** `Quit` left a process running past 15 s in 2 of 60
  runs before the shared object fix and 0 of 24 after, then in 1 of the 10 suite runs before
  the peek fix and 0 of the 10 after. The peek fix has a proven mechanism for Excel. Whether
  it accounts for all of these is not established: the earlier stuck processes were not
  inspected, and one other cause is listed above under `user_lock`.
- **Chart editing through the UI:** 1 of 44 rounds failed after Alt+F4, with Excel's save prompt
  up and the input helper reporting that Excel was not in the foreground. The edit itself was
  right. Whether a modal dialog can really fail to come forward, or only the helper's check
  raced, is not known.
- Bulk cross-process COM is slow: about 2.5 ms per call (14,000 calls in 35 s).
  In-process VBA is not affected.
- Wine's VBScript releases temporaries mid-expression, which invalidates chains
  like `db.OpenRecordset(q).Fields(0).Value`. Office's VBA does not use it.

## Open: Excel crash after an embedded chart's workbook closes

**4 November: cause found; it is the test racing Excel.** After the chart data is edited,
Excel notifies PowerPoint from its idle loop (`ChDataSink` method 81, about 120 ms while
PowerPoint reads the data back). The test's `Workbook.Close` and `Release` calls arrive
during that outgoing call. Excel's message filter accepts them (it only puts calls off
while its own busy counters are set), so they free the workbook's state under the
notification, which then writes into it. Found with a gdb watchpoint on the freed pointer
and COM traces. A person closes the data window on Excel's own thread, so this needs an
outside automation client. `office-functional.c` now waits a second before `Close`:
0 crashes in 40 PowerPoint rounds (was 8 of 40), then 40 of 40 again and 10 of 10
full suite rounds. One earlier round hung after `Close`; it did not come back in the next
90 rounds, and no call was put off in any of them, so it is not the combase change below.

Two COM bugs found on the way are fixed in combase, though neither caused this crash.
Message filters were told a call was `CALLTYPE_TOPLEVEL` while the thread waited for
its own outgoing call: Wine counted incoming calls, not outgoing ones. And a filter's
`SERVERCALL_RETRYLATER` was ignored, so the call ran at once. Such calls are now held and
run again when the outgoing call returns or after 100 ms, as a retrying caller would see,
and fail with `RPC_E_SERVERCALL_RETRYLATER` after 30 s. Excel's filter answers that while
it is busy. `tests/com-message-filter.c` checks both (7 of 7; the old combase fails 5).

**3 November:** back, at about the old rate: 3 of 10 suite rounds, then 1 of 20 and 1 of 20
PowerPoint-only rounds, same write to NULL+0x8c at `EXCEL.EXE+0xe02c6a`. It is not the day's
new user32 and uxtheme functions: with those removed it was 3 of 30. Not the printer
either: 1 of 20 with the fallback printer instead of a CUPS one. And not the new
`UiaDisconnectAllProviders`, which PowerPoint never calls in this step. All of these runs
came after Wine updated the test prefix, and the clean runs before it did not. That fits
the update, but does not prove it. The faulting code takes a per-thread context
(`[TLS+0x130]`), then its object at `+0x3e0`, and stores several fields into it after that
object has gone.

**Later on 2 November:** not seen in the last 60 scripted suite runs, whose logs record exceptions
from the chart step. At the old rate of 12%, 60 clean runs would happen by chance about 0.05% of
the time, so the rate has dropped. What changed it is not known, so it is not shown to be cured.

**2 November:** now the most frequent failure: 21 of 174 scripted suite runs (12%).
Whether the day's fixes changed the rate was not measured. The chart edit through the
desktop UI (`tests/embedded-chart-ui.py`), as a person does it, crashed in 0 of 24 runs,
though with a 12% rate that sample could still miss it about 5% of the time.

Excel is running a deferred work item (flag at `+0x128`) from its main message loop
after `[context+0x130]+0x3e0` has been cleared, with no Wine frames in the chain. There
is no Windows trace to compare with, so what shifts the order is still unknown.

PowerPoint's functional step (insert a chart, open its data in Excel's
chart data window, edit a cell, `Workbook.Close`) crashed Excel in 1 of 15
isolated repeats with the packaged runner, and three times in today's
batteries: a write to NULL+0x8c at `EXCEL.EXE+0xe02c6a` on Excel's main
thread, a few hundred milliseconds after the close, while the embedding
Excel process shuts down. There are no Wine frames on the stack; the code
loads a global state object, reads its pointer at +0x3e0 (already NULL) and
writes through it, i.e. Excel's own teardown races with work still queued.
PowerPoint is unaffected (all its checks pass, the edited data survives a
reopen), and with Wine's crash debugger disabled nothing is shown, but Excel
records an unclean exit, so its next start can ask about safe mode. Closing
the chart editor through its window (the embedded chart UI test) has not
crashed. Which Wine behaviour shifts the timing is not known yet.

## 26 October evening: distribution packages and the one-line installer

- `packaging/build-release.sh` builds a portable runner in Ubuntu 24.04
  (glibc 2.39; the Soda base itself needs 2.38) and packages it as `.deb`,
  `.rpm` and Arch packages with nfpm, a tarball, an AUR `tuxoffice-bin`
  PKGBUILD/.SRCINFO (identical to `makepkg --printsrcinfo`) and SHA256SUMS.
  `.github/workflows/release.yml` publishes them for version tags.
- The release build fails if a changed module was not built or lost a
  feature: the first container build had no Wayland driver at all
  (Ubuntu ships xkbregistry separately), which the old overlay would have
  hidden by keeping Soda's copy.
- The packaged runner drops Soda's legacy 32-bit Unix loader: a bare Arch
  container could not create a Wine environment without 32-bit glibc. With
  WoW64, 32-bit and 64-bit programs run and no 32-bit libraries are needed.
  Wine's ntdll.so also needs libunwind, now a dependency.
- The container-built runner passes the suite, Excel cells, PowerPoint
  ribbon, embedded chart and multi-app tests on this desktop. One battery
  hit the known intermittent Excel crash (EXCEL.EXE+0xe02c6a); two reruns
  passed.
- `install.sh` (curl | bash): checks the system, installs the package with
  apt, dnf, zypper or pacman, or the tarball into ~/.local (offering to
  install missing tools and libraries), verifies SHA256SUMS, then offers to
  set up TuxOffice and install Microsoft 365. Menus work when the script
  arrives on standard input (tested under a pseudo-terminal).
- End to end: `install.sh --user --office` from the release tarball in an
  empty home (inside the isolated session, Office payload seeded from the
  cache) finished in 5 minutes: setup, Office's 32-bit Click-to-Run setup
  under WoW64, desktop entries; `doctor` reports all seven apps and Word
  opens to "Sign in to get started" with one sign-in prompt on top. It also
  found that without a terminal the installer tried `sudo` for a missing
  `cabextract` and stopped; it now only reports missing requirements then.
- `packaging/test-packages.sh` installs, checks and uninstalls the packages
  in clean Ubuntu 24.04, Debian 13, Fedora 42, openSUSE Tumbleweed and Arch
  containers as a normal user with sudo. All five pass: install through
  install.sh, launcher and runner found, a Wine environment created with
  32-bit and 64-bit programs running, uninstall, and a per-user install.

## Installed 26 October 17:20: fresh install, owned windows, sign-in prompt, file chooser

Backups: `recovery-backups/owners-signin-dialogs-20261026-172038/`. The
launcher and `lib/c2rcomponents.py` changes take effect directly.


A complete `setup` + `install` from an empty data home (inside the isolated
session, downloads seeded from the existing cache) found:

- **`install` hung after Click-to-Run finished.** The Installer component
  repair waited for `wineserver -w` so that system.reg was on disk, but the
  Click-to-Run service setup starts never exits. The repair now reads the
  live registry with `wine reg export HKLM\Software` (`lib/c2rcomponents.py`
  takes the export as a third argument). With the service running the repair
  took 3 s and registered the same 1575 components as the live prefix.
- **Owned windows went behind their owner.** winewayland never told the
  compositor about window ownership, so clicking Word while its modal Font
  dialog was open raised Word over the dialog and Word looked frozen. Owned
  toplevels now get `xdg_toplevel_set_parent` to their owner's toplevel;
  the Font dialog stays above Word after the same click.
- **Sign-in prompts: duplicated and hidden.** At first start Word asked for
  tokens on two threads; each showed its own device-code prompt with a
  different code, both behind Office's "Sign in to set up Office" dialog.
  One interactive sign-in now runs at a time per process (later requests
  re-check the token cache, or are declined if the user declined), and the
  prompt keeps itself owned by the window in front, so it stays above
  Office's dialog. Cancelling it shows no second prompt; "Sign in or create
  account" then opens Office's own sign-in page normally.
- First start after install reaches "Sign in to get started with Word";
  `doctor` reports all seven apps; desktop entries are written. The embedded
  sign-in page draws its email placeholder twice (Wine's Gecko engine).
- Battery on this build: all steps passed in a rerun; one earlier run had
  an Excel null-pointer crash (EXCEL.EXE+0xe02c6a) during the PowerPoint
  suite step, followed by an Excel start that did not become ready and 3
  blank PowerPoint ribbon frames. The suite then passed 3 of 3 on both this
  build and the previous runner, so it is intermittent and not attributed.
  Dialog audit, OneNote and Outlook (offline) look as before.

## File chooser left open, UI font check (installed 17:20 with the above)

- If Office crashed or was killed while the desktop file chooser was open,
  the chooser stayed on the desktop (its helper waited on the portal
  forever; four such dialogs had piled up in the test session). Wine's spawn
  detaches the helper, so comdlg32 now passes Office's Unix PID
  (`--parent`, from ProcessWineUnixPid); the helper watches it with a pidfd
  and, when it exits or the helper gets SIGTERM/SIGINT/SIGHUP, closes the
  portal request (KDE's portal keeps the dialog of a vanished client) and
  exits. Tested: killing Word with Save As open removes the dialog and the
  helper within 2 s; Save As and Open still round-trip in all three apps.
- `office-ui.exe fonts` meant to keep Wine's UI fonts until a real Segoe UI
  is installed, but `GetTextFaceW` reports the requested name when the
  Segoe UI → Selawik substitute supplies the font, so fresh setups set the
  menus to Selawik and warned "UI font setup failed". It now reads the full
  face name from the font file (outline metrics; Office's Segoe UI Semilight
  also has the family name Segoe UI), keeps the defaults until regular
  Segoe UI or Segoe UI Variable is installed, and sets and checks that face.
  Fresh prefix: "Segoe UI resolves to Selawik", defaults kept, no warning;
  prefix with Segoe UI: applied as before.

## Checks 26 October ~15:30: dialogs, images on the clipboard, Outlook

- Dialog audit on the installed runner: Word Font, Find and Replace,
  Paragraph; Excel Format Cells, Find and Replace, Insert Function;
  PowerPoint Font and Insert Hyperlink all open with crisp text at 120 DPI
  and Windows 11 captions. Save As and Open go to the desktop file chooser.
  Current PowerPoint opens Find and Replace as a task pane, so the audit now
  selects a shape and opens Font (Ctrl+T) and Insert Hyperlink (Ctrl+K);
  an audit entry can be a list of typer commands.
- Images on the clipboard: a 300x200 image copied in a native Wayland (Qt)
  app pastes into PowerPoint as a picture; copying that picture offers
  image/png, image/jpeg, image/gif and image/svg+xml to Linux apps, and Qt
  reads it back with the right colour.
- Outlook (offline copy) started 8 of 8 times; with the 8 logged starts
  before, 16 in a row without "Cannot start Microsoft Outlook".
- Start until the window answers, opening a document (isolated session):
  Word 3.9–4.9 s, Excel 3.7–3.9 s, PowerPoint 4.1–4.4 s. One PowerPoint start
  stopped at a window titled "Microsoft PowerPoint" (likely a prompt after the
  previous run was killed); 0 of 3 repeats, so its text is not known.

## Installed 26 October 15:00: restoring a window maximized from KDE

Backups: `recovery-backups/kde-restore-size-20261026-150004/`.


- Once KDE had maximized an Office window (Meta+PgUp, double click in KDE's
  window menu, a tiling shortcut), un-maximizing it from KDE left it at the
  maximized size, only no longer maximized. Office's own restore button then
  did nothing either, and the frame shadow was missing on every edge.
  winewayland ignored the size of a configure without a maximized,
  fullscreen or tiled state; it now uses it when the compositor takes a
  window that Wine still has maximized or fullscreen out of that state. A
  restore made by the application itself keeps its own size.
- `tests/typer.c` has a `restore` command (SW_RESTORE). A state sequence
  (Word restored to 1300x800 by Win32, then KDE maximize/unmaximize, Win32
  maximize, KDE unmaximize, KDE maximize, Win32 restore): previous runner
  stays 2048x1280 after every restore; candidate returns to 1300x800 with the
  shadow on all edges each time. Shadow resize trials (8 per app, starting
  from KDE un-maximize) pass for Word, Excel and PowerPoint; before, the
  first trial failed in each. Slide show enters and leaves full screen.

## Installed 26 October 13:51: child windows clipped to the window, chain engine race

Backups: `recovery-backups/child-crop-chain-lock-20261026-135129/`.

- Ribbon collapse/expand in a normal (not maximized) window painted parts of
  the document view, scroll bars or panes below the window for a few frames.
  Office moves those GPU child windows partly or wholly outside their parent
  before resizing them; Windows clips children to the parent, Wayland
  subsurfaces are not clipped. winewayland now crops a client subsurface to
  the window with a viewport source, both at presents (win32u reports the
  size of the image being presented) and on window changes, where the crop is
  kept inside both the current and previous image so it can never leave the
  attached buffer. A child entirely outside the window is reduced to one
  pixel inside it (a subsurface can't be hidden without taking its buffer away
  from Vulkan). On-screen test (strip 3 px below a 1600x1000 window, four
  Ctrl+F1 toggles, about 1100 captures): Word 15–19 → 0–1 frames, Excel 13 →
  0–1, PowerPoint 16 → 1. No Wayland protocol errors.
- crypt32: threads verifying their first certificate chain at the same time
  each built a default chain engine and freed the extra ones; that free
  crashed PowerPoint once at startup (access violation in `Collection_release`
  from wininet). Default engine creation is now serialised. crypt32 conformance
  tests unchanged (chain 2 and protectdata 2 failures as before).
- Battery on the candidate: suite, Excel cells, PowerPoint ribbon, scatter x3,
  embedded chart UI, multi-app, scroll comparison all pass. Word ribbon clicks
  failed twice inside full batteries (the tab clicks were sent and ignored)
  and passed 10 of 10 standalone and after the PowerPoint step on both the
  candidate and the previous runner; treated as a harness flake for now.
  Later: 0 failures in 11 more runs in battery order (including after Word
  was left restored), so it is rare and not reproduced on demand. One
  PowerPoint ribbon trial showed two blank frames while the Design tab's
  theme gallery loaded; 0 in 18 later trials on either runner.

## Installed 26 October 10:15: launch focus and feedback

Backups: `recovery-backups/activation-upgrade-20261026-101534/`.

Plasma hands an application it launches an `XDG_ACTIVATION_TOKEN`; Wine's
Wayland driver ignored it, so with focus stealing prevention a document
opened from Dolphin could come up behind the current window. The first
toplevel of a process now activates itself with that token (tokens are
single-use). The generated desktop entries now have `StartupNotify=true`, so
Plasma shows launch feedback during Office's start; `tuxoffice integrate`
was rerun to update the installed entries. Tested in the isolated session
with a dummy token (used once, Word starts normally); a real token needs a
desktop launch. Documents with spaces and non-ASCII names open from a path
and from a `file://` URI.

`tests/isolated-run.py` sets `TUXOFFICE_THEME=light` by default: the
isolated portal reports the desktop's dark preference, and the screen checks
compare colours.

## Fixed 26 October 12:55: Access "Automatic configuration ... has failed"

Access showed this warning at every start. Traced with winedbg from the
alert back to MSACCESS.EXE+0x522f80: it checks that `Access.Application\CurVer`
names version 16 and that the key
`HKLM\Software\Microsoft\Office\16.0\Access\NoRepairNeeded` exists; without
the key it tries to reconfigure itself, a Click-to-Run feature check stops
that, and it shows the warning. All the registrations it would make are
present, so `configure_installer_components` (setup, post-install and
`tuxoffice repair`) now creates the key. Verified in the isolated prefix:
Access opens straight to its start page. Applied to the live prefix
(registry backup in `recovery-backups/access-norepair-20261026-125503/`).

## Launcher 26 October ~10:00: Office follows the desktop's light/dark theme

Office's default theme ("Use system setting") follows Windows' app theme
(`AppsUseLightTheme`, which Wine's `UISettings` reads). `tuxoffice run` now
sets it from the desktop's preference (freedesktop appearance portal
`color-scheme`: Plasma's Breeze Dark reports "prefer dark"), writing the
registry only when the preference changes. `TUXOFFICE_THEME=light|dark`
overrides it; a theme chosen in Office's own options still wins. Verified in
the isolated session: dark and light ribbons as expected. Dialogs drawn by
Wine stay light, as Office's classic dialogs do on Windows.

## Installed 26 October 09:44: drag and drop from Linux applications

Backups: `recovery-backups/dnd-upgrade-20261026-094440/`.

Wine's Wayland driver ignored drags entirely (empty `wl_data_device` enter,
motion and drop handlers): files dragged from Dolphin onto Word, Excel or
PowerPoint did nothing. It now reads the offered data at enter (files from
`text/uri-list` as CF_HDROP; otherwise text, HTML, RTF and images, with a
2 s limit for a silent source), hands it to Wine's drag-drop calls (user32
finds the OLE drop target or the WS_EX_ACCEPTFILES fallback), reports the
target's effect to the compositor on each motion, and drops. Verified in the
isolated session with a native Qt drag source: Word opened the dropped .docx
and the source saw a completed copy. Drags out of Wine to Linux applications
are not implemented. The clipboard now also maps `text/uri-list` to CF_HDROP
(files copied in Dolphin can be pasted where Office accepts files, and the
reverse).

## Installed 26 October 09:35: clipboard between Office and Linux apps

Backups: `recovery-backups/clipboard-upgrade-20261026-093553/`.

Tested in the isolated session with a native Qt (PySide6) Wayland peer and
compositor-level input (Wayland only accepts clipboard changes tied to a real
input event, so input injected inside Wine cannot set the selection; a real
Ctrl+C in Office can):

- Word → Linux: text arrived with a trailing NUL character; Word's
  clipboard memory is longer than its string. Export now stops at the first
  null.
- HTML: Wine offered only plain text and RTF, so formatted text pasted into
  browsers or LibreOffice lost its formatting. `text/html` now maps to
  Windows' "HTML Format" in both directions; export uses the whole document
  (StartHTML..EndHTML) because Office keeps its styles in the head. Verified:
  a Linux app receives Word's 38 KB HTML with bold, and Word pastes HTML from
  a Linux app.
- Plain `text/plain` (UTF-8) is offered and accepted besides
  `text/plain;charset=utf-8`.
- KWin does not offer `zwlr_data_control_manager_v1` to Wine, so the
  clipboard follows the focused window (Wine's `wl_data_device` path): normal
  copy and paste work; changes made while Office is not focused are seen when
  it next gets focus.

`tests/office-flicker.py` now resets Office's remembered collapsed ribbon
(`Common\Toolbars\<App>\QuickAccessToolbarStyle`) before each run; a lost
toggle in a collapse run had left it collapsed and made later ribbon tests
fail on both runners.

## Checks 26 October ~09:00: performance, startup, other apps

- Scrolling (isolated session, 3 rounds each): at 50 ms input Word 18.5,
  Excel 19.3, PowerPoint 23.1 visible updates/s (input rate 20/s), first
  update 21-33 ms, longest gap 108-156 ms. At 16 ms input Word 40.2, Excel
  45.5, PowerPoint 42.5 updates/s, p95 gap 31-35 ms: unchanged from the best
  earlier figures.
- Startup to a responsive window with a document: Word 4.0 s, Excel 3.8 s,
  PowerPoint 4.2 s; Wine itself starts in 0.6 s cold, 0.1 s warm. Word uses
  2.2 s of CPU and the wineserver 0.9 s meanwhile. Sampling Word's threads
  every 20 ms shows a 1.0 s stretch (about 1.75-2.75 s) where its main thread
  waits in a native futex and no thread of the process runs: a wait for the
  compositor, which in the nested test KWin also stalls screenshots for up to
  a second without Office. Restricting EGL to Mesa changed nothing (Vulkan is
  already Intel-only there). Startup on the real desktop is probably about
  1 s shorter; it was not measured there.
- Idle CPU: Excel and PowerPoint 27 ms/s for all Wine processes; Word 0-7
  ms/s with a small document. With a 26,000-word document Word works about
  35 s after opening (background pagination and proofing, ~730 ms/s), then
  2 ms/s. The launcher no longer
  starts `winepath` (about 0.5 s) to convert document paths; it maps them to
  `C:` (inside the prefix) or `Z:` itself, falling back to `winepath` when the
  prefix's drives are not the standard ones. The saving mostly moves into
  Wine's own startup.
- PowerPoint slide show (F5): full screen, letterboxed correctly, advances and
  returns with Escape. OneNote (offline copy of the live prefix) shows its
  notebook with the Windows 11 captions and a visible caret.
- Outlook (offline copy): normally reaches the account setup; 3 of 11 starts
  showed "Cannot start Microsoft Outlook." and 8 later starts with logging did
  not, so the cause is not known; it may be specific to running without a
  network.
- Ribbon collapse (Ctrl+F1): Word grows the document view before moving it,
  for many frames, so the GPU child reaches about 94 px below the window
  (Windows clips child windows to their parent). Fixed at 13:51 (see above).

## Installed 26 October 08:21: Office startup crash in the root certificate import

Backups: `recovery-backups/root-certs-upgrade-20261026-082124/`.

The remaining watchdog/startup crash (Excel failed to start twice in one
battery; about one multi-app run in six) was always the same thread: wininet
→ `CRYPT_CreateChainEngine` → system Root store → crypt32's
`sync_trusted_roots_from_known_locations` → `RegCloseKey(import_key)` →
App-V's `NtClose` hook releasing a freed tracking object. Every process
re-ran that import: one host certificate ("e-Szigno TLS Root CA 2023") fails
chain validation, is never recorded, and so looked new each time, making each
Office process build a chain engine and hold
`HKLM\Software\Wine\HostImportedCertificates` open for writing under App-V.

- crypt32 now reads the host certificates once, records a digest of the set
  after an import, and returns early when it is unchanged, without opening
  any key for writing (Root store open 55 → 11 ms). Rejected certificates are
  checked again only when the host's set changes.
- The launcher runs `share/tuxoffice/root-certs.exe` (`lib/root-certs.c`)
  whenever the host's CA bundle or the helper changes, so even that one
  import happens outside Office.
- Wine's crypt32 tests (15 suites) give the same results as before; the
  overlay now carries crypt32 built from source (GnuTLS enabled).

After the change: six multi-app runs, Excel cells and three scatter runs
without any fault; suite, chart and spelling tests pass.

## Installed 26 October 01:47: desktop file chooser for Open and Save As

Backups: `recovery-backups/desktop-file-dialog-upgrade-20261026-014743/`.

Office's Open, Save As and Browse dialogs (the common item dialog,
`IFileDialog::Show`; Office also adds a Tools menu and file type
callbacks) now show the desktop's own file chooser through
xdg-desktop-portal: on Plasma the KDE dialog with Dolphin's places, on
GNOME the GTK one. Any Linux folder can be used.

- `lib/file-dialog.c` (`bin/tuxoffice-file-dialog` in the runner, built by
  `runner/build.sh` with libsystemd's sd-bus) calls
  `org.freedesktop.portal.FileChooser` with the title, accept label, name,
  folder and Office's filters, and writes the answer to a file.
- comdlg32 runs it with `__wine_unix_spawnvp` when `TUXOFFICE_FILE_DIALOG`
  names it (the launcher sets it; `TUXOFFICE_FILE_DIALOG=wine` keeps Wine's
  dialog), keeps pumping messages with the owner disabled, then builds the
  results, file type index, `OnTypeChange` and `OnFileOk` as its own dialog
  does. If `OnFileOk` refuses the choice the chooser reopens. Without a
  portal it falls back to Wine's dialog.
- Linux paths come back as `Z:\...` (or `C:\...` inside the prefix). Inside
  Office, App-V hooks the file calls `wine_get_unix_file_name` makes and it
  fails; comdlg32 uses ntdll's `wine_nt_to_unix_file_name` instead.
- When Office proposes a folder of the prefix's Windows profile
  (`drive_c/users/NAME/Documents` and so on), the chooser opens at the
  matching Linux folder (`user-dirs.dirs`). Files saved there before remain in
  the prefix and can still be reached by browsing.
- Office offers several file types for `*.docx`; Plasma's dialog picks the
  default filter by its patterns, so repeats get a never-matching marker
  pattern (`*.tuxoffice-type-N`) and the right type is preselected.
- Not covered: the legacy `GetOpenFileName`/`GetSaveFileName` dialogs (Office
  does not use them for documents), Office's Tools menu (Save Options and
  similar) in the Save As dialog, and the dialog is not attached to the
  Office window as a Wayland child (Office's window is disabled while it is
  open).

Verified in the isolated session: Word, Excel and PowerPoint Save As into a
Linux folder outside the prefix (valid DOCX/XLSX/PPTX written) and Open of a
Linux file (`tests/file-dialog-probe.c` for the API; the app scripts drive
the KDE dialog with `tests/eis-type.py`). Suite and multi-app tests pass.

The isolated test session's D-Bus used to inherit the live desktop's
`WAYLAND_DISPLAY`, so services it activates (this portal) opened their
windows on the live desktop. The first chooser test did that briefly; it was
closed by killing the isolated bus's portal processes. `isolated-session.sh`
now starts that bus without any display and hands it the nested one.

## Installed 26 October 01:10: COM apartment race, message box Close

Backups: `recovery-backups/com-msgbox-upgrade-20261026-010952/`.

- **Excel/Word "hang" fail-fasts.** The intermittent `0xe0000002` crashes
  (about one multi-app run in three, on the old runner too) are Office's hang
  watchdog: the UI thread waits INFINITE (alertable) in an `mso.dll` wait
  helper and an APC from `mso30win32client` fails it fast. In the Excel case
  the thread that should have answered had crashed first in Wine's combase:
  `rpc_execute_call` dereferenced a NULL apartment. An apartment thread had
  called CoUninitialize while a COM call was still queued for its apartment
  window (kept alive by the RPC worker's reference), then pumped messages.
  Now CoUninitialize dispatches calls already queued for the STA (Windows
  documents this), and calls reaching a thread that left its apartment fail
  with RPC_E_DISCONNECTED. `tests/com-uninit-call.c` reproduces the queued
  case (hung forever before; S_OK now). Wine's ole32 `marshal` (1240 tests,
  0 failures) and `compobj` (same 19 pre-existing failures as the old runner)
  conformance tests match. Six multi-app runs afterwards: five clean, one
  Word watchdog fail-fast with a different trigger (below).
- **Remaining watchdog trigger (not fixed):** a wininet/crypt32 worker thread
  faults inside App-V's `NtClose` hook (`RegCloseKey` → App-V →
  heap trampoline → NULL call); the UI thread then waits forever. It is
  App-V-internal and intermittent. Separately, Wine's ntdll lacks
  `RtlEnumerateGenericTableAvl`, `RtlIsGenericTableEmptyAvl`,
  `RtlIsNameInExpression` and `NtQueryDirectoryFileEx`, which App-V
  resolves dynamically, and its whole AVL generic-table API is stubbed
  (`RtlInsertElementGenericTableAvl` even returns no pointer). Office did not
  call those stubs in a 30 s Word start (no FIXMEs), so they are not proven
  to matter.
- **Message boxes** now follow Windows: OK-only boxes return IDOK when closed
  or on Escape, Yes/No and Abort/Retry/Ignore boxes have a disabled Close
  button and ignore Escape (Wine returned IDCANCEL, which callers do not
  expect). `tests/msgbox-close.c` checks this.
- `tests/multi-app-smoke.py` clears Office's Resiliency keys first, so one
  crashed run no longer leaves the next one stuck at a safe-mode prompt.

Battery on the installed build: suite, Excel cells, PowerPoint ribbon,
spelling card and embedded chart passed; the Word ribbon-click test passed
on rerun after one 700 ms ping timeout (idle message wait in the snapshot;
the known intermittent stall).

## Installed 25 October 21:18: Windows 11 caption buttons and frames

Office dialogs (Font, Find and Replace, Paragraph, Save As, message boxes)
use Wine's non-client drawing, which drew Windows 95 raised caption buttons
and 3D frame edges. When the prefix's visual style is active (ThemeActive,
which setup writes), win32u now draws what Windows 10/11 do:

- flat caption buttons 46 px wide at 96 DPI (58 px at 120), with thin
  antialiased close, minimize, maximize and restore glyphs; hit testing,
  caption text layout and drawing share one width;
- hover highlights (red close, grey others) from WM_NCMOUSEMOVE and
  non-client mouse tracking, and pressed states;
- a uniform one-pixel border instead of the raised edge, one-colour
  resizable frames and no caption separator line.

Classic drawing is unchanged without a visual style and for tool windows.
`tests/caption-buttons.c` checks the button widths, hover, leave and the
restore glyph; it passes, and Word's dialog audit shows the new captions.

Word typing was measured again with the capture area on the insertion line:
about one visible update per keystroke at 80 ms input spacing, first update
about 50 ms after the first key. The earlier poor result measured lines below
the insertion point, which change only when text wraps. Isolated captures
also pause for up to about one second with no Office process running (four
pauses over 200 ms in 20 s), so long gaps in isolated samples are capture
stalls, not Office.

## Installed 25 October ~21:00: Windows Installer components (Outlook starts)

Outlook, Access and Publisher were tested for the first time (Publisher and
Outlook on the offline reflinked copy of the live prefix).

- **Outlook failed with "Cannot start Microsoft Outlook."** Click-to-Run's
  App-V layer makes every component of its integration manifests visible to
  Windows Installer; under Wine only the 774 published ones exist, and 1,678
  of 1,742 manifest components were unknown to MSI. `lib/c2rcomponents.py`
  registers each manifest component whose key path exists (1,575; 103 have no
  key path in this installation) under the Click-to-Run product, in the form
  the published ones use. Wine's `MsiGetComponentPath` also treated registry
  key paths ("20:\\Licenses\\...") as files, so such components were always
  absent; `dlls/msi/msi.c` now checks the registry for them. After both,
  Outlook starts and shows its normal first-run account setup. Setup and
  post-install run the repair; `tuxoffice repair` reapplies it. Applied to
  the live prefix (registry backup in `recovery-backups/installer-repair-*`).
- **Access** shows "Automatic configuration of the current version of
  Microsoft Access has failed..." at every start, then works: a blank
  database was created and its table opened. The MSI queries Access makes now
  all succeed, and its COM, DAO and ACE OLE DB registrations are present; the
  failing check is not identified yet (no public symbols for MSACCESS.EXE,
  the message text is not stored uncompressed in any file).
  Later findings (isolated prefix): the warning is Access's own dialog
  (dialog procedure in MSACCESS.EXE at image offset 0x4EAC60), created right
  after `MsiQueryFeatureStateW(ACCESSFiles)`; that feature and every component
  Access queries just before resolve as installed locally (state 3). Linking
  Click-to-Run's virtual `Common Files\Microsoft Shared\Office16` into the
  real Common Files tree (where Office's own path lookups of
  `mso20win32client.dll` fail under Wine, since App-V's file redirection
  does not reach Wine's loader) did not remove it either. A relay trace once
  showed MSACCESS.EXE displaying "IOPL not enabled" (Windows' error 197 text)
  after that failed lookup; normal starts have not shown it.
  To stop Access asking about safe mode after a killed test run, delete
  `HKCU\Software\Microsoft\Office\16.0\Access\Resiliency`.
- **Publisher** opens behind "Sign in to get started with Publisher" in both
  prefixes; "Skip for now" is offered. Likely licensing (Publisher is being
  retired by Microsoft in October 2026); not investigated further.
- PowerPoint's Design-tab theme gallery is still sometimes blank for about
  two frames on its first visit (pre-existing, intermittent).

## Installed 25 October 20:22: ClearType, Windows UI look, OneNote caret

Installed (backups in `recovery-backups/cleartype-ui-upgrade-20261025-195938/`
and `recovery-backups/caret-upgrade-*`), after the full regression battery:

- GDI text follows the prefix's ClearType setting (see below); Microsoft
  Tahoma, Windows 11 caption colours and regular-weight Segoe UI applied to
  the live prefix with `tuxoffice ui`.
- **OneNote caret.** OneNote draws its caret as a white nine-grid image with
  CompositeMode DestinationInvert. Only solid colours were inverted, so the
  white caret was drawn white on white. Image brushes in DestinationInvert
  are now drawn with their colours inverted (alpha kept), exact over white and
  black like the colour case; the inverted bitmap is cached so redraws cost
  about 1-2 ms instead of 8 ms. OneNote also stopped blinking almost at once:
  Office reads `CaretTimeout`; setup now writes Windows' defaults
  (CaretTimeout 5000, CursorBlinkRate 530) and win32u implements
  SPI_GET/SETCARETTIMEOUT. Verified offline on a reflinked copy of the live
  prefix (network namespace, `~/.cache/tuxoffice-validation/iso-live`): the
  caret is visible and blinks; Word's caret still blinks.

## (Superseded) built but not installed (25 October ~17:15): text rendering and UI look

In the validation runner only (`~/.cache/tuxoffice-validation/home/runners`):

- win32u/font.c: when the prefix sets FontSmoothing, Windows' ClearType
  setting decides GDI antialiasing instead of the host fontconfig default
  (grayscale without subpixel order), so GDI text such as Office's context
  menus gets ClearType as on Windows. Verified with a GDI probe and the Excel
  context menu; the full regression battery has NOT been rerun yet.
- tuxoffice: `fonts tahoma` (Microsoft Tahoma from the checksum-pinned
  IELPKTH.CAB; bsdtar/7z/cabextract), `configure_windows_look` (Windows 11 light
  caption and highlight colours, ThemeActive), new `tuxoffice ui` command;
  lib/office-ui.c sets regular-weight Segoe UI 9pt for all non-client fonts
  and the icon title font. Applied to the isolated prefix only.

Findings: Excel's formula bar Consolas font is Excel's own
`UseMonospaceFontForAllFormulas` feature with the genuine Consolas. The Excel
start-page template thumbnails are raster images scaled for 125%. Word's
Font/Find dialogs and Save As are system-DPI aware at 120 DPI and render
sharp; their caption buttons are still Wine's classic style.

OneNote caret (open): reproduced offline with a reflinked copy of the live
prefix (`~/.cache/tuxoffice-validation/iso-live`, network namespace so nothing
syncs). Typing works but no caret appears. Its caret-like composition visuals
are 1px-wide sprites with surface brushes (and hidden 1x38 visuals); next step
is to find which visual is the caret and why it does not draw (surface brush
content or visibility toggling), as was done for Word's DestinationInvert caret.

## Installed 25 October 16:38 (shadow and rendering upgrades)

Two further runner updates are installed (backups and checksums in
`recovery-backups/shadow-move-upgrade-20261025-153549/` and
`recovery-backups/render-skip-upgrade-20261025-163801/`).

**Misplaced frame shadows after resizing with KDE's Meta+right-drag.** A
subsurface's position was only sent to the compositor with its next buffer.
During an edge resize Office moves the far-edge shadow strip without resizing
or repainting it (SetWindowPos with SWP_NOSIZE), so the strip stayed at the
old edge, e.g. 120 px away from the window. `WAYLAND_WindowPosChanged` now
applies a moved-only subsurface's position immediately; resizes keep the
existing path where position and new buffer arrive together. New test
`tests/shadow-resize.py` drives real interactive resizes through KWin's EIS
input (`tests/eis-input.c`, refuses to run outside the isolated session) over
a white backdrop (`tests/backdrop.qml`) and checks the shadow on all edges:
before 7 of 24 resizes misplaced a strip (every edge-only resize); after,
0 of 48 in Word and 0 of 16 each in Excel and PowerPoint.

**Composition rendering cost.** `uiperf` (now in microseconds) showed every
commit redrawing all composition targets: the unchanged ribbon cost 7.5 ms per
frame and the document view 9-10 ms, of which most visuals were words in the
prefetch band outside the view. Changes in `windows.ui/composition.c`:

- Each target keeps a snapshot of its rendering inputs (visual properties,
  child lists, color/surface/nine-grid brushes, inset clips, drop shadows,
  and a content version on drawing surfaces). An unchanged target keeps its
  last presented frame. Expressions, effects, gradients and geometric clips
  count as changing, so the shortcut can only redraw more, never less.
- A visual whose inset clip, transformed as Direct2D applies it, misses the
  target is skipped with its subtree; its drawing is confined to that clip.

A pixel comparison (`tests/render-compare.py`) of the same Word scroll
position renders identically apart from the file name and the per-session
avatar colour. Commit cost during fast Word scrolling fell from 17.5 to 6.3 ms.

Matched fast scrolling (isolated session, 360 wheel events at 16 ms, 3 rounds):

| App | Installed before | Now | First visible change |
| --- | ---: | ---: | --- |
| Word, 1000 paragraphs | 25.9 changes/s | 40.5 | 37 -> 23 ms |
| PowerPoint, 250 slides | 31.9 | 43.7 | 105 -> 47 ms |
| Excel, 50,000 rows | 34.0 | 35.4 | similar |

The full regression set passed on this build (suite 36/36, Excel cells,
Word and PowerPoint ribbons with 0 flash/blank frames, scatter 3/3, chart
editor, spelling card, three-app smoke test).

Still open: the GPU child resize race (about one oversized frame per ribbon
collapse/expand, measured with `office-flicker.py word collapse`), Excel's
scrolling, which does not go through this composition path, and no Windows
baseline.

## Installed 25 October 15:05 (tracker-thread upgrade)

The candidate runner is installed: GPU-child drawing fix plus the
InteractionTracker thread fix below. Previous runner:
`runners/tuxoffice-11.0-33-experimental-x86_64.previous-20261025-150545`;
record and checksums: `recovery-backups/tracker-thread-upgrade-20261025-150545/`.
Changed files: windows.ui.dll (both arches), winewayland.so, two rebuilt
helpers. Protocol 932 and DXVK unchanged; no prefix update needed. Running
Office processes keep the old code until restarted.

Validation in the isolated session (matched to the 2560x1600 1.25 panel):

- Functional suite: Word 11, Excel 17, PowerPoint 8 checks passed.
- Excel 120 cell entries (3 rounds): no flash frames, values saved.
- Recommended Charts scatter 3/3, PowerPoint chart edited in full Excel,
  spelling card suggestion applied and saved, three-app smoke test: passed.
- Word ribbon, clicked tabs, 45 changes: 0 flash frames (installed: 17).
- Word ribbon, fast keyboard, 300 changes: no crash, no flash frames.
- Fast Word scrolling (360 wheel events at 16 ms, 4 launches x 3 rounds):
  candidate 0 crashes; installed crashed in 4 of 4 launches
  (`DispatcherQueueTimerWinCompWin32::Stop` from
  `CompositorWinCompWin32::ThreadProc`, then Office's 0xe0000002).
- Scrolling at 50 ms spacing is equivalent between runners (Word ~18.3,
  Excel ~19.4, PowerPoint ~20.7 visible changes/s; both near the input rate).
  At 16 ms: Word 25-27, Excel ~34 changes/s on the candidate. The 50-slide
  PowerPoint fast run hit the deck end and is invalid; a 250-slide fixture is
  in `~/.cache/tuxoffice-validation/runs/iso-fixtures/` for the next run.

Not done: PowerPoint ribbon flicker rerun (fixture path bug in the batch),
GPU child resize race fix, Windows baseline for the 1 px Insert-tab shift.

## Word ribbon-switch layout failure (fixed, installed 25 October 15:05)

Cause, established with `tests/prefetch-transaction-guard.gdb`:
`AirSpace::BackEnd::PrefetchScope` (embedded in `ScrollingLayer` at +0x68)
keeps a pending-transaction pointer at +0x60. `Add`, `Remove`, `Update` and
`Moved` each save it, point it at a stack local, and restore it on exit, with
no lock. Office assumes these calls are serialized on the compositor thread.
Wine's `windows.ui` delivered `IInteractionTrackerOwner` callbacks
(`ValuesChanged`, `IdleStateEntered`, `InertiaStateEntered`) on thread-pool
threads. Office's `ScrollingLayerWinComp::CurrentViewportOffsetChanged` then
called `PrefetchScope::Moved` from a `wine_threadpool` thread while Word's UI
thread was inside `Moved`/`Update` on the same scope. An interleaved restore
wrote back a stale value (uninitialized heap text such as `0x7d003800310045`),
and the UI thread's next set insertion faulted. The guard recorded the
overlapping threads, their stacks, and then the bad set on the UI thread.

Fix (`dlls/windows.ui/composition.c`, `interactions.c`): a Compositor records
its creating thread's DispatcherQueue, as on Windows, and tracker owner
notifications are delivered in batches through that queue, keeping the
existing frame-notification coalescing. The thread pool is used only when no
queue exists. A new composition test checks that `ValuesChanged` is not
delivered before the compositor thread pumps messages and then runs on that
thread. It fails with the old DLL (callback on a pool thread) and passes now:
246 composition checks, 0 failures.

Word, candidate runner with this fix, guard attached: 300 ribbon changes,
zero overlapping scope transactions, no bad prefetch set, no crash. Before the
fix the same guard reported overlaps within the first rounds and the fault.

Test harness corrections found in the same work:

- The Alt+KeyTip ribbon benchmark often misses KeyTip mode and types the tab
  letters into the document ("nhpnhp…"). Earlier marker offsets of 27/55 px
  and late-round "flashes" were this text pushing the marker, not rendering
  faults. `office-flicker.py` now clicks tabs located through MSAA
  (`typer.exe ribbon-click-bench`); `TUXOFFICE_RIBBON_INPUT=keys` keeps the old
  method. The one-pixel page jitter must be re-measured with clicks.
- GUI stress runs take keyboard focus on the live desktop; typing elsewhere
  during a run can land in the test document.

### Isolated test session

GUI runs now use `tests/isolated-session.sh` (virtual KWin with its own D-Bus,
reflinked disposable prefix) and `tests/isolated-run.py`, so they do not take
over the user's desktop. Three corrections make it match the live session:

- `kwin_wayland --virtual` multiplies the mode by `--scale` and leaves the
  output at scale 1. The script now asks for 2048x1280 logical and applies
  1.25 through `kscreen-doctor` on `Virtual-0`: 2560x1600 physical at 1.25,
  identical to the laptop panel.
- The nested KWin chose NVIDIA EGL while Wine renders on Intel. NVIDIA did not
  wait for Intel's implicit fences and captures showed torn, half-written
  frames. The nested KWin now uses Mesa EGL (Intel), as the live session does.
- Under Wayland, `SetCursorPos` does not decide where an injected click lands.
  `typer.exe` moves the pointer with an absolute `SendInput` event first.

### One-pixel page movement

Not a compositor or surface-placement fault. GPU child geometry is identical
on every tab (`uistack` trace: document view at logical 3,181, 2042x1074).
Word itself lays out the Insert tab's ribbon 1 px shorter (ribbon 221 instead
of 222 physical px; document view 1344 instead of 1343). Accessibility rects
show why: every group on Home/Layout is 120 px tall because their 20x20
dialog-box launchers (y 204-224) set the label row height; Insert has no
launchers, so its label row is the text line height and groups are 119 px.
Wine's Segoe UI metrics match the font tables and Windows rules (DirectWrite
natural 19.95 px, GDI-compatible 20.00 px; GDI tmHeight 20 = 16 + 4 at
-15 px), so the difference is NetUI's own rounding of the label line. Whether
native Word also moves 1 px on Insert at 125% has not been checked; a Windows
measurement would settle it.

### GPU child resize race (not fixed)

When the ribbon collapses or expands (double-click a tab, Ctrl+F1), the
document view's GPU subsurface can show its new size at its old position for
a frame: the captured window grew 94 px and the page extended below the status
bar. The present path re-reads window geometry each present; a present during
a multi-window SetWindowPos batch can see the child's new size before its
parent's new position. A real fix needs subsurface position and buffer to be
applied together (synchronized commit or deferring presents during a batch).
Clicks closer together than the double-click time triggered this in tests;
the click benchmark now spaces clicks 650 ms apart.

### Matched results in the isolated session (Word, clicked tabs)

| Runner | Tab changes | Document flash frames | Page offset |
| --- | ---: | ---: | --- |
| Installed | 45 | 17 (6, 7, 4) | 0 / -1 px by tab |
| Candidate | 45 | 0 | 0 / -1 px by tab |

Fast keyboard switching, the original crash reproduction: candidate passed
10 rounds (300 changes) without a debugger, with no unhandled exception and no
flash frames.

## Verification

- Composition tests: **232 checks, zero failures**. The new visual-offset
  CommitNeeded regression fails before the notification fix and passes after it.
- Shell stream tests: **20,881 checks, zero failures**, one existing expected
  failure for memory-stream cloning.
- Earlier Word resize runs selected a hidden helper window. Their advertised
  12 resize/maximize cycles do **not** establish visible-window resize coverage.
  With the corrected visible-window helper and final graphics bundle, three
  desktop runs passed 24 visible resize/maximize cycles before the latest
  Wayland change. The latest build passes eight more cycles with window
  effects enabled, four in the clone and four in the regular prefix, with
  spelling cards between them. The harness rejects caught graphics access violations;
  checking only for an unhandled exception missed earlier renderer faults.
- A 500-frame concurrent Direct2D/D3D rendering regression produced 104
  corrupted frames with the old draw transaction and the locally built DXVK.
  It produces zero corrupted frames with the synchronization fix, on both
  64-bit and 32-bit builds.
- Excel's exact Recommended Charts → third recommendation (Scatter) sequence
  passed three desktop runs at 179% zoom. Each run verified chart type, saved
  and inspected the XLSX, and polled responsiveness after insertion.
- A destroyed-window D3D11 swapchain test crashes before the lifetime fix and
  survives all four destruction/present cycles afterward.
- A two-process DPI test reproduces unsigned coordinate overflow before the
  server fix and gets all four expected signed coordinates afterward.
- The targeted Direct2D custom-effect test passes the new callback-order
  assertion. One pre-existing unsupported custom-shader rendering failure
  remains (34 checks, one failure, one expected failure).
- Excel's object-model round trip passed 10 assertions: formulas, four chart
  type changes, 100 scatter points, XLSX save/reopen, and PDF export.
- PowerPoint created and reopened five slides with text and shapes, ran a
  slideshow, exported PDF and quit successfully after the COM cleanup fix.
  All three presentation assertions and shutdown now pass.
- Wine marshaling tests: **1240 checks, zero failures**, 35 existing expected
  failures, with both the control and corrected COM cleanup.
- A two-process COM regression disconnects an STA object during an RPC. Before
  the fix, three interface releases run on the wrong thread. After the fix,
  all releases run on the owning thread, and the child's call succeeds.
- The printer helper was tested with CUPS unavailable in a disposable prefix:
  installation succeeds, and repeating it preserves the configured default.
- An automated click changed `consectetur` to `nonsecretor`. The test saved a
  DOCX, verified its XML, reopened it in Word, and verified the correction.
- With all three apps open together, six app switches and scrolling checks
  pass. All apps remain responsive, the renderer-fault scans pass, and the
  clean Quit calls succeed. Outputs: `/tmp/tuxoffice-multi-app-normal/`.
- The combined graphics build passes 21 object-model assertions across Word,
  Excel and PowerPoint. The harness additionally inspects saved packages,
  extracts and verifies PDF text, and confirms all three processes exit after
  Quit. Outputs: `/tmp/tuxoffice-suite-wayland-final/`.
- Expanded tests pass **36 assertions**: 11 Word, 17 Excel and eight PowerPoint.
  They add tracked changes, sorting, copied formulas, conditional formatting,
  an editable slide table and an embedded scatter chart. Saved OLE Package
  data is checked independently. Outputs: `/tmp/tuxoffice-suite-expanded-final/`.
- A full Excel chart-editor UI test passes nine PowerPoint assertions and
  independently verifies that an actual cell edit to 74 survives saving the
  presentation. It waits for the full Excel window, uses Alt+F4 to close it,
  rejects unhandled exceptions and checks that both apps exit after Quit.
  Outputs: `/tmp/tuxoffice-embedded-chart-ui-final/`.
- Five final spelling-card timing runs show a visible popup in 77–199 ms and final
  pixels in 490–946 ms (median 588 ms), with window effects enabled. These are capture-based upper bounds;
  the saved final images were inspected and include all three action icons.
- A wide-area typing trial retained all 120 injected characters, with its
  first visible update at 11 ms. Pixel changes include caret and text reflow,
  so this does not establish a latency for each character. An earlier crop
  missed the first line and is excluded from performance conclusions.
- Tests use a cloned Office prefix. Recent chart and scrolling tests run on
  the actual desktop at 1.25 scale. The earlier isolated KWin output has a
  different geometry/scale and is insufficient for desktop performance claims.
- Repeated hard-killing of Word caused recovery/session-restore startup
  delays. The harness now attempts a normal COM Quit before stopping Wine
  and records readiness attempts and failure summaries.

Useful artifacts from this run are under `/tmp/tuxoffice-*`; these are
temporary, not repository fixtures. The restored card screenshot is
`bench/suggestion-card-2026-10-25.png`. The functional test output is
`/tmp/tuxoffice-spelling-regression-clean-quit/`.

## Running the automated tests

Start an isolated display, keeping that terminal/process running:

```sh
tests/isolated-session.sh "$HOME/.cache/tuxoffice-test"
```

In another terminal:

```sh
tests/isolated-run.py "$HOME/.cache/tuxoffice-test" \
  tests/spelling-functional.sh /tmp/tuxoffice-spelling-test
tests/isolated-run.py "$HOME/.cache/tuxoffice-test" \
  tests/hang-hunt.sh /tmp/tuxoffice-resize-test resize-popup 20
```

The helpers are built by `tests/build-helpers.sh`. The default test document
is `~/Documents/TuxOffice-test/render-test.docx` and must contain the
misspelling `consectetur`. Tests require KWin, qdbus6, gdb, winegcc, and the
Python dbus and Pillow modules. They intentionally edit only the isolated
test document and save results to fresh output filenames.

## Performance and remaining work

The final graphics build was measured on the actual desktop at 1.25 scale.
Three rounds per app used 120 wheel ticks at 50 ms spacing. These numbers
measure changes in a captured region, including capture overhead, rather than
whole-window FPS or a latency for every input.

| App and document | Median first visible change | Visible changes/second | Median longest interval |
| --- | ---: | ---: | ---: |
| Word, 1000 paragraphs | 62 ms | 18.51 | 134 ms |
| Excel, 50,000 rows | 77 ms | 20.00 | 97 ms |
| PowerPoint, 50 slide thumbnails | 102 ms | 18.50 | 100 ms |

PowerPoint's earlier built-in graphics result was 4.64 changes/second and a
600 ms median longest interval. The combined graphics build improves those
measurements by about 4.0 times and 6.0 times respectively. Final built-in
versus DXVK Excel measurements are similar in responsiveness, with built-in
D3D using slightly less CPU in this workload.

A same-process Word test with two documents open passed three rounds with
18.01 changes/second and a 109 ms median longest interval. The harness now
checks the Wine foreground title, synchronizes document stacking and verifies
the document under the pointer. Merely raising a KWin window could leave
synthetic wheel events targeting another Word document. Earlier zero-update
control runs did not verify routing and are inconclusive. A corrected three-round
comparison using built-in D3D in both runners shows 11.48 versus 18.14 changes
per second, and a median longest gap of 1044 versus 169 ms, for the older
versus updated Wine builds. Median first updates were 98 versus 52 ms. This
measures the combined Wine changes. The CommitNeeded unit regression also
passes independently.

Higher input rates still expose limits. At roughly 60 wheel ticks/second,
Word and Excel showed 27 and 32 region changes/second in one run each. Three
PowerPoint runs with 250 imported slides showed 15.76–18.42 changes/second
and longest intervals of 168–295 ms. The deck's imported slide count was
verified through COM. An earlier 50-slide high-rate run reached the scrolling
boundary and is excluded. Further performance work is needed under this load.

Excel also completed a 600,000-formula workload, five full recalculations,
and a 200,000-cell readback. Recalculation took 0.40 seconds total after the
GPU environment fix. Both the readback checksum and SUMPRODUCT result match
independent Python sums. This default COM-launched trial now selects Intel,
  matching desktop scrolling; the earlier NVIDIA trial took 0.38 seconds.

Measurement data is retained in `bench/scrolling-2026-10-25.json`,
`bench/excel-calc-2026-10-25.json` and `bench/functional-2026-10-25.json`.
Earlier initial-latency figures that mixed CLOCK_MONOTONIC and
CLOCK_MONOTONIC_RAW are invalid. The corrected measurements use RAW, matching
Wine GetTickCount64. No matched Windows baseline exists; Windows parity is
unverified.

Remaining limitations:

- Card border/shadow appearance still needs refinement. The action icons are
  visible, the separate shadow window is fixed, and applying and saving a
  suggestion passes. Native Windows visual parity has not been established.
- A second resize crash was traced to DXVK resource-allocation pool corruption.
  External image/buffer import now takes the same allocator lock as allocation
  and release. A public-API concurrent-import test crashes on both architectures
  before the patch and passes three trials per architecture afterward: 480,000
  imports plus 480,000 ordinary buffers, with zero failures. This distinguishes
  the new fix from the earlier context-state lock. Both are bundled and installed.
- A window-order cache reduced API loop overhead but did not materially improve
  scrolling in quiet, matched desktop trials. That experiment was removed.
- Rapid ribbon switching triggered an intermittent Office background-layout
  access violation in `PrefetchAxis::Add`. Root cause found (see below); the
  fix is in the candidate runner only, together with the GPU-child change.
- Speech is deferred at the user's request unless it blocks functionality.
- One updated built-in D3D round still had a 1237 ms scrolling gap. The
  latest default DXVK Excel trial had a 340 ms gap in its first round;
  intermittent stalls are not considered eliminated.
- An automation sequence that calls Workbook.Close on a workbook reference
  obtained before the chart transitions into full Excel can crash Excel at
  EXCEL.EXE+0x15a61e5. Waiting longer instead leaves that reference disconnected.
  Closing the actual full editor through its UI passes, as does the embedded
  grid workflow. The automation failure reproduces with shadows disabled and
  older COM/Wayland implementations; native Windows behavior is unverified.
- The Direct2D custom-effect test still has one pre-existing unsupported
  custom-shader rendering failure; the new draw-info ordering check passes.
- Broader feature coverage remains necessary. The 21 object-model checks and
  targeted interactive regressions cover selected workflows in Word, Excel
  and PowerPoint, rather than every Office feature or app.
- The no-printer setup fallback passes independent tests, but a completely
  fresh Office installation, activation and font provisioning have not been
  re-run from an empty prefix in this session.

This is a working compatibility setup under development, not a claim that
every Microsoft Office feature works on Linux.
