$ErrorActionPreference = 'Stop'

$firmwareRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$workspaceRoot = (Resolve-Path -LiteralPath (Join-Path $firmwareRoot '..\..\..')).Path
$testBuild = Join-Path $PSScriptRoot '.build'
New-Item -ItemType Directory -Force -Path $testBuild | Out-Null

$compiler = $env:CC
if (-not $compiler) {
    $bundledCompiler = Join-Path $workspaceRoot '.tools\w64devkit\bin\gcc.exe'
    $compiler = if (Test-Path -LiteralPath $bundledCompiler) { $bundledCompiler } else { 'gcc' }
}
if (Test-Path -LiteralPath $compiler) {
    $env:Path = (Split-Path -Parent $compiler) + ';' + $env:Path
}

$python = $env:PYTHON
if (-not $python) {
    $idfPython = 'C:\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe'
    $python = if (Test-Path -LiteralPath $idfPython) { $idfPython } else { 'python' }
}

Push-Location $firmwareRoot
try {
    $binary = Join-Path $testBuild 'test_protocol_detector.exe'
    & $compiler -std=c11 -Wall -Wextra -Werror `
        -I components\drivers\include `
        tests\test_protocol_detector.c `
        components\drivers\src\glove_protocol.c `
        components\drivers\src\punch_detector.c `
        -o $binary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $binary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $python -m unittest discover -s tests -p test_glove_receiver.py -v
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
