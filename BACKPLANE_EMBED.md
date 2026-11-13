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
  rectangle in `ShowFrame`. The host may also map and unmap a frame window directly
  (`XMapWindow`/`XUnmapWindow`) for instant switching once it has been shown by `ShowFrame`;
  `HideFrame`/`ShowFrame` do the same through KiCad. KiCad reports `shown` from the window's
  real map state, and a later `ShowFrame` maps a window the host unmapped itself.
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
- **The host may move the holder itself.** Reparenting it into another of the host's windows
  (a pane moved to another window) is adopted: that window becomes the parent (the holder
  follows its size, focus/key messages go there, `GetFrames.parent` reports it) and
  `ET_ATTACHED` (detail = its id) is posted. Moving it to the root parks it like a detach
  (`ET_DETACHED`, holder unmapped); moving it back into a window adopts it again.
- **Moves without a resize** are the host's to announce: send each mapped frame a synthetic
  `ConfigureNotify` with its new root position (ICCCM 4.2.3), or popups open at stale positions.
- **Dialogs** (every WM-managed GTK top-level that is not an embedded frame and would be
  transient for an embedded frame, the hidden manager or nothing) get `WM_TRANSIENT_FOR` = the
  host top-level and are centred on it when mapped. A dialog over another dialog keeps its
  parent. `ET_DIALOG_OPENED`/`ET_DIALOG_CLOSED` carry the title in `detail`.
- AUI panes cannot float in embedded frames.

On Windows (bp.11) the window tree matches X11: KiCad creates a holder HWND (class
`BackplaneKiCadEmbedHolder`) as a `WS_CHILD` of the container, sized to the container's client
area (followed every 100 ms), and frames lose `WS_CAPTION`/`WS_THICKFRAME`, gain
`WS_CHILD | WS_CLIPCHILDREN` and are `SetParent` to the holder. `AttachToParent` moves only the
holder; `--parent-window 0` or `AttachToParent{0}` parks it as a hidden off-screen popup.
KiCad notices the host moving the holder into another window (`ET_ATTACHED`), taking it out
with `SetParent(holder, NULL)` or the container going away (`ET_DETACHED`, parked). Windows
destroys child windows with their parent, so **the host must move the holder out
(`AttachToParent{0}` or `SetParent(holder, NULL)`) before it destroys the container**; a host
that crashes takes KiCad's frames with it. `shown` is the frame's own `WS_VISIBLE`, as X11's
map state, so a host may keep the container hidden until a frame reports shown. Dialogs are
owned by the top-level HWND. `--theme dark` enables wxMSW dark mode (wx 3.3+) before any frame
exists; a live `SetTheme` reaches only windows opened later (`gtk_dark` in the reply is
false when it could not apply).

## Focus and reserved keys

- **Click to focus.** A button press in an embedded frame that does not have the keyboard calls
  `XSetInputFocus` on the frame (`SetFocus` on Windows) and sends the container
  `_BACKPLANE_FOCUS`. The click is not consumed. `ET_FOCUS_IN`/`ET_FOCUS_OUT` follow the frame's
  activation.
