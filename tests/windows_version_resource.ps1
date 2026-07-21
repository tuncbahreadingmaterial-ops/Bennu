[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$BennuExecutable,
  [Parameter(Mandatory = $true)]
  [string]$ExpectedVersion
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$item = Get-Item -LiteralPath $BennuExecutable
$info = $item.VersionInfo
$expected = @{
  FileVersion = $ExpectedVersion
  ProductVersion = $ExpectedVersion
  ProductName = "Bennu"
  FileDescription = "Bennu data-oriented programming language"
  OriginalFilename = "bennu.exe"
}
foreach ($name in $expected.Keys) {
  if ($info.$name -cne $expected[$name]) {
    throw "PE $name mismatch: expected '$($expected[$name])', observed '$($info.$name)'"
  }
}

$core = @($ExpectedVersion.Split('-')[0].Split('+')[0].Split('.'))
if ($info.FileMajorPart -ne [int]$core[0] -or
    $info.FileMinorPart -ne [int]$core[1] -or
    $info.FileBuildPart -ne [int]$core[2] -or
    $info.FilePrivatePart -ne 0) {
  throw "PE numeric FileVersion does not match $($core -join '.').0"
}

$versionOutput = @(& $BennuExecutable --version 2>&1)
if ($LASTEXITCODE -ne 0 -or $versionOutput.Count -ne 1 -or
    "$($versionOutput[0])" -cne "bennu $ExpectedVersion") {
  throw "PE --version output does not match bennu $ExpectedVersion"
}
