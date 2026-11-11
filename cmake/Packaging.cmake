# Installation layout and packaging.
#
# Install components:
#   app    the game: openmm2 executable, licences, runtime DLLs, desktop files
#   tools  developer tools (mm2tool)
#
# Layouts:
#   Windows  everything in one directory (openmm2.exe next to its DLLs);
#            licences as LICENSE.txt and licenses\*.txt
#   Linux    FHS: bin/, share/applications, share/metainfo,
#            share/icons/hicolor, share/doc/openmm2, share/licenses/openmm2
#
# Packages:
#   cpack                    Linux: .tar.gz; Windows: portable .zip (with
#                            portable.txt, so settings stay next to the exe)
#   package_installer        Windows: NSIS installer (needs makensis; see
#                            packaging/windows/openmm2.nsi)
#   cpack --config CPackSourceConfig.cmake   source tarball (never includes local/)

include(GNUInstallDirs)

set(OPENMM2_APP_ID "io.github.lowlevelmetal.OpenMM2")
set(OPENMM2_PACKAGING_DIR "${PROJECT_SOURCE_DIR}/packaging")

if(WIN32)
    set(OPENMM2_INSTALL_BINDIR ".")
    set(OPENMM2_INSTALL_DOCDIR ".")
    set(OPENMM2_INSTALL_LICENSEDIR "licenses")
