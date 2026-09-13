# Generate vs_<hash>.hlsl / ps_<hash>.hlsl and .meta sidecars from the user's SDK shader dump.
# Translate into a unique staging directory, compile every new or changed stage, then install only
# when every check succeeds. Replaced files are backed up; stages absent from the dump are retained.
# Generated library files are derived from the user's game dump and must not be committed or shipped.
#
# Usage: gen_native_shaders.ps1 -DumpDir C:\path\to\shader_dump [-OutDir ...\native_shaders]
#        [-Python python.exe] [-Fxc fxc.exe] [-Workers 4] [-RecheckAll] [-KeepArtifacts]
# By default this script claims machine.lock atomically. An already locked harness may pass
# -MachineLockOwnerPid <pid>; that PID must match the existing lock and still be alive. The harness
# remains responsible for releasing its lock. Both modes refuse a running game or build process.
# Use -RecheckAll after changing the shared prelude, compiler or defines: unchanged HLSL/meta otherwise skip
# compilation. Checks cover individual stages; runtime vertex/pixel combinations compile separately.
# Staging sources and logs are retained. -KeepArtifacts also retains .full.hlsl and .cso files after
# success; failures retain all diagnostics. No existing directory tree is deleted.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DumpDir,
    [string]$Records = "",  # Ignored compatibility input; records are no longer required.
    [string]$OutDir = (Join-Path $PSScriptRoot "..\out\build\win-amd64-release\native_shaders"),
    [string]$Python = "python",
    [string]$Fxc = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe",
    [ValidateRange(1, 16)][int]$Workers = 4,
    [string]$MachineLockPath = (Join-Path $PSScriptRoot "..\.local\machine.lock"),
    [ValidateRange(0, 2147483647)][int]$MachineLockOwnerPid = 0,
    [switch]$RecheckAll,
    [switch]$KeepArtifacts
)
$ErrorActionPreference = "Stop"
$tool = Join-Path $PSScriptRoot "ucode2hlsl.py"
$prelude = Join-Path $PSScriptRoot "..\src\gpu\native\shaders\prelude.hlsl"
if (-not (Test-Path -LiteralPath $DumpDir -PathType Container)) { throw "Dump directory not found: $DumpDir" }
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Translator not found: $tool" }
if (-not (Test-Path -LiteralPath $prelude -PathType Leaf)) { throw "Prelude not found: $prelude" }
$pythonCommand = Get-Command $Python -CommandType Application -ErrorAction Stop | Select-Object -First 1
$fxcCommand = Get-Command $Fxc -CommandType Application -ErrorAction Stop | Select-Object -First 1
$DumpDir = [IO.Path]::GetFullPath($DumpDir)
$OutDir = [IO.Path]::GetFullPath($OutDir)
$MachineLockPath = [IO.Path]::GetFullPath($MachineLockPath)
if ($Records) { Write-Warning "Records is ignored; generation consumes individual shader stages." }

