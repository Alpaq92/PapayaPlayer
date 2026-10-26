# Runs clang-tidy over our C sources the way CI does, inside the MSVC
# environment build.ps1 uses (so system headers resolve). Needs ./build.ps1
# to have run first (it writes build/compile_commands.json).
#   ./tools/tidy.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$files = @(git -C $root ls-files 'papaya/*.c' 'app/*.c' 'tests/*.c' 'tools/*.c') +
         @(git -C $root ls-files --others --exclude-standard 'papaya/*.c' 'tests/*.c' 'tools/*.c') |
         Where-Object { $_ -notmatch '(_win32|_generic_arch)\.c$' } | Sort-Object -Unique
$bat = Join-Path $env:TEMP "papaya_tidy_$PID.bat"
@(
  '@echo off',
  ('call "' + (Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat") + '" x64 >nul 2>&1'),
  ('cd /d "' + $root + '"'),
  ('"' + "${env:ProgramFiles}\LLVM\bin\clang-tidy.exe" + '" -p build --quiet ' + ($files -join ' ')),
  'exit /b %ERRORLEVEL%'
) | Set-Content -Path $bat -Encoding ascii
try {
    & cmd /c $bat
    exit $LASTEXITCODE
} finally {
    Remove-Item $bat -ErrorAction SilentlyContinue
}
