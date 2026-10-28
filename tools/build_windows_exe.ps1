$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$python = Join-Path $projectRoot ".venv\Scripts\python.exe"
$requirements = Join-Path $projectRoot "python_client\requirements-build.txt"
$spec = Join-Path $projectRoot "Lab-Environment-Monitoring.spec"
$distPath = Join-Path $projectRoot "dist"
$workPath = Join-Path $projectRoot ".packaging"
$outputExe = Join-Path $distPath "Lab-Environment-Monitoring\Lab-Environment-Monitoring.exe"
$outputInternal = Join-Path $distPath "Lab-Environment-Monitoring\_internal"
$system32 = Join-Path $env:SystemRoot "System32"
$runtimeDlls = @(
    "msvcp140.dll",
    "MSVCP140_ATOMIC_WAIT.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)
$smokeTest = Join-Path $projectRoot "tools\smoke_test_windows_exe.py"

if (-not (Test-Path -LiteralPath $python)) {
    throw "Python virtual environment not found: $python"
}
if (-not (Test-Path -LiteralPath (Join-Path $projectRoot "yolo11n.pt"))) {
    throw "YOLO person model not found: yolo11n.pt"
}

$pythonBits = & $python -c "import struct; print(struct.calcsize('P') * 8)"
if ($pythonBits.Trim() -ne "64") {
    throw "Windows x64 packaging requires a 64-bit Python environment"
}

$env:PYTHONUTF8 = "1"
# PyInstaller resolves Windows runtime DLLs through PATH. Keep SDK toolchains
# from contributing older, incompatible copies of the MSVC runtime.
$env:PATH = "$system32;$env:PATH"
$hasPyInstaller = & $python -c "import importlib.util; print(int(importlib.util.find_spec('PyInstaller') is not None))"
if ($hasPyInstaller.Trim() -ne "1") {
    Write-Host "Installing PyInstaller build dependency..."
    & $python -m pip install -r $requirements
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install PyInstaller"
    }
}

Push-Location $projectRoot
try {
    & $python -m PyInstaller `
        --noconfirm `
        --clean `
        --distpath $distPath `
        --workpath $workPath `
        $spec
    if ($LASTEXITCODE -ne 0) {
        throw "PyInstaller build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $outputExe)) {
    throw "Build completed without producing: $outputExe"
}

foreach ($dllName in $runtimeDlls) {
    $systemDll = Join-Path $system32 $dllName
    $packagedDll = Join-Path $outputInternal $dllName
    if (-not (Test-Path -LiteralPath $packagedDll)) {
        throw "Packaged MSVC runtime not found: $packagedDll"
    }
    $systemHash = (Get-FileHash -LiteralPath $systemDll -Algorithm SHA256).Hash
    $packagedHash = (Get-FileHash -LiteralPath $packagedDll -Algorithm SHA256).Hash
    if ($systemHash -ne $packagedHash) {
        throw "Packaged MSVC runtime does not match System32: $dllName"
    }
}

Write-Host "Running packaged application smoke test..."
& $python $smokeTest $outputExe
if ($LASTEXITCODE -ne 0) {
    throw "Packaged application smoke test failed with exit code $LASTEXITCODE"
}

Write-Host "Build and smoke test complete: $outputExe"
