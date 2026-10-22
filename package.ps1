[CmdletBinding()]
param(
    [string]$QtRoot = "C:\Qt\6.8.0\msvc2022_64",
    [string]$BuildDir = "build",
    [string]$OutputDirectory = "dist\PDF@home-windows"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $repoRoot

try {
    $cmake = Get-Command cmake -ErrorAction Stop
    $qtDeploy = Join-Path $QtRoot "bin\windeployqt.exe"
    if (-not (Test-Path $qtDeploy -PathType Leaf)) {
        throw "Could not find windeployqt at '$qtDeploy'. Pass the Qt installation directory with -QtRoot."
    }

    foreach ($requiredPath in @("LICENSE", "THIRD_PARTY_NOTICES.md", "licenses\LGPL-3.0.txt",
                                "licenses\GPL-3.0.txt", "licenses\AGPL-3.0.txt",
                                "licenses\vcpkg")) {
        if (-not (Test-Path $requiredPath)) {
            throw "Required license material is missing: '$requiredPath'."
        }
    }

    & $cmake.Source --build $BuildDir --config Release --target PdfApp --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "CMake Release build failed with exit code $LASTEXITCODE."
    }

    $builtExe = Join-Path $BuildDir "Release\PDF@home.exe"
    if (-not (Test-Path $builtExe -PathType Leaf)) {
        throw "The Release executable was not found at '$builtExe'."
    }

    $stagePath = [System.IO.Path]::GetFullPath($OutputDirectory)
    if (Test-Path $stagePath) {
        throw "The staging directory already exists: '$stagePath'. Remove or rename it before packaging."
    }
    New-Item -ItemType Directory -Path $stagePath | Out-Null

    $stagedExe = Join-Path $stagePath "PDF@home.exe"
    Copy-Item $builtExe $stagedExe
    & $qtDeploy --release --compiler-runtime --no-translations --dir $stagePath $stagedExe
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed with exit code $LASTEXITCODE."
    }

    foreach ($requiredDll in @("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
                               "Qt6Pdf.dll", "Qt6PdfWidgets.dll")) {
        if (-not (Test-Path (Join-Path $stagePath $requiredDll) -PathType Leaf)) {
            throw "The deployed Qt runtime is incomplete; '$requiredDll' was not found."
        }
    }

    Copy-Item "README.md" $stagePath
    Copy-Item "LICENSE" $stagePath
    Copy-Item "THIRD_PARTY_NOTICES.md" $stagePath
    Copy-Item "licenses" $stagePath -Recurse

    @"
This archive contains dynamically linked Qt DLLs as separate files. Do not add
technical or license restrictions that prevent replacing these DLLs with
compatible modified versions or LGPL-permitted reverse engineering for debugging
changes to Qt libraries.

Before distributing this binary, review THIRD_PARTY_NOTICES.md. This packaging
script does not include the complete Qt PDF third-party notice set and does not
provide the corresponding source/build materials required for the statically
linked MuPDF build.
"@ | Set-Content -Path (Join-Path $stagePath "DISTRIBUTION-NOTICE.txt") -Encoding UTF8

    $archivePath = [System.IO.Path]::GetFullPath("dist\PDF@home-windows.zip")
    New-Item -ItemType Directory -Path (Split-Path -Parent $archivePath) -Force | Out-Null
    Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $archivePath -Force

    Write-Host "Staged runtime: $stagePath"
    Write-Host "Archive: $archivePath"
    Write-Warning "This is a runtime staging archive, not a complete license-compliance package. Complete the release steps in THIRD_PARTY_NOTICES.md before distributing it."
}
finally {
    Pop-Location
}
