#!/usr/bin/env bash
#
# Builds SpaceCadetPinball.app and packages it as a DMG on macOS.
#
# Dependencies are provided by Homebrew. Vulkan on macOS is provided by MoltenVK
# (through the vulkan-loader formula), which supplies both the loader and the ICD.
#
#   brew install cmake ninja glfw vulkan-loader molten-vk dylibbundler
#   brew install fluid-synth        # optional, enables background music
#
set -xe

app_name='SpaceCadetPinball'
sw_version='2.2.1'
dmg_name="${app_name}-${sw_version}-mac.dmg"

brew_prefix=''
for prefix in /opt/homebrew /usr/local; do
	if command -v brew >/dev/null 2>&1 && [ -d "$prefix" ]; then
		brew_prefix="$(brew --prefix)"
		break
	fi
done

for formula in glfw vulkan-loader molten-vk; do
	if command -v brew >/dev/null 2>&1; then
		brew list --versions "$formula" >/dev/null 2>&1 || {
			echo "Missing Homebrew formula '$formula'. Install it with: brew install $formula" >&2
			exit 1
		}
	fi
done

# Build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

mkdir -p "${app_name}.app/Contents/MacOS"
mkdir -p "${app_name}.app/Contents/Resources"
mkdir -p "${app_name}.app/Contents/Frameworks"

cp -a Platform/macOS/Info.plist "${app_name}.app/Contents/"
cp -a Platform/macOS/${app_name}.icns "${app_name}.app/Contents/Resources/"
cp -a "bin/${app_name}" "${app_name}.app/Contents/MacOS/"

# Bundle non-system dylibs (GLFW, MoltenVK/Vulkan loader, FluidSynth and its deps)
if command -v dylibbundler >/dev/null 2>&1; then
	dylibbundler -od -b \
		-x "${app_name}.app/Contents/MacOS/${app_name}" \
		-d "${app_name}.app/Contents/Frameworks" \
		-p "@executable_path/../Frameworks"
else
	echo "dylibbundler not found; install it (brew install dylibbundler) to bundle libraries." >&2
fi

# Bundle the MoltenVK Vulkan ICD so the loader can find a driver at runtime.
for icd in "${brew_prefix}/etc/vulkan/icd.d/MoltenVK_icd.json" \
           /opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
           /usr/local/etc/vulkan/icd.d/MoltenVK_icd.json; do
	if [ -n "$icd" ] && [ -f "$icd" ]; then
		mkdir -p "${app_name}.app/Contents/Resources/vulkan/icd.d"
		cp -a "$icd" "${app_name}.app/Contents/Resources/vulkan/icd.d/"
		break
	fi
done

# Bundle a General MIDI SoundFont for music, if one is installed.
for sf in "${brew_prefix}/share/soundfonts/"*.sf2 \
          /opt/homebrew/share/soundfonts/*.sf2 \
          /usr/local/share/soundfonts/*.sf2; do
	if [ -f "$sf" ]; then
		cp -a "$sf" "${app_name}.app/Contents/Resources/"
		break
	fi
done

sed -i '' "s/CHANGEME_SW_VERSION/${sw_version}/" "${app_name}.app/Contents/Info.plist"

echo -n "APPL????" > "${app_name}.app/Contents/PkgInfo"

hdiutil create -fs HFS+ -srcfolder "${app_name}.app" -volname "${app_name} ${sw_version}" "$dmg_name"

rm -r "${app_name}.app"
