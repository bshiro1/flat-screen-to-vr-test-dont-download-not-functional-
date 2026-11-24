param(
    [string]$PdbPath,
    [string]$OutFile,
    [string[]]$Filters = @(
        'ProcessEvent',
        'UObject',
        'UWorld',
        'FName',
        'StaticConstructObject',
        'Z_Construct_UFunction_',
        'Z_Construct_UClass_',
        'FClassRegistrationInfo',
        'GUObjectArray',
        'GObjects',
        'GWorld',
        'GNames',
        'Interact',
        'Door',
        'Valve',
        'Inventory',
        'Revive',
        'Grip',
        'Hands',
        'Camera'
    )
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$pdbName = 'TOTClient-Win64-Shipping.pdb'
$gameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\The Outlast Trials'
$expected = Join-Path $gameRoot ('OPP\Binaries\Win64\' + $pdbName)

if ($PdbPath) {
    $pdb = (Resolve-Path -LiteralPath $PdbPath -ErrorAction Stop).Path
} elseif (Test-Path -LiteralPath $expected) {
    $pdb = $expected
} else {
    $pdb = @(
        $gameRoot,
        'C:\Users\shiro\AppData\Local\Temp\opencode\outlast_debug_symbols'
    ) | ForEach-Object {
        if (Test-Path -LiteralPath $_) {
            Get-ChildItem -LiteralPath $_ -Filter $pdbName -File -Recurse -ErrorAction SilentlyContinue
        }
    } | Select-Object -First 1 -ExpandProperty FullName
}

if (-not $pdb) {
    Write-Error @"
$pdbName was not found in the installed game or the debug-symbol download folder.
In Steam: Library -> The Outlast Trials -> Properties -> Installed Files ->
Download Debug Symbols. Wait for the download to finish, then run this script again.
You can also pass an explicit path with -PdbPath <path>.
"@
}

$exe = Join-Path $project 'build\vrc_pdb_dump.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Error "$exe is missing. Build the C++ solution first."
}

if (-not $OutFile) {
    $outDir = Join-Path $project 'reports'
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    $OutFile = Join-Path $outDir 'outlast_pdb_symbols.txt'
}

& $exe $pdb @Filters | Tee-Object -FilePath $OutFile
Write-Host ""
Write-Host "Saved symbol dump to: $OutFile"
Write-Host "PDB: $pdb"
