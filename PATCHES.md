# Backplane_KiCad patch triage: backplane/10.0.6 → upstream master

Current upstream pin: `f4c75f251385065755b948fce210e5a029bcee5e` (2026-10-31, KiCad 10.99.0).
Original port base: upstream master `fe761fb867` (2026-10-14). The initial triage below is retained as history.
Old base: tag 10.0.6 `caf7377e9c`. 18 commits on backplane/10.0.6 plus 4 on origin/main.

Classes: **DROP-UPSTREAM** = master has it; **DROP-OBSOLETE** = only meaningful on 10.0.6;
**PORT** = still needed; **PKG** = release packaging, port with changes.

| # | Commit | Subject | Class | Evidence / notes |
|---|---|---|---|---|
| 1 | e424a264fa | backport headless PCB/SCH IPC | DROP-UPSTREAM | `kicad-cli api-server` is upstream (caf1bcc455); master has `headless_sch_context`, `headless_pcb_context` (replaces our `headless_board_context`). |
| 2 | 25348df972 | build deps, provenance | DROP-OBSOLETE | Workflow text only; apt list folded into new CI. |
| 3 | c303bad718 | build prerequisites | DROP-OBSOLETE | Same. |
| 4 | 313b85c6e3 | schematic IPC items + save | DROP-UPSTREAM | All proto fields match master; `sch_api_save`, `schematic_jobs.proto` upstream. Smoke script → #T. |
| 5 | a9ab802ba9 | protocol backports, relocatable runtime | DROP-UPSTREAM + **PORT** | Protos upstream. **Port** `KICAD_KIFACE_HOME` / `KICAD_STOCK_PLUGIN_HOME` hooks (common/paths.cpp, kiway.cpp): needed for the relocatable archive. |
| 6 | b8690cd037 | CI lexer order | DROP-OBSOLETE | 10.0.6 build fix. |
| 7 | 926b6776e9 | stable PCB loader | DROP-OBSOLETE | 10.0.6 loader fix. |
| 8 | 29c379cb6d | PCB job + metadata | DROP-UPSTREAM | upstream 46da153141. |
| 9 | 727005a6f7 | bundle libraries + runtimes | PKG | Repin libraries/template for 10.99 (master libs), drop python3.12 hardcode. |
| 10 | ba40a26602 | template pin | DROP-OBSOLETE | Stable-tag pin; redo for master. |
| 11 | b18ad732b1 | template install layout | PKG | Ports as-is. |
| 12 | 233601cd13 | notice collection | PKG | Ports as-is. |
| 13 | 7c4ecf24c5 | smoke: legacy geometry | PKG | Ports as-is. |
| 14 | c4ca95e57a | smoke: model mesh | PKG | Ports as-is. |
| 15 | 77d957b7f9 | bundle module deps | PKG | Ports; re-validate ELF closure. |
| 16 | d0727c2036 | materialize Python links | PKG | Ports; Python version follows CI image. |
| 17 | 75e38b910c | reject broken links | PKG | Ports as-is. |
| 18 | 1c193606ed | expand editor APIs + 10.0.6 file compat | mostly DROP-UPSTREAM | 1089/1116 proto additions identical on master (adapted from master 2f18d95be7). `FocusOnItem` → upstream `FocusOnItems`; `RuleCheckerMarker` → upstream `DrcMarker`/`ErcMarker`. Line endings + custom properties are **native tokens on master** → the `.backplane.json` sidecar for them is DROP-OBSOLETE. Keep a **reduced** sidecar for fork-only data (board_structure, assembly). Regression scripts → #T. |
| — | e1309bcd6f | QA build fix | DROP-OBSOLETE | Test replaced by upstream `test_headless_pcb_context`. |
| — | 688a0c8229 | sketch router | **PORT** | Fork-only. 4 standalone router files + test; UI hooks (router_tool, pcb_actions, edit_tool, menubar) need hand-merge (heavy upstream churn). |
| — | d5b38b21fa | SketchRoute IPC | **PORT** | Register in master's `api_handler_board.cpp`. Proto in a separate fork file to avoid upstream field collisions. |
| — | 6c8feb7b9f | AGENTS.md | rewrite | New AGENTS.md for master base (2026-10-14 decision). |
| — | (uncommitted) | rigid-flex, structure canvas/validation, assembly IPC | **PORT (phase 2)** | Was never committed; snapshotted to local branch `backplane/wip-structure-2026-09-12` (139 files, +17.8k). Data lives in `.kicad_pcb.backplane.json` sidecars and `.backplane-assembly.json`, no native tokens. Touches 3D viewer, router, STEP exporter, `api_handler_pcb.cpp` (split upstream): hard conflicts expected. |

