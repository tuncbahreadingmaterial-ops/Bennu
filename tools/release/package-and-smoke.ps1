[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$BennuExecutable,
  [Parameter(Mandatory = $true)]
  [string]$Source,
  [Parameter(Mandatory = $true)]
  [string]$License,
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "windows-x64", "macos-arm64")]
  [string]$Platform,
  [Parameter(Mandatory = $true)]
  [string]$Archive
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Invoke-Checked {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [string[]]$Arguments
  )

  $output = @(& $FilePath @Arguments 2>&1)
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with status ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')`n$($output -join "`n")"
  }
  return $output
}

function Assert-RewriteOutput {
  param(
    [Parameter(Mandatory = $true)]
    [object[]]$Actual,
    [Parameter(Mandatory = $true)]
    [string]$Journey
  )

  $normalized = (($Actual | ForEach-Object { "$_" }) -join "`n").Replace("`r", "").TrimEnd("`n")
  $expected = "6`n(8 -2 12 1)`n3.5`n(false true false true)`n(true false)`n(1 2 3 4 5)"
  if ($normalized -cne $expected) {
    throw "$Journey output mismatch. Expected '$expected', observed '$normalized'"
  }
}

function Assert-ExactEntries {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Actual,
    [Parameter(Mandatory = $true)]
    [string[]]$Expected,
    [Parameter(Mandatory = $true)]
    [string]$Subject
  )

  $actualSorted = @($Actual | Sort-Object)
  $expectedSorted = @($Expected | Sort-Object)
  $difference = @(Compare-Object -ReferenceObject $expectedSorted -DifferenceObject $actualSorted)
  if ($difference.Count -ne 0) {
    throw "$Subject entries mismatch. Expected '$($expectedSorted -join ', ')', observed '$($actualSorted -join ', ')'"
  }
}

function Assert-UnixExecutable {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $mode = [System.IO.File]::GetUnixFileMode($Path)
  $required = [System.IO.UnixFileMode]::UserExecute -bor
    [System.IO.UnixFileMode]::GroupExecute -bor
    [System.IO.UnixFileMode]::OtherExecute
  if (($mode -band $required) -ne $required) {
    throw "Executable permissions were not preserved for ${Path}: $mode"
  }
}

function Assert-WindowsRuntimeDependencies {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $dumpbin = (Get-Command "dumpbin.exe" -CommandType Application -ErrorAction Stop).Source
  $output = @(& $dumpbin "/DEPENDENTS" $Path 2>&1)
  if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /DEPENDENTS failed for ${Path}:`n$($output -join "`n")"
  }

  $dependencies = @($output | ForEach-Object {
    $line = "$_"
    if ($line -match '^\s+([A-Za-z0-9._-]+\.dll)\s*$') {
      $Matches[1]
    }
  } | Sort-Object -Unique)
  if ($dependencies.Count -eq 0) {
    throw "dumpbin reported no PE dependencies for $Path"
  }

  $expectedRuntime = @("MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll")
  $missingRuntime = @($expectedRuntime | Where-Object {
    $dependencies -notcontains $_
  })
  $unexpectedRuntime = @($dependencies | Where-Object {
    $_ -match '^(?i:MSVCP|VCRUNTIME).*\.dll$' -and
      $expectedRuntime -notcontains $_
  })
  if ($missingRuntime.Count -ne 0 -or $unexpectedRuntime.Count -ne 0) {
    throw "Packaged PE runtime dependency policy mismatch. Missing: $($missingRuntime -join ', '); unexpected: $($unexpectedRuntime -join ', ')"
  }

  $undocumented = @($dependencies | Where-Object {
    $isApiSet = $_ -match '^(?i:api-ms-win-|ext-ms-win-)'
    $systemDll = Join-Path ([Environment]::SystemDirectory) $_
    -not $isApiSet -and -not (Test-Path -LiteralPath $systemDll -PathType Leaf)
  })
  if ($undocumented.Count -ne 0) {
    throw "Packaged PE has non-system dependencies absent from the archive: $($undocumented -join ', ')"
  }

  Write-Host "Verified packaged PE dependencies: $($dependencies -join ', ')"
}

$bennuPath = (Resolve-Path -LiteralPath $BennuExecutable).Path
$sourcePath = (Resolve-Path -LiteralPath $Source).Path
$licensePath = (Resolve-Path -LiteralPath $License).Path
$archivePath = [System.IO.Path]::GetFullPath($Archive)
$archiveParent = Split-Path -Parent $archivePath
if (-not (Test-Path -LiteralPath $archiveParent -PathType Container)) {
  throw "Archive parent does not exist: $archiveParent"
}
if (Test-Path -LiteralPath $archivePath) {
  throw "Refusing to replace existing archive: $archivePath"
}

