param(
    [Parameter(Mandatory=$true)][string]$ReleaseDir,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [ValidateSet('obs-root','programdata')][string]$Layout='obs-root'
)
$ErrorActionPreference='Stop'
$pluginRoot=Join-Path $ReleaseDir 'wasurene-input-display'
$dll=Join-Path $pluginRoot 'bin/64bit/wasurene-input-display.dll'
if (!(Test-Path -LiteralPath $dll)) { throw 'Installed plugin DLL is missing.' }
if (Test-Path -LiteralPath $OutputDir) { throw 'Package output already exists.' }
if ($Layout -eq 'programdata') {
    $bin=Join-Path $OutputDir 'wasurene-input-display/bin/64bit'
    $data=Join-Path $OutputDir 'wasurene-input-display/data'
} else {
    $bin=Join-Path $OutputDir 'obs-plugins/64bit'
    $data=Join-Path $OutputDir 'data/obs-plugins/wasurene-input-display'
}
New-Item -ItemType Directory -Path $bin,$data | Out-Null
Copy-Item -LiteralPath $dll -Destination $bin
Get-ChildItem -LiteralPath (Join-Path $pluginRoot 'data') | Copy-Item -Destination $data -Recurse
Copy-Item -LiteralPath README.md -Destination (Join-Path $OutputDir 'README.md')
