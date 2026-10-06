#Requires -Version 5.1
<#
.SYNOPSIS
    Checks that this Windows 11 machine is ready to build Gygax, and builds the
    reusable `gygax-dev` Docker image if so.

Gygax's C++ core is POSIX-only (sockets, pthreads/jthread, dlopen, fork/exec,
SocketCAN ioctls, ptys in tests) and is not ported to Win32; see docs/WINDOWS.md
for why. The supported path on Windows is Docker Desktop with the WSL2 backend,
which every other script in this repo (build.ps1, assemble.ps1, scripts\*.ps1)
runs through. This script only checks that path is ready; it does not itself
need Docker running to report what is missing.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ok = $true

function Test-Tool {
    param([string]$Name, [string]$Command, [string]$InstallHint)
    if (Get-Command $Command -ErrorAction SilentlyContinue) {
        Write-Host "ok        $Name" -ForegroundColor Green
        return $true
    }
    Write-Host "missing   $Name ($InstallHint)" -ForegroundColor Yellow
    return $false
}

$ok = (Test-Tool -Name 'Docker CLI' -Command 'docker' -InstallHint 'winget install Docker.DockerDesktop') -and $ok

$wsl = Get-Command wsl -ErrorAction SilentlyContinue
if ($wsl) {
    Write-Host "ok        WSL" -ForegroundColor Green
} else {
    Write-Host "missing   WSL (wsl --install; Docker Desktop needs the WSL2 backend on Windows 11)" -ForegroundColor Yellow
    $ok = $false
}

$dockerRunning = $false
if (Get-Command docker -ErrorAction SilentlyContinue) {
    docker info *> $null
    $dockerRunning = ($LASTEXITCODE -eq 0)
    if ($dockerRunning) {
        Write-Host "ok        Docker daemon is reachable" -ForegroundColor Green
    } else {
        Write-Host "missing   Docker daemon not reachable (start Docker Desktop)" -ForegroundColor Yellow
        $ok = $false
    }
}

if (-not $ok) {
    Write-Host ""
    Write-Host "Install Docker Desktop (with the WSL2 backend, the Windows 11 default) and" -ForegroundColor Yellow
    Write-Host "start it, then re-run this script. See docs/WINDOWS.md for details and for" -ForegroundColor Yellow
    Write-Host "the native alternative: installing Ubuntu from WSL and running ./setup.sh" -ForegroundColor Yellow
    Write-Host "and ./build.sh directly inside it." -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "Building the gygax-dev image (first run only takes a while; cached after)..."
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
docker build --target dev -t gygax-dev $root
if ($LASTEXITCODE -ne 0) {
    Write-Host "docker build failed" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Ready. Next: .\build.ps1 test" -ForegroundColor Green
