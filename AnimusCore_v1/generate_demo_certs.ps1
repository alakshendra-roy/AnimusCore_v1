# Generates a self-signed demo CA plus a server leaf certificate and a
# client leaf certificate (signed by that CA) for secure_transport_demo.cpp
# (Phase 8: mTLS / TLS 1.3 transport).
#
# DEMO / TEST USE ONLY. The private keys and the fixed PFX password below
# are not fit for any real deployment -- generate_demo_certs.ps1 exists so
# secure_transport_demo.cpp has a real, verifiable mutual-TLS chain to
# negotiate against, nothing more. Uses only the native Windows PKI
# cmdlets (New-SelfSignedCertificate, Export-PfxCertificate,
# Export-Certificate) -- no external tooling.
$ErrorActionPreference = "Stop"

$outDir = Join-Path $PSScriptRoot "demo_certs"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$pfxPassword = ConvertTo-SecureString -String "AnimusDemoP@ss1" -Force -AsPlainText

Write-Host "Generating demo CA..."
$ca = New-SelfSignedCertificate `
    -Subject "CN=AnimusCore Demo CA" `
    -KeyUsage CertSign, CRLSign, DigitalSignature `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -NotAfter (Get-Date).AddYears(1) `
    -KeyExportPolicy Exportable `
    -KeyAlgorithm RSA -KeyLength 2048

Write-Host "Generating server leaf certificate (CN=animus-server)..."
$server = New-SelfSignedCertificate `
    -Subject "CN=animus-server" `
    -DnsName "localhost" `
    -KeyUsage DigitalSignature, KeyEncipherment `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -Signer $ca `
    -NotAfter (Get-Date).AddYears(1) `
    -KeyExportPolicy Exportable `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.1") # EKU: Server Authentication

Write-Host "Generating client leaf certificate (CN=animus-client-tenant-42)..."
$client = New-SelfSignedCertificate `
    -Subject "CN=animus-client-tenant-42" `
    -KeyUsage DigitalSignature `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -Signer $ca `
    -NotAfter (Get-Date).AddYears(1) `
    -KeyExportPolicy Exportable `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.2") # EKU: Client Authentication

Export-PfxCertificate -Cert $ca -FilePath (Join-Path $outDir "ca.pfx") -Password $pfxPassword | Out-Null
Export-Certificate -Cert $ca -FilePath (Join-Path $outDir "ca.cer") | Out-Null
Export-PfxCertificate -Cert $server -FilePath (Join-Path $outDir "server.pfx") -Password $pfxPassword | Out-Null
Export-PfxCertificate -Cert $client -FilePath (Join-Path $outDir "client.pfx") -Password $pfxPassword | Out-Null

# Remove the private-key copies left in the CurrentUser\My store -- the
# PFX exports above already captured everything the demo needs.
Remove-Item $ca.PSPath -Force
Remove-Item $server.PSPath -Force
Remove-Item $client.PSPath -Force

Write-Host ""
Write-Host "Demo certs written to $outDir :"
Write-Host "  ca.pfx / ca.cer   - demo CA (trust anchor)"
Write-Host "  server.pfx        - server identity (CN=animus-server)"
Write-Host "  client.pfx        - client identity (CN=animus-client-tenant-42)"
Write-Host "  PFX password      : AnimusDemoP@ss1"
