#Requires -Version 5.1
<#
.SYNOPSIS
    Windows 11 equivalent of scripts\collect-diagnostics.sh: collects a
    redacted diagnostics bundle from a running Gygax service, via the
    gygax-dev Docker image. The bundle is written back into this checkout.

    Set $env:GYGAX_URL, $env:GYGAX_API_TOKEN and $env:GYGAX_BIN as you would
    on Linux/macOS. If the service is reachable from the Windows host at
    127.0.0.1 (e.g. a container with -p 1984:1984, or a native process),
    reach it from inside gygax-dev as host.docker.internal instead of
    127.0.0.1 (Docker Desktop's DNS name for the host).

.EXAMPLE
    $env:GYGAX_URL = "http://host.docker.internal:1984"
    $env:GYGAX_API_TOKEN = "..."
    .\scripts\collect-diagnostics.ps1
#>
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RestArgs
)

. (Join-Path $PSScriptRoot 'DevContainer.ps1')

$envVars = @{}
foreach ($name in 'GYGAX_URL', 'GYGAX_API_TOKEN', 'GYGAX_BIN') {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ($value) { $envVars[$name] = $value }
}

Invoke-GygaxDevContainer -Command (@('./scripts/collect-diagnostics.sh') + $RestArgs) -EnvVars $envVars
