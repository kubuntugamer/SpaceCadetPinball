# Run this script from a Developer Command Prompt for VS *.
#
# Dependencies (glfw3, the Vulkan loader and fluidsynth) are provided by vcpkg in
# manifest mode; install vcpkg and set $env:VCPKG_ROOT before running this script.
# The Vulkan SDK is not required, the loader is bundled from the vcpkg package.

$ErrorActionPreference = "Stop"

if (-not $env:VCPKG_ROOT) {
    Write-Error "VCPKG_ROOT is not set. Install vcpkg (https://vcpkg.io) and set `$env:VCPKG_ROOT."
}

$toolchain = (Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake")

function New-Release([string]$architecture, [string]$triplet, [string]$zipName) {
    # Start fresh so the two architectures don't share stale build/vcpkg state.
    # (Do not touch .\bin: it may hold the user's game data.)
    Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
    cmake -S . -B build -A $architecture -DCMAKE_WIN32_EXECUTABLE:BOOL=1 -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DVCPKG_MANIFEST_FEATURES=music
    cmake --build build --config Release

    # Copy the runtime DLLs vcpkg installed next to the executable.
    $installed = ".\build\vcpkg_installed\$triplet\bin"
    if (Test-Path $installed) {
        Copy-Item "$installed\*.dll" ".\bin\Release" -Force
    }

    Compress-Archive -Path ".\bin\Release\*" -DestinationPath ".\bin\$zipName" -Force
}

# X86 build
New-Release -architecture Win32 -triplet x86-windows -zipName "SpaceCadetPinballx86Win.zip"

# X64 build
New-Release -architecture x64 -triplet x64-windows -zipName "SpaceCadetPinballx64Win.zip"
