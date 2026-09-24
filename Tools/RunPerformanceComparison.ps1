<#
.SYNOPSIS
Runs the RmlUi rendering and communication benchmarks and samples this run's process tree.
.DESCRIPTION
The editor runs windowed. Sampling includes only the launched editor and descendants
identified by PID and process creation time. CPU totals are last observed values.
The target interval is 500 ms; CIM query latency can make actual intervals longer.
Processes that start and exit between samples can be missed.
#>
[CmdletBinding()]
param(
    [string]$Project = '',
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [ValidateSet('DX11', 'DX12')][string]$RHI = 'DX12',
    [ValidateRange(0.1, 300.0)][double]$WarmupSeconds = 3.0,
    [ValidateRange(0.1, 3600.0)][double]$SampleSeconds = 10.0,
    [ValidateRange(1, 30)][int]$CommunicationTrials = 5,
    [ValidateRange(1, 4096)][int]$CommunicationRequests = 128,
    [ValidateRange(1000, 10000000)][int]$PuertsReads = 100000,
    [ValidateRange(1, 10)][int]$CommunicationWarmupBatches = 2,
    [ValidateRange(30, 86400)][int]$TimeoutSeconds = 900
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$invariant = [Globalization.CultureInfo]::InvariantCulture

function ConvertTo-NativeArgument([string]$Value) {
    # Start-Process joins ArgumentList into one command line on Windows.
    # Double backslashes before quotes and before the closing quote.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function Get-InstanceKey([int]$ProcessId, [datetime]$Created) {
    # Win32_Process's DMTF CreationDate has microsecond precision, whereas
    # Process.StartTime retains 100 ns ticks. Canonicalize both without using
    # floating-point division, which would lose precision for a DateTime tick count.
    $ticks = $Created.ToUniversalTime().Ticks
    $microsecondTicks = $ticks - ($ticks % 10)
    return '{0}:{1}' -f $ProcessId, $microsecondTicks
}

if ([string]::IsNullOrWhiteSpace($Project)) {
    $projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
    $projects = @(Get-ChildItem -LiteralPath $projectRoot -Filter '*.uproject' -File)
    if ($projects.Count -ne 1) {
        throw "Expected one .uproject in $projectRoot; use -Project to select the host project."
    }
    $Project = $projects[0].FullName
}
$Project = (Resolve-Path -LiteralPath $Project).Path
$projectRoot = Split-Path -Parent $Project
$editorPath = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
if (!(Test-Path -LiteralPath $editorPath -PathType Leaf)) {
    throw "UnrealEditor.exe not found: $editorPath"
}
$runName = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), $RHI
$runRoot = Join-Path $projectRoot "Saved\Performance\$runName"
$automationRoot = Join-Path $runRoot 'Automation'
$null = New-Item -ItemType Directory -Path $automationRoot -Force
$logPath = Join-Path $runRoot 'UnrealEditor.log'
$samplesPath = Join-Path $runRoot 'process-samples.csv'
$summaryPath = Join-Path $runRoot 'process-summary.json'
$manifestPath = Join-Path $runRoot 'run.json'

