cmake_minimum_required(VERSION 3.0...3.5)

# Version: the VERSION file at the repo root is the single source of truth for
# the release number. Everything else (game, packages, installer, release tag)
# derives from it - see scripts/bump-version.sh for bumping a patch release.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SPACECADETPINBALL_VERSION
	LIMIT_COUNT 1)
string(STRIP "${SPACECADETPINBALL_VERSION}" SPACECADETPINBALL_VERSION)
if(NOT SPACECADETPINBALL_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
	message(FATAL_ERROR "VERSION file must contain a version like X.Y.Z, got: "
		"'${SPACECADETPINBALL_VERSION}'")
endif()

project(SpaceCadetPinball VERSION ${SPACECADETPINBALL_VERSION})

set(CMAKE_CXX_STANDARD 11)

option(SCP_USE_RUST_GEOMETRY "Use the Rust maths/geometry crate instead of the C++ reference implementation" ON)
# Moves collision box membership, the ray walk and the visited marks into the
# Rust world; C++ edges keep their own distance maths and register a callback.
# Default since 2.6.0, after a play test confirmed gameplay; set OFF to build the
# original C++ grid walk.
option(SCP_USE_RUST_WORLD "Let the Rust collision world own the edge grid" ON)
set(SCP_RUST_TARGET "" CACHE STRING "Optional Rust target triple for the geometry crate")
if(SCP_USE_RUST_GEOMETRY)
    message(STATUS "Rust geometry enabled (Cargo required)")
    find_program(CARGO_EXECUTABLE cargo)
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR "SCP_USE_RUST_GEOMETRY requires Cargo on PATH")
    endif()
    set(RUST_GEOMETRY_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/rust/geometry/Cargo.toml")
    set(RUST_GEOMETRY_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust-target")
    set(RUST_GEOMETRY_CARGO_ARGS build --release --manifest-path "${RUST_GEOMETRY_MANIFEST}"
        --target-dir "${RUST_GEOMETRY_TARGET_DIR}")
    set(RUST_GEOMETRY_BUILD_COMMANDS)
    # Every crate source has to be a dependency, otherwise editing a module
    # leaves a stale static library behind until Cargo is invoked by hand.
    file(GLOB RUST_GEOMETRY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/rust/geometry/src/*.rs")
    set(RUST_GEOMETRY_DEPENDENCIES
        "${RUST_GEOMETRY_MANIFEST}"
        "${CMAKE_CURRENT_SOURCE_DIR}/rust/geometry/Cargo.lock"
        ${RUST_GEOMETRY_SOURCES})
    if(MSVC)
        set(RUST_GEOMETRY_LIBRARY_NAME "spacecadet_geometry.lib")
    else()
        set(RUST_GEOMETRY_LIBRARY_NAME "libspacecadet_geometry.a")
    endif()
    if(SCP_RUST_TARGET)
        set(RUST_GEOMETRY_OUTPUT_DIR "${RUST_GEOMETRY_TARGET_DIR}/${SCP_RUST_TARGET}/release")
        list(APPEND RUST_GEOMETRY_CARGO_ARGS --target "${SCP_RUST_TARGET}")
        set(RUST_GEOMETRY_LIBRARY "${RUST_GEOMETRY_OUTPUT_DIR}/${RUST_GEOMETRY_LIBRARY_NAME}")
        list(APPEND RUST_GEOMETRY_BUILD_COMMANDS COMMAND "${CARGO_EXECUTABLE}" ${RUST_GEOMETRY_CARGO_ARGS})
    elseif(APPLE)
        set(RUST_GEOMETRY_LIBRARY "${RUST_GEOMETRY_TARGET_DIR}/release/${RUST_GEOMETRY_LIBRARY_NAME}")
        set(RUST_GEOMETRY_FAT_INPUTS)
        if(CMAKE_OSX_ARCHITECTURES)
            set(RUST_MACOS_ARCHITECTURES ${CMAKE_OSX_ARCHITECTURES})
        else()
            set(RUST_MACOS_ARCHITECTURES arm64 x86_64)
        endif()
        foreach(RUST_ARCH IN LISTS RUST_MACOS_ARCHITECTURES)
            if(RUST_ARCH STREQUAL "arm64")
                set(RUST_TARGET "aarch64-apple-darwin")
            elseif(RUST_ARCH STREQUAL "x86_64")
                set(RUST_TARGET "x86_64-apple-darwin")
            else()
                message(FATAL_ERROR "Unsupported Rust geometry macOS architecture: ${RUST_ARCH}")
            endif()
            list(APPEND RUST_GEOMETRY_BUILD_COMMANDS COMMAND "${CARGO_EXECUTABLE}" build --release
                --manifest-path "${RUST_GEOMETRY_MANIFEST}" --target-dir "${RUST_GEOMETRY_TARGET_DIR}"
                --target "${RUST_TARGET}")
            list(APPEND RUST_GEOMETRY_FAT_INPUTS "${RUST_GEOMETRY_TARGET_DIR}/${RUST_TARGET}/release/${RUST_GEOMETRY_LIBRARY_NAME}")
        endforeach()
        find_program(LIPO_EXECUTABLE lipo REQUIRED)
        list(APPEND RUST_GEOMETRY_BUILD_COMMANDS
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${RUST_GEOMETRY_TARGET_DIR}/release"
            COMMAND "${LIPO_EXECUTABLE}" -create
                ${RUST_GEOMETRY_FAT_INPUTS} -output "${RUST_GEOMETRY_LIBRARY}")
    else()
        set(RUST_GEOMETRY_LIBRARY "${RUST_GEOMETRY_TARGET_DIR}/release/${RUST_GEOMETRY_LIBRARY_NAME}")
        list(APPEND RUST_GEOMETRY_BUILD_COMMANDS COMMAND "${CARGO_EXECUTABLE}" ${RUST_GEOMETRY_CARGO_ARGS})
    endif()
    add_custom_command(
        OUTPUT "${RUST_GEOMETRY_LIBRARY}"
        ${RUST_GEOMETRY_BUILD_COMMANDS}
        DEPENDS
            ${RUST_GEOMETRY_DEPENDENCIES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building Rust rectangle geometry"
        VERBATIM
    )
    add_custom_target(spacecadet_rust_geometry DEPENDS "${RUST_GEOMETRY_LIBRARY}")
    add_library(spacecadet_geometry STATIC IMPORTED GLOBAL)
    set_target_properties(spacecadet_geometry PROPERTIES IMPORTED_LOCATION "${RUST_GEOMETRY_LIBRARY}")
endif()

if(SCP_USE_RUST_WORLD AND NOT SCP_USE_RUST_GEOMETRY)
    message(FATAL_ERROR "SCP_USE_RUST_WORLD requires SCP_USE_RUST_GEOMETRY")
endif()



set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/bin)

# Differential tests for the Rust ports: each one builds the original C++
# implementation alongside the Rust one and compares results bit for bit (maths)
# or box for box (edge grid). Off by default; CI enables them so a ported routine
# cannot drift from its reference silently.
option(SCP_BUILD_PARITY_TESTS "Build the C++/Rust differential tests" OFF)
if(SCP_BUILD_PARITY_TESTS)
    if(NOT SCP_USE_RUST_GEOMETRY)
        message(FATAL_ERROR "SCP_BUILD_PARITY_TESTS requires SCP_USE_RUST_GEOMETRY")
    endif()

    foreach(parity_test maths_parity edgegrid_parity proj_parity ballstep_parity)
        add_executable(${parity_test} rust/geometry/tests/${parity_test}.cpp)
        target_include_directories(${parity_test} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/SpaceCadetPinball")
        add_dependencies(${parity_test} spacecadet_rust_geometry)
        target_link_libraries(${parity_test} spacecadet_geometry)
    endforeach()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/CMakeModules")

# Link mingw libs static
if(MINGW)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static")
endif()

if(APPLE)
    set(MACOSX_RPATH)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH true)
    set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.11")
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    endif()
endif()

# Windowing + input
find_package(glfw3 REQUIRED)

# Vulkan rendering
find_package(Vulkan REQUIRED)

# MIDI music (optional): wavetable synthesis through FluidSynth + a SoundFont.
find_path(FLUIDSYNTH_INCLUDE_DIR fluidsynth.h)
find_library(FLUIDSYNTH_LIBRARY NAMES fluidsynth libfluidsynth-3 libfluidsynth)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/SpaceCadetPinball)
get_property(dirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)
foreach(dir ${dirs})
  message(STATUS "Include dir='${dir}'")
endforeach()



set(SOURCE_FILES
        SpaceCadetPinball/control.cpp
        SpaceCadetPinball/control.h
        SpaceCadetPinball/EmbeddedData.cpp
        SpaceCadetPinball/EmbeddedData.h
        SpaceCadetPinball/font_selection.cpp
        SpaceCadetPinball/font_selection.h
        SpaceCadetPinball/fullscrn.cpp
        SpaceCadetPinball/fullscrn.h
        SpaceCadetPinball/gdrv.cpp
        SpaceCadetPinball/gdrv.h
        SpaceCadetPinball/GroupData.cpp
        SpaceCadetPinball/GroupData.h
        SpaceCadetPinball/high_score.cpp
        SpaceCadetPinball/high_score.h
        SpaceCadetPinball/loader.cpp
        SpaceCadetPinball/loader.h
        SpaceCadetPinball/maths.cpp
        SpaceCadetPinball/maths.h
        SpaceCadetPinball/midi.cpp
        SpaceCadetPinball/midi.h
        SpaceCadetPinball/nudge.cpp
        SpaceCadetPinball/nudge.h
        SpaceCadetPinball/options.cpp
        SpaceCadetPinball/options.h
        SpaceCadetPinball/partman.cpp
        SpaceCadetPinball/partman.h
        SpaceCadetPinball/pb.cpp
        SpaceCadetPinball/pb.h
        SpaceCadetPinball/pch.h
        SpaceCadetPinball/proj.cpp
        SpaceCadetPinball/proj.h
        SpaceCadetPinball/render.cpp
        SpaceCadetPinball/render.h
        SpaceCadetPinball/score.cpp
        SpaceCadetPinball/score.h
        SpaceCadetPinball/Sound.cpp
        SpaceCadetPinball/Sound.h
        SpaceCadetPinball/SpaceCadetPinball.cpp
        SpaceCadetPinball/TBall.cpp
        SpaceCadetPinball/TBall.h
        SpaceCadetPinball/TBlocker.cpp
        SpaceCadetPinball/TBlocker.h
        SpaceCadetPinball/TBumper.cpp
        SpaceCadetPinball/TBumper.h
        SpaceCadetPinball/TCircle.cpp
        SpaceCadetPinball/TCircle.h
        SpaceCadetPinball/TCollisionComponent.cpp
        SpaceCadetPinball/TCollisionComponent.h
        SpaceCadetPinball/TComponentGroup.cpp
        SpaceCadetPinball/TComponentGroup.h
        SpaceCadetPinball/TDemo.cpp
        SpaceCadetPinball/TDemo.h
        SpaceCadetPinball/TDrain.cpp
        SpaceCadetPinball/TDrain.h
        SpaceCadetPinball/TEdgeBox.h
        SpaceCadetPinball/TEdgeManager.cpp
        SpaceCadetPinball/TEdgeManager.h
        SpaceCadetPinball/TEdgeSegment.cpp
        SpaceCadetPinball/TEdgeSegment.h
        SpaceCadetPinball/TFlagSpinner.cpp
        SpaceCadetPinball/TFlagSpinner.h
        SpaceCadetPinball/TFlipper.cpp
        SpaceCadetPinball/TFlipper.h
        SpaceCadetPinball/TFlipperEdge.cpp
        SpaceCadetPinball/TFlipperEdge.h
        SpaceCadetPinball/TGate.cpp
        SpaceCadetPinball/TGate.h
        SpaceCadetPinball/THole.cpp
        SpaceCadetPinball/THole.h
        SpaceCadetPinball/timer.cpp
        SpaceCadetPinball/timer.h
        SpaceCadetPinball/TKickback.cpp
        SpaceCadetPinball/TKickback.h
        SpaceCadetPinball/TKickout.cpp
        SpaceCadetPinball/TKickout.h
        SpaceCadetPinball/TLight.cpp
        SpaceCadetPinball/TLight.h
        SpaceCadetPinball/TLightBargraph.cpp
        SpaceCadetPinball/TLightBargraph.h
        SpaceCadetPinball/TLightGroup.cpp
        SpaceCadetPinball/TLightGroup.h
        SpaceCadetPinball/TLightRollover.cpp
        SpaceCadetPinball/TLightRollover.h
        SpaceCadetPinball/TLine.cpp
        SpaceCadetPinball/TLine.h
        SpaceCadetPinball/TOneway.cpp
        SpaceCadetPinball/TOneway.h
        SpaceCadetPinball/TPinballComponent.cpp
        SpaceCadetPinball/TPinballComponent.h
        SpaceCadetPinball/TPinballTable.cpp
        SpaceCadetPinball/TPinballTable.h
        SpaceCadetPinball/TPlunger.cpp
        SpaceCadetPinball/TPlunger.h
        SpaceCadetPinball/TPopupTarget.cpp
        SpaceCadetPinball/TPopupTarget.h
        SpaceCadetPinball/TRamp.cpp
        SpaceCadetPinball/TRamp.h
        SpaceCadetPinball/translations.cpp
        SpaceCadetPinball/translations.h
        SpaceCadetPinball/TRollover.cpp
        SpaceCadetPinball/TRollover.h
        SpaceCadetPinball/TSink.cpp
        SpaceCadetPinball/TSink.h
        SpaceCadetPinball/TSoloTarget.cpp
        SpaceCadetPinball/TSoloTarget.h
        SpaceCadetPinball/TSound.cpp
        SpaceCadetPinball/TSound.h
        SpaceCadetPinball/TTableLayer.cpp
        SpaceCadetPinball/TTableLayer.h
        SpaceCadetPinball/TTextBox.cpp
        SpaceCadetPinball/TTextBox.h
        SpaceCadetPinball/TTextBoxMessage.cpp
        SpaceCadetPinball/TTextBoxMessage.h
        SpaceCadetPinball/TTimer.cpp
        SpaceCadetPinball/TTimer.h
        SpaceCadetPinball/TTripwire.cpp
        SpaceCadetPinball/TTripwire.h
        SpaceCadetPinball/TWall.cpp
        SpaceCadetPinball/TWall.h
        SpaceCadetPinball/winmain.cpp
        SpaceCadetPinball/winmain.h
        SpaceCadetPinball/update.cpp
        SpaceCadetPinball/update.h
        SpaceCadetPinball/zdrv.cpp
        SpaceCadetPinball/zdrv.h
        SpaceCadetPinball/vkrender.cpp
        SpaceCadetPinball/vkrender.h
        SpaceCadetPinball/table_shader_spv.h
        SpaceCadetPinball/imconfig.h
        SpaceCadetPinball/imgui_internal.h
        SpaceCadetPinball/imgui.cpp
        SpaceCadetPinball/imgui.h
        SpaceCadetPinball/imgui_draw.cpp
        SpaceCadetPinball/imgui_widgets.cpp
        SpaceCadetPinball/imgui_tables.cpp
        SpaceCadetPinball/imgui_demo.cpp
        SpaceCadetPinball/imgui_impl_glfw.cpp
        SpaceCadetPinball/imgui_impl_glfw.h
        SpaceCadetPinball/imgui_impl_vulkan.cpp
        SpaceCadetPinball/imgui_impl_vulkan.h
        SpaceCadetPinball/imstb_textedit.h
        SpaceCadetPinball/imstb_rectpack.h
        SpaceCadetPinball/imstb_truetype.h
        SpaceCadetPinball/miniaudio.h
        SpaceCadetPinball/miniaudio_config.h
        SpaceCadetPinball/miniaudio_impl.cpp
        SpaceCadetPinball/DebugOverlay.cpp
        SpaceCadetPinball/DebugOverlay.h
        )

# On Windows, include resource file with the icon
if(WIN32)
    set_source_files_properties(SpaceCadetPinball/SpaceCadetPinball.rc LANGUAGE RC)
    list(APPEND SOURCE_FILES SpaceCadetPinball/SpaceCadetPinball.rc)
endif(WIN32)

add_executable(SpaceCadetPinball ${SOURCE_FILES})

# Skip pch on foreign code
set_source_files_properties(
        SpaceCadetPinball/imgui.cpp
        SpaceCadetPinball/imgui_draw.cpp
        SpaceCadetPinball/imgui_widgets.cpp
        SpaceCadetPinball/imgui_tables.cpp
        SpaceCadetPinball/imgui_demo.cpp
        SpaceCadetPinball/imgui_impl_glfw.cpp
        SpaceCadetPinball/imgui_impl_vulkan.cpp
        SpaceCadetPinball/miniaudio_impl.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS 1
)

if(${CMAKE_VERSION} VERSION_GREATER "3.16.0" OR ${CMAKE_VERSION} VERSION_EQUAL "3.16.0")
    target_precompile_headers(SpaceCadetPinball
            PUBLIC
            SpaceCadetPinball/pch.h
            )
endif()

target_link_libraries(SpaceCadetPinball glfw Vulkan::Vulkan)
if(SCP_USE_RUST_GEOMETRY)
    add_dependencies(SpaceCadetPinball spacecadet_rust_geometry)
    target_compile_definitions(SpaceCadetPinball PRIVATE SCP_USE_RUST_GEOMETRY)
    if(SCP_USE_RUST_WORLD)
        target_compile_definitions(SpaceCadetPinball PRIVATE SCP_USE_RUST_WORLD)
    endif()
    target_link_libraries(SpaceCadetPinball spacecadet_geometry)
    if(WIN32)
        # The Rust static library carries its own std. The objects the linker
        # pulls in for panic handling also reference Win32 networking, registry
        # and file APIs, which a plain C++ link never resolves on its own.
        target_link_libraries(SpaceCadetPinball ws2_32 ntdll userenv bcrypt advapi32 ole32 shell32)
        if(MSVC)
            target_link_libraries(SpaceCadetPinball synchronization)
        endif()
    endif()
else()
    message(STATUS "Rust geometry disabled - building the C++ maths reference implementation")
endif()

# Bake the release version into the binary (winmain::Version reads this).
target_compile_definitions(SpaceCadetPinball PRIVATE
	SCP_VERSION="${PROJECT_VERSION}")

# Enable MIDI music when FluidSynth is available, otherwise the stub is used.
if(FLUIDSYNTH_INCLUDE_DIR AND FLUIDSYNTH_LIBRARY)
    target_include_directories(SpaceCadetPinball PRIVATE ${FLUIDSYNTH_INCLUDE_DIR})
    target_compile_definitions(SpaceCadetPinball PRIVATE HAS_FLUIDSYNTH)
    target_link_libraries(SpaceCadetPinball ${FLUIDSYNTH_LIBRARY})
    message(STATUS "FluidSynth found: ${FLUIDSYNTH_LIBRARY} - MIDI music enabled")
else()
    message(STATUS "FluidSynth not found - MIDI music disabled")
endif()

# miniaudio runtime-loads its backends and needs threads/dl/math on Unix.
if(UNIX)
    target_link_libraries(SpaceCadetPinball dl pthread m)
endif()

# miniaudio's Win32 backends need winmm; ole32/shell32 are already part of
# CMake's default MSVC standard libraries.
if(WIN32)
    target_link_libraries(SpaceCadetPinball winmm)
endif()

include(GNUInstallDirs)
install(TARGETS "${PROJECT_NAME}"
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
	BUNDLE DESTINATION .)

if(UNIX AND NOT APPLE)
	install(FILES "${CMAKE_SOURCE_DIR}/Platform/Linux/${PROJECT_NAME}.desktop" DESTINATION "share/applications")
	install(FILES "${CMAKE_SOURCE_DIR}/Platform/Linux/${PROJECT_NAME}.metainfo.xml" DESTINATION "share/metainfo")
	foreach(S 16 32 48 128 192)
		install(FILES "${CMAKE_SOURCE_DIR}/${PROJECT_NAME}/Icon_${S}x${S}.png" DESTINATION
			"share/icons/hicolor/${S}x${S}/apps" RENAME "${PROJECT_NAME}.png")
	endforeach(S)
endif()

# Version: see the VERSION file at the repo root (single source of truth).
# Packaging: the binary plus desktop integration. Game data is provided by the
# user (it is copyrighted by Microsoft and cannot be redistributed).
set(CPACK_PACKAGE_NAME "SpaceCadetPinball")
set(CPACK_PACKAGE_VENDOR "k4zmu2a")
set(CPACK_PACKAGE_VERSION "${SPACECADETPINBALL_VERSION}")
set(CPACK_PACKAGE_CONTACT "k4zmu2a <k4zmu2a@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Decompilation of 3D Pinball for Windows - Space Cadet")
set(CPACK_PACKAGE_DESCRIPTION "Reverse engineering of '3D Pinball for Windows - Space Cadet', a game bundled with Windows.")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ;ZIP;DEB;RPM")

# Debian
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
# Music is a first-class feature, not an optional extra, so the General MIDI
# SoundFont is a hard dependency: an install with --no-install-recommends must
# not lose music silently. libfluidsynth3 needs no entry here, dpkg-shlibdeps
# adds it from the binary's DT_NEEDED.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "fluid-soundfont-gm")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# RPM
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
# CPack does not derive RPM dependencies from the linked libraries, so both the
# library and the SoundFont are named explicitly, and as Requires rather than
# Recommends for the same reason as the .deb above.
set(CPACK_RPM_PACKAGE_REQUIRES "fluidsynth-libs, fluid-soundfont-gm")

include(CPack)
