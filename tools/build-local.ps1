#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameRoot,
    [ValidateRange(1, 32)][int]$Jobs = 4,
    [string]$VsInstall = '',
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$gamePath = (Resolve-Path -LiteralPath $GameRoot).Path
$xex = Join-Path $gamePath 'default.xex'
if (-not (Test-Path -LiteralPath $xex -PathType Leaf)) { throw 'Select the extracted game folder containing default.xex.' }
if (Test-Path -LiteralPath (Join-Path $gamePath 'default.xexp')) { throw 'Title updates are not supported. Use the PAL base executable.' }
$expectedSha1 = '5be7c41a37e3fa1e8fa05f4a0815c9b807dcae74'
if ((Get-FileHash -LiteralPath $xex -Algorithm SHA1).Hash.ToLowerInvariant() -ne $expectedSha1) {
    throw 'Unsupported executable. Expected the PAL base default.xex; see docs/BUILDING.md.'
}
if (-not (Test-Path -LiteralPath (Join-Path $gamePath 'Bundle') -PathType Container)) { throw 'The extracted Bundle directory is missing.' }
$localDir = Join-Path $repoRoot '.local'
New-Item -ItemType Directory -Path $localDir -Force | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)
function TomlString([string]$value) { return (ConvertTo-Json -InputObject ($value.Replace('\','/')) -Compress) }
$manifestLines = @(
    '[project]', 'name = "nb"', 'sdk_version = "0.10.0"',
    ('game_root = ' + (TomlString $gamePath)), '', '[entrypoint]',
    ('file_path = ' + (TomlString $xex)),
    ('out_directory_path = ' + (TomlString (Join-Path $repoRoot 'generated/default'))),
    ('includes = [' + ((@('config/compatibility.toml','config/functions_pal.toml','config/hooks.toml') | ForEach-Object { TomlString (Join-Path $repoRoot $_) }) -join ', ') + ']')
)
$manifest = Join-Path $localDir 'nb_manifest.toml'
[IO.File]::WriteAllText($manifest, ($manifestLines -join "`n") + "`n", $utf8)
[IO.File]::WriteAllText((Join-Path $localDir 'game.json'), (ConvertTo-Json @{game_root=$gamePath;sha1=$expectedSha1}), $utf8)
if ($PrepareOnly) { Write-Output 'Game identity verified; local manifest prepared. No game files were copied.'; return }

function Run([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program failed (exit $LASTEXITCODE)." }
}
if (-not $VsInstall) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 Build Tools with C++, Clang and the Windows SDK first.' }
    $VsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $VsInstall) { throw 'Visual Studio C++ Build Tools were not found.' }
Import-Module (Join-Path $VsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $VsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=amd64 -host_arch=amd64' | Out-Null
$clangDir = Join-Path $VsInstall 'VC/Tools/Llvm/x64/bin'
$ninjaDir = Join-Path $VsInstall 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja'
$env:Path = "$clangDir;$ninjaDir;$env:Path"
$cmakeExe = Join-Path $VsInstall 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
foreach ($tool in @('git','clang','clang++','ninja')) { Get-Command $tool -CommandType Application -ErrorAction Stop | Out-Null }
$sdk = Join-Path $repoRoot '.deps/rexglue-sdk'
$sdkCommit = 'f5337cdc947ff6d4c4196737e2c807a48f2a1fc2'
if (-not (Test-Path -LiteralPath $sdk)) {
    Run git @('clone','--recursive','--branch','v0.10.0','--','https://github.com/rexglue/rexglue-sdk.git',$sdk)
}
$actualSdk = (& git -C $sdk rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualSdk -ne $sdkCommit) { throw 'The SDK is not the pinned v0.10.0 revision. Use a separate checkout; the script will not reset it.' }
Run git @('-C',$sdk,'submodule','update','--init','--recursive')
foreach ($patch in (Get-ChildItem -LiteralPath (Join-Path $repoRoot 'sdk-patches') -Filter '000*.patch' | Sort-Object Name)) {
    & git -C $sdk apply --reverse --check $patch.FullName 2>$null
    if ($LASTEXITCODE -eq 0) { continue }
    Run git @('-C',$sdk,'apply','--check',$patch.FullName)
    Run git @('-C',$sdk,'apply',$patch.FullName)
}
$build = Join-Path $repoRoot 'out/build/win-amd64-release'
Push-Location $repoRoot
try {
    Run $cmakeExe @('--preset','win-amd64-release',"-DREXSDK_DIR=$($sdk.Replace('\','/'))")
    Run $cmakeExe @('--build',$build,'--target','nb_stage_codegen_runtime','--parallel',"$Jobs")
    $codegenExe = (Get-Content -LiteralPath (Join-Path $build 'codegen-tool.txt') -Raw).Trim()
    Run $codegenExe @('codegen',$manifest)
    Run $cmakeExe @('--preset','win-amd64-release',"-DREXSDK_DIR=$($sdk.Replace('\','/'))")
    Run $cmakeExe @('--build',$build,'--target','nb','--parallel',"$Jobs")
} finally { Pop-Location }
Write-Output 'Local build complete. Run tools/play.ps1; capture and prepare local shaders for native rendering.'
