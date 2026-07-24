[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $GameDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$PakName = 'zzz-OutlastRequeue_P.pak'
$ExpectedPakSha256 = '1998125961ea66886ae41d71fe15ec2d555d045b980bc487ac5a6ea2a92d0c54'
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
    return (Test-Path -LiteralPath $gameExecutable -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Candidate $RelativePakDirectory) -PathType Container)
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

try {
    $resolvedGameDir = Find-GameDirectory -ExplicitDirectory $GameDir
    $targetPak = Join-Path (Join-Path $resolvedGameDir $RelativePakDirectory) $PakName

    if (-not (Test-Path -LiteralPath $targetPak -PathType Leaf)) {
        Write-Host 'Outlast Requeue PAK is not installed at the detected game path.'
        Write-Host "Game: $resolvedGameDir"
        exit 0
    }

    $targetHash = Get-Sha256Lower -Path $targetPak
    if ($targetHash -ne $ExpectedPakSha256) {
        [Console]::Error.WriteLine(
            "ERROR: Refusing to remove $targetPak because its SHA-256 is $targetHash, not this release's expected hash."
        )
        exit 3
    }

    if (Get-Process -Name 'TOTClient-Win64-Shipping' -ErrorAction SilentlyContinue) {
        [Console]::Error.WriteLine(
            'ERROR: Close The Outlast Trials before removing the PAK. No file was changed.'
        )
        exit 5
    }

    Remove-Item -LiteralPath $targetPak -Force -ErrorAction Stop
    if (Test-Path -LiteralPath $targetPak) {
        throw "The verified PAK still exists after removal: $targetPak"
    }

    Write-Host 'Outlast Requeue PAK removed.'
    Write-Host "Game: $resolvedGameDir"
    Write-Host "Removed SHA-256: $targetHash"
    Write-Host 'The application is portable. Delete the extracted release folder if you no longer want it.'
}
catch {
    [Console]::Error.WriteLine("ERROR: $($_.Exception.Message)")
    Write-Host 'No service or automatic-start entry exists to remove. If access was denied, close the game and retry from an Administrator terminal.'
    exit 1
}
