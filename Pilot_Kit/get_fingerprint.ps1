# Animus Core Pilot Kit -- machine fingerprint collector.
#
# Prints this machine's Animus Core hardware fingerprint: SHA-256(UTF-8
# bytes of the registry MachineGuid + 6 raw bytes of the machine's primary
# NIC MAC address) -- the exact same fingerprint animus_verify_license
# computes natively (AnimusCore_v1/animus_engine.cpp's
# compute_machine_fingerprint) and the same one
# AnimusCore_v1/license_tools/sign_license.ps1's Get-LocalFingerprintHex
# computes when signing a license (the two were cross-checked to produce
# byte-identical fingerprints for the same real machine before either was
# relied on -- see that file's comments).
#
# This is that same read-only computation, pulled into its own file with
# NO dependency on the RSA private key or any other part of this
# repository -- safe to hand to a pilot customer to run on their own
# machine. It reads only local machine identifiers (registry MachineGuid,
# network adapter info), makes no network call, and writes nothing to
# disk.
#
# Usage (run on the machine you want licensed, then send the printed
# fingerprint to your Animus Core contact -- they'll use it with
# generate_license.py to issue your evaluation license):
#   powershell -ExecutionPolicy Bypass -File .\get_fingerprint.ps1

$ErrorActionPreference = "Stop"

function Get-LocalFingerprintHex {
    $guid = (Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Cryptography" -Name "MachineGuid").MachineGuid

    # Must select the exact same MAC address animus_verify_license does on
    # the C++ side (animus_engine.cpp's compute_machine_fingerprint /
    # read_primary_mac) or a license issued from this fingerprint would
    # never verify, even on the correctly-licensed machine. Filtering to
    # genuine, manufacturer-assigned (universally administered) addresses
    # via the IEEE locally-administered-address bit (bit 1 of the first
    # octet: 0 = real burned-in address, 1 = derived/virtual/randomized)
    # matters because "first adapter" or "first Up adapter" is not a safe
    # selection rule -- a single machine can expose several virtual Wi-Fi
    # MACs (Wi-Fi Direct, hosted network) ahead of its real one. Then pick
    # the lexicographically smallest candidate for a deterministic choice
    # on multi-NIC machines, ignoring connect/disconnect status since
    # that's not stable hardware identity.
    $candidates = [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() | ForEach-Object {
        $bytes = $_.GetPhysicalAddress().GetAddressBytes()
        if ($bytes.Length -eq 6 -and ($bytes | Where-Object { $_ -ne 0 }) -and (($bytes[0] -band 0x02) -eq 0)) {
            [PSCustomObject]@{ Name = $_.Name; Bytes = $bytes; Hex = ($bytes | ForEach-Object { "{0:X2}" -f $_ }) -join "-" }
        }
    }
    if (-not $candidates) { throw "no universally-administered (real hardware) MAC address found on this machine" }
    $chosen = $candidates | Sort-Object { ($_.Bytes | ForEach-Object { "{0:D3}" -f $_ }) -join "" } | Select-Object -First 1
    $macBytes = $chosen.Bytes

    $guidBytes = [System.Text.Encoding]::UTF8.GetBytes($guid)
    $input = New-Object byte[] ($guidBytes.Length + 6)
    [Array]::Copy($guidBytes, $input, $guidBytes.Length)
    [Array]::Copy($macBytes, 0, $input, $guidBytes.Length, 6)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $hash = $sha256.ComputeHash($input)
    Write-Host "MachineGuid: $guid"
    Write-Host "MAC address: $($chosen.Hex) (adapter: $($chosen.Name))"
    return ($hash | ForEach-Object { "{0:x2}" -f $_ }) -join ""
}

$fingerprint = Get-LocalFingerprintHex
Write-Host ""
Write-Host "Your Animus Core hardware fingerprint (send this to your Animus Core contact):"
Write-Host $fingerprint
