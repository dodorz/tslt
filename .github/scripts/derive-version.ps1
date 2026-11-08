param(
    # Ref name as provided by the workflow: a tag such as "0.0.10" / "v0.0.10", or a
    # branch name for manual (workflow_dispatch) runs.
    [Parameter(Mandatory = $true)]
    [string] $RefName,

    # Repository root, used to read TSLT_VERSION_DEFAULT for non-tag refs.
    [string] $SourceDir = "."
)

$ErrorActionPreference = "Stop"

# AppendAllText writes UTF-8 without a BOM, which the runner requires when parsing
# GITHUB_ENV / GITHUB_OUTPUT (a BOM would corrupt the first variable name).
function Add-WorkflowValue([string] $file, [string] $name, [string] $value) {
    [System.IO.File]::AppendAllText($file, "$name=$value`n")
}

function Get-DefaultVersion([string] $root) {
    $cmakeLists = Join-Path $root "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $cmakeLists)) {
        throw "Cannot read TSLT_VERSION_DEFAULT: $cmakeLists does not exist."
    }
    $text = Get-Content -Raw -LiteralPath $cmakeLists
    if ($text -match 'set\(TSLT_VERSION_DEFAULT\s+"([^"]+)"') {
        return $Matches[1]
    }
    throw "TSLT_VERSION_DEFAULT is not defined in $cmakeLists."
}

# Tags may carry an optional leading "v".
$version = $RefName.Trim()
if ($version.StartsWith("v") -or $version.StartsWith("V")) {
    $version = $version.Substring(1)
}

# CMake's project(VERSION ...) and the resource script only accept numeric components,
# so a prerelease suffix such as "-rc1" is rejected here with a clear message.
$numericVersion = '^\d+\.\d+\.\d+(\.\d+)?$'
if ($version -match $numericVersion) {
    Write-Host "Derived version '$version' from ref '$RefName'"
} elseif ($env:GITHUB_REF_TYPE -eq "tag") {
    # Shipping a binary that reports the wrong version is worse than failing.
    throw "Cannot derive a numeric version from tag '$RefName'. Use X.Y.Z or X.Y.Z.W, for example 0.0.10 or v0.0.10."
} else {
    # Manual dispatch builds from a branch: reuse the default in CMakeLists.txt so the
    # fallback is defined in exactly one place.
    $version = Get-DefaultVersion $SourceDir
    if ($version -notmatch $numericVersion) {
        throw "TSLT_VERSION_DEFAULT='$version' in CMakeLists.txt is not a dotted numeric version."
    }
    Write-Host "Ref '$RefName' is not a version tag; using TSLT_VERSION_DEFAULT=$version"
}

# The resource script always needs four numeric components; pad the missing ones so the
# verification step can compare the raw Windows file version exactly.
$parts = @($version.Split("."))
while ($parts.Count -lt 4) {
    $parts += "0"
}
$fileVersion = $parts[0..3] -join "."

if ([string]::IsNullOrEmpty($env:GITHUB_ENV) -or [string]::IsNullOrEmpty($env:GITHUB_OUTPUT)) {
    throw "GITHUB_ENV and GITHUB_OUTPUT must be set; run this script from GitHub Actions."
}

Write-Host "Embedded version: $version (file version $fileVersion)"
Add-WorkflowValue $env:GITHUB_ENV "TSLT_VERSION" $version
Add-WorkflowValue $env:GITHUB_ENV "TSLT_FILE_VERSION" $fileVersion
Add-WorkflowValue $env:GITHUB_OUTPUT "version" $version
Add-WorkflowValue $env:GITHUB_OUTPUT "file_version" $fileVersion
