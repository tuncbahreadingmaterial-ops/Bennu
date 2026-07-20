[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$BennuExecutable
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Invoke-Captured {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [Parameter(Mandatory = $true)]
    [AllowEmptyCollection()]
    [string[]]$Arguments,
    [Parameter(Mandatory = $true)]
    [string]$WorkingDirectory,
    [Parameter(Mandatory = $false)]
    [AllowEmptyString()]
    [string]$StandardInput = ""
  )

  $stdoutPath = Join-Path $WorkingDirectory "stdout.txt"
  $stderrPath = Join-Path $WorkingDirectory "stderr.txt"
  $stdinPath = Join-Path $WorkingDirectory "stdin.txt"
  [System.IO.File]::WriteAllText($stdinPath, $StandardInput,
    [System.Text.UTF8Encoding]::new($false))
  Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue

  $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments `
    -WorkingDirectory $WorkingDirectory -RedirectStandardInput $stdinPath `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath `
    -Wait -PassThru
  $stdout = if (Test-Path -LiteralPath $stdoutPath) {
    [System.IO.File]::ReadAllText($stdoutPath)
  } else {
    ""
  }
  $stderr = if (Test-Path -LiteralPath $stderrPath) {
    [System.IO.File]::ReadAllText($stderrPath)
  } else {
    ""
  }
  return [pscustomobject]@{
    ExitCode = $process.ExitCode
    Stdout = $stdout.Replace("`r`n", "`n")
    Stderr = $stderr.Replace("`r`n", "`n")
  }
}

function Assert-Success {
  param(
    [Parameter(Mandatory = $true)]
    [object]$Result,
    [Parameter(Mandatory = $true)]
    [AllowEmptyString()]
    [string]$ExpectedStdout,
    [Parameter(Mandatory = $true)]
    [string]$Journey
  )

  if ($Result.ExitCode -ne 0 -or
      $Result.Stdout -cne $ExpectedStdout -or
      $Result.Stderr -cne "") {
    throw "$Journey mismatch. Exit $($Result.ExitCode), stdout [$($Result.Stdout)], stderr [$($Result.Stderr)]"
  }
}

if ($env:OS -cne "Windows_NT" -or -not [Environment]::Is64BitOperatingSystem) {
  throw "Clean package verification requires 64-bit Windows"
}

$bennuPath = (Resolve-Path -LiteralPath $BennuExecutable).Path
$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) "bennu-clean-$([guid]::NewGuid().ToString('N'))"
$sourcePath = Join-Path $workRoot "level1.bennu"
$emittedPath = Join-Path $workRoot "level1.c"
$nativePath = Join-Path $workRoot "level1.exe"
$expectedOutput = ">>(1 2 3 4 5)`n>>6`n"
$expectedHelp = @"
Usage: bennu <command> [arguments]
       bennu --help

Commands:
  repl    Start an interactive Bennu session
  run     Run a Bennu source file
  emit-c  Emit C source for a Bennu source file
  build   Build a Bennu source file
"@.Replace("`r`n", "`n") + "`n"
$savedPath = $env:PATH
$hadCC = Test-Path Env:CC
$savedCC = if ($hadCC) { $env:CC } else { $null }

New-Item -ItemType Directory -Path $workRoot | Out-Null
[System.IO.File]::WriteAllText($sourcePath, "ioata 5`ninc 5`n",
  [System.Text.UTF8Encoding]::new($false))

try {
  $env:PATH = @(
    (Join-Path $env:SystemRoot "System32")
    $env:SystemRoot
    (Join-Path $env:SystemRoot "System32\Wbem")
  ) -join ";"
  Remove-Item Env:CC -ErrorAction SilentlyContinue

  foreach ($compiler in "cl.exe", "cc.exe", "gcc.exe", "clang.exe") {
    if (Get-Command $compiler -CommandType Application -ErrorAction SilentlyContinue) {
      throw "Clean package PATH unexpectedly exposes C compiler: $compiler"
    }
  }

  $help = Invoke-Captured -FilePath $bennuPath -Arguments @("--help") `
    -WorkingDirectory $workRoot
  Assert-Success -Result $help -ExpectedStdout $expectedHelp -Journey "--help"

  $repl = Invoke-Captured -FilePath $bennuPath -Arguments @("repl") `
    -WorkingDirectory $workRoot -StandardInput "ioata 5`ninc 5`n"
  Assert-Success -Result $repl `
    -ExpectedStdout "> >>(1 2 3 4 5)`n> >>6`n> " -Journey "repl"

  $run = Invoke-Captured -FilePath $bennuPath -Arguments @("run", $sourcePath) `
    -WorkingDirectory $workRoot
  Assert-Success -Result $run -ExpectedStdout $expectedOutput -Journey "run"

  $emit = Invoke-Captured -FilePath $bennuPath `
    -Arguments @("emit-c", $sourcePath, "-o", $emittedPath) `
    -WorkingDirectory $workRoot
  Assert-Success -Result $emit -ExpectedStdout "" -Journey "emit-c"
  if (-not (Test-Path -LiteralPath $emittedPath -PathType Leaf)) {
    throw "emit-c did not create its documented C output"
  }

  $build = Invoke-Captured -FilePath $bennuPath `
    -Arguments @("build", $sourcePath, "-o", $nativePath) `
    -WorkingDirectory $workRoot
  $expectedBuildError = "error: native build: no C compiler found by platform fallback ('cl.exe')`n"
  if ($build.ExitCode -eq 0 -or $build.Stdout -cne "" -or
      $build.Stderr -cne $expectedBuildError -or
      (Test-Path -LiteralPath $nativePath)) {
    throw "build-without-compiler mismatch. Exit $($build.ExitCode), stdout [$($build.Stdout)], stderr [$($build.Stderr)]"
  }

  Write-Host "Verified clean Windows package journeys"
  Write-Host "Windows version: $([Environment]::OSVersion.Version)"
  Write-Host "Architecture: $env:PROCESSOR_ARCHITECTURE"
  Write-Host "Compiler on isolated PATH: absent"
} finally {
  $env:PATH = $savedPath
  if ($hadCC) {
    $env:CC = $savedCC
  } else {
    Remove-Item Env:CC -ErrorAction SilentlyContinue
  }
  Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
}
