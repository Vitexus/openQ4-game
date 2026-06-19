$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-VsDevCmdPath {
    param([string]$TargetArch)

    if ($env:VSINSTALLDIR) {
        $candidate = Join-Path $env:VSINSTALLDIR "Common7\Tools\VsDevCmd.bat"
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "Could not locate vswhere.exe."
    }

    $component = if ($TargetArch -eq "arm64") {
        "Microsoft.VisualStudio.Component.VC.Tools.ARM64"
    } else {
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
    }
    $installPathRaw = & $vswhere -latest -prerelease -products * -requires $component -property installationPath
    $installPath = if ($null -eq $installPathRaw) { "" } else { "$installPathRaw".Trim() }
    if ([string]::IsNullOrWhiteSpace($installPath)) {
        throw "No Visual Studio installation with C++ tools was found."
    }

    $vsDevCmd = Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd)) {
        throw "Could not locate VsDevCmd.bat at '$vsDevCmd'."
    }

    return $vsDevCmd
}

function Get-GameLibsVsProcessArch {
    $arch = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString().ToLowerInvariant()
    switch ($arch) {
        "x64" { return "x64" }
        "x86" { return "x86" }
        "arm64" { return "arm64" }
        default { return "x64" }
    }
}

function Get-GameLibsVsTargetArch {
    if (-not [string]::IsNullOrWhiteSpace($env:OPENQ4_GAMELIBS_VS_TARGET_ARCH)) {
        return $env:OPENQ4_GAMELIBS_VS_TARGET_ARCH.Trim().ToLowerInvariant()
    }
    if (-not [string]::IsNullOrWhiteSpace($env:OPENQ4_VS_TARGET_ARCH)) {
        return $env:OPENQ4_VS_TARGET_ARCH.Trim().ToLowerInvariant()
    }

    return Get-GameLibsVsProcessArch
}

function Get-GameLibsVsHostArch {
    if (-not [string]::IsNullOrWhiteSpace($env:OPENQ4_GAMELIBS_VS_HOST_ARCH)) {
        return $env:OPENQ4_GAMELIBS_VS_HOST_ARCH.Trim().ToLowerInvariant()
    }
    if (-not [string]::IsNullOrWhiteSpace($env:OPENQ4_VS_HOST_ARCH)) {
        return $env:OPENQ4_VS_HOST_ARCH.Trim().ToLowerInvariant()
    }

    return Get-GameLibsVsProcessArch
}

function Get-GameLibsModuleArch {
    param([string]$VsTargetArch)

    switch ($VsTargetArch.ToLowerInvariant()) {
        "amd64" { return "x64" }
        "x64" { return "x64" }
        "x86" { return "x86" }
        default { return $VsTargetArch.ToLowerInvariant() }
    }
}

