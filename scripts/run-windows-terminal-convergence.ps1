# Windows terminal / zero-FIN ownership convergence runner.
# Inspired by scripts/run-linux-terminal-convergence.sh (PowerShell-native).
#
# Exit codes:
#   0  - SelfTest / scenario gates passed
#   2  - usage / bad arguments
#   66 - binary or config missing (caller-supplied path)
#   69 - BLOCKED prerequisites (no binary / harness not fully wired)
#   70 - process failed to become ready
#   1  - gate assertion failed

[CmdletBinding()]
param(
    [ValidateSet('baseline', 'soak')]
    [string]$Scenario = '',

    [switch]$SelfTest,

    [int]$SoakSeconds = 1800,

    [string]$Binary = '',

    [string]$ClientConfig = '',

    [string]$ServerConfig = '',

    [string]$Out = '',

    [int]$DrainSeconds = 30
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Out) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $Out = Join-Path $Root ("docs/test/windows-terminal-convergence-{0}-{1}" -f $Scenario, $stamp)
}

function Write-Usage {
    Write-Host @"
usage: $($MyInvocation.MyCommand.Name) -SelfTest
       $($MyInvocation.MyCommand.Name) -Scenario baseline|soak [-SoakSeconds N] [-Binary PATH] [-ClientConfig PATH] [-ServerConfig PATH] [-Out DIR]
"@
}

function Get-RelayMetricInt64 {
    param(
        $Relay,
        [string]$Name
    )
    if ($null -eq $Relay -or $null -eq $Relay.PSObject.Properties[$Name]) {
        return [int64]0
    }
    return [int64]$Relay.$Name
}

function Assert-ZeroFinReleaseGate {
    param(
        [Parameter(Mandatory = $true)]
        $Relay,

        [switch]$RequireNewCompletionFields
    )

    if ($RequireNewCompletionFields) {
        foreach ($name in @(
                'windows_relay_receive_completion_pending',
                'windows_relay_receive_completion_exactly_once_violation',
                'windows_relay_receive_completion_required'
            )) {
            if ($null -eq $Relay.PSObject.Properties[$name]) {
                throw "missing required relay metric field: $name (rebuild proxy with Task 5 counters)"
            }
        }
    }

    $pending = Get-RelayMetricInt64 $Relay 'windows_relay_receive_completion_pending'
    $violation = Get-RelayMetricInt64 $Relay 'windows_relay_receive_completion_exactly_once_violation'
    $retained = Get-RelayMetricInt64 $Relay 'windows_relay_terminal_retained_owner_count'
    $sinkPending = Get-RelayMetricInt64 $Relay 'windows_relay_terminal_shutdown_sink_pending_count'
    $terminalRetained = Get-RelayMetricInt64 $Relay 'terminal_retained_owner_count'
    $terminalSink = Get-RelayMetricInt64 $Relay 'terminal_sink_pending'

    if ($terminalRetained -ne 0 -or
        $terminalSink -ne 0 -or
        $retained -ne 0 -or
        $sinkPending -ne 0 -or
        $pending -ne 0 -or
        $violation -ne 0) {
        throw ("Windows zero-FIN release gate failed: " +
            "terminal_retained_owner_count=$terminalRetained " +
            "terminal_sink_pending=$terminalSink " +
            "windows_relay_terminal_retained_owner_count=$retained " +
            "windows_relay_terminal_shutdown_sink_pending_count=$sinkPending " +
            "windows_relay_receive_completion_pending=$pending " +
            "windows_relay_receive_completion_exactly_once_violation=$violation")
    }
}

