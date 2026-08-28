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

# Phase 9: three cluster-node identities (CN=animus-node-1/2/3), signed by
# the same demo CA. Cluster nodes are peers, not a fixed client/server pair
# -- each one dials every other node AND accepts inbound connections from
# them -- so unlike server.pfx/client.pfx above, each node cert carries
# BOTH the Server Authentication and Client Authentication EKUs.
$nodeCerts = @()
foreach ($i in 1..3) {
    $cn = "animus-node-$i"
    Write-Host "Generating cluster node certificate (CN=$cn)..."
    $node = New-SelfSignedCertificate `
        -Subject "CN=$cn" `
        -DnsName "localhost" `
        -KeyUsage DigitalSignature, KeyEncipherment `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -Signer $ca `
        -NotAfter (Get-Date).AddYears(1) `
        -KeyExportPolicy Exportable `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.1,1.3.6.1.5.5.7.3.2") # EKU: Server + Client Authentication
    $nodeCerts += $node
}

Export-PfxCertificate -Cert $ca -FilePath (Join-Path $outDir "ca.pfx") -Password $pfxPassword | Out-Null
Export-Certificate -Cert $ca -FilePath (Join-Path $outDir "ca.cer") | Out-Null
Export-PfxCertificate -Cert $server -FilePath (Join-Path $outDir "server.pfx") -Password $pfxPassword | Out-Null
Export-PfxCertificate -Cert $client -FilePath (Join-Path $outDir "client.pfx") -Password $pfxPassword | Out-Null
for ($i = 0; $i -lt $nodeCerts.Count; $i++) {
    Export-PfxCertificate -Cert $nodeCerts[$i] -FilePath (Join-Path $outDir "node$($i+1).pfx") -Password $pfxPassword | Out-Null
}

# Remove the private-key copies left in the CurrentUser\My store -- the
# PFX exports above already captured everything the demo needs.
Remove-Item $ca.PSPath -Force
Remove-Item $server.PSPath -Force
Remove-Item $client.PSPath -Force
foreach ($node in $nodeCerts) { Remove-Item $node.PSPath -Force }

Write-Host ""
Write-Host "Demo certs written to $outDir :"
Write-Host "  ca.pfx / ca.cer   - demo CA (trust anchor)"
Write-Host "  server.pfx        - server identity (CN=animus-server)"
Write-Host "  client.pfx        - client identity (CN=animus-client-tenant-42)"
Write-Host "  node1/2/3.pfx     - cluster node identities (CN=animus-node-1/2/3)"
Write-Host "  PFX password      : AnimusDemoP@ss1"
