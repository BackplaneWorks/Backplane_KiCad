# Embedding the KiCad GUI in a host window

`kicad --embedded` runs the KiCad project suite (one process, one KIWAY: PCB editor, schematic
editor, symbol and footprint editors, 3D viewer) with every frame as a native child window of a
host application's window. The host controls it over the normal API socket with the fork-only
commands in `api/proto/backplane/embed.proto` (package `kiapi.backplane.embed`). Nothing here is
specific to one host. Supported: X11 (including XWayland with `GDK_BACKEND=x11`); Windows
compiles the same path (`WS_CHILD` + `SetParent`) but is not yet verified. macOS and native
Wayland refuse `--embedded`.

## Command line

```sh
kicad --embedded --parent-window <container> --toplevel-window <host-toplevel> \
      --api-socket <path> --parent-pid <pid> <project>.kicad_pro
```

| Flag | Meaning |
|---|---|
| `--embedded` | Project-suite mode with no project manager window, no first-run wizard, no update/PCM checks, no session restore, no splash. Cannot be combined with `--frame` or `--mergetool`. Fails (exit 1, message on stderr) without X11/Windows. |
| `--parent-window <id>` | The container: an X11 window with the **root visual (24-bit)**, or an HWND. Decimal or `0x` hex. Optional: without it the frames start parked (see `AttachToParent`). |
| `--toplevel-window <id>` | The host's top-level window. Dialogs are transient for it and centred on it. Defaults to the container. |
| `--api-socket <path>` | Serve the API at this path whatever the user's `api.enable_server` preference (it also stays on if that preference changes). An existing live socket, or any bind failure, is an error (exit 1, `kicad: cannot serve the API at ...`); it never falls back to `api-<pid>.sock`. Also usable without `--embedded`. |
| `--parent-pid <pid>` | Polled every 500 ms. When the process is gone KiCad exits at once, as after a crash: nothing is saved or discarded; autosave and local history are kept and offered on the next open. |

When ready, KiCad prints (stdout, flushed):

```text
KiCad embed events at ipc://<path>.events
KiCad API server listening at ipc://<path>
```

The second line is the same readiness line as `kicad-cli api-server`; read lines until it.

## Window tree (X11)

```
host top-level (WM-managed)
└── container            (the host's, root visual, --parent-window)
    └── holder           (KiCad's, override-redirect, follows the container's size)
        ├── PCB frame    (GTK top-level, reparented, never seen by the WM)
        ├── SCH frame
        └── ...
```

- **Born embedded.** `EDA_BASE_FRAME::Show()` realizes each frame before its first map and
  reparents it into the holder unmapped, so the window manager never sees a top-level. This
  covers frames opened outside `KIWAY::Player` (the 3D viewer) and the library editors' second
  `Show()`. Frame sync and the GTK `:backdrop` state are disabled for embedded frames; KiCad's
  minimum size hints are cleared (and reported in `FrameInfo.min_width/min_height`).
- **The project manager** is never shown. Modal frames (the footprint and symbol choosers) stay
  WM-managed and behave as dialogs.
- **Frames start hidden.** A `Show(true)` or `Raise()` that KiCad itself makes on a hidden frame
  (Edit Symbol, Edit Footprint, the 3D viewer, cross-probing) does not map it: it posts
  `ET_SHOW_REQUESTED` and the host decides with `ShowFrame`. `Raise()` on an embedded frame
  never maps or restacks it.
- **Geometry is the host's.** The host moves and resizes frame windows inside the container
  (holder coordinates equal container coordinates) with `XMoveResizeWindow`, or passes the
  rectangle in `ShowFrame`. The host may also map and unmap a frame window directly for instant
  switching once it has been shown by `ShowFrame`; `HideFrame`/`ShowFrame` do the same through
  KiCad.