$editorArguments = @(
    $Project, "-$RHI", '-windowed', '-ResX=1280', '-ResY=800',
    '-unattended', '-NoSound', '-NoLiveCoding', '-nosplash',
    '-ExecCmds=r.VSync 0,t.MaxFPS 120,Automation RunTests RmlUiUnreal.Performance',
    '-TestExit=Automation Test Queue Empty',
    "-ReportExportPath=$automationRoot", "-abslog=$logPath",
    "-RmlUiPerfOutput=$runRoot",
    ('-RmlUiPerfWarmup={0}' -f $WarmupSeconds.ToString($invariant)),
    ('-RmlUiPerfSeconds={0}' -f $SampleSeconds.ToString($invariant)),
    "-RmlUiCommTrials=$CommunicationTrials",
    "-RmlUiCommRequests=$CommunicationRequests",
    "-RmlUiCommReads=$PuertsReads",
    "-RmlUiCommWarmup=$CommunicationWarmupBatches"
)
$commandLine = ($editorArguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
$startedUtc = [datetime]::UtcNow
$editorProcess = Start-Process -FilePath $editorPath -ArgumentList $commandLine `
    -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
$rootCreated = $editorProcess.StartTime.ToUniversalTime()
$rootKey = Get-InstanceKey $editorProcess.Id $rootCreated
$tracked = @{}
$tracked[$rootKey] = [pscustomobject]@{
    Pid = $editorProcess.Id; ParentPid = $null; Name = $editorProcess.ProcessName
    Created = $rootCreated; Key = $rootKey; FirstObserved = $startedUtc
    LastObserved = $null; State = 'New'; Cpu = $null; PeakWorkingSet = $null
    PeakPrivate = $null; SampleErrors = 0; SuccessfulSamples = 0
}
$sampler = @{ Index = 0; InventoryErrors = 0; TimedOut = $false; SamplingError = $null }
$stopwatch = [Diagnostics.Stopwatch]::StartNew()

Write-Host "UE PID: $($editorProcess.Id)"
Write-Host "Run output: $runRoot"
Write-Host "Process samples: $samplesPath"
Write-Host "Command: $editorPath $commandLine"

$manifest = [ordered]@{
    started_utc = $startedUtc.ToString('o'); finished_utc = $null
    project = $Project; engine_root = $EngineRoot; editor = $editorPath
    editor_pid = $editorProcess.Id; editor_instance = $rootKey; rhi = $RHI
    arguments = $editorArguments; output_directory = $runRoot
    frame_limit = 120; vsync = 0; sample_interval_seconds = 0.5; sample_interval_is_target = $true
    warmup_seconds = $WarmupSeconds; sample_seconds = $SampleSeconds
    communication_trials = $CommunicationTrials; communication_requests = $CommunicationRequests
    puerts_reads = $PuertsReads; communication_warmup_batches = $CommunicationWarmupBatches
    timeout_seconds = $TimeoutSeconds; timed_out = $false; exit_code = $null
    sampler_error = $null; inventory_errors = 0; editor_successful_samples = 0; automation = $null
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

function New-SampleRow($Instance, [string]$Event, [string]$Message = '') {
    return [pscustomobject][ordered]@{
        sample_index = $sampler.Index
        timestamp_utc = [datetime]::UtcNow.ToString('o')
        elapsed_seconds = $stopwatch.Elapsed.TotalSeconds.ToString('F6', $invariant)
        pid = if ($null -eq $Instance) { $null } else { $Instance.Pid }
        parent_pid = if ($null -eq $Instance) { $null } else { $Instance.ParentPid }
        process_name = if ($null -eq $Instance) { $null } else { $Instance.Name }
        instance_key = if ($null -eq $Instance) { $null } else { $Instance.Key }
        event = $Event
        cpu_total_seconds = $null
        working_set_bytes = $null
        private_bytes = $null
        error = $Message
    }
}

function Write-ProcessSample {
    $sampler.Index++
    $rows = [Collections.Generic.List[object]]::new()
    try {
        $inventory = @(Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, Name, CreationDate)
    } catch {
        $sampler.InventoryErrors++
        $rows.Add((New-SampleRow $null 'InventoryFailed' $_.Exception.Message))
        $inventory = $null
    }

    if ($null -ne $inventory) {
        $live = @{}
        foreach ($entry in $inventory) {
            if ($null -ne $entry.CreationDate) {
                $entryKey = Get-InstanceKey ([int]$entry.ProcessId) $entry.CreationDate
                $live[$entryKey] = $entry
            }
        }
        # Continue following surviving descendants if the original editor exits.
        $parents = @{}
        foreach ($instance in @($tracked.Values)) {
            if ($live.ContainsKey($instance.Key)) {
                $parents[$instance.Pid] = $instance
                $instance.ParentPid = [int]$live[$instance.Key].ParentProcessId
            }
        }
        do {
            $discovered = $false
            foreach ($entry in $inventory) {
                if ($null -eq $entry.CreationDate) { continue }
                $entryKey = Get-InstanceKey ([int]$entry.ProcessId) $entry.CreationDate
                if ($tracked.ContainsKey($entryKey)) { continue }
                $parentId = [int]$entry.ParentProcessId
                if (!$parents.ContainsKey($parentId)) { continue }
                $created = $entry.CreationDate.ToUniversalTime()
                if ($created -lt $parents[$parentId].Created) { continue }
                $instance = [pscustomobject]@{
                    Pid = [int]$entry.ProcessId; ParentPid = $parentId; Name = [string]$entry.Name
                    Created = $created; Key = $entryKey; FirstObserved = [datetime]::UtcNow
                    LastObserved = $null; State = 'New'; Cpu = $null; PeakWorkingSet = $null
                    PeakPrivate = $null; SampleErrors = 0; SuccessfulSamples = 0
                }
                $tracked[$entryKey] = $instance
                $parents[$instance.Pid] = $instance
                $discovered = $true
            }
        } while ($discovered)
    }

    foreach ($instance in @($tracked.Values)) {
        if ($instance.State -eq 'Exited') { continue }
        if ($null -ne $inventory -and !$live.ContainsKey($instance.Key)) {
            $rows.Add((New-SampleRow $instance 'Exited'))
            $instance.State = 'Exited'
            continue
        }
        try {
            $observed = Get-Process -Id $instance.Pid -ErrorAction Stop
            $observedKey = Get-InstanceKey $observed.Id $observed.StartTime
            if ($observedKey -ne $instance.Key) {
                $rows.Add((New-SampleRow $instance 'Exited' 'PID was reused; replacement is excluded.'))
                $instance.State = 'Exited'
                continue
            }
            $cpu = $observed.TotalProcessorTime.TotalSeconds
            $workingSet = $observed.WorkingSet64
            $privateBytes = $observed.PrivateMemorySize64
            $row = New-SampleRow $instance $instance.State
            $row.cpu_total_seconds = $cpu.ToString('F6', $invariant)
            $row.working_set_bytes = $workingSet
            $row.private_bytes = $privateBytes
            $rows.Add($row)
            $instance.SuccessfulSamples++
            $instance.Cpu = $cpu
            $instance.LastObserved = [datetime]::UtcNow
            if ($null -eq $instance.PeakWorkingSet -or $workingSet -gt $instance.PeakWorkingSet) {
                $instance.PeakWorkingSet = $workingSet
            }
            if ($null -eq $instance.PeakPrivate -or $privateBytes -gt $instance.PeakPrivate) {
                $instance.PeakPrivate = $privateBytes
            }
            $instance.State = 'Sample'
        } catch {
            $instance.SampleErrors++
            $rows.Add((New-SampleRow $instance 'SampleFailed' $_.Exception.Message))
        }
    }
    if ($rows.Count -gt 0) {
        $rows | Export-Csv -LiteralPath $samplesPath -Append -NoTypeInformation -Encoding UTF8
    }
}

function Stop-ThisRun {
    $editorProcess.Refresh()
    if (!$editorProcess.HasExited) {
        Write-Warning "Requesting graceful shutdown of this run's editor PID $($editorProcess.Id)."
        $null = $editorProcess.CloseMainWindow()
        $gracefulUntil = [datetime]::UtcNow.AddSeconds(10)
        while (!$editorProcess.HasExited -and [datetime]::UtcNow -lt $gracefulUntil) {
            Write-ProcessSample
            Start-Sleep -Milliseconds 500
            $editorProcess.Refresh()
        }
    }
    foreach ($instance in @($tracked.Values | Sort-Object Created -Descending)) {
        try {
            $observed = Get-Process -Id $instance.Pid -ErrorAction Stop
            if ((Get-InstanceKey $observed.Id $observed.StartTime) -eq $instance.Key) {
                Write-Warning "Stopping this run's process $($instance.Name), PID $($instance.Pid)."
                $observed.Kill()
            }
        } catch {
            # A child may already have exited. Never stop another PID instance.
            Write-Verbose "Shutdown observation for PID $($instance.Pid): $($_.Exception.Message)"
        }
    }
}

try {
    while ($true) {
        $sampleStarted = $stopwatch.Elapsed.TotalMilliseconds
        Write-ProcessSample
        $editorProcess.Refresh()
        if ($editorProcess.HasExited) { break }
        if ($stopwatch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
            $sampler.TimedOut = $true
            Stop-ThisRun
            break
        }
        $remaining = 500 - ($stopwatch.Elapsed.TotalMilliseconds - $sampleStarted)
        if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int]$remaining) }
    }
} catch {
    $sampler.SamplingError = $_.Exception.Message
    Stop-ThisRun
} finally {
    $editorProcess.Refresh()
    if ($editorProcess.HasExited) {
        $editorProcess.WaitForExit()
        $manifest.exit_code = $editorProcess.ExitCode
    }
    try { Write-ProcessSample } catch { $sampler.SamplingError = $_.Exception.Message }
    $manifest.finished_utc = [datetime]::UtcNow.ToString('o')
    $manifest.timed_out = $sampler.TimedOut
    $manifest.inventory_errors = $sampler.InventoryErrors
    $manifest.sampler_error = $sampler.SamplingError
    $manifest.editor_successful_samples = $tracked[$rootKey].SuccessfulSamples
    $processSummary = @($tracked.Values | Sort-Object Created | ForEach-Object {
        [ordered]@{
            pid = $_.Pid; parent_pid = $_.ParentPid; process_name = $_.Name; instance_key = $_.Key
            created_utc = $_.Created.ToString('o'); first_observed_utc = $_.FirstObserved.ToString('o')
            last_observed_utc = if ($null -eq $_.LastObserved) { $null } else { $_.LastObserved.ToString('o') }
            final_observed_state = $_.State; cpu_total_last_observed_seconds = $_.Cpu
            peak_working_set_observed_bytes = $_.PeakWorkingSet
            peak_private_observed_bytes = $_.PeakPrivate; sample_errors = $_.SampleErrors
            successful_samples = $_.SuccessfulSamples
        }
    })
    [ordered]@{
        sampling_interval_seconds = 0.5; sampling_interval_is_target = $true; inventory_errors = $sampler.InventoryErrors
        limitations = @(
            '500 ms is a target interval. CIM query latency can extend actual intervals; use the CSV timestamps.'
            'Processes alive entirely between samples can be missed.'
            'Exited process CPU totals are the last successful observation, not exact exit totals.'
            'Failed samples contain null metrics; they are never replaced with zero.'
            'CPU totals include startup, warmup and tests; use timestamps to select a test interval.'
            'Working sets can share pages, and per-process peaks occur at different times; do not sum peaks as simultaneous usage.'
        )
        processes = $processSummary
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
}

if ($sampler.TimedOut) { throw "Benchmark timed out after $TimeoutSeconds seconds. See $logPath" }
if ($null -ne $sampler.SamplingError) { throw "Process sampler failed: $($sampler.SamplingError). See $runRoot" }
$rootMetrics = $tracked[$rootKey]
if ($rootMetrics.SuccessfulSamples -lt 1 -or $null -eq $rootMetrics.LastObserved -or
    $null -eq $rootMetrics.Cpu -or $null -eq $rootMetrics.PeakWorkingSet -or $null -eq $rootMetrics.PeakPrivate) {
    throw "Process sampler did not capture valid editor metrics. See $samplesPath and $summaryPath"
}
if ($null -eq $manifest.exit_code -or $manifest.exit_code -ne 0) {
    throw "Unreal automation did not exit successfully (exit=$($manifest.exit_code)). See $logPath"
}
$indexPath = Join-Path $automationRoot 'index.json'
if (!(Test-Path -LiteralPath $indexPath -PathType Leaf)) { throw "Automation did not produce $indexPath" }
$report = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
$passed = [int]$report.succeeded + [int]$report.succeededWithWarnings
$manifest.automation = [ordered]@{
    passed = $passed; warnings = [int]$report.succeededWithWarnings
    failed = [int]$report.failed; not_run = [int]$report.notRun; report = $indexPath
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
if ($report.failed -ne 0 -or $report.notRun -ne 0 -or $passed -lt 2) {
    throw "Automation results: passed=$passed, failed=$($report.failed), notRun=$($report.notRun). See $indexPath"
}
Write-Host "Performance comparison passed: $passed tests ($($report.succeededWithWarnings) with warnings)."
Write-Host "Reports and process samples: $runRoot"
