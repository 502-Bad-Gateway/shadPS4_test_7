param(
    [string]$WorkRoot = "$env:RUNNER_TEMP\shadps4-win7-phase1",
    [string]$OutputDirectory = "$PSScriptRoot\artifact"
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$UpstreamCommit = '7fb1a530c15415097836521fec6f3483e27c81ae'
$ToolchainVersion = '20260616'
$ToolchainUrl = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-msvcrt-x86_64.zip'
$ToolchainDirName = 'llvm-mingw-20260616-msvcrt-x86_64'
$PayloadZip = Join-Path $PSScriptRoot 'phase1-ci-payload.zip'
$PayloadSha256 = '35c61fd9e42e289be630edf2b58814f9d407dab5aea5c59a41042edc1a75e544'
$NlohmannUrl = 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp'

Write-Host "=== shadPS4 Win7 Phase 1 clean build ==="
Write-Host "Upstream: $UpstreamCommit"
Write-Host "Toolchain: llvm-mingw $ToolchainVersion MSVCRT / LLVM 22.1.8"
Write-Host "Defaults: null_gpu=true, show_splash=true, show_fps_counter=true"

$ActualPayloadHash = (Get-FileHash -Algorithm SHA256 $PayloadZip).Hash.ToLowerInvariant()
if ($ActualPayloadHash -ne $PayloadSha256) { throw "Payload hash mismatch: $ActualPayloadHash" }

Remove-Item -Recurse -Force $WorkRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$PayloadDir = Join-Path $WorkRoot 'payload'
Expand-Archive -Path $PayloadZip -DestinationPath $PayloadDir
$Patch = Join-Path $PayloadDir 'shadps4-win7-phase1-nullgpu-7fb1a530.patch'
$FdkPatch = Join-Path $PayloadDir 'fdk-aac-llvm-mingw-submodule.patch'
$LauncherSource = Join-Path $PayloadDir 'launcher-source'
$JsonDir = Join-Path $LauncherSource 'third_party\nlohmann'
New-Item -ItemType Directory -Force -Path $JsonDir | Out-Null
Invoke-WebRequest -Uri $NlohmannUrl -OutFile (Join-Path $JsonDir 'json.hpp')

$Source = Join-Path $WorkRoot 'source'
$Build = Join-Path $WorkRoot 'build'
$ToolchainZip = Join-Path $WorkRoot 'llvm-mingw.zip'
$ToolchainParent = Join-Path $WorkRoot 'toolchain'

git clone https://github.com/shadps4-emu/shadPS4.git $Source
git -C $Source checkout --detach $UpstreamCommit
git -C $Source submodule update --init --recursive
$Resolved = (git -C $Source rev-parse HEAD).Trim()
if ($Resolved -ne $UpstreamCommit) { throw "Wrong upstream revision: $Resolved" }

git -C $Source apply --check $Patch
git -C $Source apply $Patch
$FdkDir = Join-Path $Source 'externals\aacdec\fdk-aac'
git -C $FdkDir apply --check $FdkPatch
git -C $FdkDir apply $FdkPatch

Invoke-WebRequest -Uri $ToolchainUrl -OutFile $ToolchainZip
New-Item -ItemType Directory -Force -Path $ToolchainParent | Out-Null
Expand-Archive -Path $ToolchainZip -DestinationPath $ToolchainParent
$Toolchain = Join-Path $ToolchainParent $ToolchainDirName
$Bin = Join-Path $Toolchain 'bin'
$CC = Join-Path $Bin 'x86_64-w64-mingw32-clang.exe'
$CXX = Join-Path $Bin 'x86_64-w64-mingw32-clang++.exe'
$Windres = Join-Path $Bin 'x86_64-w64-mingw32-windres.exe'
if (!(Test-Path $Windres)) { $Windres = Join-Path $Bin 'llvm-windres.exe' }
foreach ($Required in @($CC, $CXX, $Windres)) {
    if (!(Test-Path $Required)) { throw "Required tool not found: $Required" }
}
$env:PATH = "$Bin;$env:PATH"
$env:SOURCE_DATE_EPOCH = '0'
$env:TZ = 'UTC'

$ClangVersion = (& $CXX --version | Out-String)
Write-Host $ClangVersion
if ($ClangVersion -notmatch '22\.1\.8') { throw 'Unexpected LLVM version' }

cmake -S $Source -B $Build -G Ninja `
    "-DCMAKE_C_COMPILER=$CC" `
    "-DCMAKE_CXX_COMPILER=$CXX" `
    "-DCMAKE_RC_COMPILER=$Windres" `
    '-DCMAKE_BUILD_TYPE=Release' `
    '-DCMAKE_C_FLAGS=-march=x86-64-v3' `
    '-DCMAKE_CXX_FLAGS=-march=x86-64-v3' `
    '-DENABLE_WINDOWS_7_COMPAT=ON' `
    '-DENABLE_DISCORD_RPC=OFF' `
    '-DENABLE_UPDATER=OFF' `
    '-DENABLE_TESTS=OFF'
cmake --build $Build --target shadps4 --parallel $env:NUMBER_OF_PROCESSORS

$Exe = Join-Path $Build 'shadps4.exe'
if (!(Test-Path $Exe)) { $Exe = Join-Path $Build 'shadPS4.exe' }
if (!(Test-Path $Exe)) { throw 'shadps4.exe was not produced' }

$LauncherRes = Join-Path $WorkRoot 'launcher.res'
$LauncherExe = Join-Path $WorkRoot 'shadps4-win7-launcher.exe'
Push-Location $LauncherSource
try {
    & $Windres '-I' '.' 'launcher.rc' '-O' 'coff' '-o' $LauncherRes
    if ($LASTEXITCODE -ne 0) { throw 'launcher resource compilation failed' }
    & $CXX '-std=c++17' '-O2' '-DNDEBUG' '-DUNICODE' '-D_UNICODE' '-D_WIN32_WINNT=0x0601' '-DWINVER=0x0601' `
        '-I' '.' 'main.cpp' $LauncherRes '-o' $LauncherExe '-mwindows' '-municode' '-static' '-fuse-ld=lld' `
        '-Wl,--subsystem,windows:6.01' '-Wl,--disable-high-entropy-va' '-lcomctl32' '-lcomdlg32' '-lshell32' '-lgdi32' '-lole32' '-luuid'
    if ($LASTEXITCODE -ne 0) { throw 'launcher compilation failed' }
} finally { Pop-Location }
if (!(Test-Path $LauncherExe)) { throw 'launcher was not produced' }

Remove-Item -Recurse -Force $OutputDirectory -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
Copy-Item $Exe (Join-Path $OutputDirectory 'shadps4.exe')
Copy-Item $LauncherExe (Join-Path $OutputDirectory 'shadps4-win7-launcher.exe')
Copy-Item (Join-Path $Source 'LICENSE') (Join-Path $OutputDirectory 'LICENSE-shadPS4')
Copy-Item $Patch (Join-Path $OutputDirectory 'SOURCE-PATCH-phase1-nullgpu.patch')
Copy-Item $FdkPatch (Join-Path $OutputDirectory 'SOURCE-PATCH-fdk-aac.patch')

foreach ($DllName in @('libc++.dll','libunwind.dll','libwinpthread-1.dll')) {
    $Candidates = @(Get-ChildItem -Path $Toolchain -Recurse -File -Filter $DllName)
    if ($Candidates.Count -eq 0) { throw "Runtime DLL not found: $DllName" }
    $Preferred = $Candidates | Where-Object { $_.FullName -match 'x86_64-w64-mingw32' } | Select-Object -First 1
    if ($null -eq $Preferred) { $Preferred = $Candidates | Select-Object -First 1 }
    Copy-Item $Preferred.FullName (Join-Path $OutputDirectory $DllName)
}

$ReadObj = Join-Path $Bin 'llvm-readobj.exe'
$Nm = Join-Path $Bin 'llvm-nm.exe'
$Strings = Join-Path $Bin 'llvm-strings.exe'
& $ReadObj --file-headers --coff-imports (Join-Path $OutputDirectory 'shadps4.exe') | Out-File -Encoding utf8 (Join-Path $OutputDirectory 'PE-AUDIT.txt')
$AuditText = Get-Content -Raw (Join-Path $OutputDirectory 'PE-AUDIT.txt')
foreach ($Name in @('VirtualAlloc2','CreateFileMapping2','MapViewOfFile3','UnmapViewOfFile2','SetThreadDescription','GetThreadDescription','GetSystemTimePreciseAsFileTime')) {
    if ($AuditText -match "Name: $([regex]::Escape($Name))") { throw "Forbidden post-Win7 static import detected: $Name" }
}
if (Test-Path $Strings) {
    $AllStrings = (& $Strings (Join-Path $OutputDirectory 'shadps4.exe') | Out-String)
    foreach ($ForbiddenString in @('Windows 7 NVIDIA sampler workaround: disabled push descriptors','Win7 graphics pipeline state','Win7 graphics descriptor layout')) {
        if ($AllStrings.Contains($ForbiddenString)) { throw "Old Vulkan experiment leaked into clean build: $ForbiddenString" }
    }
    foreach ($RequiredString in @('Windows 7 phase-1 address-space backend','Win7 compatibility phase: phase1-nullgpu-cpu-os')) {
        if (!$AllStrings.Contains($RequiredString)) { throw "Required phase-1 marker missing: $RequiredString" }
    }
}
$IoObj = Get-ChildItem -Path $Build -Recurse -File -Filter 'io_file.cpp.obj' | Select-Object -First 1
if ($null -ne $IoObj -and (Test-Path $Nm)) { & $Nm $IoObj.FullName | Out-File -Encoding utf8 (Join-Path $OutputDirectory 'IOFILE-SYMBOLS.txt') }

@"
lineage=clean-upstream-7fb1a530
upstream_commit=$UpstreamCommit
phase=1-nullgpu-cpu-os
llvm_mingw=20260616-msvcrt-x86_64
llvm=22.1.8
target=x86_64-w64-windows-gnu
cpu=x86-64-v3
null_gpu_default=true
show_splash_default=true
show_fps_counter_default=true
vulkan_compat_patches=false
old_v1_v18_memory_backend=false
"@ | Out-File -Encoding ascii (Join-Path $OutputDirectory 'BUILD-IDENTITY.txt')

Get-ChildItem $OutputDirectory -File | ForEach-Object {
    $Hash = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
    "$Hash  $($_.Name)"
} | Sort-Object | Out-File -Encoding ascii (Join-Path $OutputDirectory 'SHA256SUMS.txt')
Write-Host "=== Build complete ==="
Get-Content (Join-Path $OutputDirectory 'BUILD-IDENTITY.txt')
Get-Content (Join-Path $OutputDirectory 'SHA256SUMS.txt')