- **The host must put the holder in its save-set**, preferably with
  `XFixesChangeSaveSet(dpy, holder, SetModeInsert, SaveSetRoot, SaveSetUnmap)` (the holder id
  is in `AttachToParentResponse` and `GetFramesResponse`). If the host's connection closes, the
  X server then moves the holder to the root, unmapped, instead of destroying it with the
  container. KiCad notices (`ReparentNotify`), keeps it parked (override-redirect, so no WM
  sees it), posts `ET_DETACHED`, and keeps every frame, document and undo stack until the next
  `AttachToParent`. A core `XAddToSaveSet` also works when the host's top-level is not
  reparented by the WM; under a reparenting WM the server puts the holder in the WM's frame,
  and KiCad moves it to the root at once, racing the WM's cleanup. Without a save-set entry a
  host crash destroys KiCad's windows and KiCad exits.
- **Moves without a resize** are the host's to announce: send each mapped frame a synthetic
  `ConfigureNotify` with its new root position (ICCCM 4.2.3), or popups open at stale positions.
- **Dialogs** (every WM-managed GTK top-level that is not an embedded frame and would be
  transient for an embedded frame, the hidden manager or nothing) get `WM_TRANSIENT_FOR` = the
  host top-level and are centred on it when mapped. A dialog over another dialog keeps its
  parent. `ET_DIALOG_OPENED`/`ET_DIALOG_CLOSED` carry the title in `detail`.
- AUI panes cannot float in embedded frames.

On Windows the container is used directly (no holder): frames lose `WS_CAPTION`/
`WS_THICKFRAME`, gain `WS_CHILD | WS_CLIPCHILDREN` and are `SetParent` to it; dialogs are owned
by the top-level HWND.

## Focus and reserved keys

- **Click to focus.** A button press in an embedded frame that does not have the keyboard calls
  `XSetInputFocus` on the frame (`SetFocus` on Windows) and sends the container
  `_BACKPLANE_FOCUS`. The click is not consumed. `ET_FOCUS_IN`/`ET_FOCUS_OUT` follow the frame's
  activation.
