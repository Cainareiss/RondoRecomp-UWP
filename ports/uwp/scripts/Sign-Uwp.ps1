[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackagePath,
    [string]$CertificateThumbprint,
    [switch]$VerifyWithTemporaryTrust
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'Execute este script no Windows.' }
$PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
if ([IO.Path]::GetExtension($PackagePath).ToLowerInvariant() -notin @('.msix', '.appx')) {
    throw 'Informe o .msix ou .appx x64 gerado por Build-Uwp.ps1.'
}
$subject = 'CN=RondoRecomp.UWP.Dev'
if ($CertificateThumbprint) {
    $certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
} else {
    # The private key stays in the current user's Windows certificate store.
    $certificate = New-SelfSignedCertificate -Type Custom -Subject $subject `
        -FriendlyName 'RondoRecomp UWP local development' -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable `
        -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(1) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}
if ($certificate.Subject -ne $subject -or !$certificate.HasPrivateKey) {
    throw "O certificado precisa conter chave privada e Subject exatamente $subject."
}
$sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$signTool = Join-Path $sdkRoot 'bin/10.0.22621.0/x64/signtool.exe'
if (!(Test-Path -LiteralPath $signTool)) { throw 'Instale Windows SDK 10.0.22621.0 (SignTool).' }
& $signTool sign /fd SHA256 /sha1 $certificate.Thumbprint /s My $PackagePath | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'SignTool nao conseguiu assinar o pacote.' }
$publicCertificate = [IO.Path]::ChangeExtension($PackagePath, '.cer')
Export-Certificate -Cert $certificate -FilePath $publicCertificate -Force | Out-Null
$temporaryTrustPath = "Cert:\CurrentUser\TrustedPeople\$($certificate.Thumbprint)"
$addedTemporaryTrust = $false
try {
    # Opt in only on a disposable CI runner. Local use never changes trust by default.
    if ($VerifyWithTemporaryTrust -and !(Test-Path -LiteralPath $temporaryTrustPath)) {
        Import-Certificate -FilePath $publicCertificate -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' | Out-Null
        $addedTemporaryTrust = $true
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $PackagePath
    if (!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint -or
        $signature.Status -in @('HashMismatch', 'NotSigned', 'NotSupportedFileFormat', 'Incompatible')) {
        throw "A verificacao da assinatura falhou: $($signature.Status)."
    }
    if ($VerifyWithTemporaryTrust -or $signature.Status -eq 'Valid') {
        & $signTool verify /pa /all /v $PackagePath | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'SignTool rejeitou a assinatura ou a cadeia do certificado.' }
    } else {
        Write-Host "Assinatura criada com o certificado esperado; confianca local nao estabelecida ($($signature.Status))."
    }
} finally {
    if ($addedTemporaryTrust) { Remove-Item -LiteralPath $temporaryTrustPath -Force }
}
$metadataPath = Join-Path (Split-Path -Parent $PackagePath) 'package-info.json'
if (Test-Path -LiteralPath $metadataPath) {
    $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
    if ($metadata.package -eq [IO.Path]::GetFileName($PackagePath)) {
        $metadata.signed = $true
        $metadata.sha256 = (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash
        $metadata | Add-Member -NotePropertyName certificateThumbprint -NotePropertyValue $certificate.Thumbprint -Force
        $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $metadataPath -Encoding utf8
    }
}
Write-Host "Pacote assinado: $PackagePath"
Write-Host "Certificado publico: $publicCertificate"
Write-Host "Thumbprint para reutilizar: $($certificate.Thumbprint)"
Write-Host 'A chave privada permanece no repositorio de certificados do usuario; somente o CER publico foi exportado.'
