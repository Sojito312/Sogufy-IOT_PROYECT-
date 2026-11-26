[CmdletBinding()]
param(
    [string]$Proyecto = (Join-Path $env:USERPROFILE "blink")
)
$ErrorActionPreference = "Stop"
$Proyecto = (Resolve-Path -LiteralPath $Proyecto).Path
$main = Join-Path $Proyecto "main"
if (!(Test-Path -LiteralPath (Join-Path $Proyecto "CMakeLists.txt") -PathType Leaf) -or
    !(Test-Path -LiteralPath $main -PathType Container)) {
    throw "La carpeta indicada debe contener CMakeLists.txt y main."
}
if ((Resolve-Path -LiteralPath $PSScriptRoot).Path -eq $Proyecto) {
    throw "Extrae primero el ZIP en una carpeta aparte."
}
if (Test-Path -LiteralPath (Join-Path $main "spotify.c")) {
    throw "FIAD Music ya está instalado en main."
}
$archivos = @("sdkconfig.fiad.defaults", "configurar_fiad.py", "LEEME_FIAD.md")
foreach ($archivo in $archivos) {
    if (Test-Path -LiteralPath (Join-Path $Proyecto $archivo)) {
        throw "Ya existe $archivo en el proyecto."
    }
    if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot $archivo) -PathType Leaf)) {
        throw "Falta $archivo en la carpeta extraída."
    }
}
$padre = Split-Path $Proyecto -Parent
$nombre = Split-Path $Proyecto -Leaf
$stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
$respaldo = Join-Path $padre "$nombre-main-respaldo-$stamp"
$temporal = Join-Path $padre "$nombre-main-fiad-$stamp"
$nuevos = @()
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "main") -Destination $temporal -Recurse
$originalMovido = $false
try {
    Move-Item -LiteralPath $main -Destination $respaldo
    $originalMovido = $true
    Move-Item -LiteralPath $temporal -Destination $main
    foreach ($archivo in $archivos) {
        $destino = Join-Path $Proyecto $archivo
        $nuevos += $destino
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $archivo) -Destination $destino
    }
}
catch {
    foreach ($archivo in $nuevos) {
        if (Test-Path -LiteralPath $archivo) { Remove-Item -LiteralPath $archivo -Force }
    }
    if ($originalMovido) {
        if (Test-Path -LiteralPath $main) {
            Move-Item -LiteralPath $main -Destination $temporal
        }
        Move-Item -LiteralPath $respaldo -Destination $main
    }
    throw
}
Write-Host "Firmware copiado a $main"
Write-Host "Respaldo de tu carpeta main: $respaldo"
