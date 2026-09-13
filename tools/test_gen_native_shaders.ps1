# CPU-only regression fixture for gen_native_shaders.ps1. Python deliberately rejects FXC's
# arguments, so no real compiler is run. The actual production installation block is also
# exercised directly with synthetic files, including failure after replacement and creation.
# The wrapper uses a private fixture lock; this script never claims the production relay lock.
# Run when the game and build tools are closed: the production wrapper's busy-process guard
# remains enabled. All fixture files and diagnostics are retained under ScratchRoot.
# Usage: powershell.exe -NoProfile -File tools/test_gen_native_shaders.ps1 -Python python.exe
[CmdletBinding()]
param(
    [string]$Python = 'python',
    [string]$ScratchRoot = [IO.Path]::GetTempPath()
)
$ErrorActionPreference = 'Stop'
$wrapper = Join-Path $PSScriptRoot 'gen_native_shaders.ps1'
$python = (Get-Command $Python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$fixture = Join-Path ([IO.Path]::GetFullPath($ScratchRoot)) ('parallel_shader_fixture_' + [Guid]::NewGuid().ToString('N'))
$dump = Join-Path $fixture 'dump'
$installed = Join-Path $fixture 'installed'
$lockPath = Join-Path $fixture 'private_fixture.lock'
[IO.Directory]::CreateDirectory($dump) | Out-Null
[IO.Directory]::CreateDirectory($installed) | Out-Null

$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($wrapper, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw "Wrapper parsing failed: $parseErrors" }
$quoteFunction = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'ConvertTo-NativeArgument'
}, $true)
if (-not $quoteFunction) { throw 'Native argument quoting function not found' }
. ([scriptblock]::Create($quoteFunction.Extent.Text))
$installBlocks = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.TryStatementAst] -and
        $node.CatchClauses.Count -eq 1 -and $node.Body.Extent.Text.Contains('$installed.Add($file)')
}, $true))
if ($installBlocks.Count -ne 1) { throw 'Expected one production installation and rollback block' }
$installBlock = [scriptblock]::Create($installBlocks[0].Extent.Text)

function Test-Installation([string]$Name, [string]$Collision = '') {
    $directory = Join-Path $fixture $Name
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $existingPath = Join-Path $directory 'existing.hlsl'
    $newPath = Join-Path $directory 'new.meta'
    $backupPath = Join-Path $directory 'existing.backup'
    $oldBytes = 'Previously installed synthetic stage.'
    [IO.File]::WriteAllText($existingPath, $oldBytes)
    [IO.File]::Copy($existingPath, $backupPath, $false)
    $plan = [Collections.Generic.List[object]]::new()
    foreach ($entry in @(
        @{ Destination=$existingPath; Existed=$true; Text='Replacement synthetic stage.' },
        @{ Destination=$newPath; Existed=$false; Text='New synthetic metadata.' }
    )) {
        $temporary = $entry.Destination + '.tmp'
        [IO.File]::WriteAllText($temporary, $entry.Text)
        $plan.Add([pscustomobject]@{ Temporary=$temporary; Destination=$entry.Destination;
            Existed=$entry.Existed; Backup=$backupPath })
    }
    $collisionPath = Join-Path $directory 'late_collision'
    $foreignBytes = 'Concurrent foreign file must survive.'
    if ($Collision) {
        if ($Collision -eq 'file') { [IO.File]::WriteAllText($collisionPath, $foreignBytes) }
        else { [IO.Directory]::CreateDirectory($collisionPath) | Out-Null }
        [IO.File]::WriteAllText(($collisionPath + '.tmp'), 'Must not replace the collision.')
        $plan.Add([pscustomobject]@{ Temporary=($collisionPath + '.tmp'); Destination=$collisionPath;
            Existed=$false; Backup=$null })
    }
    $installed = [Collections.Generic.List[object]]::new()
    $caught = $null
    try { & $installBlock } catch { $caught = $_ }
    if ($installed.Count -ne 2) { throw "$Name did not complete both the replacement and new-file branches: $caught" }
    if ([IO.File]::ReadAllText($backupPath) -cne $oldBytes) { throw "$Name changed the preserved backup" }
    if ($Collision) {
        if (-not $caught) { throw "$Name unexpectedly accepted the collision" }
        if ([IO.File]::ReadAllText($existingPath) -cne $oldBytes -or (Test-Path -LiteralPath $newPath)) {
            throw "$Name did not restore the old stage and remove the newly installed file"
        }
        if ($Collision -eq 'file') {
            if ([IO.File]::ReadAllText($collisionPath) -cne $foreignBytes) { throw "$Name overwrote or deleted the foreign file" }
        } elseif (-not (Test-Path -LiteralPath $collisionPath -PathType Container)) {
            throw "$Name removed the foreign directory"
        }
    } else {
        if ($caught) { throw "$Name failed: $caught" }
        if ([IO.File]::ReadAllText($existingPath) -cne 'Replacement synthetic stage.' -or
            [IO.File]::ReadAllText($newPath) -cne 'New synthetic metadata.') { throw "$Name published incorrect bytes" }
    }
}
Test-Installation 'install_success'
Test-Installation 'rollback_file_collision' 'file'
Test-Installation 'rollback_directory_collision' 'directory'

