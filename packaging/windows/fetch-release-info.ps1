<#
  Used by the Corvus setup (corvus.iss) to look up download locations.

  * signal-cli: latest release of https://github.com/AsamK/signal-cli (GitHub API).
    The JVM build "signal-cli-<version>.tar.gz" is used; GitHub publishes its SHA-256 digest.
  * Java: latest Eclipse Temurin JRE (Adoptium API, binaries are hosted on github.com/adoptium).

  -GithubToken (optional): GitHub API token. Anonymous api.github.com calls are
  limited to 60 requests/hour PER IP - GitHub-hosted CI runners share huge NAT
  pools, so they routinely get 403. CI passes $env:GITHUB_TOKEN here; the
  install-time call from end-user PCs works without it (one call per install).
  Requests that fail with 403/429/5xx are retried with a short backoff.

  Result: key=value lines in -OutFile. On failure the file contains "error=<message>" and exit code 1.
#>
param(
    [Parameter(Mandatory = $true)][string]$OutFile,
    [int]$WantSignalCli = 1,
    [int]$WantJava = 0,
    [int]$JavaMajor = 25,
    [string]$GithubToken = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
} catch { }

$lines = New-Object System.Collections.Generic.List[string]

# Retry helper: anonymous GitHub calls from shared runner IPs regularly hit
# 403 (rate limit); transient server errors deserve a second try as well.
function Invoke-RestWithRetry {
    param([string]$Uri, [hashtable]$Headers, [int]$Retries = 3, [int]$TimeoutSec = 30)
    $attempt = 1
    while ($true) {
        try {
            return Invoke-RestMethod -Uri $Uri -Headers $Headers -TimeoutSec $TimeoutSec
        } catch {
            $code = 0
            if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
            $retryable = ($code -eq 403 -or $code -eq 429 -or $code -ge 500)
            if (-not $retryable -or $attempt -ge $Retries) { throw }
            Start-Sleep -Seconds (2 * $attempt)
            $attempt++
        }
    }
}

try {
    if ($WantSignalCli -eq 1) {
        $headers = @{ 'User-Agent' = 'Corvus-Setup'; 'Accept' = 'application/vnd.github+json' }
        if ($GithubToken) { $headers['Authorization'] = "Bearer $GithubToken" }
        $rel = Invoke-RestWithRetry -Uri 'https://api.github.com/repos/AsamK/signal-cli/releases/latest' `
                                    -Headers $headers
        $version = $rel.tag_name.TrimStart('v')
        $name = "signal-cli-$version.tar.gz"
        $asset = $rel.assets | Where-Object { $_.name -eq $name } | Select-Object -First 1
        if (-not $asset) { throw "Release $($rel.tag_name) has no asset named $name." }

        $sha = ''
        if ($asset.digest -and $asset.digest.ToString().StartsWith('sha256:')) {
            $sha = $asset.digest.ToString().Substring(7)
        }
        $lines.Add("signalcli_tag=$($rel.tag_name)")
        $lines.Add("signalcli_url=$($asset.browser_download_url)")
        $lines.Add("signalcli_sha256=$sha")
    }

    if ($WantJava -eq 1) {
        $uri = "https://api.adoptium.net/v3/assets/latest/$JavaMajor/hotspot?architecture=x64&image_type=jre&os=windows&vendor=eclipse"
        $assets = Invoke-RestWithRetry -Uri $uri -Headers @{ 'User-Agent' = 'Corvus-Setup' }
        $pkg = $assets | Where-Object { $_.binary.package.name -like '*.zip' } |
               Select-Object -First 1 -ExpandProperty binary | Select-Object -ExpandProperty package
        if (-not $pkg) { throw "No Temurin JRE $JavaMajor (Windows x64, zip) found." }
        $lines.Add("java_url=$($pkg.link)")
        $lines.Add("java_sha256=$($pkg.checksum)")
    }

    Set-Content -LiteralPath $OutFile -Value $lines -Encoding ASCII
    exit 0
}
catch {
    $msg = ($_.Exception.Message -replace '[\r\n]+', ' ')
    Set-Content -LiteralPath $OutFile -Value @("error=$msg") -Encoding ASCII
    exit 1
}
