param([ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release', [switch]$SkipDownload)
$ErrorActionPreference = 'Stop'
$bridgeRoot = $PSScriptRoot
$vendorRoot = Join-Path $bridgeRoot 'vendor'
$packages = @(
    @{ Name = 'RmlUi'; Repo = 'mikke89/RmlUi'; Commit = 'ba95ffe8bfb6370efb2cdcca927eaad4710c5413' },
    @{ Name = 'freetype'; Repo = 'freetype/freetype'; Commit = '0a0221a1347e2f1e07c395263540026e9a0aa7c7' },
    @{ Name = 'lunasvg'; Repo = 'sammycage/lunasvg'; Commit = 'f8aabfb444bb37f69df7290790f57e4a27730a93' },
    @{ Name = 'plutovg'; Repo = 'sammycage/plutovg'; Commit = '5e4712cf873b0c7829a4a6157763e2ad3ac49164' }
)
New-Item -ItemType Directory -Path $vendorRoot -Force | Out-Null
foreach ($package in $packages) {
    $sourceDirectory = Join-Path $vendorRoot ($package.Name + '-' + $package.Commit)
    if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory 'CMakeLists.txt'))) {
        if ($SkipDownload) { throw "Missing pinned source: $sourceDirectory" }
        $archive = Join-Path $vendorRoot ($package.Name + '-' + $package.Commit + '.zip')
        if (-not (Test-Path -LiteralPath $archive)) {
            Invoke-WebRequest -Uri ('https://codeload.github.com/' + $package.Repo + '/zip/' + $package.Commit) -OutFile $archive
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $vendorRoot -Force
        if ($package.Name -eq 'RmlUi') {
            & git -C $sourceDirectory apply --ignore-whitespace (Join-Path $bridgeRoot 'grid\RmlUi.patch')
            if ($LASTEXITCODE -ne 0) { throw 'Could not apply the pinned RmlUi integration patch.' }
        }
    }
}
$rmlDirectory = Join-Path $vendorRoot 'RmlUi-ba95ffe8bfb6370efb2cdcca927eaad4710c5413'
$hostPatch = Join-Path $bridgeRoot 'host\RmlUiHost.patch'
# Tracked vendor sources already include this patch. A fresh pinned download
# applies it after grid/RmlUi.patch; incompatible local edits fail visibly.
& git -C $rmlDirectory apply --reverse --check --ignore-whitespace $hostPatch 2>$null
if ($LASTEXITCODE -ne 0) {
    & git -C $rmlDirectory apply --check --ignore-whitespace $hostPatch
    if ($LASTEXITCODE -ne 0) { throw 'RmlUi host patch does not match the pinned vendor sources.' }
    & git -C $rmlDirectory apply --ignore-whitespace $hostPatch
    if ($LASTEXITCODE -ne 0) { throw 'Could not apply the RmlUi host API and text input patch.' }
}
$lunasvgDirectory = Join-Path $vendorRoot 'lunasvg-f8aabfb444bb37f69df7290790f57e4a27730a93'
$plutovgDirectory = Join-Path $vendorRoot 'plutovg-5e4712cf873b0c7829a4a6157763e2ad3ac49164'
$lunasvgPlutovgDirectory = Join-Path $lunasvgDirectory 'plutovg'
if (-not (Test-Path -LiteralPath (Join-Path $lunasvgPlutovgDirectory 'CMakeLists.txt'))) {
    New-Item -ItemType Directory -Path $lunasvgPlutovgDirectory -Force | Out-Null
    Get-ChildItem -LiteralPath $plutovgDirectory | Copy-Item -Destination $lunasvgPlutovgDirectory -Recurse -Force
}
$buildDirectory = Join-Path $bridgeRoot 'build'
& cmake -S $bridgeRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'RmlUi bridge configure failed.' }
& cmake --build $buildDirectory --config $Configuration --target RmlUiBridge --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'RmlUi bridge build failed.' }
