# Issues one RSA-signed offline license file for animus_verify_license
# (proprietary-edition branch only). Run this on the machine that HOLDS
# license_tools/private/license_private.blob (see
# generate_license_keypair.ps1) -- normally the vendor's own machine, not
# a customer's. Regenerating the keypair invalidates every license this
# script has already issued (they're signed against the old public key),
# so treat the private key file as long-lived and back it up accordingly.
#
# Usage:
#   .\sign_license.ps1 -OutFile customer.lic -MaxCores 4
#   .\sign_license.ps1 -OutFile customer.lic -MaxCores 8 -FingerprintHex <64 hex chars from the customer's machine>
#   .\sign_license.ps1 -OutFile customer.lic -MaxCores 4 -ExpiresInDays 365
#
# With no -FingerprintHex, this computes and licenses THIS machine (the
# same MachineGuid + first physical MAC address animus_verify_license
# reads on the machine it runs on) -- convenient for local testing, not
# how a real customer license would normally be issued (you'd want their
# fingerprint, not yours).
param(
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][int]$MaxCores,
    [string]$FingerprintHex = "",
    [int]$ExpiresInDays = 0  # 0 = no expiry
)
$ErrorActionPreference = "Stop"

function Get-LocalFingerprintHex {
    $guid = (Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Cryptography" -Name "MachineGuid").MachineGuid

    # Must select the exact same MAC address animus_verify_license does on
    # the C++ side (animus_engine.cpp), or a real license would never
    # verify even on the correctly-licensed machine. Same rule, same
    # reasoning: [System.Net.NetworkInformation.NetworkInterface] surfaces
    # the identical raw adapter set GetAdaptersAddresses does (confirmed
    # empirically against the same machine before this was relied on) --
    # including Windows' virtual per-role Wi-Fi MACs (Wi-Fi Direct, hosted
    # network), which is exactly why "first adapter" or "first Up adapter"
    # is not a safe selection rule: this machine alone exposed three
    # different Wi-Fi MACs. Filter to genuine, manufacturer-assigned
    # (universally administered) addresses via the IEEE locally-
    # administered-address bit (bit 1 of the first octet is 0 for a real
    # burned-in address, 1 for anything derived/virtual/randomized --
    # Windows sets it deliberately on those precisely so they can be told
    # apart this way), then pick the lexicographically smallest candidate
    # for a deterministic choice on multi-NIC machines, ignoring
    # connect/disconnect status since that's not stable hardware identity.
    $candidates = [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() | ForEach-Object {
        $bytes = $_.GetPhysicalAddress().GetAddressBytes()
        if ($bytes.Length -eq 6 -and ($bytes | Where-Object { $_ -ne 0 }) -and (($bytes[0] -band 0x02) -eq 0)) {
            [PSCustomObject]@{ Name = $_.Name; Bytes = $bytes; Hex = ($bytes | ForEach-Object { "{0:X2}" -f $_ }) -join "-" }
        }
    }
    if (-not $candidates) { throw "no universally-administered (real hardware) MAC address found" }
    $chosen = $candidates | Sort-Object { ($_.Bytes | ForEach-Object { "{0:D3}" -f $_ }) -join "" } | Select-Object -First 1
    $macBytes = $chosen.Bytes

    $guidBytes = [System.Text.Encoding]::UTF8.GetBytes($guid)
    $input = New-Object byte[] ($guidBytes.Length + 6)
    [Array]::Copy($guidBytes, $input, $guidBytes.Length)
    [Array]::Copy($macBytes, 0, $input, $guidBytes.Length, 6)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $hash = $sha256.ComputeHash($input)
    Write-Host "Licensing this machine: MachineGuid=$guid MAC=$($chosen.Hex) (adapter: $($chosen.Name))"
    return ($hash | ForEach-Object { "{0:x2}" -f $_ }) -join ""
}

if ([string]::IsNullOrEmpty($FingerprintHex)) {
    $FingerprintHex = Get-LocalFingerprintHex
}
if ($FingerprintHex.Length -ne 64) {
    throw "FingerprintHex must be 64 hex characters (32-byte SHA-256), got $($FingerprintHex.Length)"
}
$fingerprintBytes = [byte[]]::new(32)
for ($i = 0; $i -lt 32; $i++) {
    $fingerprintBytes[$i] = [Convert]::ToByte($FingerprintHex.Substring($i * 2, 2), 16)
}

$privateKeyPath = Join-Path $PSScriptRoot "private\license_private.blob"
if (-not (Test-Path $privateKeyPath)) {
    throw "private key not found at $privateKeyPath -- run generate_license_keypair.ps1 first"
}
$privateBlob = [System.IO.File]::ReadAllBytes($privateKeyPath)

# Build the 64-byte LicensePayload -- field order, sizes, and the 4 bytes
# of trailing padding must match animus::LicensePayload (animus.hpp)
# exactly; this layout was verified against a real sizeof()/offsetof()
# build before being relied on here, not just assumed from the struct
# definition.
$magic = 0x434C4E41  # 'ANLC' -- matches animus::kLicenseMagic
$version = 1
$issuedAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$expiresAt = if ($ExpiresInDays -gt 0) { $issuedAt + ($ExpiresInDays * 86400) } else { 0 }

$payload = New-Object byte[] 64
[Array]::Copy([BitConverter]::GetBytes([uint32]$magic), 0, $payload, 0, 4)
[Array]::Copy([BitConverter]::GetBytes([uint32]$version), 0, $payload, 4, 4)
[Array]::Copy([BitConverter]::GetBytes([uint64]$issuedAt), 0, $payload, 8, 8)
[Array]::Copy([BitConverter]::GetBytes([uint64]$expiresAt), 0, $payload, 16, 8)
[Array]::Copy([BitConverter]::GetBytes([uint32]$MaxCores), 0, $payload, 24, 4)
[Array]::Copy($fingerprintBytes, 0, $payload, 28, 32)
# bytes 60-63 are the struct's trailing alignment padding -- left zeroed.

$key = [System.Security.Cryptography.CngKey]::Import($privateBlob, [System.Security.Cryptography.CngKeyBlobFormat]::GenericPrivateBlob)
$rsa = [System.Security.Cryptography.RSACng]::new($key)
$signature = $rsa.SignData($payload, [System.Security.Cryptography.HashAlgorithmName]::SHA256, [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
$rsa.Dispose()

$licenseFile = $payload + $signature  # [64-byte payload][256-byte RSA-2048 signature] = 320 bytes
[System.IO.File]::WriteAllBytes($OutFile, $licenseFile)

Write-Host "License written to $OutFile (max_cores=$MaxCores, expires=$(if ($ExpiresInDays -gt 0) { "$ExpiresInDays days" } else { "never" }), fingerprint=$FingerprintHex)"
