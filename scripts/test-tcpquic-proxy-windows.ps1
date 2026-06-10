param(
  [string]$Bin = ".\build-x64\bin\Release\tcpquic-proxy.exe",
  [string]$Compress = "off",
  [string]$QuicPeerHost = "127.0.0.1",
  [switch]$Trace
)

$ErrorActionPreference = "Stop"

function Wait-TcpPort {
  param(
    [string]$HostName,
    [int]$Port,
    [int]$TimeoutSeconds = 10
  )

  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  while ((Get-Date) -lt $deadline) {
    try {
      $client = [System.Net.Sockets.TcpClient]::new()
      $async = $client.BeginConnect($HostName, $Port, $null, $null)
      if ($async.AsyncWaitHandle.WaitOne(200)) {
        $client.EndConnect($async)
        $client.Close()
        return $true
      }
      $client.Close()
    } catch {
    }
    Start-Sleep -Milliseconds 200
  }
  return $false
}

function Assert-FileContains {
  param(
    [string]$Path,
    [string]$Pattern,
    [string]$Description
  )

  if (-not (Test-Path $Path)) {
    throw "$Description missing: $Path"
  }
  $text = Get-Content -Raw $Path
  if ($text -notmatch $Pattern) {
    throw "$Description missing pattern '$Pattern' in $Path`n$text"
  }
}

function Resolve-OpenSslPath {
  if ($env:OPENSSL_BIN -and (Test-Path (Join-Path $env:OPENSSL_BIN "openssl.exe"))) {
    return $env:OPENSSL_BIN
  }
  foreach ($candidate in @(
    "C:\Program Files\OpenSSL-Win64\bin",
    "C:\Program Files\Git\usr\bin"
  )) {
    if (Test-Path (Join-Path $candidate "openssl.exe")) {
      return $candidate
    }
  }
  throw "openssl not found; install OpenSSL or set OPENSSL_BIN to its bin directory"
}

function Invoke-OpenSsl {
  param([string[]]$OpenSslArgs)
  $prev = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    & openssl @OpenSslArgs 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "openssl failed (exit $LASTEXITCODE): openssl $($OpenSslArgs -join ' ')" }
  } finally {
    $ErrorActionPreference = $prev
  }
}

function New-OpenSslCredentials {
  param([string]$Work)

  $OpenSslBin = Resolve-OpenSslPath
  $env:PATH = "$OpenSslBin;$env:PATH"

  $CaKey = Join-Path $Work "ca.key"
  $CaCrt = Join-Path $Work "ca.crt"
  $CaCsr = Join-Path $Work "ca.csr"
  $ServerKey = Join-Path $Work "server.key"
  $ServerCsr = Join-Path $Work "server.csr"
  $ServerCrt = Join-Path $Work "server.crt"
  $ClientKey = Join-Path $Work "client.key"
  $ClientCsr = Join-Path $Work "client.csr"
  $ClientCrt = Join-Path $Work "client.crt"
  $CaExtCfg = Join-Path $Work "ca.ext"
  $ServerExtCfg = Join-Path $Work "server.ext"
  $ClientExtCfg = Join-Path $Work "client.ext"

  @"
[v3_ca]
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid:always,issuer
"@ | Set-Content -Path $CaExtCfg -Encoding ascii

  @"
[v3_server]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=@alt_names

[alt_names]
DNS.1=localhost
IP.1=127.0.0.1
"@ | Set-Content -Path $ServerExtCfg -Encoding ascii

  @"
[v3_client]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
subjectAltName=@alt_names

[alt_names]
DNS.1=tcpquic-client
"@ | Set-Content -Path $ClientExtCfg -Encoding ascii

  Invoke-OpenSsl @('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=tcpquic-test-ca', '-keyout', $CaKey, '-out', $CaCsr)
  Invoke-OpenSsl @('x509', '-req', '-in', $CaCsr, '-signkey', $CaKey, '-days', '1', '-out', $CaCrt, '-extfile', $CaExtCfg, '-extensions', 'v3_ca')
  Invoke-OpenSsl @('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost', '-keyout', $ServerKey, '-out', $ServerCsr)
  Invoke-OpenSsl @('x509', '-req', '-in', $ServerCsr, '-CA', $CaCrt, '-CAkey', $CaKey, '-CAcreateserial', '-days', '1', '-out', $ServerCrt, '-extfile', $ServerExtCfg, '-extensions', 'v3_server')
  Invoke-OpenSsl @('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=tcpquic-client', '-keyout', $ClientKey, '-out', $ClientCsr)
  Invoke-OpenSsl @('x509', '-req', '-in', $ClientCsr, '-CA', $CaCrt, '-CAkey', $CaKey, '-CAcreateserial', '-days', '1', '-out', $ClientCrt, '-extfile', $ClientExtCfg, '-extensions', 'v3_client')

  return [pscustomobject]@{
    ServerCert = $ServerCrt
    ServerKey = $ServerKey
    ClientCert = $ClientCrt
    ClientKey = $ClientKey
    Ca = $CaCrt
  }
}