$executableName = if ($Platform -eq "windows-x64") { "bennu.exe" } else { "bennu" }
$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) "bennu-release-$([guid]::NewGuid().ToString('N'))"
$stageRoot = Join-Path $workRoot "stage"
$extractRoot = Join-Path $workRoot "extracted"
$expectedEntries = @("LICENSE", $executableName)

New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null

try {
  Push-Location $workRoot
  try {
    Assert-RewriteOutput -Actual @(Invoke-Checked -FilePath $bennuPath -Arguments @("run", $sourcePath)) -Journey "file runner"

    $generatedC = Join-Path $workRoot "rewrite.c"
    $generatedExecutable = Join-Path $workRoot $(if ($Platform -eq "windows-x64") { "emitted.exe" } else { "emitted" })
    Invoke-Checked -FilePath $bennuPath -Arguments @("emit-c", $sourcePath, "-o", $generatedC) | Out-Null
    if ($Platform -eq "windows-x64") {
      Invoke-Checked -FilePath "cl.exe" -Arguments @("/nologo", "/std:c11", $generatedC, "/Fe:$generatedExecutable") | Out-Null
    } else {
      Invoke-Checked -FilePath "cc" -Arguments @("-std=c11", $generatedC, "-o", $generatedExecutable) | Out-Null
    }
    Assert-RewriteOutput -Actual @(Invoke-Checked -FilePath $generatedExecutable -Arguments @()) -Journey "emitted C executable"

    $nativeExecutable = Join-Path $workRoot $(if ($Platform -eq "windows-x64") { "native.exe" } else { "native" })
    $hadCC = Test-Path Env:CC
    $savedCC = if ($hadCC) { $env:CC } else { $null }
    Remove-Item Env:CC -ErrorAction SilentlyContinue
    try {
      Invoke-Checked -FilePath $bennuPath -Arguments @("build", $sourcePath, "-o", $nativeExecutable) | Out-Null
    } finally {
      if ($hadCC) {
        $env:CC = $savedCC
      } else {
        Remove-Item Env:CC -ErrorAction SilentlyContinue
      }
    }
    Assert-RewriteOutput -Actual @(Invoke-Checked -FilePath $nativeExecutable -Arguments @()) -Journey "fallback native build"

    $stagedExecutable = Join-Path $stageRoot $executableName
    Copy-Item -LiteralPath $bennuPath -Destination $stagedExecutable
    Copy-Item -LiteralPath $licensePath -Destination (Join-Path $stageRoot "LICENSE")
    if ($Platform -eq "windows-x64") {
      Assert-WindowsRuntimeDependencies -Path $stagedExecutable
    } else {
      Invoke-Checked -FilePath "chmod" -Arguments @("755", $stagedExecutable) | Out-Null
    }

    if ($Platform -eq "windows-x64") {
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      [System.IO.Compression.ZipFile]::CreateFromDirectory($stageRoot, $archivePath)
      $zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
      try {
        $archiveEntries = @($zip.Entries | ForEach-Object { $_.FullName.Replace("\", "/") })
      } finally {
        $zip.Dispose()
      }
      [System.IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $extractRoot)
    } else {
      Invoke-Checked -FilePath "tar" -Arguments @("-czf", $archivePath, "-C", $stageRoot, $executableName, "LICENSE") | Out-Null
      $archiveEntries = @(Invoke-Checked -FilePath "tar" -Arguments @("-tzf", $archivePath) | ForEach-Object { "$_" })
      Invoke-Checked -FilePath "tar" -Arguments @("-xzf", $archivePath, "-C", $extractRoot) | Out-Null
    }

    Assert-ExactEntries -Actual $archiveEntries -Expected $expectedEntries -Subject "archive"
    $extractedEntries = @(Get-ChildItem -LiteralPath $extractRoot -File -Recurse | ForEach-Object {
      [System.IO.Path]::GetRelativePath($extractRoot, $_.FullName).Replace("\", "/")
    })
    Assert-ExactEntries -Actual $extractedEntries -Expected $expectedEntries -Subject "extracted archive"

    $extractedExecutable = Join-Path $extractRoot $executableName
    if ($Platform -ne "windows-x64") {
      Assert-UnixExecutable -Path $stagedExecutable
      Assert-UnixExecutable -Path $extractedExecutable
    }
    Assert-RewriteOutput -Actual @(Invoke-Checked -FilePath $extractedExecutable -Arguments @("run", $sourcePath)) -Journey "extracted archive"
    if ($Platform -eq "windows-x64") {
      & (Join-Path $PSScriptRoot "verify-clean-windows-package.ps1") `
        -BennuExecutable $extractedExecutable
    }

    $archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
    Write-Host "Verified archive: $archivePath"
    Write-Host "SHA-256: $archiveHash"
  } finally {
    Pop-Location
  }
} finally {
  Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
}
