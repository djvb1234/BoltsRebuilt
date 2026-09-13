#requires -Version 7.0
[CmdletBinding()]
param([string]$Python = 'python', [string]$Fxc = '', [ValidateRange(1,16)][int]$Workers = 4)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Fxc) {
    $sdkBins = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
    $Fxc = Get-ChildItem -LiteralPath $sdkBins -Directory | Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64/fxc.exe' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $Fxc) { throw 'fxc.exe was not found. Install the Windows SDK, or pass -Fxc <path>.' }
& (Join-Path $PSScriptRoot 'gen_native_shaders.ps1') -DumpDir (Join-Path $repoRoot '.local/shader-dump') -Python $Python -Fxc $Fxc -Workers $Workers
