param(
    [int]$AppId = 1304930,
    [string]$SteamCmd = 'C:\Users\shiro\steamcmd\steamcmd.exe',
    [string]$SymbolDir = 'C:\Users\shiro\AppData\Local\Temp\opencode\outlast_debug_symbols'
)

$ErrorActionPreference = 'Stop'
$pdbName = 'TOTClient-Win64-Shipping.pdb'
$gamePdb = 'C:\Program Files (x86)\Steam\steamapps\common\The Outlast Trials\OPP\Binaries\Win64\' + $pdbName

if (Test-Path -LiteralPath $gamePdb) {
    Write-Host "Already installed: $gamePdb"
    return
}

if (-not (Test-Path -LiteralPath $SteamCmd)) {
    Write-Error @"
SteamCMD was not found at $SteamCmd.
Either install SteamCMD there, or use Steam itself:
Library -> The Outlast Trials -> Properties -> Installed Files -> Download Debug Symbols.
"@
}

if (Get-Process steam -ErrorAction SilentlyContinue) {
    Write-Warning 'The Steam desktop client is running. SteamCMD may fail with "already logged in". Close Steam if the download does not start.'
}

New-Item -ItemType Directory -Force -Path $SymbolDir | Out-Null
$credential = Get-Credential -Message 'Steam account that owns The Outlast Trials (password is not saved)'
$password = $credential.GetNetworkCredential().Password

& $SteamCmd `
    "+login $($credential.UserName) $password" `
    "+app_debug_install $AppId $SymbolDir" `
    '+quit'

$downloaded = Get-ChildItem -LiteralPath $SymbolDir -Filter $pdbName -File -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $downloaded) {
    Write-Warning "$pdbName was not found under $SymbolDir. If Steam Guard prompted, retry after approval."
    return
}

Write-Host "Downloaded PDB: $downloaded"
$project = Split-Path -Parent $PSScriptRoot
Write-Host "Next: & '$project\tools\dump_outlast_symbols.ps1' -PdbPath '$downloaded'"