## Sync adaptations for upstream f4c75f2513

The sync preserves upstream history and the fork patch set. Runtime validation is recorded per commit in CI;
release archives must identify their own source and version rather than replace published bp.6 assets.

| Area | Adaptation | Upstream context / evidence |
|---|---|---|
| fmt | Give `format_as(const KI_ERROR&)` a concrete `std::string` return type | Upstream `861742895e`; GCC 15 cannot deduce the forward declaration's `auto` return type. |
| Windows dependencies | Pin pcre2 10.47#1 in the new vcpkg graph | The new pcre2 10.49 source download fails its upstream hash check; the retained release dependency passes the native build. |
| Mac SQLite | Resolve Homebrew's keg-only SQLite explicitly | The new upstream configuration requires SQLite headers/library outside the default prefix search. |
| Mac nng | Build nng 1.12.3 at `73f715235967afc783e0b99c52d015dc10897fda` | Homebrew 2.0 removed APIs KiCad uses. Preserve this separate dependency's notice, revision and matching source as well as Homebrew notices. |
| Sketch router | Use `SetFixedClearanceOverride` for fixed values, including zero | Upstream `5ae319d9a2` changed the override into a function; assigning zero would clear it. |
| Structure IPC viewer | Use `GetOur3DViewerFrame` for the internal frame | Upstream `2947344d3e` renamed the accessor when adding external viewer shutdown; all three fork calls follow it. |
| IPC export regression | Exercise Specctra DSN with structural output checks | Upstream `e6e8b6bf5f` added the job; retain the guard against untested export jobs. |
| Stock KiCad hand-off | Pin official PPA 10.0.8~ubuntu26.04.1 and its SHA-256 | The previous 10.0.7 URL was removed. Preserve actual stock round-trip and checksum verification. |

Upstream now supplies the schematic polyline `custom_property` parser case; the merge keeps one upstream case.

