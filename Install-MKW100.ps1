[CmdletBinding()]
param([string]$InstallRoot, [ValidateRange(1,16)][int]$Jobs=4, [switch]$AllowSourceDependencies, [switch]$PreflightOnly)
$ErrorActionPreference='Stop'
if (!$InstallRoot) { $InstallRoot=Read-Host 'WiiCompiled installation folder (contains Base, Toolkit, BuildWorkspace)' }
$InstallRoot=(Resolve-Path -LiteralPath $InstallRoot).Path
$dvd=Join-Path $InstallRoot 'GameAssets/DATA'
if (!(Test-Path -LiteralPath $dvd)) { throw "Missing game data: $dvd" }
& (Join-Path $PSScriptRoot 'Build-MKW100.ps1') -InstallRoot $InstallRoot -Jobs $Jobs -AllowSourceDependencies:$AllowSourceDependencies -PreflightOnly:$PreflightOnly
if ($LASTEXITCODE -ne 0) { throw 'Build failed; see build-logs.' }
if ($PreflightOnly) { return }
$play=Join-Path $PSScriptRoot 'play'
if (Get-Process WiiCompiled -ErrorAction SilentlyContinue) { throw 'Close WiiCompiled, then rerun the installer to finish staging.' }
New-Item -ItemType Directory -Force $play | Out-Null
Get-ChildItem -LiteralPath (Join-Path $InstallRoot 'Base') -Filter '*.dll' -File | Copy-Item -Destination $play
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native-build-win/WiiCompiled.exe') -Destination $play
$dsp=Join-Path $InstallRoot 'Base/dsp_coef.bin'
if (Test-Path -LiteralPath $dsp) { Copy-Item -LiteralPath $dsp -Destination $play }
Set-Content -LiteralPath (Join-Path $play 'portable.txt') -Value ''
$userData=Join-Path $play 'UserData'
New-Item -ItemType Directory -Force $userData | Out-Null
$config=Join-Path $userData 'Config.toml'
if (!(Test-Path -LiteralPath $config)) {
    $dvdToml=$dvd.Replace('\','/').Replace('"','\"')
    $nandToml=(Join-Path $userData 'NAND').Replace('\','/').Replace('"','\"')
    @"
[paths]
dvd_root = "$dvdToml"
nand_root = "$nandToml"
[network]
enabled = false
[discord]
enabled = false
[local_vs]
players = 100
unlock_all = true
instant_boxes = true
render_30fps = true
"@ | Set-Content -LiteralPath $config -Encoding UTF8
}
Write-Host 'Installed. Run Play-MKW100.cmd. Existing settings and saves are preserved on updates.'
