[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $GameDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$PakName = 'zzz-OutlastRequeue_P.pak'
$ExpectedPakSha256 = '1998125961ea66886ae41d71fe15ec2d555d045b980bc487ac5a6ea2a92d0c54'
$ExpectedGameBuildId = '24322931'
$RelativePakDirectory = Join-Path 'OPP' (Join-Path 'Content' 'Paks')

function Get-Sha256Lower {
    param([Parameter(Mandatory = $true)][string] $Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Read-Utf8Text {
    param([Parameter(Mandatory = $true)][string] $Path)

    # Steam writes BOM-less UTF-8. Windows PowerShell 5.1 otherwise falls
    # back to the active ANSI code page and corrupts non-ASCII library paths.
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    return [IO.File]::ReadAllText($Path, $utf8)
}

function Add-UniqueDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[string]] $List,
        [AllowNull()]
        [string] $Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }

    $expanded = [Environment]::ExpandEnvironmentVariables($Path.Trim())
    try {
        $normalized = [IO.Path]::GetFullPath($expanded)
    }
    catch {
        return
    }

    foreach ($existing in $List) {
        if ([string]::Equals($existing, $normalized, [StringComparison]::OrdinalIgnoreCase)) {
            return
        }
    }

    [void] $List.Add($normalized)
}

function Get-SteamRoots {
    $roots = [System.Collections.Generic.List[string]]::new()

    foreach ($registryKey in @(
        'HKCU:\Software\Valve\Steam',
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam'
    )) {
        try {
            $properties = Get-ItemProperty -LiteralPath $registryKey -ErrorAction Stop
            foreach ($propertyName in @('SteamPath', 'InstallPath')) {
                $property = $properties.PSObject.Properties[$propertyName]
                if ($null -ne $property) {
                    Add-UniqueDirectory -List $roots -Path ([string] $property.Value)
                }
            }
        }
        catch {
            # A missing registry location is normal on some Steam installations.
        }
    }

    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($programFilesX86) {
        Add-UniqueDirectory -List $roots -Path (Join-Path $programFilesX86 'Steam')
    }
    if ($env:ProgramFiles) {
        Add-UniqueDirectory -List $roots -Path (Join-Path $env:ProgramFiles 'Steam')
    }

    return $roots
}

function Get-SteamLibraryRoots {
    $libraries = [System.Collections.Generic.List[string]]::new()

    foreach ($steamRoot in (Get-SteamRoots)) {
        Add-UniqueDirectory -List $libraries -Path $steamRoot

        $libraryFile = Join-Path $steamRoot (Join-Path 'steamapps' 'libraryfolders.vdf')
        if (-not (Test-Path -LiteralPath $libraryFile -PathType Leaf)) {
            continue
        }

        try {
            $contents = Read-Utf8Text -Path $libraryFile
            $matches = [regex]::Matches(
                $contents,
                '"path"\s*"(?<path>(?:\\\\|[^"])*)"',
                [Text.RegularExpressions.RegexOptions]::IgnoreCase
            )
            foreach ($match in $matches) {
                $libraryPath = $match.Groups['path'].Value -replace '\\\\', '\'
                Add-UniqueDirectory -List $libraries -Path $libraryPath
            }
        }
        catch {
            Write-Verbose "Could not parse Steam library file: $libraryFile"
        }
    }

    return $libraries
}

function Test-GameDirectory {
    param([Parameter(Mandatory = $true)][string] $Candidate)

    if (-not (Test-Path -LiteralPath $Candidate -PathType Container)) {
        return $false
    }

    $gameExecutable = Join-Path $Candidate (Join-Path 'OPP' (Join-Path 'Binaries' (Join-Path 'Win64' 'TOTClient-Win64-Shipping.exe')))
    $pakDirectory = Join-Path $Candidate $RelativePakDirectory
    return (Test-Path -LiteralPath $gameExecutable -PathType Leaf) -and
        (Test-Path -LiteralPath $pakDirectory -PathType Container)
}

