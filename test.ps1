# Every hardware-free test: firmware host (CMake + ctest), server (pytest), Android :core (Gradle).
# Needs cmake, gcc (MinGW) + ninja or mingw32-make, python and a JDK >= 17. Pytest goes into server\.venv.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Invoke-Checked([scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "exit $LASTEXITCODE from: $cmd" }
}

Write-Host '== firmware: hb_core host tests'
$build = Join-Path $root 'fw/test/host/build'
$gen = if (Get-Command ninja -ErrorAction SilentlyContinue) { 'Ninja' } else { 'MinGW Makefiles' }
Invoke-Checked { cmake -S (Join-Path $root 'fw/test/host') -B $build -G $gen -DCMAKE_C_COMPILER=gcc }
Invoke-Checked { cmake --build $build }
Push-Location $build
try { Invoke-Checked { ctest --output-on-failure } } finally { Pop-Location }

Write-Host '== server: pytest'
$venv = Join-Path $root 'server/.venv'
if (-not (Test-Path $venv)) { Invoke-Checked { python -m venv $venv } }
$py = Join-Path $venv 'Scripts/python.exe'
Invoke-Checked { & $py -m pip install -q --disable-pip-version-check 'pytest>=8' }
Push-Location (Join-Path $root 'server')
try { Invoke-Checked { & $py -m pytest -q } } finally { Pop-Location }

Write-Host '== android: :core tests'
$savedJavaHome = $env:JAVA_HOME
if ($env:JAVA_HOME -and -not (Test-Path $env:JAVA_HOME)) {
    Write-Warning "JAVA_HOME=$env:JAVA_HOME does not exist; using java from PATH"
    $env:JAVA_HOME = $null
}
Push-Location (Join-Path $root 'android')
try { Invoke-Checked { .\gradlew.bat test --no-daemon --console=plain } } finally { Pop-Location; $env:JAVA_HOME = $savedJavaHome }

Write-Host '== all suites passed'
