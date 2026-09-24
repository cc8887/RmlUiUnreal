param(
    [ValidateSet('Vue', 'Chat', 'ActorObserver')][string]$App = 'ActorObserver',
    [switch]$Watch,
    [string]$ProjectRoot = (Join-Path $PSScriptRoot '..\..\..')
)

$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath $ProjectRoot).Path
& (Join-Path $PSScriptRoot 'InstallSamples.ps1') -ProjectRoot $project
$coreFrontend = Join-Path $PSScriptRoot '..\Frontend'
$sampleFrontend = Join-Path $PSScriptRoot 'Frontend'
foreach ($frontend in @($coreFrontend, $sampleFrontend)) {
    if (!(Test-Path -LiteralPath (Join-Path $frontend 'node_modules'))) {
        Push-Location $frontend
        try {
            & npm.cmd ci --no-audit --no-fund
            if ($LASTEXITCODE -ne 0) { throw "npm ci failed in $frontend" }
        } finally { Pop-Location }
    }
}
$output = Join-Path $project "Plugins\RmlUiUnrealSamples\Content\$App"
$arguments = @('tools\build.mjs')
if ($App -eq 'ActorObserver') { $arguments += '--actors' }
if ($App -eq 'Chat') { $arguments += '--chat' }
$arguments += @('--mirror-output', $output)
if ($Watch) { $arguments += '--watch' }
Push-Location $sampleFrontend
try {
    & node.exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$App frontend build failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }
