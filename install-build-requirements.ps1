[CmdletBinding()]
param(
    [switch] $SkipSubmoduleUpdate
)

$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'EDPE build requirements can only be installed on 64-bit Windows.'
}

function Invoke-WinGetInstall {
    param(
        [Parameter(Mandatory)] [string] $Id,
        [string[]] $ExtraArguments = @()
    )

    Write-Host "Installing or checking $Id..."
    & winget install --exact --id $Id --accept-source-agreements --accept-package-agreements @ExtraArguments
    if ($LASTEXITCODE -ne 0) {
        throw "winget failed to install $Id (exit code $LASTEXITCODE)."
    }
}

function Get-VisualStudioInstaller {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

$winget = Get-Command winget -ErrorAction SilentlyContinue
if (-not $winget) {
    throw 'winget was not found. Install or update App Installer from Microsoft Store, then run this script again.'
}

# Install tools separately so a failure identifies the missing requirement.
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Invoke-WinGetInstall -Id 'Git.Git'
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeVersion = $null
if ($cmake) {
    $versionOutput = & $cmake.Source --version | Select-Object -First 1
    if ($versionOutput -match 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)') {
        $cmakeVersion = [version] $Matches[1]
    }
}
if (-not $cmakeVersion -or $cmakeVersion -lt [version] '3.25.0') {
    Invoke-WinGetInstall -Id 'Kitware.CMake'
}

$vswhere = Get-VisualStudioInstaller
$vsInstance = $null
if ($vswhere) {
    $vsInstance = & $vswhere -latest -products '*' -version '[17.0,18.0)' -property installationPath
    if ($LASTEXITCODE -ne 0) { throw 'vswhere failed while locating Visual Studio 2022.' }
}

$workload = 'Microsoft.VisualStudio.Workload.VCTools'
if ($vsInstance) {
    $setup = Join-Path (Split-Path $vswhere -Parent) 'setup.exe'
    if (-not (Test-Path -LiteralPath $setup)) {
        throw "Visual Studio Installer was not found at '$setup'."
    }

    Write-Host "Adding the C++ workload and recommended Windows SDK components to $vsInstance..."
    & $setup modify --installPath $vsInstance --add $workload --includeRecommended --quiet --wait --norestart
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio Installer failed to add $workload (exit code $LASTEXITCODE)."
    }
}
else {
    # The recommended C++ workload components include the Windows SDK. NMake
    # and the x64 MSVC tools are installed by the workload's required components.
    $vsOverride = "--wait --quiet --norestart --add $workload --includeRecommended"
    Invoke-WinGetInstall -Id 'Microsoft.VisualStudio.2022.BuildTools' -ExtraArguments @('--override', $vsOverride)
}

$machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$env:Path = "$machinePath;$userPath"

if (-not $SkipSubmoduleUpdate) {
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '.')).Path
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        $gitCandidate = Join-Path $env:ProgramFiles 'Git\cmd\git.exe'
        if (Test-Path -LiteralPath $gitCandidate) { $git = Get-Item -LiteralPath $gitCandidate }
    }
    if (-not $git) {
        throw 'Git was installed, but git.exe is not available in this PowerShell session. Open a new terminal and rerun the script.'
    }
    $gitPath = if ($git.Source) { $git.Source } else { $git.FullName }

    Write-Host 'Initializing the Dear ImGui submodule...'
    Push-Location $repoRoot
    try {
        & $gitPath submodule update --init --recursive
        if ($LASTEXITCODE -ne 0) { throw "Git submodule initialization failed (exit code $LASTEXITCODE)." }
    }
    finally {
        Pop-Location
    }
}

Write-Host ''
Write-Host 'Build requirements are installed. Open a new x64 Native Tools PowerShell for VS 2022 (or Developer PowerShell) before configuring and building EDPE.'