function Quote-CmdArg([string]$Value) {
    if ($Value -match '[\s"&<>|()]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Invoke-Meson {
    param(
        [string[]]$MesonArgs,
        [string]$VsDevCmdPath,
        [string]$VsTargetArch,
        [string]$VsHostArch
    )

    if ([string]::IsNullOrWhiteSpace($VsDevCmdPath)) {
        & meson @MesonArgs
        return
    }

    $mesonCmd = "meson " + (($MesonArgs | ForEach-Object { Quote-CmdArg $_ }) -join " ")
    $fullCmd = 'call "' + $VsDevCmdPath + '" -arch=' + $VsTargetArch + ' -host_arch=' + $VsHostArch + ' >nul && set CC=cl && set CXX=cl && ' + $mesonCmd
    & $env:ComSpec /d /c $fullCmd
}

function Get-BuildDirInfo {
    param(
        [string[]]$MesonArgs,
        [string]$DefaultBuildDir
    )

    $result = [PSCustomObject]@{
        BuildDir = $DefaultBuildDir
        HasExplicit = $false
    }

    for ($i = 0; $i -lt $MesonArgs.Length; $i++) {
        $arg = $MesonArgs[$i]
        if ($arg -eq "-C" -and ($i + 1) -lt $MesonArgs.Length) {
            $result.BuildDir = $MesonArgs[$i + 1]
            $result.HasExplicit = $true
            break
        }

        if ($arg.StartsWith("-C") -and $arg.Length -gt 2) {
            $result.BuildDir = $arg.Substring(2)
            $result.HasExplicit = $true
            break
        }
    }

    $result.BuildDir = [System.IO.Path]::GetFullPath($result.BuildDir)
    return $result
}

function Test-MesonBuildDirectory {
    param([string]$BuildDir)

    $coreData = Join-Path $BuildDir "meson-private\coredata.dat"
    $ninjaFile = Join-Path $BuildDir "build.ninja"
    return (Test-Path $coreData) -and (Test-Path $ninjaFile)
}

function Copy-GameLibrariesToInstallGameDir {
    param(
        [string]$BuildDir,
        [string]$RepoRoot,
        [string]$GameArch
    )

    $installGameDir = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot "..\openQ4\.install\baseoq4"))
    New-Item -Path $installGameDir -ItemType Directory -Force | Out-Null

    $binaries = @(
        "game-sp_$GameArch.dll",
        "game-mp_$GameArch.dll"
    )

    foreach ($binary in $binaries) {
        $sourcePath = Join-Path $BuildDir (Join-Path "src" $binary)
        if (-not (Test-Path $sourcePath)) {
            throw "Expected build output '$sourcePath' was not found."
        }

        $destinationPath = Join-Path $installGameDir $binary
        Copy-Item -Path $sourcePath -Destination $destinationPath -Force
    }

    Write-Host "Copied game libraries to '$installGameDir'."
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDir "..\.."))
$defaultBuildDir = Join-Path $repoRoot "builddir"
$vsTargetArch = Get-GameLibsVsTargetArch
$vsHostArch = Get-GameLibsVsHostArch
$gameModuleArch = Get-GameLibsModuleArch -VsTargetArch $vsTargetArch

$vsDevCmd = $null
if ($null -eq (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vsDevCmd = Get-VsDevCmdPath -TargetArch $vsTargetArch
}

$effectiveArgs = @($args)
if ($effectiveArgs.Count -eq 0) {
    throw "No Meson arguments were provided to meson_setup.ps1."
}

$isCompileCommand = $effectiveArgs[0] -eq "compile"

if ($effectiveArgs.Length -gt 0 -and ($effectiveArgs[0] -eq "compile" -or $effectiveArgs[0] -eq "install")) {
    $buildInfo = Get-BuildDirInfo -MesonArgs $effectiveArgs -DefaultBuildDir $defaultBuildDir

    if (-not (Test-MesonBuildDirectory $buildInfo.BuildDir)) {
        Write-Host "Meson build directory '$($buildInfo.BuildDir)' is missing or invalid. Running meson setup..."
        $setupArgs = @(
            "setup",
            "--wipe",
            $buildInfo.BuildDir,
            $repoRoot,
            "--backend",
            "ninja",
            "--buildtype=release",
            "--vsenv"
        )
        Invoke-Meson -MesonArgs $setupArgs -VsDevCmdPath $vsDevCmd -VsTargetArch $vsTargetArch -VsHostArch $vsHostArch
        $setupCode = [int]$LASTEXITCODE
        if ($setupCode -ne 0) {
            exit $setupCode
        }
    }

    if (-not $buildInfo.HasExplicit) {
        $remainingArgs = @()
        if ($effectiveArgs.Length -gt 1) {
            $remainingArgs = $effectiveArgs[1..($effectiveArgs.Length - 1)]
        }
        $effectiveArgs = @($effectiveArgs[0], "-C", $buildInfo.BuildDir) + $remainingArgs
    }
}

Invoke-Meson -MesonArgs $effectiveArgs -VsDevCmdPath $vsDevCmd -VsTargetArch $vsTargetArch -VsHostArch $vsHostArch
$exitCode = [int]$LASTEXITCODE

if ($exitCode -eq 0 -and $isCompileCommand -and $env:OPENQ4_COPY_GAMELIBS_TO_INSTALL -eq "1") {
    $compileBuildInfo = Get-BuildDirInfo -MesonArgs $effectiveArgs -DefaultBuildDir $defaultBuildDir
    Copy-GameLibrariesToInstallGameDir -BuildDir $compileBuildInfo.BuildDir -RepoRoot $repoRoot -GameArch $gameModuleArch
}

exit $exitCode
