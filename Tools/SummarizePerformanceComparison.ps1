<#
.SYNOPSIS
Summarizes saved rendering/communication reports and interval process samples; never starts UE.
.EXAMPLE
.\SummarizePerformanceComparison.ps1 -RunDirectory 'F:\Project\Saved\Performance\run'
.EXAMPLE
.\SummarizePerformanceComparison.ps1 -RunDirectory @('F:\runs\DX12','F:\runs\DX11') -OutputDirectory 'F:\runs\analysis'
.DESCRIPTION
Writes analysis.json and analysis.md. With one run, the default output is that run;
with multiple runs, the default is a CombinedAnalysis directory in the first run.
CPU uses cumulative-counter differences strictly within scenario UTC boundaries.
Missing timestamps or insufficient successful samples produce null, never zero.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][string[]]$RunDirectory,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Get-Field($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    if ($Object -is [Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Get-Number($Value) {
    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return $null }
    $number = 0.0
    if ([double]::TryParse([string]$Value, [Globalization.NumberStyles]::Float, $invariant, [ref]$number) -and
        ![double]::IsNaN($number) -and ![double]::IsInfinity($number)) { return $number }
    return $null
}

function Get-Time($Value) {
    # PowerShell 7.5+ ConvertFrom-Json materializes ISO strings as DateTime. Do not
    # cast that value to culture text, which discards UTC kind and subsecond data.
    if ($Value -is [datetimeoffset]) { return $Value.ToUniversalTime() }
    if ($Value -is [datetime]) { return ([datetimeoffset]$Value).ToUniversalTime() }
    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return $null }
    try { return [datetimeoffset]::Parse([string]$Value, $invariant).ToUniversalTime() }
    catch { return $null }
}

function Read-Json([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Get-Percentile($Values, [double]$Fraction) {
    $sorted = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    # Match RmlUiCommunicationBenchmark.cpp's nearest-rank convention.
    $index = [math]::Max(0, [math]::Min($sorted.Count - 1, [math]::Ceiling($sorted.Count * $Fraction) - 1))
    return [double]$sorted[$index]
}

function Get-Mean($Values) {
    $valid = @($Values | Where-Object { $null -ne $_ })
    if ($valid.Count -eq 0) { return $null }
    return [double](($valid | Measure-Object -Average).Average)
}

function Format-Number($Value, [string]$Format = 'F3') {
    if ($null -eq $Value) { return 'null' }
    return ([double]$Value).ToString($Format, $invariant)
}

function Measure-ProcessInterval($Rows, $Start, $Finish, [string]$GroupName) {
    $result = [ordered]@{
        group = $GroupName; status = 'missing_scenario_timestamps'
        selected_row_count = $null; observed_instance_count = $null; failed_sample_count = $null
        first_observation_utc = $null; last_observation_utc = $null
        cpu_delta_seconds = $null; cpu_observed_wall_seconds = $null
        cpu_single_core_percent = $null; cpu_coverage_fraction = $null
        cpu_complete_for_observed_instances = $null; cpu_instances = @()
        memory_sample_count = $null; memory_rejected_batch_count = $null
        working_set_sum_mean_mib = $null; working_set_sum_peak_mib = $null
        private_sum_mean_mib = $null; private_sum_peak_mib = $null
        memory_max_batch_skew_ms = $null; memory_samples = @()
    }
    if ($null -eq $Start -or $null -eq $Finish -or $Finish -le $Start) { return $result }
    $selected = @($Rows | Where-Object { $null -ne $_.Time -and $_.Time -ge $Start -and $_.Time -le $Finish -and $_.Event -ne 'Exited' })
    $result.selected_row_count = $selected.Count
    $result.failed_sample_count = @($selected | Where-Object { $_.Event -eq 'SampleFailed' }).Count
    $instances = @($selected | Group-Object Key)
    $result.observed_instance_count = $instances.Count
    if ($selected.Count -eq 0) { $result.status = 'no_process_observations_in_interval'; return $result }
    $orderedRows = @($selected | Sort-Object Time)
    $result.first_observation_utc = $orderedRows[0].Time.ToString('o')
    $result.last_observation_utc = $orderedRows[-1].Time.ToString('o')
    $cpuIntervals = [Collections.Generic.List[object]]::new()
    $allInstancesMeasured = $true
    foreach ($instance in $instances) {
        $valid = @($instance.Group | Where-Object { $null -ne $_.Cpu } | Sort-Object Time)
        if ($valid.Count -lt 2 -or $valid[-1].Time -le $valid[0].Time -or $valid[-1].Cpu -lt $valid[0].Cpu) {
            $allInstancesMeasured = $false
            continue
        }
        $cpuIntervals.Add([pscustomobject]@{
            instance_key = $instance.Name; process_name = $valid[0].Name
            sample_count = $valid.Count; first_utc = $valid[0].Time.ToString('o'); last_utc = $valid[-1].Time.ToString('o')
            delta_seconds = $valid[-1].Cpu - $valid[0].Cpu
            wall_seconds = ($valid[-1].Time - $valid[0].Time).TotalSeconds
        })
    }
    $result.cpu_complete_for_observed_instances = $allInstancesMeasured
    $result.cpu_instances = @($cpuIntervals.ToArray())
    if ($cpuIntervals.Count -gt 0) {
        $first = @($cpuIntervals | ForEach-Object { Get-Time $_.first_utc } | Sort-Object)[0]
        $last = @($cpuIntervals | ForEach-Object { Get-Time $_.last_utc } | Sort-Object)[-1]
        $wall = ($last - $first).TotalSeconds
        $result.cpu_observed_wall_seconds = $wall
        $result.cpu_coverage_fraction = $wall / ($Finish - $Start).TotalSeconds
        if ($allInstancesMeasured -and $wall -gt 0) {
            $result.cpu_delta_seconds = [double](($cpuIntervals | Measure-Object delta_seconds -Sum).Sum)
            $result.cpu_single_core_percent = 100.0 * $result.cpu_delta_seconds / $wall
        }
    }

    # A batch is accepted only if all observed rows for this group are inside the
    # scenario and have memory values. Sum contemporaneous rows, never process peaks.
    $memory = [Collections.Generic.List[object]]::new()
    $rejected = 0
    foreach ($batch in @($Rows | Where-Object { $_.Event -ne 'Exited' -and $null -ne $_.Time } | Group-Object Index)) {
        $ordered = @($batch.Group | Sort-Object Time)
        if ($ordered[-1].Time -lt $Start -or $ordered[0].Time -gt $Finish) { continue }
        if ($ordered[0].Time -lt $Start -or $ordered[-1].Time -gt $Finish -or
            @($ordered | Where-Object { $null -eq $_.WorkingSet -or $null -eq $_.Private }).Count -gt 0) {
            $rejected++; continue
        }
        $memory.Add([pscustomobject]@{
            sample_index = $batch.Name; first_utc = $ordered[0].Time.ToString('o'); last_utc = $ordered[-1].Time.ToString('o')
            observed_processes = $ordered.Count
            working_set_sum_mib = [double](($ordered | Measure-Object WorkingSet -Sum).Sum) / 1MB
            private_sum_mib = [double](($ordered | Measure-Object Private -Sum).Sum) / 1MB
            batch_skew_ms = ($ordered[-1].Time - $ordered[0].Time).TotalMilliseconds
        })
    }
    $result.memory_sample_count = $memory.Count
    $result.memory_rejected_batch_count = $rejected
    $result.memory_samples = @($memory.ToArray())
    if ($memory.Count -gt 0) {
        $result.working_set_sum_mean_mib = Get-Mean @($memory | ForEach-Object { $_.working_set_sum_mib })
        $result.working_set_sum_peak_mib = [double](($memory | Measure-Object working_set_sum_mib -Maximum).Maximum)
        $result.private_sum_mean_mib = Get-Mean @($memory | ForEach-Object { $_.private_sum_mib })
        $result.private_sum_peak_mib = [double](($memory | Measure-Object private_sum_mib -Maximum).Maximum)
        $result.memory_max_batch_skew_ms = [double](($memory | Measure-Object batch_skew_ms -Maximum).Maximum)
    }
    $result.status = if ($null -ne $result.cpu_single_core_percent) { 'observed_interval' } else { 'insufficient_cpu_observations' }
    return $result
}

function Get-ScenarioProcessMetrics($Scenario, $RootRows, $CefRows) {
    $start = Get-Time (Get-Field $Scenario 'sample_started_utc')
    $finish = Get-Time (Get-Field $Scenario 'sample_finished_utc')
    return [ordered]@{
        sample_started_utc = Get-Field $Scenario 'sample_started_utc'
        sample_finished_utc = Get-Field $Scenario 'sample_finished_utc'
        ue = Measure-ProcessInterval $RootRows $start $finish 'root_ue'
        cef = Measure-ProcessInterval $CefRows $start $finish 'cef_descendants'
    }
}

$runs = [Collections.Generic.List[object]]::new()
foreach ($directory in $RunDirectory) {
    $resolved = (Resolve-Path -LiteralPath $directory).Path
    $manifest = Read-Json (Join-Path $resolved 'run.json')
    $rootProcessId = Get-Number (Get-Field $manifest 'editor_pid')
    $rootInstance = Get-Field $manifest 'editor_instance'
    $csvPath = Join-Path $resolved 'process-samples.csv'
    $rows = @()
    if (Test-Path -LiteralPath $csvPath -PathType Leaf) {
        $rows = @(Import-Csv -LiteralPath $csvPath | ForEach-Object {
            [pscustomobject]@{
                Index = $_.sample_index; Time = Get-Time $_.timestamp_utc
                ProcessId = Get-Number $_.pid; Key = $_.instance_key; Name = $_.process_name; Event = $_.event
                Cpu = Get-Number $_.cpu_total_seconds; WorkingSet = Get-Number $_.working_set_bytes
                Private = Get-Number $_.private_bytes
            }
        })
    }
    $rootRows = @($rows | Where-Object {
        $null -ne $rootProcessId -and $_.ProcessId -eq $rootProcessId -and
        ([string]::IsNullOrWhiteSpace([string]$rootInstance) -or $_.Key -eq $rootInstance)
    })
    $cefRows = @($rows | Where-Object { $_.ProcessId -ne $rootProcessId -and $_.Name -match '(?i)(EpicWebHelper|UnrealCEFSubProcess|CEF)' })
    $rendering = [Collections.Generic.List[object]]::new()
    $renderMetadata = [Collections.Generic.List[object]]::new()
    foreach ($renderFile in @(Get-ChildItem -LiteralPath $resolved -Filter 'RmlUiComparison-*.json' -File)) {
        $report = Read-Json $renderFile.FullName
        $metadata = [ordered]@{ source = $renderFile.Name }
        foreach ($field in @('engine', 'rhi', 'cpu', 'rhi_adapter', 'primary_display_adapter',
            'warmup_seconds', 'sample_seconds', 'created_rows', 'method', 'frame_metric', 'widget_traversal_metric', 'workload_limitations')) {
            $metadata[$field] = Get-Field $report $field
        }
        $renderMetadata.Add([pscustomobject]$metadata)
        foreach ($scenario in @(Get-Field $report 'scenarios')) {
            $item = [ordered]@{ source = $renderFile.Name; name = Get-Field $scenario 'name'; rhi = Get-Field $report 'rhi' }
            foreach ($field in @('samples', 'frame_p50_ms', 'frame_p95_ms', 'frame_p99_ms',
                'widget_traversal_p50_ms', 'widget_traversal_p95_ms', 'widget_traversal_p99_ms',
                'rmlui_stages', 'frames', 'draws', 'full_frame_upload_mib', 'geometry', 'content_pixels_verified',
                'header_text_pixels_verified', 'rmlui_metrics_verified', 'unsupported_slate_features', 'screenshot')) {
                $item[$field] = Get-Field $scenario $field
            }
            $item.process = Get-ScenarioProcessMetrics $scenario $rootRows $cefRows
            $rendering.Add([pscustomobject]$item)
        }
    }
    $communication = [Collections.Generic.List[object]]::new()
    $communicationReport = Read-Json (Join-Path $resolved 'CommunicationComparison.json')
    foreach ($scenario in @(Get-Field $communicationReport 'scenarios')) {
        $trials = @(Get-Field $scenario 'trials')
        $means = @($trials | ForEach-Object { Get-Number (Get-Field $_ 'batch_mean_ns_per_read') })
        $latencies = @($trials | ForEach-Object { Get-Field $_ 'latencies_ms' } | ForEach-Object { Get-Number $_ })
        $item = [ordered]@{
            mode = Get-Field $scenario 'mode'; runtime = Get-Field $scenario 'runtime'
            concurrency = Get-Field $scenario 'concurrency'; reads_per_trial = Get-Field $scenario 'reads_per_trial'
            measured_trials = $trials.Count
            batch_mean_ns_per_read_p50 = Get-Percentile $means 0.50
            batch_mean_ns_per_read_p95 = Get-Percentile $means 0.95
            batch_mean_ns_per_read_p99 = Get-Percentile $means 0.99
            completed_reads_per_second_p50 = Get-Field $scenario 'completed_reads_per_second_p50'
            round_trip_ms_p50 = Get-Percentile $latencies 0.50
            round_trip_ms_p95 = Get-Percentile $latencies 0.95
            round_trip_ms_p99 = Get-Percentile $latencies 0.99
            process = Get-ScenarioProcessMetrics $scenario $rootRows $cefRows
        }
        $stageMeans = [ordered]@{}
        foreach ($stage in @('native_parse_ms', 'native_read_ms', 'native_serialize_ms', 'native_enqueue_ms', 'stringify_ms', 'parse_ms')) {
            $stageMeans[$stage] = Get-Mean @($trials | ForEach-Object { Get-Number (Get-Field $_ $stage) })
        }
        $item.native_and_json_mean_ms_per_trial = $stageMeans
        $communication.Add([pscustomobject]$item)
    }
    $runs.Add([pscustomobject][ordered]@{
        run_directory = $resolved; manifest = $manifest
        communication_verified = Get-Field $communicationReport 'verified'
        communication_timing_scope = Get-Field $communicationReport 'timing_scope'
        communication_event_transport = Get-Field $communicationReport 'event_transport'
        communication_property_scope = Get-Field $communicationReport 'property_scope'
        csv_rows = $rows.Count; root_ue_rows = $rootRows.Count; cef_rows = $cefRows.Count
        cef_process_names = @($cefRows | Select-Object -ExpandProperty Name -Unique)
        rendering_metadata = @($renderMetadata.ToArray())
        rendering = @($rendering.ToArray()); communication = @($communication.ToArray())
    })
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = if ($runs.Count -eq 1) { $runs[0].run_directory } else { Join-Path $runs[0].run_directory 'CombinedAnalysis' }
}
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$limitations = @(
    'CPU仅对同一instance_key在scenario UTC区间内的累计CPU做差；分母是观测首尾墙秒，100%表示一个逻辑核持续占用，可超过100%。未扣除Editor背景负载。',
    'CPU丢弃区间首尾到最近样本间的未观测部分；CEF按各实例首尾增量求和、以整体覆盖墙秒计算，实例生命周期或边界错位会低估，详情见cpu_instances。',
    '内存按同一sample_index内成功观测聚合，均值是有效批次的算术均值；sample_index是一轮顺序采样，并非硬件同时快照，最大时间偏差另列。缺失/失败批次不补零。',
    'Working set之和可能重复统计共享页；private bytes是私有提交量，不等于物理驻留。报告给出同时观测总和的均值/峰值，未相加各进程历史峰值。',
    '各场景顺序运行，UE/CEF实例、VM、字体及资源缓存可能保留。内存是当时进程总量，不能当作该UI后端的新增内存。',
    'CEF仅取本轮采样进程树中EpicWebHelper/UnrealCEFSubProcess/包含CEF名称的进程；UE按run.json editor_pid及instance匹配。zenserver、CrashReportClient等未计入。',
    '通信batch均值分位数描述试次间波动；浏览器RTT分位数描述每请求延迟。异步排队RTT与Puerts同步每次读取CPU成本口径不同，不可称作CPU加速倍数。',
    '通信JSON各阶段的浏览器计时受时钟精度限制；实测为0不表示该操作没有成本。原生阶段均值仅是调用内部阶段，未包含CEF进程和队列等待。',
    '浏览器并发32衡量最多32个在途请求的完成吞吐；实际并发还受reads_per_trial限制。Puerts是同步循环，没有并发32对应行。',
    '缺少scenario时间戳或不足两个成功CPU观测时，CPU为null；毫秒级Puerts批次短于进程采样间隔，不能从粗采样推导其独立CPU。',
    '渲染frame是宿主automation间隔；widget_traversal包含子控件Tick，CEF异步子进程不在其范围内。GPU和input-to-photon未在本分析中测量。'
)
$analysis = [ordered]@{ schema_version = 1; generated_utc = [datetime]::UtcNow.ToString('o'); limitations = $limitations; runs = @($runs.ToArray()) }
$analysis | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'analysis.json') -Encoding UTF8

$md = [Collections.Generic.List[string]]::new()
$md.Add('# 性能对比分析')
$md.Add('')
$md.Add('以下数值来自指定运行目录中的原始报告；null 表示没有足够的观测。保留各轮结果，不将多轮分位数相加或冒充合并样本分位数。')
foreach ($run in $runs) {
    $md.Add(''); $md.Add('## ' + (Split-Path -Leaf $run.run_directory)); $md.Add('')
    $md.Add('原始目录：' + $run.run_directory)
    foreach ($metadata in $run.rendering_metadata) {
        $md.Add(''); $md.Add(('环境：UE {0}；{1}；RHI适配器 {2}；CPU {3}；预热 {4}s，采样 {5}s。' -f
            $metadata.engine, $metadata.rhi, $metadata.rhi_adapter, $metadata.cpu, $metadata.warmup_seconds, $metadata.sample_seconds))
    }
    $md.Add(''); $md.Add('### 数据通信'); $md.Add('')
    $md.Add('| 路径 | 并发 | 读取/试次 | 试次 | batch均值 ns/read p50 / p95 / p99 | 完成读取/秒 p50 | 请求RTT ms p50 / p95 / p99 |')
    $md.Add('| --- | ---: | ---: | ---: | ---: | ---: | ---: |')
    foreach ($row in $run.communication) {
        $md.Add(('| {0} | {1} | {2} | {3} | {4} / {5} / {6} | {7} | {8} / {9} / {10} |' -f
            $row.mode, $row.concurrency, $row.reads_per_trial, $row.measured_trials,
            (Format-Number $row.batch_mean_ns_per_read_p50), (Format-Number $row.batch_mean_ns_per_read_p95), (Format-Number $row.batch_mean_ns_per_read_p99),
            (Format-Number $row.completed_reads_per_second_p50), (Format-Number $row.round_trip_ms_p50),
            (Format-Number $row.round_trip_ms_p95), (Format-Number $row.round_trip_ms_p99)))
    }
    $md.Add(''); $md.Add('通信有效性：' + [string]$run.communication_verified + '。Web事件正反JSON、官方Promise控制组和Puerts直接绑定采用不同调用机制，具体边界保存在 analysis.json。')
    $md.Add(''); $md.Add('### 渲染'); $md.Add('')
    $md.Add('| 路径 | frame ms p50 / p95 / p99 | widget traversal ms p50 / p95 / p99 |')
    $md.Add('| --- | ---: | ---: |')
    foreach ($row in $run.rendering) {
        $md.Add(('| {0} | {1} / {2} / {3} | {4} / {5} / {6} |' -f $row.name,
            (Format-Number $row.frame_p50_ms), (Format-Number $row.frame_p95_ms), (Format-Number $row.frame_p99_ms),
            (Format-Number $row.widget_traversal_p50_ms), (Format-Number $row.widget_traversal_p95_ms), (Format-Number $row.widget_traversal_p99_ms)))
    }
    $md.Add(''); $md.Add('| RmlUi路径 | 阶段 | 调用数 | 总ms | 均值ms |'); $md.Add('| --- | --- | ---: | ---: | ---: |')
    foreach ($row in $run.rendering) {
        if ($null -eq $row.rmlui_stages) { continue }
        foreach ($stage in $row.rmlui_stages.PSObject.Properties) {
            $md.Add(('| {0} | {1} | {2} | {3} | {4} |' -f $row.name, $stage.Name, $stage.Value.calls,
                (Format-Number $stage.Value.total_ms), (Format-Number $stage.Value.mean_ms)))
        }
    }
    $md.Add(''); $md.Add('### 采样区间内进程成本'); $md.Add('')
    $md.Add('| 场景 | 进程组 | 单核CPU% | CPU覆盖墙秒 | 内存批次 | working-set总和 MiB 均值 / 峰值 | private总和 MiB 均值 / 峰值 | 状态 |')
    $md.Add('| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |')
    foreach ($row in @($run.rendering) + @($run.communication)) {
        $label = Get-Field $row 'name'
        if ($null -eq $label) { $label = $row.mode + '/c' + $row.concurrency }
        foreach ($group in @('ue', 'cef')) {
            $metric = Get-Field $row.process $group
            $md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} / {6} | {7} / {8} | {9} |' -f $label, $group,
                (Format-Number $metric.cpu_single_core_percent), (Format-Number $metric.cpu_observed_wall_seconds),
                (Format-Number $metric.memory_sample_count 'F0'), (Format-Number $metric.working_set_sum_mean_mib),
                (Format-Number $metric.working_set_sum_peak_mib), (Format-Number $metric.private_sum_mean_mib),
                (Format-Number $metric.private_sum_peak_mib), $metric.status))
        }
    }
}
$md.Add(''); $md.Add('## 测量边界'); $md.Add('')
foreach ($limit in $limitations) { $md.Add('- ' + $limit) }
$md | Set-Content -LiteralPath (Join-Path $OutputDirectory 'analysis.md') -Encoding UTF8
Write-Host ('Analysis JSON: ' + (Join-Path $OutputDirectory 'analysis.json'))
Write-Host ('Analysis Markdown: ' + (Join-Path $OutputDirectory 'analysis.md'))
