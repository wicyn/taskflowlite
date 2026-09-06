param([string]$BuildDirectory = 'build')

$ErrorActionPreference = 'Stop'
# 从本次配置的编译器记录读取路径，不再猜测最新安装的 MSVC 版本。
$compilerFiles = @(Get-ChildItem -LiteralPath "$BuildDirectory/CMakeFiles" -Directory |
    ForEach-Object { Join-Path $_.FullName 'CMakeCXXCompiler.cmake' } |
    Where-Object { Test-Path -LiteralPath $_ })
if ($compilerFiles.Count -ne 1) {
    throw "Expected one configured CMake compiler record in $BuildDirectory"
}
$compilerRecord = Get-Content -LiteralPath $compilerFiles[0] -Raw
$compilerMatch = [regex]::Match($compilerRecord, 'set\(CMAKE_CXX_COMPILER "([^"]+)"\)')
if (-not $compilerMatch.Success) {
    throw 'CMAKE_CXX_COMPILER was not found in the configured compiler record.'
}
$runtimeDirectory = Split-Path -Parent $compilerMatch.Groups[1].Value
if (-not (Get-ChildItem -LiteralPath $runtimeDirectory -Filter 'clang_rt.asan*.dll' -File)) {
    throw "ASan runtime DLLs were not found in $runtimeDirectory"
}
if ($env:GITHUB_PATH) {
    $runtimeDirectory | Out-File -LiteralPath $env:GITHUB_PATH -Encoding utf8 -Append
} else {
    $env:PATH = "$runtimeDirectory;$env:PATH"
}
Write-Host "ASan runtime directory: $runtimeDirectory"
