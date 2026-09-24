param([string]$ProjectRoot = (Join-Path $PSScriptRoot '..\..\..'))

$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath $ProjectRoot).Path
$source = Join-Path $PSScriptRoot 'RmlUiUnrealSamples'
$destination = Join-Path $project 'Plugins\RmlUiUnrealSamples'
if (!(Test-Path -LiteralPath (Join-Path $project 'RmlUiUnrealTest.uproject'))) {
    throw "Expected the RmlUiUnrealTest host project at $project"
}
if (!(Test-Path -LiteralPath (Join-Path $project 'Plugins\RmlUiUnreal\RmlUiUnreal.uplugin'))) {
    throw 'The core RmlUiUnreal plugin must be installed before its samples.'
}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$descriptor = Join-Path $destination 'RmlUiUnrealSamples.uplugin'
if ((Test-Path -LiteralPath $descriptor) -and
    ((Get-Content -LiteralPath $descriptor -Raw | ConvertFrom-Json).FriendlyName -ne 'RmlUi Unreal Samples')) {
    throw "Refusing to replace an unrelated plugin at $destination"
}
foreach ($name in @('Source', 'Content')) {
    $path = Join-Path $source $name
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $destination -Recurse -Force }
}
Copy-Item -LiteralPath (Join-Path $source 'RmlUiUnrealSamples.uplugin.in') -Destination $descriptor -Force
Write-Host "Installed RmlUi Unreal samples: $destination"
