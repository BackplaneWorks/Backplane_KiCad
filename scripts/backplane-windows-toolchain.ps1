# SPDX-License-Identifier: GPL-3.0-or-later
# The toolchain of .github/workflows/backplane-windows.yml (GitHub's windows-2022 image) on a build
# machine of our own (bp-winbuild), so scripts/backplane-windows-local.ps1 builds as the workflow does:
#   - Visual Studio 2022 Build Tools 17.x: MSVC x64, the Windows 11 SDK, CMake and Ninja (VS's own copies,
#     as ilammy/msvc-dev-cmd gives the workflow)
#   - Python 3.12 (actions/setup-python 3.12), Git for Windows with bash and git-lfs
#   - ccache for MSVC (the compiler cache backplane-windows-local.ps1 -Ccache uses)
# vcpkg itself is the pinned checkout scripts/backplane-windows.ps1 fetches into the build root.
# Idempotent: what is installed already is left alone.  Run elevated:
#
#   pwsh -File scripts/backplane-windows-toolchain.ps1 [-Tools C:\tools]
param([string]$Tools = 'C:\tools')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$PSNativeCommandUseErrorActionPreference = $true
$downloads = Join-Path $env:TEMP 'bp-toolchain'
New-Item -ItemType Directory -Force $downloads, $Tools | Out-Null
function Fetch([string]$url) {
    $file = Join-Path $downloads (Split-Path $url -Leaf)
    if (!(Test-Path $file)) { Invoke-WebRequest $url -OutFile $file -UseBasicParsing }
    $file
}
function Run([string]$exe, [string[]]$arguments, [int[]]$ok = @(0)) {
    $p = Start-Process $exe -ArgumentList $arguments -Wait -PassThru -NoNewWindow
    if ($ok -notcontains $p.ExitCode) { throw "$exe exited $($p.ExitCode)" }
}

# Visual Studio 2022 Build Tools (3010: installed, a reboot is pending)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (!$vs) {
    $installer = Fetch 'https://aka.ms/vs/17/release/vs_buildtools.exe'
    Run $installer @('--quiet', '--wait', '--norestart', '--nocache',
        '--add', 'Microsoft.VisualStudio.Workload.VCTools',
        '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '--add', 'Microsoft.VisualStudio.Component.VC.CMake.Project',
        '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621') @(0, 3010)
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
"Visual Studio: $vs $(& $vswhere -latest -products * -property catalog_productDisplayVersion)"

# Python 3.12 (3.12.10: the last 3.12 with a Windows installer)
$python = 'C:\Program Files\Python312\python.exe'
if (!(Test-Path $python)) {
    Run (Fetch 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe') @('/quiet', 'InstallAllUsers=1', 'PrependPath=1', 'Include_test=0')
}
& $python --version

# Git for Windows (bash for scripts/backplane-libraries.sh, git-lfs for the 3D models)
$git = 'C:\Program Files\Git\cmd\git.exe'
if (!(Test-Path $git)) {
    Run (Fetch 'https://github.com/git-for-windows/git/releases/download/v2.56.1.windows.1/Git-2.56.1-64-bit.exe') @('/VERYSILENT', '/NORESTART', '/NOCANCEL', '/SP-')
}
& $git --version
& $git lfs install --system | Out-Null
& $git config --system core.autocrlf false
& $git config --system core.eol lf
& $git config --system core.longpaths true

# ccache for MSVC
$ccacheVersion = '4.14.3'
$ccache = Join-Path $Tools 'ccache\ccache.exe'
if (!(Test-Path $ccache) -or (& $ccache --version | Select-Object -First 1) -notmatch [regex]::Escape($ccacheVersion)) {
    $zip = Fetch "https://github.com/ccache/ccache/releases/download/v$ccacheVersion/ccache-$ccacheVersion-windows-x86_64.zip"
    $unpacked = Join-Path $downloads 'ccache'
    if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
    Expand-Archive $zip $unpacked
    New-Item -ItemType Directory -Force (Split-Path $ccache) | Out-Null
    Copy-Item -Force (Get-ChildItem $unpacked -Recurse -Filter ccache.exe | Select-Object -First 1).FullName $ccache
}
& $ccache --version | Select-Object -First 1

# Machine PATH: Python, Git, ccache (VS's tools come from its developer shell)
$path = [Environment]::GetEnvironmentVariable('Path', 'Machine') -split ';' | Where-Object { $_ }
foreach ($p in 'C:\Program Files\Python312', 'C:\Program Files\Python312\Scripts', 'C:\Program Files\Git\cmd', (Split-Path $ccache)) {
    if ($path -notcontains $p) { $path += $p }
}
[Environment]::SetEnvironmentVariable('Path', ($path -join ';'), 'Machine')
'toolchain ready'
