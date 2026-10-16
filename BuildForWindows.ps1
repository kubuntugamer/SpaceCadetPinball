# Builds and packages the Windows release (x86 + x64) into bin\*.zip.
#
# Run from a normal PowerShell prompt; use a "Developer Command Prompt for VS" only
# if cmake cannot locate Visual Studio on its own.
#
# Dependencies (glfw3, the Vulkan loader, and fluidsynth for music) come from vcpkg
# in manifest mode. Set VCPKG_ROOT (or VCPKG_INSTALLATION_ROOT) to your vcpkg install.

$ErrorActionPreference = "Stop"

$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) { $vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT }
if (-not $vcpkgRoot) {
    Write-Error "Set VCPKG_ROOT (or VCPKG_INSTALLATION_ROOT) to your vcpkg installation."
}

$toolchain = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"

function New-Release([string]$architecture, [string]$triplet, [string]$zipName) {
    # Start fresh so the two architectures don't share stale build/vcpkg state.
    # (Do not touch .\bin: it may hold the user's game data.)
    Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue

    cmake -S . -B build -A $architecture -DCMAKE_WIN32_EXECUTABLE:BOOL=1 -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DVCPKG_MANIFEST_FEATURES=music
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $architecture" }

    cmake --build build --config Release
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $architecture" }

    # Copy the runtime DLLs vcpkg installed next to the executable.
    $installed = ".\build\vcpkg_installed\$triplet\bin"
    if (Test-Path $installed) {
        Copy-Item "$installed\*.dll" ".\bin\Release" -Force
    }

    if (-not (Test-Path ".\bin\Release\SpaceCadetPinball.exe")) {
        throw "SpaceCadetPinball.exe was not produced for $architecture"
    }

    Compress-Archive -Path ".\bin\Release\*" -DestinationPath ".\bin\$zipName" -Force
}

# X86 build
New-Release -architecture Win32 -triplet x86-windows -zipName "SpaceCadetPinballx86Win.zip"

# X64 build
New-Release -architecture x64 -triplet x64-windows -zipName "SpaceCadetPinballx64Win.zip"