$expected = @('', 'plain', 'two words', 'C:\folder with spaces\', 'embedded "quote"', 'three\\\"slashes', '$notShell`text')
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $python
$argv = @('-c', 'import json,sys; print(json.dumps(sys.argv[1:]))') + $expected
$start.Arguments = ($argv | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
try {
    if (-not $process.Start()) { throw 'Argument round-trip process failed to start' }
    $output = $process.StandardOutput.ReadToEndAsync()
    $errorText = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(20000)) {
        $process.Kill()
        if (-not $process.WaitForExit(5000)) { throw 'Argument round-trip child did not stop after termination' }
        throw 'Argument round-trip timed out'
    }
    if ($process.ExitCode) { throw $errorText.GetAwaiter().GetResult() }
    $actual = $output.GetAwaiter().GetResult() | ConvertFrom-Json
    if ($actual.Count -ne $expected.Count) { throw 'Argument count changed' }
    for ($i = 0; $i -lt $expected.Count; $i++) {
        if ($actual[$i] -cne $expected[$i]) { throw "Argument $i changed during native invocation" }
    }
} finally { $process.Dispose() }

for ($i = 1; $i -le 8; $i++) {
    $hash = $i.ToString('X16')
    $extension = if ($i % 2) { 'vert' } else { 'frag' }
    $source = if ($i % 2) { 'add oPos, c0, c1' } else { 'add oC0, c0, c1' }
    [IO.File]::WriteAllText((Join-Path $dump "shader_$hash.ucode.$extension"), $source)
}
$sentinelPath = Join-Path $installed 'vs_0000000000000001.hlsl'
$sentinel = 'Existing library content must survive every compile failure.'
[IO.File]::WriteAllText($sentinelPath, $sentinel)
$sentinelTime = [IO.File]::GetLastWriteTimeUtc($sentinelPath)
$log = Join-Path $fixture 'failed_compile.log'
$ErrorActionPreference = 'Continue'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper -DumpDir $dump -OutDir $installed -Python $python -Fxc $python -Workers 4 -MachineLockPath $lockPath *> $log
$wrapperExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($wrapperExit -eq 0) { throw 'Deliberately failing compiler was accepted' }
if (Test-Path -LiteralPath $lockPath) { throw 'Private fixture lock was not released after worker completion' }
if ([IO.File]::ReadAllText($sentinelPath) -cne $sentinel -or [IO.File]::GetLastWriteTimeUtc($sentinelPath) -ne $sentinelTime) {
    throw 'Existing installed stage changed during failed compilation'
}
if (@(Get-ChildItem -LiteralPath $installed -Recurse -File).Count -ne 1) { throw 'Failed compilation installed a new file' }
$logText = [IO.File]::ReadAllText($log)
if ($logText -notmatch 'Compiled 8/8 stages; 8 failures') { throw "Did not observe all eight worker results. See $log" }
if ($logText -notmatch '(?m)^Staging shader generation in (.+)\r?$') { throw 'Staging path was not reported' }
$staging = $Matches[1].Trim()
if (@(Get-ChildItem -LiteralPath $staging -Filter '*.fxc.log' -File).Count -ne 8) { throw 'Missing per-stage diagnostics' }
$remaining = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine.Contains($staging) })
if ($remaining.Count) { throw 'Compiler children remain after wrapper exit' }
foreach ($workers in @(0,17)) {
    $invalidLog = Join-Path $fixture "invalid_workers_$workers.log"
    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper -DumpDir $dump -OutDir $installed -Python $python -Fxc $python -Workers $workers -MachineLockPath $lockPath *> $invalidLog
    $invalidExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($invalidExit -eq 0 -or (Test-Path -LiteralPath $lockPath)) { throw "Invalid Workers=$workers accepted or mutated the lock" }
}
$report = [ordered]@{ result='PASS'; argument_roundtrip=$expected.Count; workers=4; rejected_compiles=8;
    installed_files_changed=0; remaining_children=0; invalid_worker_values_rejected=@(0,17);
    installation_branch_checks=3; replaced_existing_and_created_new=$true;
    rollback_restored_existing_and_removed_new=$true; foreign_file_and_directory_preserved=$true;
    production_machine_lock_used=$false; actual_fxc_invocations=0; staging=$staging; log=$log }
$report | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture 'report.json') -Encoding UTF8
$report | ConvertTo-Json
Write-Output "Fixture retained in $fixture"
