<#
.SYNOPSIS
Runs independent long-duration RmlUi clip-topology churn processes without GPU tracing.
#>
[CmdletBinding()]
param(
    [string]$Project = '',
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [ValidateSet('DX11', 'DX12')][string]$RHI = 'DX12',
    [ValidateRange(1, 32)][int]$ViewCount = 8,
    [ValidateRange(0.25, 120.0)][double]$Hz = 30.0,
    [int[]]$DurationsSeconds = @(300, 900, 1800),
    [ValidateRange(0.0, 600.0)][double]$TeardownSeconds = 30.0,
    [ValidateRange(1, 20)][int]$SampleCount = 1,
    [ValidateRange(30, 1800)][int]$TimeoutPaddingSeconds = 180,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function ConvertTo-NativeArgument([string]$Value) {
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function Get-Percentile([double[]]$Values, [double]$Probability) {
    if ($Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $position = ($sorted.Count - 1) * $Probability
    $lower = [Math]::Floor($position)
    $upper = [Math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$sorted[$lower] }
    $weight = $position - $lower
    return [double]$sorted[$lower] * (1.0 - $weight) + [double]$sorted[$upper] * $weight
}

function Measure-NumericDistribution([object[]]$InputValues) {
    [double[]]$values = @($InputValues | Where-Object { $null -ne $_ } |
        ForEach-Object { [double]$_ })
    if ($values.Count -eq 0) {
        return [ordered]@{ samples = @(); min = $null; max = $null; mean = $null;
            p50 = $null; p95 = $null; p99 = $null }
    }
    $measure = $values | Measure-Object -Minimum -Maximum -Average
    return [ordered]@{
        samples = $values
        min = $measure.Minimum
        max = $measure.Maximum
        mean = $measure.Average
        p50 = Get-Percentile $values 0.50
        p95 = Get-Percentile $values 0.95
        p99 = Get-Percentile $values 0.99
    }
}

if ($DurationsSeconds.Count -eq 0) { throw 'DurationsSeconds must contain at least one duration.' }
foreach ($duration in $DurationsSeconds) {
    if ($duration -lt 1 -or $duration -gt 7200) {
        throw "Each duration must be between 1 and 7200 seconds; received $duration."
    }
}

if ([string]::IsNullOrWhiteSpace($Project)) {
    $projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..')).Path
    $projects = @(Get-ChildItem -LiteralPath $projectRoot -Filter '*.uproject' -File)
    if ($projects.Count -ne 1) {
        throw "Expected one .uproject in $projectRoot; use -Project to select the host project."
    }
    $Project = $projects[0].FullName
} else {
    $Project = (Resolve-Path -LiteralPath $Project).Path
    $projectRoot = Split-Path -Parent $Project
}
$EngineRoot = (Resolve-Path -LiteralPath $EngineRoot).Path
$editorPath = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
if (!(Test-Path -LiteralPath $editorPath -PathType Leaf)) {
    throw "UnrealEditor was not found at $editorPath"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $runName = 'TopologyChurnSoak-{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $RHI
    $OutputDirectory = Join-Path $projectRoot "Saved\Performance\$runName"
} elseif (![IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot $OutputDirectory
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force

$startedUtc = [datetime]::UtcNow
$records = [Collections.Generic.List[object]]::new()
$rhiArgument = if ($RHI -eq 'DX12') { '-DX12' } else { '-DX11' }

foreach ($duration in $DurationsSeconds) {
    for ($sampleIndex = 1; $sampleIndex -le $SampleCount; ++$sampleIndex) {
        $runDirectory = Join-Path $OutputDirectory ('Duration-{0:D4}\Sample-{1:D3}' -f
            $duration, $sampleIndex)
        $automationDirectory = Join-Path $runDirectory 'Automation'
        $performanceDirectory = Join-Path $runDirectory 'Performance'
        $logPath = Join-Path $runDirectory 'UnrealEditor.log'
        $null = New-Item -ItemType Directory -Path $runDirectory -Force

        $arguments = @(
            $Project, $rhiArgument, '-Unattended', '-NoSplash', '-NoSound', '-NoLiveCoding',
            '-RenderOffscreen',
            '-ExecCmds=Automation RunTests RmlUiUnreal.Performance.ClipTopologyChurn;Quit',
            '-TestExit=Automation Test Queue Empty',
            "-ReportExportPath=$automationDirectory",
            "-RmlUiPerfOutput=$performanceDirectory",
            "-RmlUiTopologyChurnViewCount=$ViewCount",
            ('-RmlUiTopologyChurnHz={0}' -f $Hz.ToString(
                [Globalization.CultureInfo]::InvariantCulture)),
            "-RmlUiTopologyChurnSeconds=$duration",
            ('-RmlUiTopologyChurnTeardownSeconds={0}' -f $TeardownSeconds.ToString(
                [Globalization.CultureInfo]::InvariantCulture)),
            "-abslog=$logPath"
        )
        $commandLine = ($arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
        $runStartedUtc = [datetime]::UtcNow
        $process = Start-Process -FilePath $editorPath -ArgumentList $commandLine `
            -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        $timeoutSeconds = [int][Math]::Ceiling($duration + $TeardownSeconds) +
            $TimeoutPaddingSeconds
        $timedOut = !$process.WaitForExit($timeoutSeconds * 1000)
        if ($timedOut) {
            try { $process.Kill($true) } catch { Write-Warning $_.Exception.Message }
            $process.WaitForExit()
        }
        $process.Refresh()

        $automationPath = Join-Path $automationDirectory 'index.json'
        $automation = if (Test-Path -LiteralPath $automationPath) {
            Get-Content -Raw -LiteralPath $automationPath | ConvertFrom-Json
        } else { $null }
        $performanceFile = Get-ChildItem -LiteralPath $performanceDirectory `
            -Filter 'RmlUiClipTopologyChurn-*.json' -ErrorAction SilentlyContinue |
            Select-Object -First 1
        $scenario = $null
        if ($null -ne $performanceFile) {
            $performance = Get-Content -Raw -LiteralPath $performanceFile.FullName |
                ConvertFrom-Json
            if (@($performance.scenarios).Count -eq 1) { $scenario = $performance.scenarios[0] }
        }

        $automationPassed = $null -ne $automation -and $automation.failed -eq 0 -and
            ($automation.succeeded + $automation.succeededWithWarnings) -gt 0
        $viewsReleased = $TeardownSeconds -le 0.0 -or ($null -ne $scenario -and
            $scenario.process_memory.live_widgets_after_teardown -eq 0 -and
            !$scenario.process_memory.window_alive_after_teardown)
        $mutationContractPassed = $null -ne $scenario -and
            $scenario.successful_property_mutations -eq $scenario.observed_topology_changes
        $passed = !$timedOut -and $process.ExitCode -eq 0 -and $automationPassed -and
            $null -ne $scenario -and $viewsReleased -and $mutationContractPassed

        $memory = if ($null -ne $scenario) { $scenario.process_memory } else { $null }
        $samples = if ($null -ne $memory) { @($memory.samples) } else { @() }
        $tailSlope = if ($null -ne $memory -and $memory.PSObject.Properties.Name -contains
            'tail_endpoint_physical_slope_mib_per_min') {
            [double]$memory.tail_endpoint_physical_slope_mib_per_min
        } else { $null }
        $physicalDelta = if ($samples.Count -gt 1) {
            [double]($samples[-1].used_physical_mib - $samples[0].used_physical_mib)
        } else { $null }
        $virtualDelta = if ($samples.Count -gt 1) {
            [double]($samples[-1].used_virtual_mib - $samples[0].used_virtual_mib)
        } else { $null }
        $memoryPropertyNames = if ($null -ne $memory) {
            @($memory.PSObject.Properties.Name)
        } else { @() }
        $tailOlsPhysical = if ($memoryPropertyNames -contains
            'tail_ols_physical_slope_mib_per_min') {
            [double]$memory.tail_ols_physical_slope_mib_per_min
        } else { $null }
        $tailOlsVirtual = if ($memoryPropertyNames -contains
            'tail_ols_virtual_slope_mib_per_min') {
            [double]$memory.tail_ols_virtual_slope_mib_per_min
        } else { $null }
        $last60OlsPhysical = if ($memoryPropertyNames -contains
            'last_60s_ols_physical_slope_mib_per_min') {
            [double]$memory.last_60s_ols_physical_slope_mib_per_min
        } else { $null }
        $last60OlsVirtual = if ($memoryPropertyNames -contains
            'last_60s_ols_virtual_slope_mib_per_min') {
            [double]$memory.last_60s_ols_virtual_slope_mib_per_min
        } else { $null }
        $drainToFinal = if ($TeardownSeconds -gt 0.0 -and $null -ne $memory) {
            [double]($memory.used_physical_after_teardown_mib -
                $memory.used_physical_after_drain_mib)
        } else { $null }

        $record = [ordered]@{
            duration_seconds = $duration
            sample_index = $sampleIndex
            started_utc = $runStartedUtc.ToString('o')
            finished_utc = [datetime]::UtcNow.ToString('o')
            timeout_seconds = $timeoutSeconds
            timed_out = $timedOut
            exit_code = $process.ExitCode
            passed = $passed
            automation_report = $automationPath
            performance_report = if ($null -ne $performanceFile) {
                $performanceFile.FullName
            } else { $null }
            log = $logPath
            automation_succeeded = if ($null -ne $automation) { $automation.succeeded } else { 0 }
            automation_succeeded_with_warnings = if ($null -ne $automation) {
                $automation.succeededWithWarnings
            } else { 0 }
            automation_failed = if ($null -ne $automation) { $automation.failed } else { $null }
            achieved_hz = if ($null -ne $scenario) { $scenario.achieved_hz } else { $null }
            mutations = if ($null -ne $scenario) {
                $scenario.successful_property_mutations
            } else { $null }
            topology_changes = if ($null -ne $scenario) {
                $scenario.observed_topology_changes
            } else { $null }
            missed_intervals = if ($null -ne $scenario) { $scenario.missed_intervals } else { $null }
            physical_delta_mib = $physicalDelta
            virtual_delta_mib = $virtualDelta
            tail_endpoint_physical_slope_mib_per_min = $tailSlope
            tail_ols_physical_slope_mib_per_min = $tailOlsPhysical
            tail_ols_virtual_slope_mib_per_min = $tailOlsVirtual
            last_60s_ols_physical_slope_mib_per_min = $last60OlsPhysical
            last_60s_ols_virtual_slope_mib_per_min = $last60OlsVirtual
            drain_to_teardown_final_physical_delta_mib = $drainToFinal
            teardown_physical_range_mib = if ($TeardownSeconds -gt 0.0 -and $null -ne $memory) {
                $memory.used_physical_teardown_range_mib
            } else { $null }
            teardown_endpoint_physical_slope_mib_per_min = if ($TeardownSeconds -gt 0.0 -and
                $null -ne $memory) {
                $memory.teardown_endpoint_physical_slope_mib_per_min
            } else { $null }
            teardown_ols_physical_slope_mib_per_min = if ($memoryPropertyNames -contains
                'teardown_ols_physical_slope_mib_per_min') {
                $memory.teardown_ols_physical_slope_mib_per_min
            } else { $null }
            teardown_ols_virtual_slope_mib_per_min = if ($memoryPropertyNames -contains
                'teardown_ols_virtual_slope_mib_per_min') {
                $memory.teardown_ols_virtual_slope_mib_per_min
            } else { $null }
            all_views_released = $viewsReleased
            mutation_contract_passed = $mutationContractPassed
        }
        $records.Add($record)
        Write-Host ('Topology soak: duration={0}s sample={1} passed={2} changes={3}' -f
            $duration, $sampleIndex, $passed, $record.topology_changes)
    }
}

$durationSummaries = [Collections.Generic.List[object]]::new()
foreach ($duration in $DurationsSeconds) {
    $durationRecords = @($records | Where-Object { $_.duration_seconds -eq $duration })
    $durationSummaries.Add([ordered]@{
        duration_seconds = $duration
        samples = $durationRecords.Count
        passed = @($durationRecords | Where-Object { !$_.passed }).Count -eq 0
        all_views_released = @($durationRecords | Where-Object { !$_.all_views_released }).Count -eq 0
        achieved_hz = Measure-NumericDistribution @($durationRecords.achieved_hz)
        topology_changes = Measure-NumericDistribution @($durationRecords.topology_changes)
        physical_delta_mib = Measure-NumericDistribution @($durationRecords.physical_delta_mib)
        virtual_delta_mib = Measure-NumericDistribution @($durationRecords.virtual_delta_mib)
        tail_endpoint_physical_slope_mib_per_min = Measure-NumericDistribution @(
            $durationRecords.tail_endpoint_physical_slope_mib_per_min)
        tail_ols_physical_slope_mib_per_min = Measure-NumericDistribution @(
            $durationRecords.tail_ols_physical_slope_mib_per_min)
        tail_ols_virtual_slope_mib_per_min = Measure-NumericDistribution @(
            $durationRecords.tail_ols_virtual_slope_mib_per_min)
        last_60s_ols_physical_slope_mib_per_min = Measure-NumericDistribution @(
            $durationRecords.last_60s_ols_physical_slope_mib_per_min)
        last_60s_ols_virtual_slope_mib_per_min = Measure-NumericDistribution @(
            $durationRecords.last_60s_ols_virtual_slope_mib_per_min)
        drain_to_teardown_final_physical_delta_mib = Measure-NumericDistribution @(
            $durationRecords.drain_to_teardown_final_physical_delta_mib)
        teardown_physical_range_mib = Measure-NumericDistribution @(
            $durationRecords.teardown_physical_range_mib)
        teardown_endpoint_physical_slope_mib_per_min = Measure-NumericDistribution @(
            $durationRecords.teardown_endpoint_physical_slope_mib_per_min)
        teardown_ols_physical_slope_mib_per_min = Measure-NumericDistribution @(
            $durationRecords.teardown_ols_physical_slope_mib_per_min)
        teardown_ols_virtual_slope_mib_per_min = Measure-NumericDistribution @(
            $durationRecords.teardown_ols_virtual_slope_mib_per_min)
    })
}

$seriesPassed = @($records | Where-Object { !$_.passed }).Count -eq 0
$manifest = [ordered]@{
    schema_version = 1
    kind = 'rmlui_clip_topology_churn_soak'
    started_utc = $startedUtc.ToString('o')
    finished_utc = [datetime]::UtcNow.ToString('o')
    project = $Project
    engine_root = $EngineRoot
    engine_association = (Get-Content -Raw -LiteralPath $Project | ConvertFrom-Json).EngineAssociation
    rhi = $RHI
    view_count = $ViewCount
    target_hz = $Hz
    durations_seconds = $DurationsSeconds
    teardown_seconds = $TeardownSeconds
    sample_count_per_duration = $SampleCount
    independent_processes = $true
    percentile_method = 'R-7 linear interpolation: index=(n-1)*p'
    passed = $seriesPassed
    runs = $records
    durations = $durationSummaries
    note = 'Process memory is sampled by the fixture every 0.5 seconds. Positive working-set deltas alone do not prove a leak; use repeatable tail slopes together with teardown weak-reference and memory behavior before allocation tracing.'
}
$manifestPath = Join-Path $OutputDirectory 'soak-series.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Topology soak report: $manifestPath"
if (!$seriesPassed) { throw "Topology churn soak failed. See $manifestPath" }