function Assert-FinActivityGate {
    param(
        [Parameter(Mandatory = $true)]$ClientBefore,
        [Parameter(Mandatory = $true)]$ServerBefore,
        [Parameter(Mandatory = $true)]$ClientAfter,
        [Parameter(Mandatory = $true)]$ServerAfter,
        [Parameter(Mandatory = $true)][int]$CurlAttempts,
        [Parameter(Mandatory = $true)][int]$CurlSuccesses,
        [double]$MinSuccessRate = 0.5
    )

    if ($CurlAttempts -le 0) {
        throw 'Windows FIN activity gate failed: no curl attempts were recorded'
    }
    if ($CurlSuccesses -le 0) {
        throw ("Windows FIN activity gate failed: zero successful curls " +
            "(successes=$CurlSuccesses attempts=$CurlAttempts)")
    }
    $rate = $CurlSuccesses / [double]$CurlAttempts
    if ($rate -lt $MinSuccessRate) {
        throw ("Windows FIN activity gate failed: curl success rate {0:N2} < {1} ({2}/{3})" -f
            $rate, $MinSuccessRate, $CurlSuccesses, $CurlAttempts)
    }

    $clientRequiredDelta =
        (Get-RelayMetricInt64 $ClientAfter 'windows_relay_receive_completion_required') -
        (Get-RelayMetricInt64 $ClientBefore 'windows_relay_receive_completion_required')
    $serverRequiredDelta =
        (Get-RelayMetricInt64 $ServerAfter 'windows_relay_receive_completion_required') -
        (Get-RelayMetricInt64 $ServerBefore 'windows_relay_receive_completion_required')
    $clientZeroLength = Get-RelayMetricInt64 $ClientAfter 'windows_relay_receive_completion_zero_length'
    $serverZeroLength = Get-RelayMetricInt64 $ServerAfter 'windows_relay_receive_completion_zero_length'

    if ($clientRequiredDelta -le 0 -and
        $serverRequiredDelta -le 0 -and
        $clientZeroLength -le 0 -and
        $serverZeroLength -le 0) {
        throw ("Windows FIN activity gate failed: no receive-completion activity " +
            "(client_required_delta=$clientRequiredDelta server_required_delta=$serverRequiredDelta " +
            "client_zero_length=$clientZeroLength server_zero_length=$serverZeroLength)")
    }
}