function Get-ContentHash([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
    if (Test-Path -LiteralPath $Path) { throw "A directory collides with a shader file: $Path" }
    return $null
}

function ConvertTo-NativeArgument([string]$Value) {
    # ProcessStartInfo.Arguments uses Windows argv quoting, without a command shell. Double
    # backslashes before quotes and the closing quote; ordinary backslashes stay literal.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

$runId = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + [Guid]::NewGuid().ToString("N")
$claimToken = "gen-native-shaders-$runId"
$ownsLock = $false
$completed = $false
$compileArtifacts = [Collections.Generic.List[string]]::new()
$pendingFiles = [Collections.Generic.List[string]]::new()
$activeCompilers = [Collections.Generic.List[object]]::new()
$canReleaseLock = $true
$stageDir = $null
try {
    if ($MachineLockOwnerPid -gt 0) {
        if (-not (Test-Path -LiteralPath $MachineLockPath -PathType Leaf)) { throw "Caller lock not found: $MachineLockPath" }
        $lockText = [IO.File]::ReadAllText($MachineLockPath)
        if ($lockText -notmatch '(?m)^pid:\s*(\d+)\s*$' -or [int]$Matches[1] -ne $MachineLockOwnerPid) {
            throw "The existing machine lock does not belong to PID $MachineLockOwnerPid."
        }
        if (-not (Get-Process -Id $MachineLockOwnerPid -ErrorAction SilentlyContinue)) { throw "Machine lock owner PID $MachineLockOwnerPid is not running." }
    } else {
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($MachineLockPath)) | Out-Null
        $lockStream = [IO.File]::Open($MachineLockPath, 'CreateNew', 'Write', 'None')
        $ownsLock = $true
        try {
            $writer = [IO.StreamWriter]::new($lockStream)
            try {
                $writer.WriteLine("holder: gen_native_shaders.ps1")
                $writer.WriteLine("pid: $PID")
                $writer.WriteLine("purpose: generate and compile native shader stages")
                $writer.WriteLine("taken: $([DateTime]::UtcNow.ToString('o'))")
                $writer.WriteLine("claim: $claimToken")
                $writer.Flush()
            } finally { $writer.Dispose() }
        } finally { $lockStream.Dispose() }
    }
    $busy = @(Get-Process -Name nb,cmake,MSBuild,ninja,cl,clang,'clang++',link,fxc -ErrorAction SilentlyContinue)
    if ($busy.Count) {
        $processNames = ($busy | ForEach-Object { "$($_.ProcessName) PID $($_.Id)" }) -join ', '
        throw "Game or build process is running: $processNames"
    }
    $stageDir = Join-Path ([IO.Path]::GetTempPath()) "nb-native-shaders-$runId"
    [IO.Directory]::CreateDirectory($stageDir) | Out-Null
    $log = Join-Path $stageDir "generation.log"
    Write-Output "Staging shader generation in $stageDir"
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $pythonCommand.Source -B $tool $DumpDir -o $stageDir *> $log
        $translationExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    Get-Content -LiteralPath $log | Select-Object -Last 12
    if ($translationExit -ne 0) { throw "Shader translation failed with exit code $translationExit. See $log" }
    if (Select-String -LiteralPath $log -SimpleMatch ': translator error:' -Quiet) {
        throw "The translator reported an internal error. No library files were installed. See $log"
    }
    $stagedMetadata = @(Get-ChildItem -LiteralPath $stageDir -Filter '*.meta' -File | Sort-Object Name)
    if (-not $stagedMetadata.Count) { throw "No supported shader stages were generated. No library files were installed. See $log" }

    $candidates = [Collections.Generic.List[object]]::new()
    $unchanged = 0
    foreach ($metaFile in $stagedMetadata) {
        if ($metaFile.BaseName -notmatch '^(vs|ps)_([0-9A-Fa-f]{16})$') { throw "Unexpected stage filename: $($metaFile.Name)" }
        $stage = $Matches[1]; $shaderHash = $Matches[2]
        $hlsl = Join-Path $stageDir ($metaFile.BaseName + '.hlsl')
        if (-not (Test-Path -LiteralPath $hlsl -PathType Leaf)) { throw "Staged sidecar has no HLSL: $($metaFile.FullName)" }
        $meta = @{}
        foreach ($line in [IO.File]::ReadAllLines($metaFile.FullName)) {
            if ($line -match '^([^=]+)=(.*)$') { $meta[$Matches[1]] = $Matches[2] }
        }
        if ($meta['hash'] -ne $shaderHash) { throw "Sidecar hash does not match its filename: $($metaFile.FullName)" }
        $packedConstants = 0
        if ($meta['cruns']) {
            foreach ($range in $meta['cruns'].Split(',')) {
                if ($range -notmatch '^(\d+)-(\d+)$') { throw "Invalid constant range in $($metaFile.Name): $range" }
                $first = [int]$Matches[1]; $last = [int]$Matches[2]
                if ($first -gt $last -or $last -gt 255) { throw "Invalid constant range in $($metaFile.Name): $range" }
                $packedConstants += $last - $first + 1
            }
        }
        if ($packedConstants -gt 256) { throw "Too many packed constants in $($metaFile.Name)" }
        $targetHlsl = Join-Path $OutDir ($metaFile.BaseName + '.hlsl')
        $targetMeta = Join-Path $OutDir $metaFile.Name
        $oldHlslHash = Get-ContentHash $targetHlsl
        $oldMetaHash = Get-ContentHash $targetMeta
        $changed = $oldHlslHash -ne (Get-ContentHash $hlsl) -or $oldMetaHash -ne (Get-ContentHash $metaFile.FullName)
        if (-not $changed) { $unchanged++ }
        if ($changed -or $RecheckAll) {
            $candidates.Add([pscustomobject]@{ Stem=$metaFile.BaseName; Stage=$stage; Hlsl=$hlsl; Meta=$metaFile.FullName;
                TargetHlsl=$targetHlsl; TargetMeta=$targetMeta; OldHlslHash=$oldHlslHash; OldMetaHash=$oldMetaHash;
                Changed=$changed; PackedConstants=$packedConstants })
        }
    }

    $failed = [Collections.Generic.List[string]]::new()
    $preludeText = [IO.File]::ReadAllText($prelude)
    $compiled = 0
    $nextCandidate = 0
    Write-Output "Compiling $($candidates.Count) stages with up to $Workers workers."
    while ($nextCandidate -lt $candidates.Count -or $activeCompilers.Count) {
        while ($nextCandidate -lt $candidates.Count -and $activeCompilers.Count -lt $Workers) {
            $candidate = $candidates[$nextCandidate]
            $nextCandidate++
            $full = Join-Path $stageDir ($candidate.Stem + '.full.hlsl')
            $binary = Join-Path $stageDir ($candidate.Stem + '.cso')
            $compileLog = Join-Path $stageDir ($candidate.Stem + '.fxc.log')
            [IO.File]::WriteAllText($full, $preludeText + [Environment]::NewLine + [IO.File]::ReadAllText($candidate.Hlsl))
            $compileArtifacts.Add($full); $compileArtifacts.Add($binary)
            $pixelStage = if ($candidate.Stage -eq 'ps') { '1' } else { '0' }
            $constantDefine = if ($candidate.Stage -eq 'ps') { 'NB_PS_CONSTANTS' } else { 'NB_VS_CONSTANTS' }
            $constantCount = if ($candidate.PackedConstants) { $candidate.PackedConstants } else { 256 }
            $fxcArgs = @('/nologo', '/O3', '/T', "$($candidate.Stage)_5_1", '/E', 'main', '/D', "NB_PIXEL_STAGE=$pixelStage",
                '/D', 'NB_EFFECTIVE_SAMPLERS=1', '/D', "$constantDefine=$constantCount",
                '/enable_unbounded_descriptor_tables', '/Fo', $binary, $full)
            $start = [Diagnostics.ProcessStartInfo]::new()
            $start.FileName = $fxcCommand.Source
            $start.Arguments = ($fxcArgs | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
            $start.WorkingDirectory = $stageDir
            $start.UseShellExecute = $false
            $start.CreateNoWindow = $true
            $start.RedirectStandardOutput = $true
            $start.RedirectStandardError = $true
            $process = [Diagnostics.Process]::new()
            $process.StartInfo = $start
            $job = [pscustomobject]@{ Process=$process; Stem=$candidate.Stem; Binary=$binary; Log=$compileLog;
                StandardOutput=$null; StandardError=$null }
            # Register ownership before starting, so cancellation between launch and pipe setup
            # still reaches the child through the cleanup list.
            $activeCompilers.Add($job)
            if (-not $process.Start()) { throw "Could not start FXC for $($candidate.Stem)" }
            # Drain both pipes immediately; waiting for one pipe before the other can deadlock
            # when several compilers produce diagnostics at once.
            $job.StandardOutput = $process.StandardOutput.ReadToEndAsync()
            $job.StandardError = $process.StandardError.ReadToEndAsync()
        }
        $madeProgress = $false
        foreach ($job in @($activeCompilers.ToArray())) {
            if (-not $job.Process.HasExited) { continue }
            $stdout = $job.StandardOutput.GetAwaiter().GetResult()
            $stderr = $job.StandardError.GetAwaiter().GetResult()
            [IO.File]::WriteAllText($job.Log, $stdout + [Environment]::NewLine + $stderr)
            $compilerExit = $job.Process.ExitCode
            if ($compilerExit -ne 0 -or -not (Test-Path -LiteralPath $job.Binary -PathType Leaf) -or
                (Get-Item -LiteralPath $job.Binary).Length -eq 0) {
                $failed.Add($job.Stem)
                Write-Warning "Compile failed for $($job.Stem); see $($job.Log)"
            }
            $activeCompilers.Remove($job) | Out-Null
            $job.Process.Dispose()
            $compiled++
            $madeProgress = $true
            if (($compiled % 25) -eq 0 -or $compiled -eq $candidates.Count) {
                Write-Output "Compiled $compiled/$($candidates.Count) stages; $($failed.Count) failures."
            }
        }
        if (-not $madeProgress -and $activeCompilers.Count) { Start-Sleep -Milliseconds 50 }
    }
    if ($failed.Count) { throw "$($failed.Count) stage checks failed. No library files were installed. Diagnostics: $stageDir" }

    # Plan every destination and preserve previous contents before installing the first stage.
    $install = @($candidates | Where-Object Changed)
    $backupDir = Join-Path $OutDir ('.native-shader-backups\' + $runId)
    $plan = [Collections.Generic.List[object]]::new()
    foreach ($candidate in $install) {
        foreach ($suffix in @('Hlsl','Meta')) {
            $destination = $candidate.("Target" + $suffix)
            $oldHash = $candidate.("Old" + $suffix + "Hash")
            if ((Get-ContentHash $destination) -ne $oldHash) { throw "Library file changed during generation: $destination" }
            $plan.Add([pscustomobject]@{ Source=$candidate.$suffix; Destination=$destination; Existed=($null -ne $oldHash);
                Backup=(Join-Path $backupDir ([IO.Path]::GetFileName($destination))); Temporary=($destination + ".$runId.tmp") })
        }
    }
    if ($plan.Count) {
        [IO.Directory]::CreateDirectory($OutDir) | Out-Null
        [IO.Directory]::CreateDirectory($backupDir) | Out-Null
        foreach ($file in $plan) {
            if ($file.Existed) { [IO.File]::Copy($file.Destination, $file.Backup, $false) }
            $pendingFiles.Add($file.Temporary)
            [IO.File]::Copy($file.Source, $file.Temporary, $false)
        }
        $installed = [Collections.Generic.List[object]]::new()
        try {
            foreach ($file in $plan) {
                # Windows PowerShell 5 converts $null to an empty path for this string
                # argument. NullString supplies the CLR null required for no extra backup.
                if ($file.Existed) { [IO.File]::Replace($file.Temporary, $file.Destination, [NullString]::Value) }
                else { [IO.File]::Move($file.Temporary, $file.Destination) }
                $installed.Add($file)
            }
        } catch {
            $installError = $_
            foreach ($file in $installed) {
                try {
                    if ($file.Existed) { [IO.File]::Copy($file.Backup, $file.Destination, $true) }
                    elseif (Test-Path -LiteralPath $file.Destination -PathType Leaf) { Remove-Item -LiteralPath $file.Destination }
                } catch { Write-Warning "Could not restore $($file.Destination); preserved backup: $($file.Backup). $_" }
            }
            throw $installError
        }
    }
    $completed = $true
    Write-Output "Native shader library: $($install.Count) stages installed; $unchanged unchanged; $($candidates.Count) compiled."
    Write-Output "Generation log and diagnostics: $stageDir"
    if ($plan.Count) { Write-Output "Previous library files preserved in $backupDir" }
} finally {
    # An exception or cancellation must not leave this run's compilers alive after releasing
    # the machine lock. Other processes are never terminated by this cleanup.
    foreach ($job in $activeCompilers) {
        try {
            # Start may have failed before associating a process. There is no child to stop then.
            try { $childPid = $job.Process.Id } catch { continue }
            if (-not $job.Process.HasExited) { $job.Process.Kill() }
            if (-not $job.Process.WaitForExit(5000)) {
                $canReleaseLock = $false
                Write-Warning "FXC PID $childPid is still running; retain the machine lock."
            }
            if ($job.Process.HasExited -and $job.StandardOutput -and $job.StandardError) {
                try {
                    [IO.File]::WriteAllText($job.Log, $job.StandardOutput.GetAwaiter().GetResult() +
                        [Environment]::NewLine + $job.StandardError.GetAwaiter().GetResult())
                } catch { Write-Warning "Could not retain interrupted compiler diagnostics for $($job.Stem). $_" }
            }
        } catch {
            $canReleaseLock = $false
            Write-Warning "Could not finish FXC cleanup for $($job.Stem); retain the machine lock. $_"
        } finally { $job.Process.Dispose() }
    }
    foreach ($temporary in $pendingFiles) {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary -ErrorAction Continue }
    }
    if ($completed -and -not $KeepArtifacts) {
        foreach ($artifact in $compileArtifacts) {
            if (Test-Path -LiteralPath $artifact -PathType Leaf) { Remove-Item -LiteralPath $artifact -ErrorAction Continue }
        }
    }
    if ($ownsLock -and $canReleaseLock -and (Test-Path -LiteralPath $MachineLockPath -PathType Leaf)) {
        if ([IO.File]::ReadAllText($MachineLockPath).Contains("claim: $claimToken")) { Remove-Item -LiteralPath $MachineLockPath }
        else { Write-Warning "Machine lock changed ownership; leaving it in place: $MachineLockPath" }
    }
}
