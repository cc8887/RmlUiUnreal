$ErrorActionPreference = 'Stop'
$bridge = Split-Path $PSScriptRoot -Parent
$name = 'RmlUi-ba95ffe8bfb6370efb2cdcca927eaad4710c5413'
$baseline = Join-Path $bridge 'build\upstream-baseline'
if (!(Test-Path (Join-Path $baseline "$name\CMakeLists.txt"))) {
    Expand-Archive -LiteralPath (Join-Path $bridge "vendor\$name.zip") -DestinationPath $baseline
}
Push-Location $bridge
try {
    $lines = & git -c core.autocrlf=false diff --no-index --binary -- "build/upstream-baseline/$name" "vendor/$name"
    if ($LASTEXITCODE -gt 1) { throw 'Cannot generate upstream patch.' }
    $patch = ($lines -join "`n") + "`n"
    $patch = $patch.Replace("a/build/upstream-baseline/$name/", 'a/').Replace("b/vendor/$name/", 'b/')
    $patchPath = Join-Path $PSScriptRoot 'RmlUi.patch'
    [IO.File]::WriteAllText($patchPath, $patch, [Text.UTF8Encoding]::new($false))
    & git -C (Join-Path $baseline $name) apply --check $patchPath
    if ($LASTEXITCODE -ne 0) { throw 'Generated patch does not apply to pinned upstream source.' }
    & git -C (Join-Path $bridge "vendor/$name") apply --reverse --check $patchPath
    if ($LASTEXITCODE -ne 0) { throw 'Generated patch differs from vendored source.' }
} finally { Pop-Location }
