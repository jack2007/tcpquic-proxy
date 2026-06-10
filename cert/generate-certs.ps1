# Generate 10-year internal mTLS test certificates for tcpquic-proxy.
#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

$CertRoot = $PSScriptRoot
$Days = if ($env:CERT_DAYS) { [int]$env:CERT_DAYS } else { 3650 }

function Require-OpenSsl {
    if (-not (Get-Command openssl -ErrorAction SilentlyContinue)) {
        throw 'openssl not found in PATH. Install OpenSSL or use Git for Windows openssl.exe.'
    }
}

function Write-Log([string]$Message) {
    Write-Host "[generate-certs] $Message"
}

function Invoke-OpenSsl {
    param([string[]]$Args)
    & openssl @Args
    if ($LASTEXITCODE -ne 0) {
        throw "openssl failed: openssl $($Args -join ' ')"
    }
}

Require-OpenSsl
New-Item -ItemType Directory -Force -Path (Join-Path $CertRoot 'server') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $CertRoot 'client') | Out-Null

Write-Log "generating CA ($Days days) in $CertRoot"
Invoke-OpenSsl @(
    'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
    '-keyout', (Join-Path $CertRoot 'ca.key'),
    '-out', (Join-Path $CertRoot 'ca.crt'),
    '-subj', '/CN=tcpquic-internal-test-ca/O=tcpquic-proxy/C=CN',
    '-days', "$Days", '-sha256'
)

@'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,DNS:tcpquic-server,IP:127.0.0.1,IP:169.254.250.230,IP:169.254.59.196
'@ | Set-Content -Encoding ascii (Join-Path $CertRoot 'server\server.ext')

@'
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=DNS:localhost,DNS:tcpquic-client,IP:127.0.0.1
'@ | Set-Content -Encoding ascii (Join-Path $CertRoot 'client\client.ext')

Write-Log 'generating server leaf certificate'
Invoke-OpenSsl @(
    'req', '-newkey', 'rsa:2048', '-nodes',
    '-keyout', (Join-Path $CertRoot 'server\server.key'),
    '-out', (Join-Path $CertRoot 'server\server.csr'),
    '-subj', '/CN=tcpquic-server/O=tcpquic-proxy/C=CN'
)
Invoke-OpenSsl @(
    'x509', '-req',
    '-in', (Join-Path $CertRoot 'server\server.csr'),
    '-CA', (Join-Path $CertRoot 'ca.crt'),
    '-CAkey', (Join-Path $CertRoot 'ca.key'),
    '-CAcreateserial',
    '-out', (Join-Path $CertRoot 'server\server.crt'),
    '-days', "$Days", '-sha256',
    '-extfile', (Join-Path $CertRoot 'server\server.ext')
)

Write-Log 'generating client leaf certificate'
Invoke-OpenSsl @(
    'req', '-newkey', 'rsa:2048', '-nodes',
    '-keyout', (Join-Path $CertRoot 'client\client.key'),
    '-out', (Join-Path $CertRoot 'client\client.csr'),
    '-subj', '/CN=tcpquic-client/O=tcpquic-proxy/C=CN'
)
Invoke-OpenSsl @(
    'x509', '-req',
    '-in', (Join-Path $CertRoot 'client\client.csr'),
    '-CA', (Join-Path $CertRoot 'ca.crt'),
    '-CAkey', (Join-Path $CertRoot 'ca.key'),
    '-CAcreateserial',
    '-out', (Join-Path $CertRoot 'client\client.crt'),
    '-days', "$Days", '-sha256',
    '-extfile', (Join-Path $CertRoot 'client\client.ext')
)

Remove-Item -Force -ErrorAction SilentlyContinue `
    (Join-Path $CertRoot 'server\server.csr'),
    (Join-Path $CertRoot 'server\server.ext'),
    (Join-Path $CertRoot 'client\client.csr'),
    (Join-Path $CertRoot 'client\client.ext'),
    (Join-Path $CertRoot 'ca.srl')

Invoke-OpenSsl @('verify', '-CAfile', (Join-Path $CertRoot 'ca.crt'), (Join-Path $CertRoot 'server\server.crt')) | Out-Null
Invoke-OpenSsl @('verify', '-CAfile', (Join-Path $CertRoot 'ca.crt'), (Join-Path $CertRoot 'client\client.crt')) | Out-Null

Write-Log 'done'
Write-Log "  CA:     $(Join-Path $CertRoot 'ca.crt')"
Write-Log "  server: $(Join-Path $CertRoot 'server\server.crt')"
Write-Log "  client: $(Join-Path $CertRoot 'client\client.crt')"
