[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$GeneratedDir,
    [string]$DiscPath,
    [string]$OutputDir,
    [ValidateRange(1, 16)][int]$Jobs = 2
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'O pacote UWP precisa ser compilado no Windows com Visual Studio 2022.' }
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$logDirectory = Join-Path $repository 'build/uwp/logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
Start-Transcript -Path (Join-Path $logDirectory 'build-uwp.log') -Force | Out-Null
try {
$project = Join-Path $repository 'ports/uwp/RondoRecomp.vcxproj'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Instale Visual Studio 2022 com as ferramentas C++ UWP v143.' }
$visualStudio = & $vswhere -latest -products '*' -version '[17.0,18.0)' -property installationPath
if ($LASTEXITCODE -ne 0 -or !$visualStudio) { throw 'Visual Studio 2022 nao encontrado (toolset v143).' }
$msbuild = Join-Path $visualStudio 'MSBuild/Current/Bin/amd64/MSBuild.exe'
$uwpToolset = Join-Path $visualStudio 'MSBuild/Microsoft/VC/v170/Application Type/Windows Store/10.0/Platforms/x64/PlatformToolsets/v143/Toolset.props'
if (!(Test-Path -LiteralPath $msbuild) -or !(Test-Path -LiteralPath $uwpToolset)) {
    throw 'Faltam C++ (v143) Universal Windows Platform tools. No VS Installer, adicione Microsoft.VisualStudio.ComponentGroup.UWP.VC.'
}
$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
foreach ($tool in @('fxc.exe', 'makeappx.exe', 'signtool.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $sdkRoot "bin/10.0.22621.0/x64/$tool"))) {
        throw "Instale Windows SDK 10.0.22621.0: ferramenta ausente $tool."
    }
}

if ($DiscPath) {
    $DiscPath = (Resolve-Path -LiteralPath $DiscPath).Path
    if ([IO.Path]::GetExtension($DiscPath).ToLowerInvariant() -notin @('.chd', '.cue')) {
        throw 'DiscPath deve apontar para seu disco .chd ou .cue, com todas as faixas presentes.'
    }
    if (!$GeneratedDir) { $GeneratedDir = Join-Path $repository 'work/uwp-generated' }
    $GeneratedDir = [IO.Path]::GetFullPath($GeneratedDir)
    if ((Test-Path -LiteralPath $GeneratedDir) -and @(Get-ChildItem -LiteralPath $GeneratedDir -Force).Count -gt 0) {
        throw 'Para gerar novamente, escolha GeneratedDir vazio. Para reutilizar codigo existente, omita DiscPath.'
    }
    New-Item -ItemType Directory -Path $GeneratedDir -Force | Out-Null
    $hostBuild = Join-Path $repository 'build/uwp-host'
    & cmake -S $repository -B $hostBuild -G 'Visual Studio 17 2022' -A x64 | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'CMake nao conseguiu configurar rondo_gen.' }
    & cmake --build $hostBuild --config Release --target rondo_gen --parallel $Jobs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar rondo_gen.' }
    & (Join-Path $hostBuild 'Release/rondo_gen.exe') $DiscPath --out $GeneratedDir | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao gerar C a partir do disco.' }
}

$flavor = if ($GeneratedDir) { 'recompiled' } else { 'interpreter-bootstrap' }
$appxDirectory = Join-Path $repository "build/uwp/AppPackages/$Configuration-$flavor"
New-Item -ItemType Directory -Path $appxDirectory -Force | Out-Null
if ($appxDirectory.Contains(';')) { throw 'MSBuild nao aceita ponto e virgula neste caminho.' }
$buildArguments = @(
    $project, '/restore', '/t:Build', "/m:$Jobs",
    "/p:Configuration=$Configuration", '/p:Platform=x64',
    '/p:AppxPackageSigningEnabled=false', '/p:GenerateAppxPackageOnBuild=true',
    '/p:UapAppxPackageBuildMode=SideloadOnly', '/p:AppxBundle=Never',
    "/p:AppxPackageDir=$appxDirectory/", "/bl:$(Join-Path $logDirectory 'uwp.binlog')"
)
if ($GeneratedDir) {
    $GeneratedDir = (Resolve-Path -LiteralPath $GeneratedDir).Path
    if (!(Test-Path -LiteralPath (Join-Path $GeneratedDir 'table.c'))) { throw 'GeneratedDir nao contem table.c.' }
    if ($GeneratedDir.Contains(';')) { throw 'MSBuild nao aceita ponto e virgula neste caminho de propriedade.' }
    $buildArguments += "/p:RondoGeneratedDir=$GeneratedDir"
    Write-Host 'Compilando a tabela e os trechos C gerados; recomp_none.c sera excluido.'
} else {
    Write-Host 'Compilando a base com interpretador. Nenhum codigo de jogo gerado foi fornecido.'
}
& $msbuild @buildArguments | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Falha no build ou empacotamento UWP. Consulte o erro MSBuild acima.' }
[xml]$sourceManifest = Get-Content -LiteralPath (Join-Path $repository 'ports/uwp/Package.appxmanifest') -Raw
$versionPattern = '_' + [regex]::Escape($sourceManifest.Package.Identity.Version) + '_x64(?:_|\.)'
$packages = @(Get-ChildItem -LiteralPath $appxDirectory -Recurse -File -Filter '*.appx' |
    Where-Object { $_.FullName -notmatch '[\\/]Dependencies[\\/]' -and
        $_.Name -match $versionPattern })
if ($packages.Count -ne 1) {
    throw "Esperado exatamente um APPX principal deste build em $appxDirectory; encontrados: $($packages.Count)."
}
$packArguments = @{ AppxPath = $packages[0].FullName; Flavor = $flavor }
if ($OutputDir) { $packArguments.OutputDir = $OutputDir }
$package = & (Join-Path $PSScriptRoot 'Package-Uwp.ps1') @packArguments
Write-Host "MSIX x64 sem assinatura: $($package.PackagePath)"
Write-Host 'Assine com Sign-Uwp.ps1 -PackagePath <caminho.msix> antes de instalar no Dev Mode.'
$package
} finally {
    Stop-Transcript | Out-Null
}
