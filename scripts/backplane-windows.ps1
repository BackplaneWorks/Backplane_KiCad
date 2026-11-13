# SPDX-License-Identifier: GPL-3.0-or-later
# Native MSVC build; upstream's pinned vcpkg/public binary feed, no WSL.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$PSNativeCommandUseErrorActionPreference = $true
$source = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $source
$build = 'C:\bp\build'
$stage = 'C:\bp\stage'
# MSVC's developer environment sets VCPKG_ROOT to its bundled copy.
# Use only this job's dedicated pinned checkout.
$env:VCPKG_ROOT = 'C:\bp\vcpkg'
$vcpkgCommit = '66c0373dc7fca549e5803087b9487edfe3aca0a1'
# KiCad packaging/kicad-win-builder build.ps1 uses this vcpkg tool commit.
New-Item -ItemType Directory -Force 'C:\bp', $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
Get-PSDrive C,D | Select-Object Name,Used,Free
python scripts/backplane-banned-functions-test.py --build-root C:\bp --require-cross-drive
if (!(Test-Path (Join-Path $env:VCPKG_ROOT '.git'))) {
    git init $env:VCPKG_ROOT
    git -C $env:VCPKG_ROOT remote add origin https://github.com/microsoft/vcpkg.git
}
git -C $env:VCPKG_ROOT fetch --depth 1 origin $vcpkgCommit
git -C $env:VCPKG_ROOT checkout --detach FETCH_HEAD
if ((git -C $env:VCPKG_ROOT rev-parse HEAD) -ne $vcpkgCommit) { throw 'vcpkg pin mismatch' }
& (Join-Path $env:VCPKG_ROOT 'bootstrap-vcpkg.bat') -disableMetrics
$version = (Get-Content (Join-Path $source 'BACKPLANE_VERSION') -Raw).Trim()
if ($version -notmatch '^10\.99\.0-(bp\.[0-9]+(\.[0-9]+)?)$') { throw 'Invalid Backplane release version' }
$versionExtra = $Matches[1]
if ($env:GITHUB_REF_TYPE -eq 'tag' -and $env:GITHUB_REF_NAME -ne "backplane-kicad-v$version") {
    throw 'Release tag does not match BACKPLANE_VERSION'
}
$upstream = (Get-Content BACKPLANE_UPSTREAM -Raw).Trim()
cmake -S . -B $build -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    "-DVCPKG_OVERLAY_TRIPLETS=$source/tools/custom_vcpkg_triplets" `
    '-DVCPKG_INSTALL_OPTIONS=--x-abi-tools-use-exact-versions' `
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$stage" `
    -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF -DKICAD_INSTALL_DEMOS=OFF `
    -DKICAD_SCRIPTING_WXPYTHON=OFF -DKICAD_USE_OCC=ON -DKICAD_USE_API=ON `
    -DKICAD_USE_SENTRY=OFF -DKICAD_USE_PCH=OFF -DKICAD_WIN32_INSTALL_PDBS=ON `
    "-DKICAD_VERSION_EXTRA=$versionExtra" "-DBACKPLANE_FORK_VERSION=$version" `
    "-DBACKPLANE_UPSTREAM_COMMIT=$upstream"
