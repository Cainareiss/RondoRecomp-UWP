[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$AppxPath,
    [ValidateSet('interpreter-bootstrap', 'recompiled')][string]$Flavor = 'interpreter-bootstrap',
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'Execute o empacotamento com o Windows SDK no Windows.' }
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$AppxPath = (Resolve-Path -LiteralPath $AppxPath).Path
if ([IO.Path]::GetExtension($AppxPath).ToLowerInvariant() -notin @('.appx', '.msix')) {
    throw 'AppxPath deve ser o APPX/MSIX principal produzido por MSBuild.'
}
if (!$OutputDir) { $OutputDir = Join-Path $repository 'build/uwp/MSIX' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$makeAppx = Join-Path $sdkRoot 'bin/10.0.22621.0/x64/makeappx.exe'
if (!(Test-Path -LiteralPath $makeAppx)) { throw 'Instale Windows SDK 10.0.22621.0 (MakeAppx).' }
$staging = Join-Path $repository ('build/uwp/msix-staging/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging -Force | Out-Null
try {
    # Repackage the processed UWP payload, preserving dependencies/PRI/metadata.
    # Both MakeAppx calls retain default validation; do not use /nv.
    & $makeAppx unpack /p $AppxPath /d $staging /o | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'MakeAppx nao conseguiu extrair o APPX original.' }
    $manifestPath = Join-Path $staging 'AppxManifest.xml'
    [xml]$manifest = Get-Content -LiteralPath $manifestPath -Raw
    $identity = $manifest.Package.Identity
    if ($identity.ProcessorArchitecture -ne 'x64' -or
        $identity.Name -ne 'RondoRecomp.UWP.Dev' -or
        $identity.Publisher -ne 'CN=RondoRecomp.UWP.Dev') {
        throw 'Manifesto processado nao corresponde a identidade UWP x64 esperada.'
    }
    $applications = @($manifest.Package.Applications.Application)
    if ($applications.Count -ne 1 -or $applications[0].Executable -ne 'RondoRecomp.exe' -or
        $applications[0].EntryPoint -ne 'RondoRecomp.App') {
        throw 'O APPX deve conter o executavel e EntryPoint UWP processados por MSBuild.'
    }
    $requiredFiles = @(
        'RondoRecomp.exe', 'Shaders/FrameVS.cso', 'Shaders/FramePS.cso',
        'Assets/StoreLogo.png', 'Assets/Square44x44Logo.png',
        'Assets/Square150x150Logo.png', 'Assets/Wide310x150Logo.png',
        'Assets/SplashScreen.png', 'Assets/GameplayOverlay.png',
        'Assets/GameplayOverlay2.png'
    )
    foreach ($relativePath in $requiredFiles) {
        $file = Join-Path $staging $relativePath
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -eq 0) {
            throw "Payload UWP ausente ou vazio: $relativePath"
        }
    }
    $executable = [IO.BinaryReader]::new([IO.File]::OpenRead((Join-Path $staging 'RondoRecomp.exe')))
    try {
        if ($executable.ReadUInt16() -ne 0x5a4d) { throw 'RondoRecomp.exe nao possui cabecalho PE.' }
        $executable.BaseStream.Position = 0x3c
        $peOffset = $executable.ReadUInt32()
        $executable.BaseStream.Position = $peOffset
        if ($executable.ReadUInt32() -ne 0x00004550 -or $executable.ReadUInt16() -ne 0x8664) {
            throw 'RondoRecomp.exe deve ser um executavel PE x64 (AMD64).'
        }
        $executable.BaseStream.Position = $peOffset + 20
        if ($executable.ReadUInt16() -lt 72) { throw 'Cabecalho opcional PE ausente ou truncado.' }
        $optionalHeader = $peOffset + 24
        $executable.BaseStream.Position = $optionalHeader
        if ($executable.ReadUInt16() -ne 0x20b) { throw 'RondoRecomp.exe deve usar PE32+ (64 bits).' }
        $executable.BaseStream.Position = $optionalHeader + 68
        if ($executable.ReadUInt16() -ne 2) { throw 'RondoRecomp.exe deve usar o subsistema Windows GUI.' }
        if (($executable.ReadUInt16() -band 0x1000) -eq 0) {
            throw 'RondoRecomp.exe deve conter a caracteristica PE APPCONTAINER de um aplicativo UWP.'
        }
    } finally { $executable.Dispose() }
    $gameFiles = @(Get-ChildItem -LiteralPath $staging -Recurse -File |
        Where-Object { $_.Extension.ToLowerInvariant() -in @('.chd', '.cue', '.iso', '.pce', '.wav') })
    if ($gameFiles.Count -gt 0) { throw 'O pacote distribuivel nao deve incluir dados do disco do jogo.' }
    # MakeAppx generates new footprint files. Keep CodeIntegrity.cat if MSBuild supplied it.
    foreach ($footprint in @('[Content_Types].xml', 'AppxBlockMap.xml', 'AppxSignature.p7x')) {
        $path = Join-Path $staging $footprint
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
    $packageName = "RondoRecomp-UWP_$($identity.Version)_x64_$Flavor.msix"
    $packagePath = Join-Path $OutputDir $packageName
    & $makeAppx pack /d $staging /p $packagePath /h SHA256 /o | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'MakeAppx rejeitou o payload ou manifesto ao gerar o MSIX.' }

    $dependencySource = Join-Path (Split-Path -Parent $AppxPath) 'Dependencies'
    $dependencyTarget = Join-Path $OutputDir 'Dependencies'
    if (Test-Path -LiteralPath $dependencyTarget) { Remove-Item -LiteralPath $dependencyTarget -Recurse -Force }
    $requiredDependencies = @($manifest.Package.Dependencies.ChildNodes |
        Where-Object { $_.LocalName -eq 'PackageDependency' })
    $dependencyNames = @($requiredDependencies | ForEach-Object { $_.GetAttribute('Name') })
    $availableDependencies = @()
    if (Test-Path -LiteralPath $dependencySource -PathType Container) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        foreach ($file in @(Get-ChildItem -LiteralPath $dependencySource -Recurse -File |
            Where-Object { $_.Extension.ToLowerInvariant() -in @('.appx', '.msix') })) {
            # Dependency filenames do not necessarily match their manifest identity
            # (e.g. Microsoft.VCLibs.x64.14.00.appx vs Microsoft.VCLibs.140.00).
            $archive = [IO.Compression.ZipFile]::OpenRead($file.FullName)
            try {
                $entry = $archive.GetEntry('AppxManifest.xml')
                if (!$entry) { throw "Dependencia sem AppxManifest.xml: $($file.Name)" }
                $reader = [IO.StreamReader]::new($entry.Open())
                try { [xml]$dependencyManifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
                $dependencyIdentity = $dependencyManifest.Package.Identity
                if ($dependencyIdentity.ProcessorArchitecture -notin @('x64', 'neutral')) { continue }
                $availableDependencies += [pscustomobject]@{
                    Name = [string]$dependencyIdentity.Name
                    Publisher = [string]$dependencyIdentity.Publisher
                    Version = [version]$dependencyIdentity.Version
                    File = $file
                }
            } finally { $archive.Dispose() }
        }
    }
    foreach ($required in $requiredDependencies) {
        $name = $required.GetAttribute('Name')
        $minVersion = $required.GetAttribute('MinVersion')
        $publisher = $required.GetAttribute('Publisher')
        $matching = @($availableDependencies | Where-Object {
            $_.Name -eq $name -and (!$minVersion -or $_.Version -ge [version]$minVersion) -and
            (!$publisher -or $_.Publisher -eq $publisher)
        } | Sort-Object Version -Descending)
        if ($matching.Count -eq 0) { throw "Dependencia declarada sem pacote x64/neutral compativel: $name" }
        $target = Join-Path $dependencyTarget 'x64'
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        Copy-Item -LiteralPath $matching[0].File.FullName -Destination $target -Force
    }
    $metadata = [ordered]@{
        package = $packageName
        architecture = 'x64'
        flavor = $Flavor
        includesGameData = $false
        signed = $false
        version = [string]$identity.Version
        dependencies = $dependencyNames
        sha256 = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
    }
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDir 'package-info.json') -Encoding utf8
    @"
RondoRecomp UWP x64 - $Flavor
Este pacote de desenvolvimento nao inclui o disco de Rondo of Blood.
interpreter-bootstrap: base funcional com interpretador, sem codigo recompilado do jogo.
recompiled: inclui somente o codigo gerado localmente pelo rondo_gen.
Assine o MSIX com Sign-Uwp.ps1. O CER ao lado contem apenas o certificado publico.
No Xbox Dev Mode, envie o MSIX e as dependencias x64 pelo Xbox Device Portal.
Defina o tipo do aplicativo como Game e teste no console antes de distribuir.
Este artefato nao comprova que o aplicativo tenha sido testado no Xbox.
"@ | Set-Content -LiteralPath (Join-Path $OutputDir 'LEIA-ME.txt') -Encoding utf8
    [pscustomobject]@{ PackagePath = $packagePath; OutputDirectory = $OutputDir; Flavor = $Flavor }
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}
