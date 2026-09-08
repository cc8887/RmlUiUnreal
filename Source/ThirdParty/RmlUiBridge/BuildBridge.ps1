param([ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release', [switch]$SkipDownload)
$ErrorActionPreference = 'Stop'
$bridgeRoot = $PSScriptRoot
$vendorRoot = Join-Path $bridgeRoot 'vendor'
$packages = @(
    @{ Name = 'RmlUi'; Repo = 'mikke89/RmlUi'; Commit = 'ba95ffe8bfb6370efb2cdcca927eaad4710c5413' },
    @{ Name = 'freetype'; Repo = 'freetype/freetype'; Commit = '0a0221a1347e2f1e07c395263540026e9a0aa7c7' }
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
$buildDirectory = Join-Path $bridgeRoot 'build'
& cmake -S $bridgeRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'RmlUi bridge configure failed.' }
& cmake --build $buildDirectory --config $Configuration --target RmlUiBridge --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'RmlUi bridge build failed.' }
