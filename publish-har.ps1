﻿# Publish HarmonyOS HAR to the OpenHarmony ohpm central registry.
# Root cause of the earlier failure fixed here: PATH is set BEFORE ohpm is invoked
# (the two commands were previously run in reverse order).
# The private-key passphrase prompt is INTERACTIVE -> run this in your OWN terminal.
#   Usage: powershell -ExecutionPolicy Bypass -File publish-har.ps1
#   Or:    .\publish-har.ps1
#   Optional override: .\publish-har.ps1 -Har 'D:\path\to\pkg.har'

param(
  # 默认取本仓 globecore-harmonyos 构建产物（脚本位置推导，随仓库搬迁）；
  # 版本号随 library/oh-package.json5 演进，用 -Har 显式覆盖更稳。
  [string]$Har = (Join-Path $PSScriptRoot 'globecore-harmonyos\library\build\default\outputs\default\@zys-globecore-0.1.2.har'),
  [string]$Deveco = 'D:\Program Files\Huawei\DevEco Studio'
)

$ErrorActionPreference = 'Stop'

# 1) Put DevEco ohpm + node on PATH FIRST
$ohpmBin = Join-Path $Deveco 'tools\ohpm\bin'
$nodeBin = Join-Path $Deveco 'tools\node'
$env:Path = $ohpmBin + ';' + $nodeBin + ';' + $env:Path

# 2) Sanity checks
if (-not (Get-Command ohpm -ErrorAction SilentlyContinue)) {
  Write-Error "ohpm not found after adding '$ohpmBin' to PATH. Verify your DevEco Studio install path (param -Deveco)."
}
if (-not (Test-Path $Har)) {
  Write-Error "HAR not found: $Har (override with -Har '<path>')"
}

Write-Host ('ohpm version : ' + (ohpm -v))
Write-Host ('publishing   : ' + $Har)
Write-Host 'When prompted "what is your passphrase of the private key:", type your passphrase (hidden).'
Write-Host ''

# 3) Run publish (native command, not piped, so the interactive prompt works)
ohpm publish $Har
$code = $LASTEXITCODE

Write-Host ''
if ($code -eq 0) {
  Write-Host 'Done. Success marker: "+@zys/globecore 0.1.2 ... under review".'
  Write-Host 'After review approval it appears in the central registry (check Personal Center -> My Packages).'
} else {
  Write-Host ("ohpm publish exited with code " + $code + '. If HttpCode 400, share the output to fix metadata.')
}
