#Requires -Version 5.1
<#
.SYNOPSIS
    Windows equivalent of scripts\docker-build.sh: builds both the production
    `gygax` image and the `gygax-dev` toolchain image. This talks to Docker
    directly (no bind-mounted repo), so it needs no gygax-dev image to exist
    first, unlike build.ps1/dogfood.ps1/lint.ps1.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Host "docker was not found on PATH. Install Docker Desktop and run .\setup.ps1 first." -ForegroundColor Red
    exit 1
}

$root = Split-Path -Parent $PSScriptRoot
$tag = if ($env:GYGAX_IMAGE) { $env:GYGAX_IMAGE } else { 'gygax' }
$devTag = if ($env:GYGAX_DEV_IMAGE) { $env:GYGAX_DEV_IMAGE } else { 'gygax-dev' }

Write-Host "=== building $tag (production)"
docker build -t $tag $root
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "=== building $devTag (toolchain, for build.ps1/dogfood.ps1/lint.ps1/...)"
docker build --target dev -t $devTag $root
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "built: $tag, $devTag"
Write-Host "run the service:  docker run --rm -p 1984:1984 -e GYGAX_API_TOKEN=... $tag"
Write-Host "dev loop:         .\build.ps1 test"