$Work = Join-Path $env:TEMP ("tcpquic-win-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Work | Out-Null

try {
  $Creds = New-OpenSslCredentials -Work $Work
  $ServerCert = $Creds.ServerCert
  $ServerKey = $Creds.ServerKey
  $ClientCert = $Creds.ClientCert
  $ClientKey = $Creds.ClientKey
  $Ca = $Creds.Ca

  $HttpPort = 18080
  $QuicPort = 18443
  $SocksPort = 11080
  $ConnectPort = 18081

  $Http = Start-Process -PassThru python -ArgumentList "-m", "http.server", "$HttpPort", "--bind", "127.0.0.1"
  Start-Sleep -Seconds 1

  $ServerErr = Join-Path $Work "server.err"
  $ClientErr = Join-Path $Work "client.err"

  $BinPath = (Resolve-Path $Bin).Path
  $BinDir = Split-Path -Parent $BinPath
  $LogDir = Join-Path $BinDir "log"
  $ServerLog = Join-Path $LogDir "server.log"
  $ClientLog = Join-Path $LogDir "client.log"
  if ($Trace -and (Test-Path $LogDir)) {
    Remove-Item -Recurse -Force $LogDir
  }

  $ServerArgs = @(
    "server",
    "--quic-listen", "127.0.0.1:$QuicPort",
    "--quic-cert", $ServerCert,
    "--quic-key", $ServerKey,
    "--quic-ca", $Ca,
    "--allow-targets", "127.0.0.0/8",
    "--compress", $Compress
  )
  if ($Trace) {
    $ServerArgs += @("--trace", "--trace-interval", "1")
  }
  $Server = Start-Process -PassThru -FilePath $BinPath -ArgumentList $ServerArgs -RedirectStandardError $ServerErr
  Start-Sleep -Seconds 1
  if ($Server.HasExited) {
    $errText = if (Test-Path $ServerErr) { Get-Content -Raw $ServerErr } else { "" }
    throw "server exited during startup with code $($Server.ExitCode): $errText"
  }

  $ClientArgs = @(
    "client",
    "--quic-peer", "${QuicPeerHost}:$QuicPort",
    "--quic-cert", $ClientCert,
    "--quic-key", $ClientKey,
    "--quic-ca", $Ca,
    "--socks-listen", "127.0.0.1:$SocksPort",
    "--http-listen", "127.0.0.1:$ConnectPort",
    "--compress", $Compress
  )
  if ($Trace) {
    $ClientArgs += @("--trace", "--trace-interval", "1", "--trace-connect-on-start")
  }
  $Client = Start-Process -PassThru -FilePath $BinPath -ArgumentList $ClientArgs -RedirectStandardError $ClientErr
  Start-Sleep -Seconds 1
  if ($Client.HasExited) {
    $errText = if (Test-Path $ClientErr) { Get-Content -Raw $ClientErr } else { "" }
    throw "client exited during startup with code $($Client.ExitCode): $errText"
  }
  if (-not (Wait-TcpPort -HostName "127.0.0.1" -Port $ConnectPort -TimeoutSeconds 10)) {
    $errText = if (Test-Path $ClientErr) { Get-Content -Raw $ClientErr } else { "" }
    throw "client HTTP CONNECT listener did not become ready: $errText"
  }
  if (-not (Wait-TcpPort -HostName "127.0.0.1" -Port $SocksPort -TimeoutSeconds 10)) {
    $errText = if (Test-Path $ClientErr) { Get-Content -Raw $ClientErr } else { "" }
    throw "client SOCKS5 listener did not become ready: $errText"
  }

  curl.exe --max-time 30 -f -x "http://127.0.0.1:$ConnectPort" --proxytunnel "http://127.0.0.1:$HttpPort/" | Out-Null
  if ($LASTEXITCODE -ne 0) {
    $serverText = if (Test-Path $ServerErr) { Get-Content -Raw $ServerErr } else { "" }
    $clientText = if (Test-Path $ClientErr) { Get-Content -Raw $ClientErr } else { "" }
    throw "HTTP CONNECT loopback failed with exit code $LASTEXITCODE`nSERVER:`n$serverText`nCLIENT:`n$clientText"
  }
  curl.exe --max-time 30 -f --socks5-hostname "127.0.0.1:$SocksPort" "http://127.0.0.1:$HttpPort/" | Out-Null
  if ($LASTEXITCODE -ne 0) {
    $serverText = if (Test-Path $ServerErr) { Get-Content -Raw $ServerErr } else { "" }
    $clientText = if (Test-Path $ClientErr) { Get-Content -Raw $ClientErr } else { "" }
    throw "SOCKS5 loopback failed with exit code $LASTEXITCODE`nSERVER:`n$serverText`nCLIENT:`n$clientText"
  }

  if ($Trace) {
    Start-Sleep -Seconds 2
    Assert-FileContains -Path $ClientLog -Pattern "event=trace_started role=client" -Description "client trace startup log"
    Assert-FileContains -Path $ClientLog -Pattern "event=quic_connected role=client" -Description "client QUIC connected log"
    Assert-FileContains -Path $ClientLog -Pattern "event=proxy_tunnel_ok proto=http" -Description "client HTTP tunnel log"
    Assert-FileContains -Path $ClientLog -Pattern "event=proxy_tunnel_ok proto=socks5" -Description "client SOCKS5 tunnel log"
    Assert-FileContains -Path $ServerLog -Pattern "event=trace_started role=server" -Description "server trace startup log"
    Assert-FileContains -Path $ServerLog -Pattern "event=quic_connected role=server" -Description "server QUIC connected log"
    Assert-FileContains -Path $ServerLog -Pattern "event=target_tcp_connected" -Description "server target TCP connected log"
    Assert-FileContains -Path $ServerLog -Pattern "event=stats_tick" -Description "server periodic stats log"
  }
}
finally {
  if ($Client) { Stop-Process -Id $Client.Id -Force -ErrorAction SilentlyContinue }
  if ($Server) { Stop-Process -Id $Server.Id -Force -ErrorAction SilentlyContinue }
  if ($Http) { Stop-Process -Id $Http.Id -Force -ErrorAction SilentlyContinue }
  Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
}