function Find-GameDirectory {
    param([AllowNull()][string] $ExplicitDirectory)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitDirectory)) {
        $candidate = [IO.Path]::GetFullPath(
            [Environment]::ExpandEnvironmentVariables($ExplicitDirectory.Trim())
        )
        if (-not (Test-GameDirectory -Candidate $candidate)) {
            throw "-GameDir must be the The Outlast Trials folder containing OPP: $candidate"
        }
        return $candidate
    }

    $environmentGameDir = $env:OUTLAST_REQUEUE_GAME_DIR
    if ([string]::IsNullOrWhiteSpace($environmentGameDir)) {
        $environmentGameDir = $env:OUTLAST_TRIALS_DIR
    }
    if (-not [string]::IsNullOrWhiteSpace($environmentGameDir)) {
        $candidate = [IO.Path]::GetFullPath(
            [Environment]::ExpandEnvironmentVariables($environmentGameDir.Trim())
        )
        if (Test-GameDirectory -Candidate $candidate) {
            return $candidate
        }
    }

    foreach ($library in (Get-SteamLibraryRoots)) {
        $steamApps = Join-Path $library 'steamapps'
        $installName = 'The Outlast Trials'
        $manifest = Join-Path $steamApps 'appmanifest_1304930.acf'

        if (Test-Path -LiteralPath $manifest -PathType Leaf) {
            try {
                $manifestContents = Read-Utf8Text -Path $manifest
                $match = [regex]::Match(
                    $manifestContents,
                    '"installdir"\s*"(?<name>[^"]+)"',
                    [Text.RegularExpressions.RegexOptions]::IgnoreCase
                )
                if ($match.Success) {
                    $installName = $match.Groups['name'].Value
                }
            }
            catch {
                Write-Verbose "Could not parse Steam app manifest: $manifest"
            }
        }

        $candidate = Join-Path (Join-Path $steamApps 'common') $installName
        if (Test-GameDirectory -Candidate $candidate) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }

    throw 'The Outlast Trials was not found. Rerun with -GameDir pointing to the folder that contains OPP.'
}

function Find-PayloadPak {
    $payloadPath = Join-Path (Join-Path $PSScriptRoot 'payload') $PakName
    if (Test-Path -LiteralPath $payloadPath -PathType Leaf) {
        return $payloadPath
    }

    # This fallback keeps the script useful in a developer staging directory.
    $adjacentPath = Join-Path $PSScriptRoot $PakName
    if (Test-Path -LiteralPath $adjacentPath -PathType Leaf) {
        return $adjacentPath
    }

    throw "Required payload is missing: payload\$PakName"
}

function Get-InstalledGameBuildId {
    param([Parameter(Mandatory = $true)][string] $GameDirectory)

    $commonDirectory = Split-Path -Path $GameDirectory -Parent
    $steamAppsDirectory = Split-Path -Path $commonDirectory -Parent
    $manifest = Join-Path $steamAppsDirectory 'appmanifest_1304930.acf'

    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "Steam app manifest was not found, so game build compatibility cannot be verified: $manifest"
    }

    $manifestContents = Read-Utf8Text -Path $manifest
    $match = [regex]::Match(
        $manifestContents,
        '"buildid"\s*"(?<build>[0-9]+)"',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase
    )
    if (-not $match.Success) {
        throw "Steam app manifest does not contain a build ID: $manifest"
    }

    return $match.Groups['build'].Value
}

$temporaryPath = $null

