# Sets up the ESP-IDF environment for this repository, then runs idf.py.
#
#   .\scripts\idf.ps1 build                   build the esp32c3_test example
#   .\scripts\idf.ps1 -p COM7 flash monitor   flash it and watch it
#   . .\scripts\idf.ps1                       env in *your* session, no build
#
# Every argument is handed to idf.py verbatim. There is deliberately no param()
# block and no [CmdletBinding()], because either one adds PowerShell's own
# parameters to the ones this script accepts, and they collide with idf.py's
# short flags. CmdletBinding contributes -PipelineVariable, -Verbose, -Debug,
# -WarningAction and more, so `-p COM7` binds to -PipelineVariable and the port
# is silently dropped (`idf.py flash monitor` then auto-detects a port), `-v`
# binds to -Verbose, and `-w` is an ambiguous-parameter error before idf.py
# starts. Taking $args leaves nothing for them to bind to.
#
# Configure through the environment instead, which cannot collide with a flag:
#
#   $env:IDF_TARGET  = "esp32s3"   chip to build for  (default esp32c3)
#   $env:IDF_EXAMPLE = "other"     example to build   (default esp32c3_test)
#
# Run it for a one-shot command; dot-source it to keep IDF_PATH, the toolchain
# and the working directory in the calling shell and drive idf.py yourself.

# Deliberately no $ErrorActionPreference = "Stop": this script is advertised for
# dot-sourcing, and setting it would leave every later non-terminating error in
# the caller's session terminating - a confusing failure a long way from here.
# Nothing below needs it: the checks that matter end in throw, and idf.py reports
# its own failures through its exit code.

$IdfArgs = $args

# Dot-sourcing means the caller keeps this session; running it means this
# process is the whole of it. Only the second should ever end in exit, or a
# stray one closes the user's shell.
$dotSourced = $MyInvocation.InvocationName -eq "."

$target = if ($env:IDF_TARGET) { $env:IDF_TARGET } else { "esp32c3" }
$example = if ($env:IDF_EXAMPLE) { $env:IDF_EXAMPLE } else { "esp32c3_test" }

# Where ESP-IDF lives. IDF_PATH wins when it already points at a real install,
# so a second one can be used without editing this file; a stale or unrelated
# IDF_PATH is ignored rather than allowed to break the build.
#
# Select-Object -First 1 rather than an index: a pipeline that emits a single
# string yields a bare String, and indexing that returns its first *character* -
# so with IDF_PATH unset the fallback below would have become "D" and dot-sourced
# D\export.ps1. -First 1 always yields the object itself, or nothing.
$idfPath = @($env:IDF_PATH, "D:\ESP32\Espressif\frameworks\esp-idf-v5.5.2") |
    Where-Object { $_ -and (Test-Path (Join-Path $_ "export.ps1")) } |
    Select-Object -First 1
if (-not $idfPath) {
    throw "No ESP-IDF found. Set IDF_PATH to an install holding export.ps1."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot "examples\$example"
if (-not (Test-Path $projectDir)) {
    throw "No example '$example' under $repoRoot\examples"
}

# IDF_TARGET only chooses the target for a tree that has no sdkconfig yet. Faced
# with one already configured for a different chip, idf.py refuses rather than
# switching - so say what to run, instead of letting it fail with its own message
# about an inconsistent sdkconfig.
$sdkconfig = Join-Path $projectDir "sdkconfig"
if (Test-Path $sdkconfig) {
    $line = Select-String -Path $sdkconfig -Pattern '^CONFIG_IDF_TARGET="(.+)"' |
        Select-Object -First 1
    $configured = if ($line) { $line.Matches[0].Groups[1].Value } else { $null }
    if ($configured -and $configured -ne $target) {
        throw "This build tree is configured for '$configured'. To switch: " +
              "idf.py -C `"$projectDir`" set-target $target  " +
              "(set-target discards the existing sdkconfig)"
    }
}

# ESP-IDF's activation refuses to run at all while MSYSTEM is set - it answers
# "MSys/Mingw is not supported" and exits, taking the whole setup with it - and
# a PowerShell started from Git Bash inherits MSYSTEM from it. Dropping the
# variable here is what makes this script work from either shell, and it costs
# nothing in a native PowerShell, where it was never set.
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

# Dot-sourcing export.ps1 is what puts the toolchain, the venv python and
# IDF_PATH into this session; running it as a child would set them nowhere.
. (Join-Path $idfPath "export.ps1")

$env:IDF_TARGET = $target
Set-Location $projectDir

Write-Host "IDF     $env:IDF_PATH"
Write-Host "TARGET  $env:IDF_TARGET"
Write-Host "PROJECT $projectDir"
Write-Host ""

if ($IdfArgs.Count -gt 0) {
    & idf.py @IdfArgs
    if (-not $dotSourced) { exit $LASTEXITCODE }
} else {
    Write-Host "Environment ready. Dot-source the script to keep it:"
    Write-Host "  . .\scripts\idf.ps1"
}