- **Reserved chords** are forwarded instead of handled, in embedded frames only (dialogs keep
  every key). Default `Ctrl+K`, `Ctrl+J`, `Ctrl+Tab`, `Ctrl+Shift+Tab`, `` Ctrl+` ``; change with
  `SetReservedKeys` (chord syntax in the proto).

ClientMessages go to the **container** (`--parent-window`, the current `AttachToParent.window`),
format 32, sent with an empty event mask, so they reach the X connection that created the
container (nothing is sent while detached):

| Atom | `l[0]` | `l[1]` | `l[2]` | `l[3]` | `l[4]` |
|---|---|---|---|---|---|
| `_BACKPLANE_FOCUS` | frame window | 1 (took focus) | `FrameKind` | 0 | 0 |
| `_BACKPLANE_KEY` | frame window | X keysym (lower-case for letters; `Tab` is `0xff09` also with Shift) | X modifier mask (Shift 1, Control 4, Mod1 8, Mod4 0x40) | index in the reserved list | 1 (press) |

Windows: `PostMessage(container, WM_APP + 0x101 /* focus */ or WM_APP + 0x102 /* key */,
wParam = l[3], lParam = l[0])`.

## API (`kiapi.backplane.embed`)

| Command | Response | Notes |
|---|---|---|
| `AttachToParent {window, toplevel, scale}` | `{holder, frames}` | Re-parents the holder (and so every frame) under `window`; `window` 0 parks everything unmapped. `toplevel` 0 keeps the previous one. Focus/key messages go to `window`. `scale` is recorded and reported; GTK follows `Xft.dpi` (start KiCad with `GDK_SCALE=1`). |
| `ShowFrame {kind, document?, item?, focus?, x, y, width, height}` | `{frame}` | Opens the frame if needed (through the project manager, as KiCad's own buttons do; `FK_PCB_3D` loads the board hidden first), applies the geometry, maps it, then navigates: SCH `document` = sheet file; PCB/SCH `item` = reference to select and centre; symbol/footprint editor `item` = LIB_ID. `AS_BUSY` if a frame would have to be opened while a modal dialog is up. |
| `HideFrame {kind or window}` | `{}` | Unmaps without closing. |
| `GetFrames {}` | `{frames, parent, toplevel, holder, scale, reserved_keys, events_socket}` | |
| `NextEvents {since, wait_ms, max}` | `{events, next, lost}` | Long poll on `events_socket` (`<api-socket>.events`, a second REQ/REP socket that serves only this command, so a waiting poll never blocks other requests). On the main socket `wait_ms` is ignored. `lost` = events after `since` were dropped (buffer 1024); resync with `GetFrames`. Event seqs start at 1 per process. |
| `SetChrome {menubar?, top_toolbar?, side_toolbars?, status_bar?}` | the current values | Applies now and to frames opened later (and survives menubar rebuilds). |
| `SetReservedKeys {chords}` | `{chords}` normalised | A chord that does not parse fails the whole request. |
| `Close {mode, kind?, window?}` | `{modified, exiting}` | `CM_FAIL_IF_MODIFIED` (default) refuses with `AS_BAD_REQUEST` naming the unsaved frames; `CM_SAVE` saves them; `CM_DISCARD` drops changes. KiCad's own close paths run with their "save changes?" prompts answered silently. No kind/window = everything, and the process exits after the reply. |

Event types: `ET_FRAME_OPENED` (with the window id, when a frame is parked in the holder),
`ET_FRAME_CLOSED`, `ET_FRAME_SHOWN`, `ET_FRAME_HIDDEN`, `ET_TITLE_CHANGED`,
`ET_MODIFIED_CHANGED`, `ET_DOCUMENT_CHANGED`, `ET_FOCUS_IN`, `ET_FOCUS_OUT`,
`ET_SHOW_REQUESTED`, `ET_DIALOG_OPENED`, `ET_DIALOG_CLOSED`, `ET_DETACHED`. Title, modified,
document and shown state are sampled every 250 ms.

`GetCapabilities` lists the `embed` capability and the `kiapi.backplane.embed.*` commands only
in a process started with `--embedded`.

## Modal dialogs and the API (all GUI processes)

A modal or quasi-modal dialog runs a nested event loop that also dispatches API requests. While
one is open (a wx modal or quasi-modal dialog, a toolkit-modal window, or a modal frame's loop),
requests that change state answer `AS_BUSY` with `KiCad is showing the dialog '<title>'; only
reads are served until it is closed`. Reads still work: commands named `Get*`, `List*`, `Ping*`,
`HitTest*`, `Check*`, `Expand*`, and every `kiapi.backplane.embed` command except `Close`
(`ShowFrame` checks for itself). The headless `kicad-cli api-server` is unaffected.

## Tests

`scripts/backplane-embed-x11-check.py <kicad> <kicad-cli> --out <dir>` runs the GUI on a private
Xvfb with i3 and a python-xlib stand-in host, and saves screenshots. It checks: no KiCad frame
is ever managed by the WM; PCB, schematic, 3D viewer, symbol and footprint editors show and
paint inside the host's container; click-to-focus and `_BACKPLANE_FOCUS`; `Ctrl+K` and
`Ctrl+Shift+Tab` arrive as `_BACKPLANE_KEY`; a dialog is transient for the host top-level;
`AS_BUSY` under it while reads still work; the long poll; detach, re-attach and the host's
connection dying; `SetChrome`; `Close` refusing, saving one frame and discarding everything
without a prompt. CI runs it in `backplane-linux-release.yml`.

## Not in bp.8

- `ListActions` (the palette feed) and selection-change events; `GetSelection` and the
  cross-probe commands work as before.
- Live scale changes: `AttachToParent.scale` is only recorded.
- Windows and macOS are untested.
- KiCad's 3D viewer binds Ctrl+Tab / Shift+Tab to cycle its presets and viewports; with the
  default reserved keys Ctrl+Tab goes to the host instead.