try {
    $sourcePak = Find-PayloadPak
    $sourceHash = Get-Sha256Lower -Path $sourcePak
    if ($sourceHash -ne $ExpectedPakSha256) {
        [Console]::Error.WriteLine(
            "ERROR: Bundled PAK failed verification. Expected $ExpectedPakSha256 but found $sourceHash."
        )
        exit 2
    }

    $resolvedGameDir = Find-GameDirectory -ExplicitDirectory $GameDir
    try {
        $installedBuildId = Get-InstalledGameBuildId -GameDirectory $resolvedGameDir
        if ($installedBuildId -ne $ExpectedGameBuildId) {
            Write-Warning "Installed game build is $installedBuildId; this release was verified on $ExpectedGameBuildId. Installing anyway (the PAK is hash-verified). If the game misbehaves in the Trial Board after an update, remove the PAK until a re-verified release is published."
        }
    }
    catch {
        Write-Warning "Could not read the Steam build ID ($($_.Exception.Message)). Continuing; the PAK is still hash-verified."
        $installedBuildId = 'unknown'
    }

    $targetDirectory = Join-Path $resolvedGameDir $RelativePakDirectory
    $targetPak = Join-Path $targetDirectory $PakName

    if (Test-Path -LiteralPath $targetPak -PathType Leaf) {
        $targetHash = Get-Sha256Lower -Path $targetPak
        if ($targetHash -ne $ExpectedPakSha256) {
            [Console]::Error.WriteLine(
                "ERROR: Refusing to overwrite $targetPak because its SHA-256 is $targetHash, not this release's expected hash."
            )
            exit 3
        }

        Write-Host 'Outlast Requeue PAK is already installed and verified.'
        Write-Host "Game: $resolvedGameDir"
        Write-Host "SHA-256: $targetHash"
        exit 0
    }

    if (Get-Process -Name 'TOTClient-Win64-Shipping' -ErrorAction SilentlyContinue) {
        [Console]::Error.WriteLine(
            'ERROR: Close The Outlast Trials before installing the PAK. No file was changed.'
        )
        exit 5
    }

    $temporaryPath = Join-Path $targetDirectory (".$PakName.installing-$PID")
    Copy-Item -LiteralPath $sourcePak -Destination $temporaryPath -ErrorAction Stop

    $temporaryHash = Get-Sha256Lower -Path $temporaryPath
    if ($temporaryHash -ne $ExpectedPakSha256) {
        throw "Temporary copy failed verification. Expected $ExpectedPakSha256 but found $temporaryHash."
    }

    # Do not overwrite a file that appeared between the initial check and copy.
    if (Test-Path -LiteralPath $targetPak -PathType Leaf) {
        $racedHash = Get-Sha256Lower -Path $targetPak
        if ($racedHash -ne $ExpectedPakSha256) {
            [Console]::Error.WriteLine(
                "ERROR: Refusing to overwrite a PAK that appeared during installation. Its SHA-256 is $racedHash."
            )
            exit 3
        }

        Remove-Item -LiteralPath $temporaryPath -Force
        $temporaryPath = $null
    }
    else {
        Move-Item -LiteralPath $temporaryPath -Destination $targetPak -ErrorAction Stop
        $temporaryPath = $null
    }

    $installedHash = Get-Sha256Lower -Path $targetPak
    if ($installedHash -ne $ExpectedPakSha256) {
        throw "Installed PAK failed final verification. Found $installedHash. The file was left in place for manual inspection."
    }

    Write-Host 'Outlast Requeue PAK installed and verified.'
    Write-Host "Game: $resolvedGameDir"
    Write-Host "Game build: $installedBuildId"
    Write-Host "PAK: $targetPak"
    Write-Host "SHA-256: $installedHash"
    Write-Host 'Run Outlast Requeue.exe from the extracted release folder. Start the first Imposter search manually.'
}
catch {
    [Console]::Error.WriteLine("ERROR: $($_.Exception.Message)")
    Write-Host 'No service or automatic-start entry was created. If access was denied, close the game and retry from an Administrator terminal.'
    exit 1
}
finally {
    if ($temporaryPath -and (Test-Path -LiteralPath $temporaryPath -PathType Leaf)) {
        Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
    }
}