## New fork patches (not in the old branch)
| ID | Patch | Why |
|---|---|---|
| N1 | `kicad-cli version --format json` | Cheap version + capability handshake without starting a server. |
| N2 | `kiapi.backplane.GetCapabilities` IPC command (own proto package) | Lists fork version, upstream commit, registered API commands, file-format versions. |
| N3 | `api-server --parent-pid PID` | Server exits when the Backplane hub dies; no orphan servers. Windows checks the native PID with wxProcess::Exists; the readiness line is flushed for named-pipe clients. |
| N4 | downgrade carries `.backplane.json` companions | `project downgrade --target 10.0` copies fork sidecars beside the output and reports them as preserved-but-ignored by stock KiCad. |
| N5 | partial PCB footprint updates preserve omitted data | `UpdateItems` requests without `FootprintInstance.definition` retain the existing footprint body and other presence-tracked fields; the PCB IPC regression covers memory and save/reopen. |
| N6 | `kicad --embedded` + `kiapi.backplane.embed` (bp.8) | The GUI project suite runs as child windows of a host's window: frames born parked in an override-redirect holder inside `--parent-window` (EDA_BASE_FRAME Show/Raise hooks; GL probe frame parked too), dialogs transient for `--toplevel-window`, click-to-focus and reserved-chord forwarding as ClientMessages, host-driven show/hide, chrome, events long-poll socket, Close without prompts (the unsaved-changes dialogs answer for the host), `--api-socket` (strict, forced on; KINNG `Start()` now reports bind failure), `--parent-pid`. The 3D canvas keeps drawing while the host hides its PCB editor. All GUI processes: changing API requests answer `AS_BUSY` while a modal dialog's nested loop runs. See BACKPLANE_EMBED.md; verified by `scripts/backplane-embed-x11-check.py` on Xvfb + i3. Windows path compiles the WS_CHILD/SetParent variant but is untested; macOS refuses `--embedded`. |
| N7 | embed bp.9: theme, cross-probe, reload on disk change | `SetTheme`/`--theme` (GTK prefer-dark with an Adwaita-dark fallback, the built-in `_builtin_backplane_dark` colour theme as an in-memory override in SETTINGS_MANAGER::GetColorSettings, the host's icon set in BITMAP_STORE, the 3D backdrop); `SelectItems` in the PCB and schematic API handlers routed by frame kind, `ET_SELECTION_CHANGED` from BOARD_EDITOR_CONTROL/SCH_EDITOR_CONTROL; a polling watcher (debounce, completeness, own-save scopes in SavePcbFile/saveSchematicFile) with `EmbedReload` keeping view/layer/selection/sheet, a conflict infobar, `ReloadDocument`/`ResolveConflict`. Schematic nets resolve through `GetConnectionName` (the connectivity engine). Verified by `scripts/backplane-embed-features-check.py`. Symbol and footprint editors reload the library item on screen; in embedded mode KiCad's own footprint watcher (modal prompt) is off. Speed: `ShowFrame.prewarm`, frames in Show/HideFrame replies, `BACKPLANE_KICAD_FRAMETRACE` (GLib poll-func loop timer + spans), no menubar rebuild when saving the first recent file (EDA_BASE_FRAME::UpdateFileHistory), ApplyEmbedChrome only relayouts on change, history savers copy clean files (PCB_EDIT_FRAME saver, SCHEMATIC::SaveToHistory), embedded canvas paints coalesced; `scripts/backplane-embed-perf.py`. |
| W1 | sketch-router Windows SDK compatibility | On pinned upstream master `fe761fb867` plus fork router port `688a0c8229`, rename local `far` vectors to avoid the Windows SDK legacy `far` macro; the radius calculation is unchanged. |
| W2 | companion/structure JSON DLL declarations | On pinned upstream master `fe761fb867` plus fork companion/structure headers, include upstream `json_common.h` before JSON members instantiate. It supplies KiCad's MSVC JSON import/export stub and libc++ character traits. Native Windows run `39172359908` reproduced duplicate JSON definitions at the eeschema link; no linker suppression or serialization changes. |
| W3 | assembly JSON DLL declarations | The same upstream `json_common.h` declaration is required by the fork assembly model and board-operations headers. Native Windows run `39181380227` passed the eeschema link, then reproduced duplicate JSON definitions from `assembly_model.cpp.obj` and `assembly_exporter.cpp.obj` at the PCB DLL link. No assembly algorithms, data formats or linker options change. |
| W4 | banned-function checker across Windows drives | Native run `39198988449` reached 2628/2643 after linking the editor DLLs, but the checker prefixed absolute `D:` source paths with the `C:` build directory. Retain absolute glob results and read them directly. The ALL target and bans remain intact; a preflight exercises the real checker accepting source, rejecting a banned format, and honoring its existing explicit allowance. |
| W5 | headless CLI JSON DLL declarations | Native run `39211355156` passed the cross-drive checker and editor executable links, then failed at 2637/2643 on duplicate JSON definitions from `command_api_server.cpp.obj`. Include upstream `json_common.h` before the fork CLI's JSON instances; explicitly use the same header in the fork PCB structure adapter. Audited all fork-added JSON includes against the pinned upstream. Manifest and IPC behavior are unchanged. |
| W6 | Windows build/test version contracts | Native run `39223192558` compiled and installed, but protoc 33.4 generated Python bindings newer than the pinned client. Use Python protobuf 6.33.4 and import bindings before compiling. Its retained runtime passed native IPC/exports, revealing PowerShell split the unquoted `bp.4-windows-ci` argument into `bp` and an ignored path. Quote `bp.4`, keep the manifest's platform separate from its fork version, assert CMake retained the suffix, and check live GetVersion against it. Backplane's strict identity check remains intact. |

## Maintained fork patches (initial port base fe761fb867; current pin is BACKPLANE_UPSTREAM)
| Commit | Patch | Source |
|---|---|---|
| docs | AGENTS.md (master base, 2026-10-14), PATCHES.md | new |
| ci(release) | Linux runtime workflow for master; removes upstream lockdown.yml | 727005a6f7 … 75e38b910c |
| ci | weekly upstream merge on a sync branch | new |
| test(ipc) | regressions ported to master protos; `--stock-cli` checks `downgrade --target 10.0` copies | scripts from 6c8feb7b9f |
| feat(runtime) | `KICAD_KIFACE_HOME`, `KICAD_STOCK_PLUGIN_HOME` | a9ab802ba9 |
| feat(build) | fork identity + file-format versions in `backplane_build_version.h` | N1/N2 |
| feat(cli) | `api-server --describe` (manifest v2) | N1 |
| feat(cli) | `api-server --parent-pid` | N3 |
| feat(ipc) | `kiapi.backplane.GetCapabilities` | N2 |
| feat(cli) | project downgrade carries `*.backplane.json` / `*.backplane-assembly.json` | N4 |
| docs | BACKPLANE_RUNTIME.md | new |
| feat(pcbnew) | sketch router core | 688a0c8229 |
| feat(pcbnew) | Sketch Route / Autoroute Selected in the router tool | 688a0c8229 |
| feat(ipc) | `kiapi.backplane.SketchRoute` | d5b38b21fa |
| test(ipc) | runtime handshake regression | new |
| fix(pcbnew) | copy teardrop settings in `BOARD_DESIGN_SETTINGS::initFromOther` (SetBoardDesignRules dropped them) | from 1c193606ed; upstream candidate |
| fix(api) | reply `AS_INTERNAL_ERROR` when a handler throws (clients hung forever) | new; upstream candidate |
| fix(eeschema) | parse `custom_property` on schematic graphic polylines (saved files failed to load) | new; upstream candidate |
| fix(eeschema) | resolve headless symbol library IDs synchronously for placement and library-only `CreateItems` | this branch; CI regression below |
| docs | BACKPLANE_IPC.md agent guide for master | new |

## Upstream gaps found by the IPC regressions (fork-patch candidates)
Priority (user decision, 2026-10-15): almost all editing goes through IPC, so IPC-side correctness (create,
update, delete, rollback, save/reopen over IPC) is the goal. **Interactive GUI undo is a known non-goal**: fork
patches are verified over IPC only, and GUI undo behaviour of patched paths (e.g. user-field undo level, PR #7) is
not checked.
Tests print `KNOWN GAP ...` for these instead of failing; each needs a focused patch (and an upstream report).
| Gap | Impact for Backplane | Status |
|---|---|---|
| `CloseAllDocuments(force=False)` ignores `force` and discards unsaved changes | data loss risk for agents | **fixed** in the fork (honours `force`; also guards closing the project document); upstream candidate |
| Footprint/pad children created in a footprint are not found after commit | footprint editing over IPC | **fixed** in the fork (PR #7: 307c41268d, f14a2d270f, f7898cdaf3, 68f5db0933); upstream candidate (GUI undo not checked: non-goal) |
| Partial `FootprintInstance` update clears the footprint body | pads, graphics, zones, groups, constraints, models, and points are lost on update/save | **fixed** in `impl/footprint-partial-update`; upstream candidate |
| Sheet symbol created over IPC (reusing a sheet file) makes the next save fail | schematic becomes unsaveable | candidate patch |
| Placed-symbol update detaches its library link (`ecc83-pp:R` → `:R_1`) | silent library-link loss | candidate patch |
| Headless footprint modified state reflects the board context | wrong dirty flag | candidate patch |
| Footprint `SaveCopyOfDocument` writes no file | export of footprints | candidate patch |
| Footprints open by library ID only (10.0.6 fork also opened `.kicad_mod` by path) | fork feature not carried | decide |
| PCB table cell direct update not applied; standalone cell create/delete not rejected | table editing | candidate patch |
| Schematic line ending set over IPC lost on save | fidelity | candidate patch |
| Schematic graphic shapes do not write custom properties | fidelity (writer change would add tokens upstream can't read: needs upstream) | report upstream |
| Bus-to-bus entry conversion on save gets a new UUID | ID stability | report upstream |
| BOM "Grouped By Value" field preset throws `std::bad_alloc` (kicad-cli too) | BOM preset unusable | report upstream |

Rigid-flex/board structure is source-ported on `port/structure` from snapshot
`1747f64599`, excluding the assembly implementation. The reduced metadata layer
reads v1 item data into native master fields and writes v2 fork extensions only.
Fork IPC lives in `api/proto/backplane/structure.proto`; `board-structure` is a
shared capability. No C++ build or native runtime validation was run on the
workstation. The lead build host must run the migration gate and existing geometry,
DRC/router, GUI/viewer and export regressions. Assembly remains a separate port.
