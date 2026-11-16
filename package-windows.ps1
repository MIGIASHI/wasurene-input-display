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
$layoutNote=if ($Layout -eq 'programdata') {
    "# このZIPはProgramData版です`n`n展開した wasurene-input-display フォルダを C:\ProgramData\obs-studio\plugins\ へコピーしてください。obs-plugins と data の2フォルダは、このZIPには入りません。"
} else {
    "# このZIPはOBS本体フォルダ用の通常版です`n`n展開した obs-plugins と data の2フォルダをOBS本体フォルダ（通常 C:\Program Files\obs-studio\）へコピーしてください。"
}
[IO.File]::WriteAllText((Join-Path $OutputDir 'README.md'),$layoutNote+"`n`n"+(Get-Content -LiteralPath README.md -Raw),[Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath UPDATE-UNINSTALL.md -Destination (Join-Path $OutputDir 'UPDATE-UNINSTALL.md')