else()
    set(OPENMM2_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
    set(OPENMM2_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/openmm2")
    set(OPENMM2_INSTALL_LICENSEDIR "${CMAKE_INSTALL_DATAROOTDIR}/licenses/openmm2")
endif()

# --- Programs ----------------------------------------------------------------
if(TARGET openmm2)
    install(TARGETS openmm2 RUNTIME DESTINATION "${OPENMM2_INSTALL_BINDIR}" COMPONENT app)
endif()
if(TARGET mm2tool)
    install(TARGETS mm2tool RUNTIME DESTINATION "${OPENMM2_INSTALL_BINDIR}" COMPONENT tools)
endif()

# Fallback fonts (cmake/Fonts.cmake). The game looks in <exe dir>/fonts and
# then <exe dir>/../share/openmm2/fonts (see src/ui/Font.cpp): next to the
# executable on Windows, under share/ on Linux.
if(DEFINED OPENMM2_FONT_DIR AND IS_DIRECTORY "${OPENMM2_FONT_DIR}")
    if(WIN32)
        set(_font_dest "${OPENMM2_INSTALL_BINDIR}/fonts")
    else()
        set(_font_dest "${CMAKE_INSTALL_DATADIR}/openmm2/fonts")
    endif()
    install(DIRECTORY "${OPENMM2_FONT_DIR}/" DESTINATION "${_font_dest}" COMPONENT app)
endif()

# MSVC runtime DLLs next to the executable, so no VC++ redistributable is needed.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${OPENMM2_INSTALL_BINDIR}")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT app)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF) # part of Windows 10+; Windows 7/8 need KB2999226
    include(InstallRequiredSystemLibraries)
endif()

# --- Documentation and licences ----------------------------------------------
if(WIN32)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${OPENMM2_INSTALL_DOCDIR}" RENAME LICENSE.txt
            COMPONENT app)
    if(EXISTS "${PROJECT_SOURCE_DIR}/README.md")
        install(FILES "${PROJECT_SOURCE_DIR}/README.md" DESTINATION "${OPENMM2_INSTALL_DOCDIR}" COMPONENT app)
    endif()
else()
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${OPENMM2_INSTALL_LICENSEDIR}" COMPONENT app)
    if(EXISTS "${PROJECT_SOURCE_DIR}/README.md")
        install(FILES "${PROJECT_SOURCE_DIR}/README.md" DESTINATION "${OPENMM2_INSTALL_DOCDIR}" COMPONENT app)
    endif()
endif()

# Licences of bundled third-party code: every dependency fetched with
# FetchContent and linked into the binaries ships its licence text. Test-only
# dependencies are skipped. Dependencies found as system packages are not
# bundled, so they need no notice.
set(_openmm2_unbundled_deps googletest gtest)
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
endif()
file(GLOB _openmm2_dep_dirs LIST_DIRECTORIES true "${FETCHCONTENT_BASE_DIR}/*-src")
foreach(dep_dir IN LISTS _openmm2_dep_dirs)
    get_filename_component(dep "${dep_dir}" NAME)
    string(REGEX REPLACE "-src$" "" dep "${dep}")
    if(dep IN_LIST _openmm2_unbundled_deps)
        continue()
    endif()
    # Licence at the top of the repository, or one level down (miniupnp keeps
    # one per subproject).
    file(GLOB licenses LIST_DIRECTORIES false
         "${dep_dir}/LICENSE*" "${dep_dir}/LICENCE*" "${dep_dir}/COPYING*"
         "${dep_dir}/*/LICENSE" "${dep_dir}/*/LICENSE.txt" "${dep_dir}/*/LICENSE.md")
    list(SORT licenses)
    if(licenses)
        list(GET licenses 0 license)
        install(FILES "${license}" DESTINATION "${OPENMM2_INSTALL_LICENSEDIR}" RENAME "${dep}.txt" COMPONENT app)
    endif()
endforeach()

# Code adapted from projects that are not fetched dependencies: their notices.
# audio/SynthReverb follows OpenMPT's Waves Reverb emulation (BSD-3-Clause).
install(FILES "${OPENMM2_PACKAGING_DIR}/licenses/openmpt.txt" DESTINATION "${OPENMM2_INSTALL_LICENSEDIR}"
        COMPONENT app)

# --- Linux desktop integration ---------------------------------------------
if(UNIX AND NOT APPLE)
    install(FILES "${OPENMM2_PACKAGING_DIR}/linux/${OPENMM2_APP_ID}.desktop"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications" COMPONENT app)
    install(FILES "${OPENMM2_PACKAGING_DIR}/linux/${OPENMM2_APP_ID}.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/metainfo" COMPONENT app)
    install(FILES "${OPENMM2_PACKAGING_DIR}/icons/openmm2.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
            RENAME "${OPENMM2_APP_ID}.svg" COMPONENT app)
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES "${OPENMM2_PACKAGING_DIR}/icons/openmm2-${size}.png"
                DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/apps"
                RENAME "${OPENMM2_APP_ID}.png" COMPONENT app)
    endforeach()
endif()

# --- Product name -----------------------------------------------------------------
# A build of a fork can install beside OpenMM2 under its own name (its own
# install folder, registry entries, uninstaller and shortcuts; the per-user
# settings and profiles are shared). The packages' file names start with
# OPENMM2_PACKAGE_PREFIX.
set(OPENMM2_PRODUCT_NAME "OpenMM2" CACHE STRING "The installed program's name (Windows installer)")
set(OPENMM2_PACKAGE_PREFIX "OpenMM2" CACHE STRING "The packages' file name prefix")
set(OPENMM2_PRODUCT_HOMEPAGE "${PROJECT_HOMEPAGE_URL}" CACHE STRING "The installer's home page link")

# --- CPack: archives -----------------------------------------------------------
set(CPACK_PACKAGE_NAME "OpenMM2")
set(CPACK_PACKAGE_VENDOR "OpenMM2 contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "OpenMM2")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES ON)
# Ship only our components (one archive): dependencies such as ENet have their
# own install() rules for headers and static libraries, which must not end up
# in the package. CPACK_COMPONENTS_ALL alone does not filter a monolithic
# archive, so list the components to install explicitly.
set(CPACK_COMPONENTS_ALL app tools)
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${PROJECT_BINARY_DIR};${PROJECT_NAME};app;/"
    "${PROJECT_BINARY_DIR};${PROJECT_NAME};tools;/")
set(CPACK_VERBATIM_VARIABLES ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_openmm2_arch x86_64)
else()
    set(_openmm2_arch x86)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(_openmm2_arch arm64)
endif()

if(WIN32)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_FILE_NAME "${OPENMM2_PACKAGE_PREFIX}-${PROJECT_VERSION}-windows-${_openmm2_arch}-portable")
    # Mark the archive as a portable install (see core/Paths.h).
    set(CPACK_PRE_BUILD_SCRIPTS "${OPENMM2_PACKAGING_DIR}/windows/CPackPortable.cmake")
    set(CPACK_OPENMM2_PORTABLE_TXT "${OPENMM2_PACKAGING_DIR}/windows/portable.txt")
else()
    set(CPACK_GENERATOR TGZ)
    set(CPACK_PACKAGE_FILE_NAME "${OPENMM2_PACKAGE_PREFIX}-${PROJECT_VERSION}-linux-${_openmm2_arch}")
endif()

# Source package: the repository minus build output and local game data.
set(CPACK_SOURCE_GENERATOR TGZ)
set(CPACK_SOURCE_PACKAGE_FILE_NAME "OpenMM2-${PROJECT_VERSION}-source")
set(CPACK_SOURCE_IGNORE_FILES
    "/\\.git/" "/\\.cache/" "/\\.vscode/" "/\\.idea/"
    "/build[^/]*/" "/out/" "/install/" "/local/"
    "\\.[Aa][Rr]$" "\\.[Ii][Ss][Oo]$" "\\.[Ii][Cc][Dd]$" "\\.bin$" "\\.cue$"
    "/CMakeUserPresets\\.json$" "/compile_commands\\.json$" "~$" "\\.swp$")

