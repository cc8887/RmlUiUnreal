$ErrorActionPreference = 'Stop'
$bridge = Split-Path $PSScriptRoot -Parent
$destination = [IO.Path]::GetFullPath((Join-Path $bridge '..\..\..\Content\RmlUi\Licenses\Grid'))
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$taffyLicense = Join-Path $PSScriptRoot 'Taffy-LICENSE.txt'
if (!(Test-Path -LiteralPath $taffyLicense)) {
    Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/DioxusLabs/taffy/77f385683c1d698c91a23a259f87fdddf26925fb/LICENSE' -OutFile $taffyLicense
}
Push-Location $bridge
try {
    $metadata = (& cargo metadata --format-version 1 --locked --offline --manifest-path grid/Cargo.toml | ConvertFrom-Json)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read locked dependency metadata.' }
    $notices = @('Native Grid dependencies. Sources are included in the plugin under Source/ThirdParty/RmlUiBridge/grid/vendor.',
        'cssparser and cssparser-macros use MPL-2.0. Their unmodified corresponding source is also available at the versioned crates.io download URLs below.', '')
    foreach ($package in ($metadata.packages | Where-Object { $_.name -ne 'rmlui_grid' } | Sort-Object name,version)) {
        $label = "$($package.name)-$($package.version)"
        $notices += "$label | $($package.license) | https://crates.io/api/v1/crates/$($package.name)/$($package.version)/download"
        $source = Split-Path $package.manifest_path -Parent
        Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Name -match '^(LICENSE|COPYING|NOTICE)' } | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $destination "$label-$($_.Name).txt") -Force
        }
    }
    Copy-Item -LiteralPath $taffyLicense -Destination (Join-Path $destination 'taffy-0.14.0-LICENSE.txt') -Force
    $notices | Set-Content -LiteralPath (Join-Path $destination 'NOTICE.txt') -Encoding utf8NoBOM
} finally { Pop-Location }
