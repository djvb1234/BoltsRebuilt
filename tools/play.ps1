#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$GameRoot = '',
    [string]$Ultrawide = 'off',
    [string]$RenderScale = 'auto',
    [switch]$CaptureShaders,
    [switch]$Emulated,
    [switch]$Fullscreen
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $GameRoot) {
    $settings = Join-Path $repoRoot '.local/game.json'
    if (-not (Test-Path -LiteralPath $settings)) { throw 'Run tools/build-local.ps1 -GameRoot <your extracted game folder> first.' }
    $GameRoot = (Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json).game_root
}
$xex = Join-Path $GameRoot 'default.xex'
if ((Get-FileHash -LiteralPath $xex -Algorithm SHA1).Hash.ToLowerInvariant() -ne '5be7c41a37e3fa1e8fa05f4a0815c9b807dcae74') { throw 'Unsupported game executable.' }
$outputDir = Join-Path $repoRoot 'out/build/win-amd64-release'
$exe = Join-Path $outputDir 'nb.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Local executable missing; run tools/build-local.ps1 first.' }
$native = if ($Emulated -or $CaptureShaders) { 'false' } else { 'true' }
$full = if ($Fullscreen) { 'true' } else { 'false' }
$arguments = @("--game_data_root=$GameRoot", '--gpu_plugin=nb', "--nb_native_generic=$native", "--nb_ultrawide=$Ultrawide", "--nb_ultrawide_scale=$RenderScale", "--fullscreen=$full", "--user_data_root=$(Join-Path $repoRoot '.local/user-data')")
if ($CaptureShaders) {
    $dump = Join-Path $repoRoot '.local/shader-dump'
    New-Item -ItemType Directory -Path $dump -Force | Out-Null
    $arguments += "--dump_shaders=$dump"
    Write-Output 'Play the areas you want to test, then close the game and run tools/prepare-shaders.ps1.'
} elseif (-not $Emulated -and -not (Get-ChildItem -LiteralPath (Join-Path $outputDir 'native_shaders') -Filter '*.meta' -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    Write-Warning 'No local native shaders found. Draws will fall back until you capture and prepare shaders.'
}
# PowerShell can return immediately from a GUI executable. Wait on the process
# explicitly, and use ArgumentList so spaces in game paths are preserved.
$startInfo = [Diagnostics.ProcessStartInfo]::new($exe)
$startInfo.UseShellExecute = $false
$startInfo.WorkingDirectory = $outputDir
foreach ($argument in $arguments) { $startInfo.ArgumentList.Add($argument) }
$gameProcess = [Diagnostics.Process]::Start($startInfo)
try {
    $gameProcess.WaitForExit()
    if ($gameProcess.ExitCode -ne 0) { throw "Game exited with code $($gameProcess.ExitCode)." }
} finally { $gameProcess.Dispose() }