- **Reserved chords** are forwarded instead of handled, in embedded frames only (dialogs keep
  every key). Default `Ctrl+K`, `Ctrl+J`, `Ctrl+Tab`, `Ctrl+Shift+Tab`, `` Ctrl+` ``; change with
  `SetReservedKeys` (chord syntax in the proto).
- **The comment key** (bp.13, capability `comment-key-handoff`): in an embedded PCB or schematic
  editor, the "Comment to Backplane" action (`;` by default, rebindable in Preferences > Hotkeys)
  opens no dialog and posts no `ET_COMMENT`. It sends the container the reserved-chord
  `_BACKPLANE_KEY` message with index 6 (`COMMENT_KEY_INDEX`): `{frame window, keysym 0x3b,
  modifiers 0, 6, 1}`. The host opens its own comment window and takes the selection with
  `CommentSelection{capture_only}`. Being an action, it follows KiCad's hotkey rules: a focused
  text field, search box, grid cell or property editor types `;`. A host that sees this capability
  should not reserve a bare `;`. That swallowed `;` in every text field.

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
| `ShowFrame {kind, document?, item?, focus?, x, y, width, height, prewarm?}` | `{frame, frames}` | Opens the frame if needed (through the project manager, as KiCad's own buttons do; `FK_PCB_3D` loads the board hidden first), applies the geometry, maps it, then navigates: SCH `document` = sheet file; PCB/SCH `item` = reference to select and centre; symbol/footprint editor `item` = LIB_ID. `AS_BUSY` if a frame would have to be opened while a modal dialog is up. |
| `HideFrame {kind or window}` | `{frames}` | Unmaps without closing. |
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

## bp.9: theme, cross-probe, reload on disk change

### Theme
`SetTheme{mode}` (`TM_KICAD` = the person's own settings, `TM_LIGHT`, `TM_DARK`) switches the
running process; `kicad --theme light|dark` sets it before the first paint. It answers
`{mode, gtk_dark, color_theme, icon_theme}` and posts `ET_THEME_CHANGED` (detail `light`, `dark`
or `kicad`); `GetFramesResponse.theme` reports it. Nothing is written to KiCad's settings files.
- Toolkit: GTK prefer-dark for the process. If the configured GTK theme has no dark variant
  installed (`<theme>/gtk-3.0/gtk-dark.css`), dark mode uses Adwaita's dark variant, and
  `TM_LIGHT`/`TM_KICAD` restore the original theme.
- Canvases: every editor draws with `_builtin_default` (light) or the fork's built-in
  `_builtin_backplane_dark` (KiCad Default with a dark schematic/symbol palette, a near-black
  board background and a dark 3D gradient), whatever theme each editor is set to. The 3D
  viewer's backdrop follows it whatever preset is active.
- Icons: KiCad's light or dark icon set.

### Cross-probe
- `SelectItems{kind, references, nets, pins, zoom}` is served by the PCB or schematic editor
  itself (the other answers `AS_UNHANDLED`, so the server passes it on), shown or hidden; the
  frame must be open. It replaces the selection, highlights nets (and selects their items in the
  schematic), and with `zoom` fits the view to what it found. Pins are `<reference>.<number>`.
  It answers `found` and `not_found` (`ItemNames`). In the schematic only one sheet is shown: the
  sheet of the first hit. Net names match with or without a leading `/`.
- `ET_SELECTION_CHANGED`: `Event.frame` is the editor, `Event.selection` the person's selection
  after the change (empty = cleared): footprint/symbol references, pads/pins as `U3.4`, and the
  nets of selected tracks, vias, zones, wires and labels. Not emitted for SelectItems, reloads,
  or selections KiCad's own PCB↔SCH cross-probe makes.
- KiCad's own cross-probe (KIWAY ExpressMail between the embedded frames) works with either
  frame hidden: a host SelectItems in one editor also selects in the other.

### Reload on disk change
Watched (polled every 250 ms): the board file, every schematic sheet file, the symbol
editor's library of the symbol on screen (its file in a `.kicad_symdir` library), and the
footprint editor's `.kicad_mod` of the footprint on screen.
- A change counts once the file has been quiet for 300 ms, is a complete s-expression
  (parentheses balance) and its content differs from what the frame has. An atomic rename-over
  and a slow partial write therefore reload once, complete. KiCad's own saves (GUI or IPC
  SaveDocument) are not changes.
- No unsaved changes: the document reloads in place keeping zoom/pan, the active layer, the
  selection (by UUID) and the schematic sheet; `ET_DOCUMENT_RELOADED` (detail = the file).
- Unsaved changes: nothing is reloaded. `ET_RELOAD_CONFLICT` and KiCad's infobar "This file
  changed on disk." with **Reload (lose your edits)** and **Keep yours**. `ResolveConflict{kind or
  path, CC_KEEP | CC_RELOAD}` answers it from the host (`RR_UNCHANGED` / `RR_RELOADED`); Keep
  leaves the edits and the undo stack; the next save overwrites the file.
- `ReloadDocument{kind or path, RM_IF_CLEAN | RM_DISCARD}` answers `RR_RELOADED`, `RR_UNCHANGED`
  (the file is what the frame has) or `RR_CONFLICT` (unsaved changes, `RM_IF_CLEAN`).
- Under a modal dialog `SelectItems`, `ReloadDocument` and `ResolveConflict` answer `AS_BUSY`.
- Library editors reload the library item on screen: the symbol editor drops its buffer for
  that library and reloads the symbol (same unit and body style); the footprint editor reloads
  the footprint keeping item UUIDs and the selection. In embedded mode KiCad's own footprint
  watcher (and its modal "reload?" prompt) is off; conflicts go through the event and infobar.
  A library editor counts as having unsaved changes if any of its libraries does.
- A reload waits while a dialog is open on the frame or an interactive tool is running.

`scripts/backplane-embed-features-check.py <kicad> <kicad-cli> --out <dir>` tests all of this,
plus frame state across HideFrame/ShowFrame, popups/tooltips/3D-viewer dialogs never escaping,
and re-attaching after the host's connection dies; CI runs it beside the bp.8 check.

### Prewarm and batched replies

`ShowFrame{kind, width, height, prewarm: true}` replies at once; at the UI thread's next idle
time KiCad opens the frame if needed without mapping it on screen, lays it out and paints it once
off-screen at that size (canvas, GL context and first view built), then hides it again.
`ET_FRAME_OPENED` follows; there is no `ET_SHOW_REQUESTED`, `ET_SHOWN` or `ET_HIDDEN` for a
prewarm, and it is skipped while a modal dialog is up. The reply's `frame` is filled when the
frame already existed (else only `kind`). A later plain `ShowFrame` maps it in a few ms.
A prewarm ends with the frame realized (shown to the toolkit) but its window unmapped, and
`FrameInfo.realized` true: the host may `XMapWindow` it directly for its first show, with no IPC
and no first-map cost, exactly as for a frame `ShowFrame` showed and the host unmapped.
`realized` is true for any frame the host may map and unmap itself; `HideFrame` clears it.
`ShowFrameResponse.frames` and `HideFrameResponse.frames` hold every frame's state after the
call, so a switch needs no `GetFrames`.

### Frame-time trace

`BACKPLANE_KICAD_FRAMETRACE=<file>` makes KiCad append JSON lines: one
`{"kind":"loop","t","ms","ran":[...]}` per UI-thread loop iteration over 8 ms (`t` in ms since
the epoch; `ran` = what ran in it, with counts: event types and the window class/frame they hit,
tool actions, IPC commands, timers, `redraw_by:<paint|idle|refresh|force|timer>`), and
`{"kind":"span","t","ms","what"}` for timed sections (`gal_redraw@<frame>`, `save:pcb[:write|
history|...]`, `save:sch`, `ipc:<command>`, `embed:show@<frame>`, `embed:prewarm@<frame>`, ...).
`scripts/backplane-embed-perf.py <kicad> <kicad-cli> --out DIR [--prewarm]` drives first
opens, IPC and host-local switches and saves on a private Xvfb and summarises the trace.

### Speed (bp.9)

- A save no longer rebuilds the menubar (and relayouts the whole frame) when the file is
  already first in the recent-files list; the embed chrome only relayouts when something changed.
- The local-history snapshot after a save copies the files of clean documents (the board just
  saved, unmodified schematic sheets) on its background thread instead of formatting them again
  on the UI thread.
- Embedded canvases paint once for the exposes a show delivers together.

## bp.10: alignment hotkeys, "Comment to Backplane..."

### Alt+V / Alt+H (every process, not only `--embedded`)

| Key | Action | Result |
|---|---|---|
| Alt+V | `pcbnew.AlignAndDistribute.alignCenterX`, `eeschema.Align.alignCenterX` | the selection on a common X centre (a vertical line) |
| Alt+H | `pcbnew.AlignAndDistribute.alignCenterY`, `eeschema.Align.alignCenterY` | the selection on a common Y centre (a horizontal line) |

These are KiCad's own align-centre actions, now with default hotkeys (`include/backplane_hotkeys.h`).
They are listed in Preferences > Hotkeys, where they can be rebound, and in the selection's
Align/Distribute (PCB) and Align (schematic) context submenus. Centres are bounding-box centres
(a footprint's without its texts). The anchor is the first match in this order:

1. a locked item in the selection (it cannot move);
2. the selected item under the cursor (KiCad's own rule);
3. the item the person selected first. KiCad records selection order; upstream used the
   left-/top-most item here.

In the schematic each moved item is snapped to the connection grid (KiCad's align tool already
did this), so pins stay connectable. Centres then agree to within half a grid step.

Upstream, Alt+V and Alt+H are the View and Help menu mnemonics. They have moved:

- Help is "He&lp" (Alt+L) in every frame.
- The schematic editor's View is "Vie&w" (Alt+W).
- The PCB editor's View has no mnemonic. Alt+W there is the router's default Track Corner
  Mode Arc 90 hotkey, and View's other letters belong to Inspect and Edit.

Other frames that have a View menu keep "&View". Only the PCB and schematic editors handle
Alt+V/Alt+H.

### Comment to Backplane...

The context menu of the PCB editor and the schematic editor ends with **Comment to Backplane...**
when a Backplane host is present, that is, in `--embedded` processes or when `--api-socket` was
given. It is on every right-click: footprints, symbols, pads, pins, tracks, wires, a box
selection, or nothing (then the comment is about the area in view). Its default hotkey is `;`
(bp.13; free in both editors and not a host-reserved key), rebindable in Preferences > Hotkeys.

It opens a small dialog (transient for the host's top-level like every dialog) that says what is
selected and takes a note. Send (or Ctrl+Enter) pushes `ET_COMMENT = 19`. Cancel, or an empty
note, sends nothing.

`Event.comment` (`Comment`, see embed.proto for every field):

- `note`.
- `names`: references, nets and pins, as in `ET_SELECTION_CHANGED`; also copied to
  `Event.selection`. `Event.detail` = the note.
- `items`, in selection order (at most 500, then `truncated`). Each has `kind` (KiCad's class
  name: FOOTPRINT, PAD, PCB_TRACK, PCB_VIA, ZONE, SCH_SYMBOL, SCH_PIN, SCH_LINE, SCH_LABEL, ...),
  `id` (KIID), `reference` (the parent's for pads, pins and fields), `pin` ("U3.4"), `net`,
  `layer` (PCB) or `sheet_path` (schematic), `x_mm`/`y_mm` and `bbox`, and `value`.
- `bbox`, the union of the items' boxes. With nothing selected it is the area in view, and
  `empty_selection` is set.
- `sheet_path`: the schematic sheet shown.
- `screenshot`: the canvas around `bbox`, padded by a tenth of its size and at least 2 mm, as a
  PNG at most 800 px on its long side. It is written to
  `<dir of the API socket>/backplane-comments/comment-<pid>-<ms>-<n>.png`. KiCad keeps the 32
  newest there, so the host copies what it keeps. `screenshot_region` is the document area the
  PNG shows; `screenshot_width` and `screenshot_height` are its size. If the selection is already
  in view and at least 240 px on screen, the shot is cropped from the canvas as drawn. Otherwise
  the view is zoomed onto it for one frame and put back. With the frame hidden there is no
  screenshot (`screenshot` empty).
- `from_api`.

Coordinates are millimetres in the document's own frame, as in the file: origin at the
sheet/board origin, X right, Y down.

`CommentSelection {kind, note}` → `{seq, comment}` does the same without the dialog, on the
current selection of `kind`'s frame (FK_PCB or FK_SCH). It is routed by kind like `SelectItems`
(AS_UNHANDLED for any other kind) and answers AS_BUSY while a modal dialog is up. With
`capture_only: true` (bp.13) it returns the comment (names, items, bbox, screenshot) with seq 0
and pushes no ET_COMMENT; `note` is ignored, so the host can take the note itself.

Without `--embedded`, `--api-socket` alone also starts the `<api-socket>.events` socket, so a
host can long-poll `NextEvents` for ET_COMMENT there. Such frames are not registered with the
embedder, so `frame` carries only kind, title and shown/focused. A bind failure of that socket
is not fatal without `--embedded`.

`scripts/backplane-embed-align-comment-check.py <kicad> <kicad-cli> --out <dir>` tests all of
this in both editors.

## bp.12: progress without windows

In `--embedded` KiCad shows no progress window: `WX_PROGRESS_REPORTER` and
`APP_PROGRESS_DIALOG` (board and schematic loads, zone fills, imports, plots, …) are created
and never mapped, and background jobs (library loads) are reported too. Each job is an
`ET_PROGRESS` event with `Event.progress` = `Progress{id, title, detail, fraction (0..1 or -1),
done, cancellable, cancelled}` and `Event.frame` = the frame it runs in (FK_UNKNOWN when none).
The first event is sent at once, updates at most every 100 ms (none in a job's first 150 ms),
and `done` always, also after a cancel or failure. `GetFramesResponse.progress` lists the jobs
running now. `CancelProgress{id}` asks a cancellable job to stop; it is answered at once on
either socket, including `events_socket`, which is the one to use while a job started by an API
request (a `ShowFrame` that loads a board) holds the main socket. When no host has polled
`NextEvents` for 5 s, the job's title and percentage show in its frame's status bar instead.
Real questions (save/discard, errors, file pickers) stay dialogs, transient for the host and
centred on it. The features check asserts load, zone fill and library load jobs end `done` and
that no progress window is ever mapped.

## bp.13: GerbView and editor tabs

- `FK_GERBVIEW = 7`: GerbView runs in KiCad's own process when embedded (the project manager
  otherwise starts a separate one), shown with `ShowFrame` like the editors.
- `OpenGerbers{files, show, x, y, width, height}` replaces what GerbView shows with these Gerber,
  drill, job (`.gbrjob`, its listed layers) or zip files, opening GerbView hidden (laid out
  off-screen, as a prewarm) unless `show`. Each file loads on its own: `loaded`, and `failed` with
  the reason in `errors` at the same index (no dialog appears). `FrameInfo.files` lists the files
  loaded.
- `FrameInfo.lib_id` names the footprint or symbol on screen in the library editors.
- `ET_EDITOR_REQUESTED = 25`: KiCad itself opened or brought up the footprint or symbol editor
  ("Open Footprint in Footprint Editor", "Edit Symbol"); `Event.frame` is that frame with its
  `lib_id`, already loaded. A hidden frame also gets `ET_SHOW_REQUESTED` (older hosts); a host
  that handles `ET_EDITOR_REQUESTED` shows the editor as a tab with `ShowFrame` and ignores the
  `ET_SHOW_REQUESTED` for these two kinds.

## bp.13: checks in the editor, markers, selection by id and place

- `RunChecks{kind, refill_zones, parity}` runs DRC (FK_PCB) or ERC (FK_SCH) on the open document,
  saved or not, in the editor (no second copy of the board, so no second set of libraries), as the
  DRC/ERC dialog does: old markers go (exclusions are kept), new ones stay in the editor. It
  answers at once with `progress_id`; `ET_PROGRESS` with that id follows (cancellable with
  `CancelProgress`), then `ET_CHECKS_DONE = 27` with `Event.checks` (errors, warnings, exclusions,
  unconnected, cancelled, document_modified, error). Zones are checked as filled unless
  `refill_zones`, as kicad-cli does. `AS_BUSY` under a modal dialog, while a tool other than
  selection is active, or while a check or another task runs; other kinds are unhandled.
- `GetMarkers{kind}` lists the DRC/ERC markers in the editor now (after RunChecks, the dialog, or
  as loaded): type (the settings key, e.g. `clearance`), severity (`error`, `warning`,
  `exclusion`), message, position in nm, the items' KIIDs, the marker's own KIID, the custom rule's
  name when one fired, the layer (FK_PCB) or the sheet's KIID path (FK_SCH). `include_host` is
  for the host's own markers (SetHostMarkers, a later branch).
  Each marker also has `items_detail`: its items as `kicad-cli`'s JSON report gives them
  (`items[]`, built by the same code: the same `description` text, e.g. "Pad 1 [/SHPHLD] of SW3 on
  F.Cu"), with the item's `id`, `at_nm` and, for FK_SCH, the item's own `sheet_path`. `group` says
  which list of the CLI report it is in: `violation`, `unconnected` (DRC's unconnected items) or
  `parity` (schematic parity); all three are markers. `comment` is an exclusion's reason.
- `SelectItems` takes `ids` (exact items by KIID, with the references, pins and nets; unknown ones
  come back in `ids_not_found`), `zoom_box_nm` (zoom to fit, overrides `zoom`), `at_nm` (centre
  there at the current scale) and, for FK_SCH, `sheet_path` (show that sheet first). A host's
  checks panel selects a marker's items with `ids` and the marker's place with `zoom_box_nm`.
- `SetHostMarkers{kind, set, markers}` replaces one named set of the host's review markers in the
  PCB or schematic editor (an empty list clears it; sets are independent). A `HostMarker` has the
  host's id, a title, a severity (info, warning, error), a place (`at_nm`, else the centre of its
  `items`), and for FK_SCH a sheet (`sheet_path`, else its items' sheet; drawn only while that
  sheet is shown). They are host state: drawn in the editor's overlay as rings in the host's
  colours (magenta error, amber warning, sky info; not KiCad's DRC arrows), a fixed size on screen,
  never written to the board or schematic, not in DRC/ERC results, not undoable.
  `GetMarkers{include_host}` lists them (type `host`, `host.set`, `host.id`).
- `ET_MARKER_CLICKED = 26`: a click on a host marker (`Event.marker` = set and id) or on one of
  KiCad's DRC/ERC markers (`Event.drc_marker`, as GetMarkers lists it), so the host selects its row.

## bp.13: side panels in a narrow pane

In `--embedded`, the editors' side-panel columns (Agents, search, net inspector, design blocks,
hierarchy, properties, selection filter, appearance) hide while the frame is too narrow for the
canvas to keep 60% of its width, least needed first (Agents first, Appearance last), and come back,
most needed first, as it widens. A panel the person opens or closes themselves is theirs from then
on for the session: auto leaves it alone. Checked by the embed poll and on every ShowFrame; nothing
is saved (no AUI layout is saved in `--embedded`).

## Known limitations (as of bp.9)

- `ListActions` (the palette feed).
- Live scale changes: `AttachToParent.scale` is only recorded.
- Windows and macOS are untested.
- An IPC `ShowFrame` of a frame already open still repaints its canvas twice (a host-local
  `XMapWindow` repaints once).
- The save itself still runs on the UI thread (board write ~8-12 ms plus the snapshot step
  ~10 ms on the demo board).
- KiCad's 3D viewer binds Ctrl+Tab / Shift+Tab to cycle its presets and viewports; with the
  default reserved keys Ctrl+Tab goes to the host instead.
