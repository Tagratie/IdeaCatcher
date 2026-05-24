$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$imgui = Join-Path $root "third_party\imgui"
$json  = Join-Path $root "third_party\json"
$gpp   = "C:\msys64\ucrt64\bin\g++.exe"

if (-not (Test-Path $gpp)) {
    Write-Host "ERROR: g++ not found at $gpp" -ForegroundColor Red
    exit 1
}

# MSYS2 UCRT64 tools (cc1plus, ld, etc.) must be on PATH for g++ to find them.
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH

Write-Host "Building IdeaCatcher..." -ForegroundColor Cyan

$gccArgs = @(
    "-std=c++17", "-O2", "-DNDEBUG",
    "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX",
    "-D_WIN32_WINNT=0x0A00",
    "-I", $imgui,
    "-I", (Join-Path $imgui "backends"),
    "-I", $json,
    "-mwindows", "-municode",
    "-static", "-static-libgcc", "-static-libstdc++",
    (Join-Path $root  "src\main.cpp"),
    (Join-Path $imgui "imgui.cpp"),
    (Join-Path $imgui "imgui_draw.cpp"),
    (Join-Path $imgui "imgui_tables.cpp"),
    (Join-Path $imgui "imgui_widgets.cpp"),
    (Join-Path $imgui "backends\imgui_impl_win32.cpp"),
    (Join-Path $imgui "backends\imgui_impl_dx11.cpp"),
    "-o", (Join-Path $root "IdeaCatcher.exe"),
    "-ld3d11", "-ldxgi", "-ld3dcompiler", "-ldwmapi", "-ldcomp", "-lgdi32",
    "-luser32", "-lshell32", "-lole32", "-limm32", "-lcomctl32", "-ladvapi32", "-luuid"
)

& $gpp @gccArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

$exe = Join-Path $root "IdeaCatcher.exe"
$sz  = [math]::Round((Get-Item $exe).Length / 1MB, 2)
Write-Host "BUILD OK: $exe  ($sz MB)" -ForegroundColor Green
