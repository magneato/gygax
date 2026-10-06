<#
.SYNOPSIS
    Shared helper: run a command from this repo inside the `gygax-dev` Docker
    image, bind-mounting the working tree so edits on the Windows side are
    picked up immediately (no copy, no stale image).

Dot-sourced by build.ps1, assemble.ps1, scripts\dogfood.ps1, scripts\lint.ps1,
scripts\sdk-check.ps1 and scripts\collect-diagnostics.ps1. Not meant to be run
directly. See docs/WINDOWS.md.
#>

function Invoke-GygaxDevContainer {
    param(
        [Parameter(Mandatory)][string[]]$Command,
        [hashtable]$EnvVars = @{},
        [switch]$Interactive,
        [int[]]$PublishPorts = @()
    )

    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        Write-Host "docker was not found on PATH. Install Docker Desktop (winget install Docker.DockerDesktop)," -ForegroundColor Red
        Write-Host "start it, and re-run .\setup.ps1. See docs/WINDOWS.md." -ForegroundColor Red
        exit 1
    }

    $repoRoot = (Get-Item (Join-Path $PSScriptRoot '..')).FullName

    docker build --target dev -t gygax-dev $repoRoot
    if ($LASTEXITCODE -ne 0) {
        Write-Host "docker build --target dev failed" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    $dockerArgs = @('run', '--rm')
    if ($Interactive) { $dockerArgs += @('-it') } else { $dockerArgs += @('-i') }
    $dockerArgs += @('-v', "${repoRoot}:/src", '-w', '/src')
    foreach ($port in $PublishPorts) { $dockerArgs += @('-p', "${port}:${port}") }
    foreach ($key in $EnvVars.Keys) { $dockerArgs += @('-e', "$key=$($EnvVars[$key])") }
    $dockerArgs += 'gygax-dev'
    $dockerArgs += $Command

    & docker @dockerArgs
    exit $LASTEXITCODE
}
