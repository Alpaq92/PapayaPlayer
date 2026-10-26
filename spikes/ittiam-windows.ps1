# Spikes S1/S2 (IMPLEMENTATION-PLAN.md phase 0): build each Ittiam library with
# its own CMake, unmodified, using clang-cl on Windows. Logs go to build-spike/.
#   ./spikes/ittiam-windows.ps1 [libxaac libavc libhevc libmpeg2]
param([string[]]$Libs = @("libxaac", "libavc", "libhevc", "libmpeg2"))

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
$llvm = "${env:ProgramFiles}\LLVM\bin"

foreach ($lib in $Libs) {
    $src = Join-Path $root "third_party\$lib"
    $out = Join-Path $root "build-spike\$lib"
    $log = Join-Path $root "build-spike\$lib.log"
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    $bat = Join-Path $env:TEMP "papaya_spike_$PID.bat"
    @(
      '@echo off',
      ('call "' + $vcvars + '" x64 >nul 2>&1'),
      ('set "PATH=' + $llvm + ';%PATH%"'),
      ('cmake -S "' + $src + '" -B "' + $out + '" -G Ninja -DCMAKE_BUILD_TYPE=Release ' +
       '-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ' +
       # libhevc's gtest suites need OpenSSL; the spike is about the library.
       '-DENABLE_TESTS=OFF -DENABLE_BENCHMARKS=OFF ' +
       '-DCMAKE_PROJECT_INCLUDE="' + (Join-Path $PSScriptRoot "ittiam-inject.cmake") + '"'),
      'if errorlevel 1 exit /b 1',
      $(if ($lib -eq "libxaac") { 'cmake --build "' + $out + '" --parallel' }
        else { 'cmake --build "' + $out + '" --parallel --target link_' + $lib + 'dec && "' + $out + '\link_' + $lib + 'dec.exe"' }),
      'exit /b %ERRORLEVEL%'
    ) | Set-Content -Path $bat -Encoding ascii
    & cmd /c $bat *> $log
    $code = $LASTEXITCODE
    Remove-Item $bat -ErrorAction SilentlyContinue
    $errors = (Select-String -Path $log -Pattern ' error[: ]' -SimpleMatch:$false).Count
    "{0,-9} exit {1}  error lines {2}  log {3}" -f $lib, $code, $errors, $log
}
