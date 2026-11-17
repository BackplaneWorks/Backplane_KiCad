# SPDX-License-Identifier: GPL-3.0-or-later
# A Windows runtime built on a machine of our own (bp-win), as .github/workflows/backplane-windows.yml
# builds it on GitHub's runner: the same MSVC (VS 2022 17.x, x64), the same pinned vcpkg and binary
# sources (GitLab's NuGet feed read-only, a files cache on this machine), the pinned libraries and
# 3D models, then scripts/backplane-windows.ps1 (build, relocate, IPC smoke, package, libraries gate).
#
#   pwsh -File scripts/backplane-windows-local.ps1 -Root E:\bpk [-Jobs 2] [-SkipLibraries] [-ConfigureOnly]
#
# -Root holds everything (vcpkg, its binary cache, build, stage, the library stage) and must be on a
# drive other than this checkout's (the build script checks it, as on GitHub: source D:, build C:).
# The package lands in <checkout>\release\windows.  Runs at below-normal priority; -Jobs limits the
# compile jobs.  Re-running reuses the build folder: only what changed is compiled.
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [int]$Jobs = 2,
    [switch]$SkipLibraries,
    # Install vcpkg's dependencies and configure only (no libraries, no compile)
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 (pwsh) is required, as on GitHub' }
$source = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
if ((Split-Path -Qualifier $Root) -eq (Split-Path -Qualifier $source)) {
    throw "-Root $Root must be on another drive than the checkout $source"
}
New-Item -ItemType Directory -Force $Root, (Join-Path $Root 'vcpkg-cache'), (Join-Path $Root 'temp') | Out-Null
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'   # children inherit it

$env:BACKPLANE_WIN_ROOT = $Root
$env:BACKPLANE_WIN_JOBS = "$Jobs"
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $Root 'vcpkg-cache'
$env:VCPKG_BINARY_SOURCES = "clear;nuget,https://gitlab.com/api/v4/projects/27426693/packages/nuget/index.json,read;files,$($env:VCPKG_DEFAULT_BINARY_CACHE),readwrite"
$env:VCPKG_MAX_CONCURRENCY = "$Jobs"
$env:RUNNER_TEMP = Join-Path $Root 'temp'
Remove-Item Env:GITHUB_ACTIONS -ErrorAction SilentlyContinue

# The MSVC x64 developer environment (what ilammy/msvc-dev-cmd gives the workflow)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'no Visual Studio with the MSVC x64 tools' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
Set-Location $source
cl 2>&1 | Select-Object -First 1

# The IPC test client, as the workflow installs it (protoc 33.4 from the vcpkg graph: gencode 6.33.4)
python -m pip install --quiet --disable-pip-version-check protobuf==6.33.4 pynng==0.9.0

if ($ConfigureOnly) {
    $env:BACKPLANE_WIN_CONFIGURE_ONLY = '1'
    $SkipLibraries = $true
}
if (!$SkipLibraries) {
    $libraries = Join-Path $Root 'kicad-library-stage'
    if (Test-Path $libraries) { Remove-Item -Recurse -Force $libraries }
    $bash = Join-Path (Split-Path (Split-Path (Get-Command git).Source)) 'bin\bash.exe'
    & $bash scripts/backplane-libraries.sh ($libraries -replace '\\', '/') --with-3d
    $env:BACKPLANE_LIBRARY_STAGE = $libraries
}
$release = Join-Path $source 'release\windows'
if (!$ConfigureOnly -and (Test-Path $release)) { Remove-Item -Recurse -Force $release }
$started = Get-Date
& (Join-Path $PSScriptRoot 'backplane-windows.ps1')
"backplane-windows.ps1 took {0:hh\:mm\:ss}" -f ((Get-Date) - $started)
