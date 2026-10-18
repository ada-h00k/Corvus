<#
  Used by the Wren setup (wren.iss) to look up download locations.

  * signal-cli: latest release of https://github.com/AsamK/signal-cli (GitHub API).
    The JVM build "signal-cli-<version>.tar.gz" is used; GitHub publishes its SHA-256 digest.
  * Java: latest Eclipse Temurin JRE (Adoptium API, binaries are hosted on github.com/adoptium).

  Result: key=value lines in -OutFile. On failure the file contains "error=<message>" and exit code 1.
#>
param(
    [Parameter(Mandatory = $true)][string]$OutFile,
    [int]$WantSignalCli = 1,
    [int]$WantJava = 0,
    [int]$JavaMajor = 25
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
} catch { }

$lines = New-Object System.Collections.Generic.List[string]

try {
    if ($WantSignalCli -eq 1) {
        $headers = @{ 'User-Agent' = 'Wren-Setup'; 'Accept' = 'application/vnd.github+json' }
        $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/AsamK/signal-cli/releases/latest' `
                                 -Headers $headers -TimeoutSec 30
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
        $assets = Invoke-RestMethod -Uri $uri -Headers @{ 'User-Agent' = 'Wren-Setup' } -TimeoutSec 30
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