$extra = Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') -Pattern '^KICAD_VERSION_EXTRA:STRING=(.*)$'
if (!$extra -or $extra.Matches[0].Groups[1].Value -ne $versionExtra) {
    throw 'CMake lost the exact Backplane version suffix; refusing to build'
}
# Fail before the C++ build if protoc's generated Python client cannot import.
$installed = Join-Path $build 'vcpkg_installed\x64-windows'
$env:PATH = (Join-Path $installed 'tools\protobuf') + ';' + $env:PATH
python scripts/backplane-ipc-smoke.py --check-bindings
cmake --build $build --parallel 4
cmake --install $build
# The PDBs (KICAD_WIN32_INSTALL_PDBS) go into their own symbols archive, never into the runtime,
# so a stall stack from the release build can be resolved
$symbols = Join-Path $source 'release\symbols'
New-Item -ItemType Directory -Force $symbols | Out-Null
$stageRoot = (Resolve-Path $stage).Path
Get-ChildItem $stageRoot -Recurse -File -Filter '*.pdb' | ForEach-Object {
    $destination = Join-Path $symbols $_.FullName.Substring($stageRoot.Length + 1)
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Move-Item $_.FullName $destination
}
if (!(Get-ChildItem $symbols -Recurse -Filter 'kicad.pdb')) { Write-Warning 'WINDOWS_SYMBOLS_MISSING: kicad.pdb is not in the symbols archive' }
$installed = Join-Path $build 'vcpkg_installed\x64-windows'
$bin = Join-Path $stage 'bin'
# vcpkg's app-local copies are in the build tree; the relocated runtime needs
# its release DLLs and their notices too. Never include debug DLLs or PDBs.
Copy-Item (Join-Path $installed 'bin\*.dll') $bin
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $licenses | Out-Null
Get-ChildItem (Join-Path $installed 'share') -Directory | ForEach-Object {
    $notice = Join-Path $_.FullName 'copyright'
    if (Test-Path $notice) {
        $destination = Join-Path $licenses ('vcpkg\' + $_.Name)
        New-Item -ItemType Directory -Force $destination | Out-Null
        Copy-Item $notice $destination
    }
}
Copy-Item LICENSE.README,LICENSE.GPLv3,BACKPLANE_UPSTREAM,vcpkg.json,vcpkg-configuration.json $licenses
Get-ChildItem thirdparty -Recurse -File | Where-Object {
    $_.Name -match '^(LICENSE|COPYING|NOTICE|COPYRIGHT)'
} | ForEach-Object {
    $relative = $_.FullName.Substring((Join-Path $source 'thirdparty').Length + 1)
    $destination = Join-Path $licenses ('vendor\' + $relative)
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    Copy-Item $_.FullName $destination
}
Copy-Item (Join-Path $build 'vcpkg_installed\vcpkg\status') (Join-Path $licenses 'VCPKG_PACKAGES.txt')
Set-Content (Join-Path $licenses 'VCPKG_TOOL_COMMIT') $vcpkgCommit
# Remove the build and dependency directories from runtime lookup. The package
# must run using its own bin, not a separately installed KiCad/dependency DLL.
$cleanPath = ($env:PATH -split ';' | Where-Object {
    $_ -and !$_.StartsWith('C:\bp\', [StringComparison]::OrdinalIgnoreCase)
}) -join ';'
$env:PATH = "$bin;$cleanPath"
$cli = Join-Path $bin 'kicad-cli.exe'
& $cli --version
$description = (& $cli api-server --describe) -join "`n"
$manifest = $description | ConvertFrom-Json
if ($manifest.platform -ne 'windows-x86_64' -or $manifest.apiVersion -ne 2 -or $manifest.version -ne $version) {
    throw 'Native runtime identity mismatch'
}
if (!(Test-Path (Join-Path $stage $manifest.apiDescriptor))) { throw 'API descriptor missing' }
Set-Content (Join-Path $stage 'manifest.json') $description -Encoding utf8NoBOM
git rev-parse HEAD | Set-Content (Join-Path $stage 'SOURCE_COMMIT')
$env:PATH = (Join-Path $installed 'tools\protobuf') + ';' + $env:PATH
# Keep the final runtime distinct from its temporary install prefix.
$release = Join-Path $source 'release\windows'
New-Item -ItemType Directory -Force $release | Out-Null
$archive = "backplane-kicad-windows-x86_64-$version"
$runtime = Join-Path $release $archive
Move-Item $stage $runtime
$env:PATH = (Join-Path $installed 'tools\protobuf') + ';' + (Join-Path $runtime 'bin') + ';' + $cleanPath
$cli = Join-Path $runtime 'bin\kicad-cli.exe'
& $cli api-server --describe
python scripts/backplane-ipc-smoke.py $cli --expected-version $version
if (Get-ChildItem $runtime -Recurse -Filter '*.pdb') { throw 'a PDB is in the runtime' }
Compress-Archive -Path $runtime -DestinationPath (Join-Path $release "$archive.zip")
Compress-Archive -Path (Join-Path $symbols '*') -DestinationPath (Join-Path $release "$archive-pdb.zip")
git archive --format=tar.gz "--prefix=$archive-source/" "--output=$release/$archive-source.tar.gz" HEAD
# LF only (Set-Content writes CRLF on Windows, which broke a lookup by name)
$sums = Get-ChildItem $release -File | Where-Object { $_.Name -ne 'SHA256SUMS' } | ForEach-Object {
    $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($_.Name)"
}
[IO.File]::WriteAllText((Join-Path $release 'SHA256SUMS'), (($sums -join "`n") + "`n"))
Write-Output 'WINDOWS_KICAD_NATIVE_BUILD_RELOCATED_IPC_AND_EXPORT_PASS'
