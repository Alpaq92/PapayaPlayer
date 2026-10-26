# Configures and builds with clang-cl and Ninja inside the MSVC environment
# (clang-cl needs MSVC's linker, libraries and the Windows SDK).
#   ./build.ps1                    build everything into build/
#   ./build.ps1 -Out build-static -DPAPAYA_SHARED_LIBS=OFF
#   ./build.ps1 -Test              build, then run ctest
param([string]$Out = "build", [string]$Target = "", [switch]$Test)

$extra = @($args)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$outDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath((Join-Path $root $Out))

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio Build Tools" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "MSVC toolset not installed (need 'Desktop development with C++')" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"

$clang = Get-Command clang-cl -ErrorAction SilentlyContinue
$llvm = if ($clang) { Split-Path $clang.Source } else { "${env:ProgramFiles}\LLVM\bin" }
if (-not (Test-Path (Join-Path $llvm "clang-cl.exe"))) { throw "clang-cl not found - winget install LLVM.LLVM" }
$paths = @($llvm, (Join-Path $env:USERPROFILE ".cargo\bin"), (Join-Path $env:LOCALAPPDATA "bin\NASM"))

if (-not (Test-Path (Join-Path $root "third_party\reaktor\external\SDL\CMakeLists.txt"))) {
    throw "submodules missing - run: git submodule update --init --recursive"
}

$bat = Join-Path $env:TEMP "papaya_build_$PID.bat"
@(
  '@echo off',
  ('call "' + $vcvars + '" x64 >nul 2>&1'),
  ('set "PATH=' + ($paths -join ';') + ';%PATH%"'),
  ('cmake -S "' + $root + '" -B "' + $outDir + '" -G Ninja ' +
   '-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl' +
   (($extra | ForEach-Object { ' "' + $_ + '"' }) -join '')),
  'if errorlevel 1 exit /b 1',
  ('cmake --build "' + $outDir + '" --parallel' + $(if ($Target) { ' --target ' + $Target } else { '' })),
  'if errorlevel 1 exit /b 1',
  $(if ($Test) { 'ctest --test-dir "' + $outDir + '" --output-on-failure' } else { 'rem' }),
  'exit /b %ERRORLEVEL%'
) | Set-Content -Path $bat -Encoding ascii

try {
    & cmd /c $bat
    exit $LASTEXITCODE
} finally {
    Remove-Item $bat -ErrorAction SilentlyContinue
}
