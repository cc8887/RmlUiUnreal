<#
.SYNOPSIS
Captures an Unreal Insights GPU trace for the RmlUi Slate RHI direct-rendering test.
#>
[CmdletBinding()]
param(
    [string]$Project = '',
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [ValidateSet('DX11', 'DX12')][string]$RHI = 'DX12',
    [ValidateSet('Direct', 'Matrix', 'Churn')][string]$Scenario = 'Direct',
    [ValidateSet('Offscreen', 'Windowed')][string]$RenderMode = 'Offscreen',
    [ValidateRange(1, 32)][int]$ChurnViewCount = 8,
    [ValidateRange(0.25, 120.0)][double]$ChurnHz = 30.0,
    [ValidateRange(1.0, 300.0)][double]$ChurnSeconds = 5.0,
    [ValidateRange(0.0, 300.0)][double]$ChurnTeardownSeconds = 0.0,
    [switch]$AnalyzePresent,
    [switch]$AnalyzeDisplay,
    [string]$PresentMonPath = '',
    [ValidateRange(30, 900)][int]$TimeoutSeconds = 180,
    [ValidateRange(1, 50)][int]$SampleCount = 1,
    [ValidateRange(0, 10)][int]$WarmupRuns = 0,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$isRemoteDisplaySession = $false

function ConvertTo-NativeArgument([string]$Value) {
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
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
if ($AnalyzePresent -and $Scenario -notin @('Matrix', 'Churn')) {
    throw '-AnalyzePresent requires -Scenario Matrix or Churn because those tests emit the profile trace region.'
}
if ($AnalyzeDisplay -and ($Scenario -notin @('Matrix', 'Churn') -or $RenderMode -ne 'Windowed')) {
    throw '-AnalyzeDisplay requires -Scenario Matrix or Churn with -RenderMode Windowed.'
}
if ($AnalyzeDisplay) {
    if ($null -eq ('RmlUiGpuTrace.NativeSessionMetrics' -as [type])) {
        Add-Type -TypeDefinition @'
namespace RmlUiGpuTrace {
    using System.Runtime.InteropServices;
    public static class NativeSessionMetrics {
        [DllImport("user32.dll")]
        public static extern int GetSystemMetrics(int index);
    }
}
'@
    }
    $isRemoteDisplaySession = [RmlUiGpuTrace.NativeSessionMetrics]::GetSystemMetrics(0x1000) -ne 0
    if ($isRemoteDisplaySession) {
        throw '-AnalyzeDisplay cannot measure physical presentation from an RDP/indirect-display session. Run the capture from the machine console.'
    }
    if ([string]::IsNullOrWhiteSpace($PresentMonPath)) {
        $presentMonCommand = Get-Command presentmon.exe -ErrorAction SilentlyContinue
        if ($null -ne $presentMonCommand) { $PresentMonPath = $presentMonCommand.Source }
    }
    if ([string]::IsNullOrWhiteSpace($PresentMonPath) -or
        !(Test-Path -LiteralPath $PresentMonPath -PathType Leaf)) {
        throw '-AnalyzeDisplay requires PresentMon Console. Install Intel.PresentMon.Console or pass -PresentMonPath.'
    }
    $PresentMonPath = (Resolve-Path -LiteralPath $PresentMonPath).Path
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $runName = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'), $RHI
    $OutputDirectory = Join-Path $projectRoot "Saved\GpuTrace\$runName"
} elseif (![IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot $OutputDirectory
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

if ($SampleCount -gt 1 -or $WarmupRuns -gt 0) {
    $null = New-Item -ItemType Directory -Path $OutputDirectory -Force
    $seriesStartedUtc = [datetime]::UtcNow
    $runRecords = [Collections.Generic.List[object]]::new()
    $sampleManifests = [Collections.Generic.List[object]]::new()

    function Invoke-ProfileRun([string]$Kind, [int]$Index) {
        $runDirectory = Join-Path $OutputDirectory ('{0}-{1:D3}' -f $Kind, $Index)
        $runError = $null
        try {
            $childArguments = @{
                Project = $Project
                EngineRoot = $EngineRoot
                RHI = $RHI
                Scenario = $Scenario
                RenderMode = $RenderMode
                ChurnViewCount = $ChurnViewCount
                ChurnHz = $ChurnHz
                ChurnSeconds = $ChurnSeconds
                ChurnTeardownSeconds = $ChurnTeardownSeconds
                TimeoutSeconds = $TimeoutSeconds
                SampleCount = 1
                WarmupRuns = 0
                OutputDirectory = $runDirectory
            }
            if ($AnalyzePresent) { $childArguments.AnalyzePresent = $true }
            if ($AnalyzeDisplay) {
                $childArguments.AnalyzeDisplay = $true
                $childArguments.PresentMonPath = $PresentMonPath
            }
            & $PSCommandPath @childArguments
        } catch {
            $runError = $_.Exception.Message
        }
        $runManifestPath = Join-Path $runDirectory 'capture.json'
        $runManifest = if (Test-Path -LiteralPath $runManifestPath) {
            Get-Content -Raw -LiteralPath $runManifestPath | ConvertFrom-Json
        } else {
            $null
        }
        $runRecords.Add([ordered]@{
            kind = $Kind.ToLowerInvariant()
            index = $Index
            directory = $runDirectory
            manifest = $runManifestPath
            passed = $null -ne $runManifest -and $runManifest.passed -and $null -eq $runError
            error = $runError
        })
        if ($Kind -eq 'Sample' -and $null -ne $runManifest) {
            $sampleManifests.Add($runManifest)
        }
    }

    for ($index = 1; $index -le $WarmupRuns; ++$index) {
        Invoke-ProfileRun 'Warmup' $index
    }
    for ($index = 1; $index -le $SampleCount; ++$index) {
        Invoke-ProfileRun 'Sample' $index
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

    function Measure-Distribution([scriptblock]$Selector) {
        [double[]]$values = @($sampleManifests | ForEach-Object { [double](& $Selector $_) })
        if ($values.Count -eq 0) {
            return [ordered]@{ samples_ms = @(); min_ms = $null; max_ms = $null; mean_ms = $null;
                p50_ms = $null; p95_ms = $null; p99_ms = $null }
        }
        $measure = $values | Measure-Object -Minimum -Maximum -Average
        return [ordered]@{
            samples_ms = $values
            min_ms = $measure.Minimum
            max_ms = $measure.Maximum
            mean_ms = $measure.Average
            p50_ms = Get-Percentile $values 0.50
            p95_ms = Get-Percentile $values 0.95
            p99_ms = Get-Percentile $values 0.99
        }
    }

    function Measure-ValueArray([double[]]$Values) {
        if ($Values.Count -eq 0) {
            return [ordered]@{ samples_ms = @(); min_ms = $null; max_ms = $null; mean_ms = $null;
                p50_ms = $null; p95_ms = $null; p99_ms = $null }
        }
        $measure = $Values | Measure-Object -Minimum -Maximum -Average
        return [ordered]@{
            samples_ms = $Values
            min_ms = $measure.Minimum
            max_ms = $measure.Maximum
            mean_ms = $measure.Average
            p50_ms = Get-Percentile $Values 0.50
            p95_ms = Get-Percentile $Values 0.95
            p99_ms = Get-Percentile $Values 0.99
        }
    }

    function Measure-NumericDistribution([double[]]$Values) {
        if ($Values.Count -eq 0) {
            return [ordered]@{ samples = @(); min = $null; max = $null; mean = $null;
                p50 = $null; p95 = $null; p99 = $null }
        }
        $measure = $Values | Measure-Object -Minimum -Maximum -Average
        return [ordered]@{
            samples = $Values
            min = $measure.Minimum
            max = $measure.Maximum
            mean = $measure.Average
            p50 = Get-Percentile $Values 0.50
            p95 = Get-Percentile $Values 0.95
            p99 = Get-Percentile $Values 0.99
        }
    }

    $churnScenarios = [Collections.Generic.List[object]]::new()
    if ($Scenario -eq 'Churn') {
        foreach ($record in @($runRecords | Where-Object { $_.kind -eq 'sample' })) {
            $profilingDirectory = Join-Path $record.directory 'Profiling'
            $performanceReport = Get-ChildItem -LiteralPath $profilingDirectory `
                -Filter 'RmlUiClipTopologyChurn-*.json' -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if ($null -ne $performanceReport) {
                $performanceJson = Get-Content -Raw -LiteralPath $performanceReport.FullName |
                    ConvertFrom-Json
                if (@($performanceJson.scenarios).Count -eq 1) {
                    $scenarioResult = $performanceJson.scenarios[0]
                    $scenarioResult | Add-Member -NotePropertyName source_report `
                        -NotePropertyValue $performanceReport.FullName
                    $churnScenarios.Add($scenarioResult)
                }
            }
        }
    }

    $failedRuns = @($runRecords | Where-Object { !$_.passed })
    $eventShapes = @($sampleManifests | ForEach-Object {
        '{0}/{1}/{2}/{3}' -f $_.profile_gpu.passes.events, $_.profile_gpu.groups.events,
            $_.profile_gpu.masks.events, $_.profile_gpu.content.events
    })
    $eventCountsConsistent = @($eventShapes | Select-Object -Unique).Count -eq 1
    $churnReportsComplete = $Scenario -ne 'Churn' -or $churnScenarios.Count -eq $SampleCount
    $seriesPassed = $failedRuns.Count -eq 0 -and $sampleManifests.Count -eq $SampleCount -and
        $eventCountsConsistent -and $churnReportsComplete
    $seriesManifestPath = Join-Path $OutputDirectory 'capture-series.json'
    $seriesManifest = [ordered]@{
        schema_version = 2
        kind = 'profile_gpu_series'
        started_utc = $seriesStartedUtc.ToString('o')
        finished_utc = [datetime]::UtcNow.ToString('o')
        project = $Project
        engine_root = [IO.Path]::GetFullPath($EngineRoot)
        rhi = $RHI
        scenario = $Scenario
        render_mode = $RenderMode
        analyze_present = [bool]$AnalyzePresent
        analyze_display = [bool]$AnalyzeDisplay
        sample_count = $SampleCount
        warmup_runs = $WarmupRuns
        independent_processes = $true
        percentile_method = 'R-7 linear interpolation: index=(n-1)*p'
        passed = $seriesPassed
        runs = $runRecords
        gpu_event_shapes = $eventShapes
        gpu_event_counts_consistent = $eventCountsConsistent
        churn_reports_complete = $churnReportsComplete
        profile_gpu_distribution = [ordered]@{
            frame = Measure-Distribution { param($m) $m.profile_gpu.frame_ms }
            pass_inclusive = Measure-Distribution { param($m) $m.profile_gpu.passes.inclusive_ms_sum }
            group_inclusive = Measure-Distribution { param($m) $m.profile_gpu.groups.inclusive_ms_sum }
            mask_inclusive = Measure-Distribution { param($m) $m.profile_gpu.masks.inclusive_ms_sum }
            content_inclusive = Measure-Distribution { param($m) $m.profile_gpu.content.inclusive_ms_sum }
            pass_prologue_upper_bound = Measure-Distribution {
                param($m) $m.profile_gpu.pass_prologue_upper_bound_ms
            }
        }
        note = 'Each sample launches a fresh UnrealEditor process. The fixture performs its own load, motion, settle, and stable-cache validation before ProfileGPU. Warmup runs are preserved but excluded from distributions. ProfileGPU text is rounded to 0.001 ms.'
    }
    if ($Scenario -eq 'Churn') {
        [double[]]$achievedHz = @($churnScenarios | ForEach-Object { [double]$_.achieved_hz })
        [double[]]$changes = @($churnScenarios | ForEach-Object { [double]$_.observed_topology_changes })
        [double[]]$churnDelta = @($churnScenarios | ForEach-Object {
            [double]($_.process_memory.samples[-1].used_physical_mib -
                $_.process_memory.samples[0].used_physical_mib)
        })
        [double[]]$churnTailSlope = @($churnScenarios | ForEach-Object {
            if ($_.process_memory.PSObject.Properties.Name -contains
                'tail_endpoint_physical_slope_mib_per_min') {
                [double]$_.process_memory.tail_endpoint_physical_slope_mib_per_min
            }
        })
        $seriesManifest.churn_distribution = [ordered]@{
            reports = @($churnScenarios | ForEach-Object { $_.source_report })
            achieved_hz = Measure-NumericDistribution $achievedHz
            topology_changes = Measure-NumericDistribution $changes
            physical_delta_mib = Measure-NumericDistribution $churnDelta
            tail_endpoint_physical_slope_mib_per_min =
                Measure-NumericDistribution $churnTailSlope
        }
        if ($ChurnTeardownSeconds -gt 0.0) {
            [double[]]$drainToFinal = @($churnScenarios | ForEach-Object {
                [double]($_.process_memory.used_physical_after_teardown_mib -
                    $_.process_memory.used_physical_after_drain_mib)
            })
            [double[]]$teardownRange = @($churnScenarios | ForEach-Object {
                [double]$_.process_memory.used_physical_teardown_range_mib
            })
            [double[]]$teardownSlope = @($churnScenarios | ForEach-Object {
                [double]$_.process_memory.teardown_endpoint_physical_slope_mib_per_min
            })
            $allViewsReleased = @($churnScenarios | Where-Object {
                $_.process_memory.live_widgets_after_teardown -ne 0 -or
                $_.process_memory.window_alive_after_teardown
            }).Count -eq 0
            $seriesManifest.teardown_distribution = [ordered]@{
                all_views_released = $allViewsReleased
                all_widgets_released_at_seconds = Measure-NumericDistribution @(
                    $churnScenarios | ForEach-Object {
                        [double]$_.process_memory.all_widgets_released_at_seconds
                    })
                window_released_at_seconds = Measure-NumericDistribution @(
                    $churnScenarios | ForEach-Object {
                        [double]$_.process_memory.window_released_at_seconds
                    })
                drain_to_final_physical_delta_mib = Measure-NumericDistribution $drainToFinal
                physical_range_mib = Measure-NumericDistribution $teardownRange
                endpoint_physical_slope_mib_per_min = Measure-NumericDistribution $teardownSlope
            }
            $seriesPassed = $seriesPassed -and $allViewsReleased
            $seriesManifest.passed = $seriesPassed
        }
    }
    if ($AnalyzePresent) {
        [double[]]$presentSamples = @($sampleManifests | ForEach-Object {
            if ($null -ne $_.present_cpu.primary) {
                @($_.present_cpu.primary.samples_ms) | ForEach-Object { [double]$_ }
            }
        })
        $seriesManifest.present_cpu_distribution = Measure-ValueArray $presentSamples
        $seriesManifest.present_cpu_scope = if ($RHI -eq 'DX12') { 'D3D12_Present' } else { 'D3D11_Present' }
        $seriesManifest.present_cpu_note = 'CPU duration inside the engine present scope during RmlUiGpuProfileWindow; this is not display scanout or input-to-photon latency.'
    }
    if ($AnalyzeDisplay) {
        $metricNames = @('frame_time', 'cpu_busy', 'cpu_wait', 'gpu_latency', 'gpu_time',
            'gpu_busy', 'gpu_wait', 'display_latency', 'displayed_time', 'animation_error',
            'animation_time', 'flip_delay', 'all_input_to_photon', 'click_to_photon')
        $displayMetrics = [ordered]@{}
        foreach ($metricName in $metricNames) {
            [double[]]$metricSamples = @($sampleManifests | ForEach-Object {
                $metric = $_.presentation.metrics.$metricName
                if ($null -ne $metric) {
                    @($metric.samples_ms) | ForEach-Object { [double]$_ }
                }
            })
            $displayMetrics[$metricName] = Measure-ValueArray $metricSamples
        }
        $seriesManifest.presentation_distribution = [ordered]@{
            provider = 'PresentMon'
            qpc_aligned_to = 'RmlUiGpuProfileWindow'
            frames = @($sampleManifests | ForEach-Object { [int]$_.presentation.frames } |
                Measure-Object -Sum).Sum
            displayed_frames = @($sampleManifests | ForEach-Object { [int]$_.presentation.displayed_frames } |
                Measure-Object -Sum).Sum
            dropped_frames = @($sampleManifests | ForEach-Object { [int]$_.presentation.dropped_frames } |
                Measure-Object -Sum).Sum
            metrics = $displayMetrics
            note = 'PresentMon display metrics filtered by the fixture QPC window. DisplayLatency is frame CPU start to screen display; input latency remains unavailable when no input event contributes to a frame.'
        }
    }
    $seriesManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $seriesManifestPath -Encoding UTF8
    Write-Host "Capture series: $seriesManifestPath"
    if (!$seriesPassed) {
        throw "GPU trace capture series failed. See $seriesManifestPath"
    }
    return
}

$automationRoot = Join-Path $OutputDirectory 'Automation'
$tracePath = Join-Path $OutputDirectory 'RmlUiSlateRhi.utrace'
$logPath = Join-Path $OutputDirectory 'UnrealEditor.log'
$manifestPath = Join-Path $OutputDirectory 'capture.json'
$presentWindowMarkerPath = Join-Path $OutputDirectory 'PresentWindow.json'
$presentMonCsvPath = Join-Path $OutputDirectory 'PresentMon.csv'
$presentMonStdoutPath = Join-Path $OutputDirectory 'PresentMon.stdout.log'
$presentMonStderrPath = Join-Path $OutputDirectory 'PresentMon.stderr.log'
$null = New-Item -ItemType Directory -Path $automationRoot -Force
$testName = switch ($Scenario) {
    'Matrix' { 'RmlUiUnreal.Performance.PaintCacheRouteMatrix' }
    'Churn' { 'RmlUiUnreal.Performance.ClipTopologyChurn' }
    default { 'RmlUiUnreal.Slate.RhiDirectRendering' }
}

$editorArguments = @(
    $Project, "-$RHI", '-windowed', '-ResX=800', '-ResY=480',
    '-unattended', '-NoSound', '-NoLiveCoding', '-nosplash'
)
if ($RenderMode -eq 'Offscreen') {
    $editorArguments += '-RenderOffscreen'
}
$editorArguments += @(
    '-trace=default,gpu', "-tracefile=$tracePath", '-statnamedevents',
    '-RmlUiCaptureGpuProfile',
    "-RmlUiPerfOutput=$(Join-Path $OutputDirectory 'Profiling')",
    "-ExecCmds=r.ProfileGPU.ShowUI 0,r.ProfileGPU.ThresholdPercent 0,r.ProfileGPU.ShowLeafEvents 1,Automation RunTests $testName",
    '-TestExit=Automation Test Queue Empty',
    "-ReportExportPath=$automationRoot", "-abslog=$logPath"
)
if ($Scenario -eq 'Churn') {
    $editorArguments += @(
        "-RmlUiTopologyChurnViewCount=$ChurnViewCount",
        ('-RmlUiTopologyChurnHz={0}' -f $ChurnHz.ToString([Globalization.CultureInfo]::InvariantCulture)),
        ('-RmlUiTopologyChurnSeconds={0}' -f $ChurnSeconds.ToString([Globalization.CultureInfo]::InvariantCulture)),
        ('-RmlUiTopologyChurnTeardownSeconds={0}' -f $ChurnTeardownSeconds.ToString([Globalization.CultureInfo]::InvariantCulture))
    )
}
if ($AnalyzeDisplay) {
    $editorArguments += "-RmlUiPresentWindowMarker=$presentWindowMarkerPath"
    Remove-Item -LiteralPath $presentWindowMarkerPath, $presentMonCsvPath,
        $presentMonStdoutPath, $presentMonStderrPath -ErrorAction SilentlyContinue
}
$commandLine = ($editorArguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
$startedUtc = [datetime]::UtcNow
$editorWindowStyle = if ($AnalyzeDisplay) { 'Normal' } else { 'Hidden' }
$process = Start-Process -FilePath $editorPath -ArgumentList $commandLine `
    -WorkingDirectory $projectRoot -WindowStyle $editorWindowStyle -PassThru

$presentMonProcess = $null
$presentMonSessionName = $null
if ($AnalyzeDisplay) {
    $presentMonSessionName = "RmlUiPresentMon-$($process.Id)"
    $presentMonArguments = @(
        '--process_id', [string]$process.Id,
        '--output_file', $presentMonCsvPath,
        '--qpc_time', '--v2_metrics', '--no_console_stats',
        '--terminate_on_proc_exit', '--stop_existing_session',
        '--session_name', $presentMonSessionName
    )
    $presentMonCommandLine = ($presentMonArguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $presentMonProcess = Start-Process -FilePath $PresentMonPath -ArgumentList $presentMonCommandLine `
        -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $presentMonStdoutPath -RedirectStandardError $presentMonStderrPath
}

$timedOut = !$process.WaitForExit($TimeoutSeconds * 1000)
if ($timedOut) {
    try { $process.Kill($true) } catch { Write-Warning $_.Exception.Message }
    $process.WaitForExit()
}
$process.Refresh()
if ($null -ne $presentMonProcess) {
    if (!$presentMonProcess.HasExited) {
        $stopArguments = @('--session_name', $presentMonSessionName, '--terminate_existing_session')
        $stopCommandLine = ($stopArguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
        $stopProcess = Start-Process -FilePath $PresentMonPath -ArgumentList $stopCommandLine `
            -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        $null = $stopProcess.WaitForExit(10000)
    }
    $presentMonTimedOut = !$presentMonProcess.WaitForExit(10000)
    if ($presentMonTimedOut) {
        try { $presentMonProcess.Kill($true) } catch { Write-Warning $_.Exception.Message }
        $presentMonProcess.WaitForExit()
    }
    $presentMonProcess.Refresh()
}

$reportPath = Join-Path $automationRoot 'index.json'
$report = if (Test-Path -LiteralPath $reportPath) {
    Get-Content -Raw -LiteralPath $reportPath | ConvertFrom-Json
} else {
    $null
}
$performanceReportPath = if ($Scenario -eq 'Churn') {
    Get-ChildItem -LiteralPath (Join-Path $OutputDirectory 'Profiling') `
        -Filter 'RmlUiClipTopologyChurn-*.json' -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
} else { $null }
$traceLength = if (Test-Path -LiteralPath $tracePath) {
    (Get-Item -LiteralPath $tracePath).Length
} else {
    0
}
$logText = if (Test-Path -LiteralPath $logPath) {
    Get-Content -Raw -LiteralPath $logPath
} else {
    ''
}
$requiredGpuScopes = @('RmlUiSlateRhiGroup', 'RmlUiSlateRhiMask', 'RmlUiSlateRhiContent')
$profileScopesFound = @($requiredGpuScopes | Where-Object { $logText.Contains($_) })
$profileLines = @($logText -split "`r?`n" | Where-Object { $_ -match 'LogRHI: Display:' })
$frameTimes = @($profileLines | ForEach-Object {
    if ($_ -match 'Frame Time\s*:\s*([0-9.]+)ms') {
        [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
    }
})
$frameMilliseconds = if ($frameTimes.Count -gt 0) {
    ($frameTimes | Measure-Object -Maximum).Maximum
} else {
    $null
}
function Measure-GpuRows([string]$NamePattern) {
    $rows = @($profileLines | Where-Object { $_ -match $NamePattern })
    $exclusive = 0.0
    $inclusive = 0.0
    $parsed = 0
    foreach ($line in $rows) {
        $times = [regex]::Matches($line, '([0-9]+(?:\.[0-9]+)?)\s*ms')
        if ($times.Count -lt 2) { continue }
        $exclusive += [double]::Parse($times[0].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
        $inclusive += [double]::Parse($times[1].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
        $parsed++
    }
    return [ordered]@{
        events = $parsed
        exclusive_ms_sum = $exclusive
        inclusive_ms_sum = $inclusive
    }
}
$passProfile = Measure-GpuRows 'RmlUiSlateRhi Geometry='
$groupProfile = Measure-GpuRows 'RmlUiSlateRhiGroup\s'
$maskProfile = Measure-GpuRows 'RmlUiSlateRhiMask\s'
$contentProfile = Measure-GpuRows 'RmlUiSlateRhiContent\s'
$passPrologueUpperBound = [Math]::Max(0.0,
    [double]$passProfile.inclusive_ms_sum - [double]$groupProfile.inclusive_ms_sum)
$presentAnalysisPassed = $true
$presentCpu = [ordered]@{
    requested = [bool]$AnalyzePresent
    render_mode = $RenderMode
    available = $false
    primary_scope = if ($RHI -eq 'DX12') { 'D3D12_Present' } else { 'D3D11_Present' }
    primary = $null
    timers = [ordered]@{}
    events_file = $null
    insights_log = $null
    error = $null
    note = 'CPU present-scope duration only; not display scanout or input-to-photon latency.'
}
if ($AnalyzePresent) {
    $insightsPath = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealInsights.exe'
    $presentEventsPath = Join-Path $OutputDirectory 'PresentEvents.csv'
    $insightsLogPath = Join-Path $OutputDirectory 'UnrealInsights-Present.log'
    $presentCpu.events_file = $presentEventsPath
    $presentCpu.insights_log = $insightsLogPath
    $presentTimerNames = if ($RHI -eq 'DX12') {
        @('D3D12_Present', 'PresentEvent_Wait Frame', 'FD3D12Viewport::Present',
            'SchedulePresent', 'Present_SubmissionThread')
    } else {
        @('D3D11_Present', 'FD3D11Viewport::Present')
    }
    $existingInsights = @(Get-Process UnrealInsights -ErrorAction SilentlyContinue)
    if (!(Test-Path -LiteralPath $insightsPath -PathType Leaf)) {
        $presentCpu.error = "UnrealInsights.exe not found: $insightsPath"
        $presentAnalysisPassed = $false
    } elseif ($existingInsights.Count -gt 0) {
        $presentCpu.error = 'UnrealInsights is already running; close it so the no-UI export owns its process.'
        $presentAnalysisPassed = $false
    } else {
        $exportCommand = 'TimingInsights.ExportTimingEvents {0} -columns=ThreadName,TimerName,StartTime,EndTime,Duration -timers={1} -region=RmlUiGpuProfileWindow' -f
            $presentEventsPath, ($presentTimerNames -join ',')
        $insightsCommandLine = '-OpenTraceFile="{0}" -ABSLOG="{1}" -AutoQuit -NoUI -ExecOnAnalysisCompleteCmd="{2}" -log' -f
            $tracePath, $insightsLogPath, $exportCommand.Replace('"', '\"')
        $insightsProcess = Start-Process -FilePath $insightsPath -ArgumentList $insightsCommandLine `
            -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        $insightsTimedOut = !$insightsProcess.WaitForExit(60000)
        if ($insightsTimedOut) {
            try { $insightsProcess.Kill($true) } catch { Write-Warning $_.Exception.Message }
            $insightsProcess.WaitForExit()
        }
        $insightsProcess.Refresh()
        if ($insightsTimedOut -or $insightsProcess.ExitCode -ne 0 -or
            !(Test-Path -LiteralPath $presentEventsPath -PathType Leaf)) {
            $presentCpu.error = "Unreal Insights present export failed or timed out (exit=$($insightsProcess.ExitCode))."
            $presentAnalysisPassed = $false
        } else {
            $exportedRows = @(Import-Csv -LiteralPath $presentEventsPath)
            $presentRows = @($exportedRows | Where-Object { $presentTimerNames -contains $_.TimerName })
            $timerStats = [ordered]@{}
            foreach ($timerName in $presentTimerNames) {
                [double[]]$durations = @($presentRows | Where-Object TimerName -eq $timerName | ForEach-Object {
                    1000.0 * [double]::Parse($_.Duration, [Globalization.CultureInfo]::InvariantCulture)
                })
                if ($durations.Count -eq 0) { continue }
                $sorted = @($durations | Sort-Object)
                $measure = $durations | Measure-Object -Minimum -Maximum -Average
                $percentile = {
                    param([double]$Probability)
                    $position = ($sorted.Count - 1) * $Probability
                    $lower = [Math]::Floor($position)
                    $upper = [Math]::Ceiling($position)
                    if ($lower -eq $upper) { return [double]$sorted[$lower] }
                    $weight = $position - $lower
                    return [double]$sorted[$lower] * (1.0 - $weight) + [double]$sorted[$upper] * $weight
                }
                $timerStats[$timerName] = [ordered]@{
                    events = $durations.Count
                    samples_ms = $durations
                    min_ms = $measure.Minimum
                    max_ms = $measure.Maximum
                    mean_ms = $measure.Average
                    p50_ms = & $percentile 0.50
                    p95_ms = & $percentile 0.95
                    p99_ms = & $percentile 0.99
                }
            }
            $presentCpu.timers = $timerStats
            $presentCpu.available = $timerStats.Count -gt 0
            $presentCpu.primary = $timerStats[$presentCpu.primary_scope]
            if ($RenderMode -eq 'Windowed' -and $null -eq $presentCpu.primary) {
                $presentCpu.error = "Windowed trace did not contain required scope $($presentCpu.primary_scope)."
                $presentAnalysisPassed = $false
            }
        }
    }
}
$presentationAnalysisPassed = $true
$presentation = [ordered]@{
    requested = [bool]$AnalyzeDisplay
    provider = 'PresentMon'
    provider_path = if ($AnalyzeDisplay) { $PresentMonPath } else { $null }
    provider_exit_code = $null
    provider_timed_out = $false
    remote_display_session = $isRemoteDisplaySession
    available = $false
    qpc_window_file = if ($AnalyzeDisplay) { $presentWindowMarkerPath } else { $null }
    csv_file = if ($AnalyzeDisplay) { $presentMonCsvPath } else { $null }
    process_id = $process.Id
    frames = 0
    displayed_frames = 0
    dropped_frames = 0
    swap_chains = 0
    present_modes = [ordered]@{}
    sync_intervals = [ordered]@{}
    metrics = [ordered]@{}
    error = $null
    note = 'PresentMon metrics are filtered by the same QueryPerformanceCounter window as RmlUiGpuProfileWindow. DisplayLatency covers frame CPU start to screen display; input latency requires a contributing input event.'
}
if ($AnalyzeDisplay) {
    $presentMonExitCode = if ($null -eq $presentMonProcess) { $null } else { $presentMonProcess.ExitCode }
    $presentation.provider_exit_code = $presentMonExitCode
    $presentation.provider_timed_out = $presentMonTimedOut
    if ($null -eq $presentMonProcess) {
        $presentation.error = "PresentMon failed or timed out (exit=$presentMonExitCode)."
        $presentationAnalysisPassed = $false
    } elseif (!(Test-Path -LiteralPath $presentWindowMarkerPath -PathType Leaf)) {
        $presentation.error = "PresentMon QPC window marker was not written: $presentWindowMarkerPath"
        $presentationAnalysisPassed = $false
    } elseif (!(Test-Path -LiteralPath $presentMonCsvPath -PathType Leaf)) {
        $presentation.error = "PresentMon did not produce CSV output: $presentMonCsvPath"
        $presentationAnalysisPassed = $false
    } else {
        try {
            $windowMarker = Get-Content -Raw -LiteralPath $presentWindowMarkerPath | ConvertFrom-Json
            if (!$windowMarker.complete) { throw 'QPC window marker is incomplete.' }
            $startQpc = [uint64]::Parse($windowMarker.start_qpc, [Globalization.CultureInfo]::InvariantCulture)
            $endQpc = [uint64]::Parse($windowMarker.end_qpc, [Globalization.CultureInfo]::InvariantCulture)
            if ($endQpc -le $startQpc) { throw 'QPC window marker has an invalid range.' }
            $csvRows = @(Import-Csv -LiteralPath $presentMonCsvPath)
            $windowRows = @($csvRows | Where-Object {
                if ($_.ProcessID -ne [string]$process.Id -or [string]::IsNullOrWhiteSpace($_.CPUStartQPC)) {
                    return $false
                }
                $rowQpc = [uint64]::Parse($_.CPUStartQPC, [Globalization.CultureInfo]::InvariantCulture)
                return $rowQpc -ge $startQpc -and $rowQpc -le $endQpc
            })
            if ($windowRows.Count -eq 0) { throw 'PresentMon CSV has no frames inside the QPC window.' }
            $displayedRows = @($windowRows | Where-Object {
                $_.DisplayLatency -ne 'NA' -and ![string]::IsNullOrWhiteSpace($_.DisplayLatency) -and
                $_.DisplayedTime -ne 'NA' -and ![string]::IsNullOrWhiteSpace($_.DisplayedTime)
            })
            if ($displayedRows.Count -eq 0) { throw 'PresentMon did not observe a displayed frame inside the QPC window.' }

            function Measure-PresentMonMetric([string]$Column) {
                [double[]]$values = @($windowRows | ForEach-Object {
                    $textValue = [string]$_.$Column
                    $parsedValue = 0.0
                    if ($textValue -ne 'NA' -and
                        [double]::TryParse($textValue, [Globalization.NumberStyles]::Float,
                            [Globalization.CultureInfo]::InvariantCulture, [ref]$parsedValue)) {
                        $parsedValue
                    }
                })
                if ($values.Count -eq 0) {
                    return [ordered]@{ samples_ms = @(); min_ms = $null; max_ms = $null;
                        mean_ms = $null; p50_ms = $null; p95_ms = $null; p99_ms = $null }
                }
                $sorted = @($values | Sort-Object)
                $getPercentile = {
                    param([double]$Probability)
                    $position = ($sorted.Count - 1) * $Probability
                    $lower = [Math]::Floor($position)
                    $upper = [Math]::Ceiling($position)
                    if ($lower -eq $upper) { return [double]$sorted[$lower] }
                    $weight = $position - $lower
                    return [double]$sorted[$lower] * (1.0 - $weight) + [double]$sorted[$upper] * $weight
                }
                $measure = $values | Measure-Object -Minimum -Maximum -Average
                return [ordered]@{
                    samples_ms = $values
                    min_ms = $measure.Minimum
                    max_ms = $measure.Maximum
                    mean_ms = $measure.Average
                    p50_ms = & $getPercentile 0.50
                    p95_ms = & $getPercentile 0.95
                    p99_ms = & $getPercentile 0.99
                }
            }

            $metricColumns = [ordered]@{
                frame_time = 'FrameTime'; cpu_busy = 'CPUBusy'; cpu_wait = 'CPUWait'
                gpu_latency = 'GPULatency'; gpu_time = 'GPUTime'; gpu_busy = 'GPUBusy'; gpu_wait = 'GPUWait'
                display_latency = 'DisplayLatency'; displayed_time = 'DisplayedTime'
                animation_error = 'AnimationError'; animation_time = 'AnimationTime'; flip_delay = 'MsFlipDelay'
                all_input_to_photon = 'AllInputToPhotonLatency'; click_to_photon = 'ClickToPhotonLatency'
            }
            foreach ($entry in $metricColumns.GetEnumerator()) {
                $presentation.metrics[$entry.Key] = Measure-PresentMonMetric $entry.Value
            }
            foreach ($group in @($windowRows | Group-Object PresentMode)) {
                $presentation.present_modes[$group.Name] = $group.Count
            }
            foreach ($group in @($windowRows | Group-Object SyncInterval)) {
                $presentation.sync_intervals[$group.Name] = $group.Count
            }
            $presentation.frames = $windowRows.Count
            $presentation.displayed_frames = $displayedRows.Count
            $presentation.dropped_frames = $windowRows.Count - $displayedRows.Count
            $presentation.swap_chains = @($windowRows.SwapChainAddress | Select-Object -Unique).Count
            $presentation.available = $true
        } catch {
            $presentation.error = $_.Exception.Message
            $presentationAnalysisPassed = $false
        }
    }
}
$passed = !$timedOut -and $process.ExitCode -eq 0 -and $null -ne $report -and
    $report.failed -eq 0 -and ($report.succeeded + $report.succeededWithWarnings) -gt 0 -and
    $traceLength -gt 0 -and $profileScopesFound.Count -eq $requiredGpuScopes.Count -and
    $groupProfile.events -gt 0 -and $maskProfile.events -gt 0 -and $contentProfile.events -gt 0 -and
    $presentAnalysisPassed -and $presentationAnalysisPassed

$manifest = [ordered]@{
    schema_version = 2
    started_utc = $startedUtc.ToString('o')
    finished_utc = [datetime]::UtcNow.ToString('o')
    project = $Project
    engine_root = [IO.Path]::GetFullPath($EngineRoot)
    editor = $editorPath
    rhi = $RHI
    scenario = $Scenario
    render_mode = $RenderMode
    churn = if ($Scenario -eq 'Churn') { [ordered]@{
        view_count = $ChurnViewCount
        target_hz = $ChurnHz
        sample_seconds = $ChurnSeconds
        teardown_seconds = $ChurnTeardownSeconds
    } } else { $null }
    test = $testName
    arguments = $editorArguments
    timeout_seconds = $TimeoutSeconds
    timed_out = $timedOut
    exit_code = $process.ExitCode
    trace = $tracePath
    trace_bytes = $traceLength
    automation_report = $reportPath
    performance_report = $performanceReportPath
    automation_succeeded = if ($null -eq $report) { 0 } else { $report.succeeded }
    automation_succeeded_with_warnings = if ($null -eq $report) { 0 } else { $report.succeededWithWarnings }
    automation_failed = if ($null -eq $report) { $null } else { $report.failed }
    passed = $passed
    gpu_scopes = @('RmlUiSlateRhi', 'RmlUiSlateRhiGroup', 'RmlUiSlateRhiMask', 'RmlUiSlateRhiContent')
    profile_gpu_scopes_found = $profileScopesFound
    profile_gpu = [ordered]@{
        frame_ms = $frameMilliseconds
        passes = $passProfile
        groups = $groupProfile
        masks = $maskProfile
        content = $contentProfile
        pass_prologue_upper_bound_ms = $passPrologueUpperBound
    }
    present_cpu = $presentCpu
    presentation = $presentation
    note = 'ProfileGPU text is rounded to 0.001 ms. Pass minus group includes the RDG load-action clear, pass prologue, breadcrumb boundaries, and rounding; it is not a direct stencil-clear measurement.'
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Capture output: $OutputDirectory"
Write-Host "Trace: $tracePath ($traceLength bytes)"
Write-Host "Automation report: $reportPath"
if (!$passed) {
    throw "GPU trace capture failed. See $manifestPath and $logPath"
}