include(CPack)

# --- NSIS installer (Windows targets) --------------------------------------------
# The installer is a hand-written NSIS script (custom game data page, import
# with progress), so it is built by this target rather than CPack's NSIS
# generator. makensis may be the Windows build (NSIS installs it under
# Program Files), a native POSIX build when cross-compiling, or, with
# OPENMM2_MAKENSIS_WINE=ON, a Windows makensis.exe run through Wine (set
# OPENMM2_MAKENSIS to the .exe; paths are passed as Z:\...).
option(OPENMM2_MAKENSIS_WINE "Run OPENMM2_MAKENSIS (a Windows makensis.exe) through Wine" OFF)

# Path as makensis sees it.
function(_openmm2_nsis_path out path)
    if(OPENMM2_MAKENSIS_WINE)
        string(REPLACE "/" "\\" path "Z:${path}")
    else()
        cmake_path(NATIVE_PATH path NORMALIZE path)
    endif()
    set(${out} "${path}" PARENT_SCOPE)
endfunction()

if(WIN32 AND TARGET openmm2)
    set(_pf86 "ProgramFiles(x86)")
    find_program(OPENMM2_MAKENSIS makensis
        PATHS "$ENV{${_pf86}}/NSIS" "$ENV{ProgramFiles}/NSIS" "$ENV{NSIS_HOME}"
        DOC "NSIS compiler used by the package_installer target")
    set(_makensis_cmd "${OPENMM2_MAKENSIS}")
    if(OPENMM2_MAKENSIS_WINE)
        find_program(OPENMM2_WINE NAMES wine wine64 REQUIRED)
        set(_makensis_cmd "${OPENMM2_WINE}" "${OPENMM2_MAKENSIS}")
    endif()
    if(OPENMM2_MAKENSIS)
        set(_stage "${PROJECT_BINARY_DIR}/installer-stage")
        set(_installer "${PROJECT_BINARY_DIR}/${OPENMM2_PACKAGE_PREFIX}-${PROJECT_VERSION}-windows-${_openmm2_arch}-setup.exe")
        _openmm2_nsis_path(_source_native "${PROJECT_SOURCE_DIR}")
        _openmm2_nsis_path(_installer_native "${_installer}")
        _openmm2_nsis_path(_app_stage_native "${_stage}/app")
        _openmm2_nsis_path(_script_native "${OPENMM2_PACKAGING_DIR}/windows/openmm2.nsi")

        set(_install_component_cmds)
        set(_nsis_defines)
        foreach(component app tools)
            if(component STREQUAL "tools" AND NOT TARGET mm2tool)
                continue()
            endif()
            string(TOUPPER "${component}" upper)
            list(APPEND _install_component_cmds
                COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}" --config "$<CONFIG>"
                        --component ${component} --prefix "${_stage}/${component}" --strip
                COMMAND "${CMAKE_COMMAND}" "-DSTAGE=${_stage}/${component}" "-DOUTPUT=${_stage}/${component}.nsh"
                        -DMACRO=OPENMM2_${upper} "-DWINE_PATHS=${OPENMM2_MAKENSIS_WINE}"
                        -P "${OPENMM2_PACKAGING_DIR}/windows/GenerateFileList.cmake")
            _openmm2_nsis_path(_nsh "${_stage}/${component}.nsh")
            list(APPEND _nsis_defines "-D${upper}_FILES_NSH=${_nsh}")
        endforeach()

        add_custom_target(package_installer
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_stage}"
            ${_install_component_cmds}
            COMMAND ${_makensis_cmd} -V3 -INPUTCHARSET UTF8
                    "-DVERSION=${PROJECT_VERSION}"
                    "-DPRODUCT=${OPENMM2_PRODUCT_NAME}"
                    "-DHOMEPAGE=${OPENMM2_PRODUCT_HOMEPAGE}"
                    "-DSTAGE_DIR=${_app_stage_native}"
                    ${_nsis_defines}
                    "-DSOURCE_DIR=${_source_native}"
                    "-DOUTFILE=${_installer_native}"
                    "${_script_native}"
            BYPRODUCTS "${_installer}"
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
            COMMENT "Building ${_installer}"
            VERBATIM)
        add_dependencies(package_installer openmm2)
        if(TARGET mm2tool)
            add_dependencies(package_installer mm2tool)
        endif()
    else()
        message(STATUS "makensis not found: the package_installer target is unavailable")
    endif()
endif()
