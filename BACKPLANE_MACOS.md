# Native macOS runtime (development build)

`scripts/backplane-macos.sh` builds this pinned master-based fork on Apple
silicon, installs a private KiCad bundle, retains its CLI and both kifaces,
relocates Homebrew and build-tree dylibs into the bundle, ad-hoc signs the modified Mach-O
objects and packages the launcher, descriptor, manifest, corresponding source
and license records. It never installs into `/Applications` or a user's
managed runtime. No signing identity, notarization account or release tag is
required for this development artifact.

The `Backplane KiCad macOS` workflow uses a standard public `macos-15` runner.
It checks the relocated package with isolated PCB/schematic IPC smoke and
save/reopen regressions, plus simultaneous GLB exports into a shared Unicode
output directory. The latter compares every geometry accessor, component materials
and node transforms against sequential full/bare controls and checks source
preservation and temporary-file cleanup. Diagnostic output is uploaded even on failure.
An explicit compiler-cache directory is shared by ccache and Actions. If a
later packaging gate fails, the compiled bundle is also retained for inspection.
The native relocation regression accepts an already-rewritten, signed copy of
the same library, rejects a different library with the same basename, and runs
a relocated executable after its original build libraries have been removed.
Successful archives include `SOURCE_COMMIT`, `UPSTREAM_COMMIT`, a runtime
manifest and SHA256SUMS. A workflow running or an archive existing is not a
claim that actual Backplane Mac viewer acceptance passed: that requires
opening copied projects through the Mac Backplane package and inspecting the
Board, Schematic and 3D views in the shipped web interface, saved updates and
native helper shutdown.

Protobuf 29.6 and Python protobuf 5.29.6 are paired. Other native dependencies
are recorded from Homebrew in the artifact's license/package inventory.
The separately built nng 1.x dependency includes its license, source revision
and matching source archive under `licenses/nng`. The build requires both
`BACKPLANE_NNG_PREFIX` and its matching `BACKPLANE_NNG_SOURCE` checkout; CI
provides them from the same pinned build.
The runtime follows BACKPLANE_UPSTREAM plus the maintained fork patch set.
Its version comes from BACKPLANE_VERSION;
the CMake suffix and live IPC version are checked against that exact value.
Only the fork release owner publishes a new tag after the native gates pass.
