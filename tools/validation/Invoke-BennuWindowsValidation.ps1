param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("windows-release", "windows-strict")]
  [string]$Preset,

  [Parameter(Mandatory = $true)]
  [ValidateSet("focused", "review", "full", "qa", "strict")]
  [string]$Tier
)

$ErrorActionPreference = "Stop"
$utf8 = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
$env:PYTHONUTF8 = "1"

$sourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} `
  "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
  throw "vswhere was not found at $vswhere"
}

$installationPath = & $vswhere -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($installationPath)) {
  throw "No Visual Studio installation with the x64 C++ tools was found"
}

$vsDevCmd = Join-Path $installationPath "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
  throw "VsDevCmd.bat was not found at $vsDevCmd"
}

$environmentCommand =
  'call "{0}" -arch=x64 -host_arch=x64 >nul && set' -f $vsDevCmd
$environmentLines = & $env:ComSpec /d /s /c $environmentCommand
if ($LASTEXITCODE -ne 0) {
  throw "VsDevCmd.bat failed with exit code $LASTEXITCODE"
}
foreach ($line in $environmentLines) {
  if ($line -match "^(?<name>[^=]+)=(?<value>.*)$") {
    Set-Item -LiteralPath "Env:$($Matches.name)" -Value $Matches.value
  }
}

foreach ($tool in @("cl", "link", "rc", "mt", "cmake", "ctest", "ninja")) {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
    throw "Required Windows validation tool is unavailable after VsDevCmd: $tool"
  }
}

if ($Preset -eq "windows-release" -and $Tier -eq "strict") {
  throw "The strict tier requires the windows-strict preset"
}
if ($Preset -eq "windows-strict" -and $Tier -ne "strict") {
  throw "The windows-strict preset must run the strict tier"
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
  $python = Get-Command python3 -ErrorAction SilentlyContinue
}
if (-not $python) {
  throw "Python 3 is required for the logged CTest runner"
}

$buildDirectory = Join-Path $sourceDirectory "build\preset-$Preset"
$logDirectory = Join-Path $buildDirectory "validation-logs"
$logPath = Join-Path $logDirectory "tier-$Tier.log"
$label = "^tier[.]$Tier$"

Push-Location $sourceDirectory
try {
  & cmake --preset $Preset
  if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
  }

  & cmake --build --preset $Preset
  if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
  }

  Get-Content -LiteralPath (Join-Path $buildDirectory "bennu-build-info.txt")

  & $python.Source `
    (Join-Path $sourceDirectory "tools\validation\run_ctest.py") `
    --ctest (Get-Command ctest).Source `
    --build-dir $buildDirectory `
    --label $label `
    --log $logPath
  if ($LASTEXITCODE -ne 0) {
    throw "CTest tier $Tier failed with exit code $LASTEXITCODE"
  }
} finally {
  Pop-Location
}