function Invoke-SelfTest {
    # Validate gate helper against fixture JSON (ConvertFrom-Json).
    $good = @'
{
  "active_relays": 0,
  "terminal_sink_pending": 0,
  "terminal_retained_owner_count": 0,
  "terminal_exactly_once_violation": 0,
  "windows_relay_terminal_retained_owner_count": 0,
  "windows_relay_terminal_shutdown_sink_pending_count": 0,
  "windows_relay_receive_completion_pending": 0,
  "windows_relay_receive_completion_exactly_once_violation": 0,
  "windows_relay_receive_completion_required": 3,
  "windows_relay_receive_completion_active_completed": 2,
  "windows_relay_receive_completion_terminal_discarded": 1,
  "windows_relay_receive_completion_zero_length": 1
}
'@ | ConvertFrom-Json
    Assert-ZeroFinReleaseGate -Relay $good

    $badPending = $good | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badPending.windows_relay_receive_completion_pending = 1
    $failed = $false
    try {
        Assert-ZeroFinReleaseGate -Relay $badPending
    } catch {
        $failed = $true
    }
    if (-not $failed) {
        throw 'self-test accepted non-zero windows_relay_receive_completion_pending'
    }

    $badViolation = $good | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badViolation.windows_relay_receive_completion_exactly_once_violation = 1
    $failed = $false
    try {
        Assert-ZeroFinReleaseGate -Relay $badViolation
    } catch {
        $failed = $true
    }
    if (-not $failed) {
        throw 'self-test accepted non-zero windows_relay_receive_completion_exactly_once_violation'
    }

    $badRetained = $good | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $badRetained.terminal_retained_owner_count = 1
    $failed = $false
    try {
        Assert-ZeroFinReleaseGate -Relay $badRetained
    } catch {
        $failed = $true
    }
    if (-not $failed) {
        throw 'self-test accepted non-zero terminal_retained_owner_count'
    }

    $idle = @'
{
  "windows_relay_receive_completion_required": 0,
  "windows_relay_receive_completion_zero_length": 0
}
'@ | ConvertFrom-Json
    $failed = $false
    try {
        Assert-FinActivityGate -ClientBefore $idle -ServerBefore $idle `
            -ClientAfter $idle -ServerAfter $idle -CurlAttempts 4 -CurlSuccesses 4
    } catch {
        $failed = $true
    }
    if (-not $failed) {
        throw 'self-test accepted idle metrics with no FIN/receive activity'
    }

    $failed = $false
    try {
        Assert-FinActivityGate -ClientBefore $idle -ServerBefore $idle `
            -ClientAfter $good -ServerAfter $idle -CurlAttempts 4 -CurlSuccesses 0
    } catch {
        $failed = $true
    }
    if (-not $failed) {
        throw 'self-test accepted zero successful curls'
    }

    $failed = $false
    try {
        Assert-FinActivityGate -ClientBefore $idle -ServerBefore $idle `
            -ClientAfter $good -ServerAfter $idle -CurlAttempts 10 -CurlSuccesses 4
    } catch {
        $failed = $true
    }
    if (-not $failed) {
        throw 'self-test accepted curl success rate below 50%'
    }

    Assert-FinActivityGate -ClientBefore $idle -ServerBefore $idle `
        -ClientAfter $good -ServerAfter $idle -CurlAttempts 4 -CurlSuccesses 3

    # Parse syntax of this script by re-tokenizing.
    $null = [System.Management.Automation.Language.Parser]::ParseFile(
        $PSCommandPath,
        [ref]$null,
        [ref]$null)

    Write-Host 'windows terminal convergence runner self-test passed'
}

function Resolve-OpenSslBin {
    if ($env:OPENSSL_BIN -and (Test-Path (Join-Path $env:OPENSSL_BIN 'openssl.exe'))) {
        return $env:OPENSSL_BIN
    }
    foreach ($candidate in @(
            (Join-Path $Root 'build-x64\bin\Release'),
            'C:\Program Files\OpenSSL-Win64\bin',
            'C:\Program Files\Git\usr\bin'
        )) {
        if (Test-Path (Join-Path $candidate 'openssl.exe')) {
            return $candidate
        }
    }
    return $null
}

function Invoke-OpenSsl {
    param([string[]]$OpenSslArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & openssl @OpenSslArgs 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "openssl failed (exit $LASTEXITCODE): openssl $($OpenSslArgs -join ' ')"
        }
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Resolve-ProxyBinary {
    param([string]$Preferred)
    if ($Preferred -and (Test-Path -LiteralPath $Preferred)) {
        return (Resolve-Path -LiteralPath $Preferred).Path
    }
    if ($env:TCPQUIC_BINARY -and (Test-Path -LiteralPath $env:TCPQUIC_BINARY)) {
        return (Resolve-Path -LiteralPath $env:TCPQUIC_BINARY).Path
    }
    foreach ($candidate in @(
            (Join-Path $Root 'build-x64\bin\Release\raypx2.exe'),
            (Join-Path $Root 'build-x64\bin\Release\tcpquic-proxy.exe'),
            (Join-Path $Root 'build\bin\Release\raypx2.exe'),
            (Join-Path $Root 'build\bin\Release\tcpquic-proxy.exe')
        )) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Invoke-CurlAttempt {
    param([Parameter(Mandatory = $true)][string[]]$CurlArgs)
    # Prefer HTTP status over curl exit code: tunnel FIN/RST often yields
    # curl exit 56 after a successful 200 response body.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $argsWithStatus = @('-sS', '-o', 'NUL', '-w', '%{http_code}') + $CurlArgs
        $code = (& curl.exe @argsWithStatus 2>$null | Out-String).Trim()
        return ($code -eq '200')
    } catch {
        return $false
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    return $port
}

function Wait-TcpPort {
    param(
        [string]$HostName,
        [int]$Port,
        [int]$TimeoutSeconds = 15
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

function Get-AdminMetrics {
    param(
        [int]$Port,
        [string]$Token
    )
    $uri = "http://127.0.0.1:$Port/api/v1/relay/metrics"
    $headers = @{ Authorization = "Bearer $Token" }
    return Invoke-RestMethod -Uri $uri -Headers $headers -TimeoutSec 5
}

function Write-EvidenceNote {
    param(
        [string]$Path,
        [string]$Text
    )
    Set-Content -LiteralPath $Path -Value $Text -Encoding utf8
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

if (-not $Scenario) {
    Write-Usage
    exit 2
}

$binPath = Resolve-ProxyBinary -Preferred $Binary
$openSslBin = Resolve-OpenSslBin
New-Item -ItemType Directory -Force -Path $Out | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Out 'logs') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Out 'metrics') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Out 'summary') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Out 'config') | Out-Null

try {
    git -C $Root rev-parse HEAD | Set-Content -LiteralPath (Join-Path $Out 'git-sha.txt')
} catch {
    Set-Content -LiteralPath (Join-Path $Out 'git-sha.txt') -Value 'unknown'
}

if (-not $binPath) {
    $msg = @"
performance release BLOCKED: proxy binary missing under build-x64/bin/Release (raypx2.exe or tcpquic-proxy.exe).
SelfTest remains available via -SelfTest. Scenario=$Scenario was not executed.
"@
    Write-EvidenceNote -Path (Join-Path $Out 'BLOCKED.txt') -Text $msg
    Write-Error $msg
    exit 69
}

if (-not $openSslBin) {
    $msg = @"
performance release BLOCKED: openssl.exe not found (set OPENSSL_BIN or install OpenSSL / use build-x64\bin\Release\openssl.exe).
Binary found: $binPath
Scenario=$Scenario was not executed.
"@
    Write-EvidenceNote -Path (Join-Path $Out 'BLOCKED.txt') -Text $msg
    Write-Error $msg
    exit 69
}
$env:PATH = "$openSslBin;$env:PATH"
# Prefer local openssl.cnf next to the binary when present; clear broken OPENSSL_CONF.
$localConf = Join-Path $openSslBin 'openssl.cnf'
if (Test-Path -LiteralPath $localConf) {
    $env:OPENSSL_CONF = $localConf
} else {
    Remove-Item Env:OPENSSL_CONF -ErrorAction SilentlyContinue
}

$work = Join-Path $Out 'work'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$tokenBytes = New-Object byte[] 32
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($tokenBytes)
$token = -join ($tokenBytes | ForEach-Object { $_.ToString('x2') })
$tokenFile = Join-Path $Out 'admin-token.json'
@"
{"version":1,"token_type":"Bearer","token":"$token","listen":"runner"}
"@ | Set-Content -LiteralPath $tokenFile -Encoding ascii

$serverAdminPort = Get-FreeTcpPort
$clientAdminPort = Get-FreeTcpPort
$quicPort = Get-FreeTcpPort
$httpListenPort = Get-FreeTcpPort
$targetPort = Get-FreeTcpPort

$procs = @()
function Stop-RunnerChildren {
    foreach ($p in $procs) {
        if ($null -eq $p) { continue }
        try {
            if (-not $p.HasExited) {
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            }
        } catch {
        }
    }
}
Register-EngineEvent -SourceIdentifier PowerShell.Exiting -Action { Stop-RunnerChildren } | Out-Null

try {
    # Ephemeral TLS target (FIN path only) + proxy smoke.
    $caKey = Join-Path $work 'ca.key'
    $caCrt = Join-Path $work 'ca.crt'
    $serverKey = Join-Path $work 'server.key'
    $serverCrt = Join-Path $work 'server.crt'
    $clientKey = Join-Path $work 'client.key'
    $clientCrt = Join-Path $work 'client.crt'
    $serverExt = Join-Path $work 'server.ext'
    $clientExt = Join-Path $work 'client.ext'
    $caExt = Join-Path $work 'ca.ext'

    @"
[v3_ca]
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
"@ | Set-Content -LiteralPath $caExt -Encoding ascii
    @"
[v3_server]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
"@ | Set-Content -LiteralPath $serverExt -Encoding ascii
    @"
[v3_client]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=clientAuth
"@ | Set-Content -LiteralPath $clientExt -Encoding ascii

    Invoke-OpenSsl @('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=tcpquic-win-ca', '-keyout', $caKey, '-out', (Join-Path $work 'ca.csr'))
    Invoke-OpenSsl @('x509', '-req', '-in', (Join-Path $work 'ca.csr'), '-signkey', $caKey, '-days', '1', '-out', $caCrt, '-extfile', $caExt, '-extensions', 'v3_ca')
    Invoke-OpenSsl @('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost', '-keyout', $serverKey, '-out', (Join-Path $work 'server.csr'))
    Invoke-OpenSsl @('x509', '-req', '-in', (Join-Path $work 'server.csr'), '-CA', $caCrt, '-CAkey', $caKey, '-CAcreateserial', '-days', '1', '-out', $serverCrt, '-extfile', $serverExt, '-extensions', 'v3_server')
    Invoke-OpenSsl @('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=tcpquic-client', '-keyout', $clientKey, '-out', (Join-Path $work 'client.csr'))
    Invoke-OpenSsl @('x509', '-req', '-in', (Join-Path $work 'client.csr'), '-CA', $caCrt, '-CAkey', $caKey, '-CAcreateserial', '-days', '1', '-out', $clientCrt, '-extfile', $clientExt, '-extensions', 'v3_client')

    if (-not ((Test-Path $caCrt) -and (Test-Path $serverCrt) -and (Test-Path $clientCrt))) {
        throw 'openssl failed to mint ephemeral certs for Windows convergence smoke'
    }

    $targetHits = Join-Path $Out 'target-hits.log'
    Set-Content -LiteralPath $targetHits -Value '' -Encoding ascii
    $targetScript = Join-Path $work 'target.py'
    @"
import http.server, socketserver, sys
class H(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def do_GET(self):
        body=b'ok'
        self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        n=int(self.headers.get('Content-Length','0'))
        while n:
            chunk=self.rfile.read(min(65536,n))
            if not chunk: break
            n-=len(chunk)
        open(sys.argv[2],'a',encoding='ascii').write('fin\n')
        body=b'reverse-flow-ok'
        self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads=True
S(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
"@ | Set-Content -LiteralPath $targetScript -Encoding ascii

    $target = Start-Process -PassThru -FilePath python -ArgumentList @(
        $targetScript, "$targetPort", $targetHits
    ) -RedirectStandardOutput (Join-Path $Out 'logs\target.out') `
      -RedirectStandardError (Join-Path $Out 'logs\target.err') `
      -WindowStyle Hidden
    $procs += $target
    if (-not (Wait-TcpPort -HostName '127.0.0.1' -Port $targetPort -TimeoutSeconds 10)) {
        throw "loopback HTTP target failed to start on $targetPort"
    }

    $serverLog = Join-Path $Out 'logs\server.log'
    $clientLog = Join-Path $Out 'logs\client.log'
    $serverArgs = @(
        'server',
        '--listen', "127.0.0.1:$quicPort",
        '--cert', $serverCrt,
        '--key', $serverKey,
        '--ca', $caCrt,
        '--allow-targets', '127.0.0.0/8',
        '--admin-listen', "127.0.0.1:$serverAdminPort",
        '--admin-token-file', $tokenFile
    )
    $clientArgs = @(
        'client',
        '--peer', "127.0.0.1:$quicPort",
        '--cert', $clientCrt,
        '--key', $clientKey,
        '--ca', $caCrt,
        '--http-listen', "127.0.0.1:$httpListenPort",
        '--admin-listen', "127.0.0.1:$clientAdminPort",
        '--admin-token-file', $tokenFile
    )

    $server = Start-Process -PassThru -FilePath $binPath -ArgumentList $serverArgs `
        -RedirectStandardError $serverLog -WindowStyle Hidden
    $procs += $server
    Start-Sleep -Seconds 1
    if ($server.HasExited) {
        throw "server exited during startup: $(Get-Content -Raw $serverLog)"
    }

    $client = Start-Process -PassThru -FilePath $binPath -ArgumentList $clientArgs `
        -RedirectStandardError $clientLog -WindowStyle Hidden
    $procs += $client
    Start-Sleep -Seconds 1
    if ($client.HasExited) {
        throw "client exited during startup: $(Get-Content -Raw $clientLog)"
    }

    if (-not (Wait-TcpPort -HostName '127.0.0.1' -Port $httpListenPort -TimeoutSeconds 15)) {
        throw "client HTTP CONNECT listener did not become ready on $httpListenPort"
    }

    $adminReady = $false
    foreach ($port in @($serverAdminPort, $clientAdminPort)) {
        $deadline = (Get-Date).AddSeconds(15)
        $ok = $false
        while ((Get-Date) -lt $deadline) {
            try {
                $null = Get-AdminMetrics -Port $port -Token $token
                $ok = $true
                break
            } catch {
                Start-Sleep -Milliseconds 200
            }
        }
        if (-not $ok) {
            throw "admin endpoint failed to start on port $port"
        }
        $adminReady = $true
    }
    if (-not $adminReady) {
        throw 'admin endpoints not ready'
    }

    $clientBaseline = Get-AdminMetrics -Port $clientAdminPort -Token $token
    $serverBaseline = Get-AdminMetrics -Port $serverAdminPort -Token $token
    ($clientBaseline | ConvertTo-Json -Depth 12) |
        Set-Content -LiteralPath (Join-Path $Out 'metrics\client-baseline-relay.json') -Encoding utf8
    ($serverBaseline | ConvertTo-Json -Depth 12) |
        Set-Content -LiteralPath (Join-Path $Out 'metrics\server-baseline-relay.json') -Encoding utf8

    if ($null -eq $clientBaseline.PSObject.Properties['windows_relay_receive_completion_pending'] -or
        $null -eq $serverBaseline.PSObject.Properties['windows_relay_receive_completion_pending']) {
        $msg = @"
performance release BLOCKED: proxy binary lacks Task 5 receive-completion metrics.
Binary: $binPath
Rebuild tcpquic-proxy/raypx2 after fixing any compile blockers, then re-run -Scenario $Scenario.
"@
        Write-EvidenceNote -Path (Join-Path $Out 'BLOCKED.txt') -Text $msg
        Write-Error $msg
        exit 69
    }

    $workloadSeconds = if ($Scenario -eq 'soak') { [Math]::Max(1, $SoakSeconds) } else { 5 }
    $deadline = (Get-Date).AddSeconds($workloadSeconds)
    $sample = 0
    $curlAttempts = 0
    $curlSuccesses = 0
    while ((Get-Date) -lt $deadline) {
        $curlAttempts++
        if (Invoke-CurlAttempt -CurlArgs @(
                '--max-time', '10',
                '-x', "http://127.0.0.1:$httpListenPort",
                '--proxytunnel', "http://127.0.0.1:$targetPort/"
            )) {
            $curlSuccesses++
        }

        $curlAttempts++
        if (Invoke-CurlAttempt -CurlArgs @(
                '--max-time', '10',
                '-x', "http://127.0.0.1:$httpListenPort",
                '-X', 'POST', '--data-binary', 'fin-body',
                "http://127.0.0.1:$targetPort/fin"
            )) {
            $curlSuccesses++
        }

        if (($sample % 6) -eq 0) {
            try {
                (Get-AdminMetrics -Port $clientAdminPort -Token $token | ConvertTo-Json -Depth 8) |
                    Set-Content -LiteralPath (Join-Path $Out ("metrics\client-sample-{0:D4}.json" -f $sample)) -Encoding utf8
                (Get-AdminMetrics -Port $serverAdminPort -Token $token | ConvertTo-Json -Depth 8) |
                    Set-Content -LiteralPath (Join-Path $Out ("metrics\server-sample-{0:D4}.json" -f $sample)) -Encoding utf8
            } catch {
            }
        }
        $sample++
        Start-Sleep -Seconds 1
    }

    Write-Host ("workload curl successes={0}/{1}" -f $curlSuccesses, $curlAttempts)
    Write-Host "draining for $DrainSeconds seconds before release gates..."
    Start-Sleep -Seconds $DrainSeconds

    $clientFinal = Get-AdminMetrics -Port $clientAdminPort -Token $token
    $serverFinal = Get-AdminMetrics -Port $serverAdminPort -Token $token
    ($clientFinal | ConvertTo-Json -Depth 12) |
        Set-Content -LiteralPath (Join-Path $Out 'metrics\client-final-relay.json') -Encoding utf8
    ($serverFinal | ConvertTo-Json -Depth 12) |
        Set-Content -LiteralPath (Join-Path $Out 'metrics\server-final-relay.json') -Encoding utf8

    Assert-FinActivityGate `
        -ClientBefore $clientBaseline -ServerBefore $serverBaseline `
        -ClientAfter $clientFinal -ServerAfter $serverFinal `
        -CurlAttempts $curlAttempts -CurlSuccesses $curlSuccesses
    Assert-ZeroFinReleaseGate -Relay $clientFinal -RequireNewCompletionFields
    Assert-ZeroFinReleaseGate -Relay $serverFinal -RequireNewCompletionFields

    $summary = [ordered]@{
        scenario = $Scenario
        soak_seconds_requested = $SoakSeconds
        workload_seconds = $workloadSeconds
        binary = $binPath
        curl_attempts = $curlAttempts
        curl_successes = $curlSuccesses
        note = if ($Scenario -eq 'soak' -and $SoakSeconds -lt 1800) {
            'short soak smoke; full release soak expects -SoakSeconds 1800'
        } else {
            'ok'
        }
        client_receive_completion_pending = $clientFinal.windows_relay_receive_completion_pending
        server_receive_completion_pending = $serverFinal.windows_relay_receive_completion_pending
        client_exactly_once_violation = $clientFinal.windows_relay_receive_completion_exactly_once_violation
        server_exactly_once_violation = $serverFinal.windows_relay_receive_completion_exactly_once_violation
        client_receive_completion_required = $clientFinal.windows_relay_receive_completion_required
        server_receive_completion_required = $serverFinal.windows_relay_receive_completion_required
        client_receive_completion_zero_length = $clientFinal.windows_relay_receive_completion_zero_length
        server_receive_completion_zero_length = $serverFinal.windows_relay_receive_completion_zero_length
    }
    ($summary | ConvertTo-Json -Depth 6) |
        Set-Content -LiteralPath (Join-Path $Out 'summary\result.json') -Encoding utf8
    Write-Host "windows terminal convergence $Scenario passed; evidence=$Out"
}
catch {
    Write-EvidenceNote -Path (Join-Path $Out 'FAILED.txt') -Text $_.Exception.Message
    Write-Error $_
    exit 1
}
finally {
    Stop-RunnerChildren
}

exit 0
