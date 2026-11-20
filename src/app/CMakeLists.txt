# mm2_app: application logic shared by the executable and its tests.
# Frontend screens live in frontend/*.cpp and are picked up automatically.
file(GLOB app_frontend_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/frontend/*.cpp")
add_library(mm2_app STATIC
    Settings.cpp
    GameData.cpp
    CommandLine.cpp
    Context.cpp
    Controls.cpp
    GameInput.cpp
    ForceFeedback.cpp
    App.cpp
    SetupScreen.cpp
    RaceScreen.cpp
    DrawTrace.cpp
    FrameTiming.cpp
    IntroScreen.cpp
    ${app_frontend_sources})
target_link_libraries(mm2_app
    PUBLIC OpenMM2::core OpenMM2::vfs OpenMM2::platform OpenMM2::render OpenMM2::audio OpenMM2::ui
           OpenMM2::game OpenMM2::video OpenMM2::imgui)
if(MSVC)
    # RaceScreen.cpp is large enough to overflow the default object section limit in Debug.
    target_compile_options(mm2_app PRIVATE /bigobj)
endif()
add_library(OpenMM2::app ALIAS mm2_app)

add_executable(openmm2 main.cpp)
target_link_libraries(openmm2 PRIVATE mm2_app SDL3::SDL3)
set_target_properties(openmm2 PROPERTIES WIN32_EXECUTABLE TRUE)
# A built game is portable: settings and driver profiles live in "user" next to the executable (src/core/Paths.h).
# The installers do not copy this file, so an installed game keeps using the per-user folders.
option(OPENMM2_PORTABLE_BUILD "Put portable.txt next to the built openmm2 executable" ON)
if(OPENMM2_PORTABLE_BUILD)
    add_custom_command(TARGET openmm2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/packaging/windows/portable.txt"
                "$<TARGET_FILE_DIR:openmm2>/portable.txt"
        VERBATIM)
endif()

if(WIN32)
    set(OPENMM2_ICON "${PROJECT_SOURCE_DIR}/packaging/icons/openmm2.ico")
    configure_file(openmm2.rc.in "${CMAKE_CURRENT_BINARY_DIR}/openmm2.rc" @ONLY)
    target_sources(openmm2 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/openmm2.rc")
endif()
if(MINGW)
    # UNICODE is defined project-wide, so SDL3 provides wWinMain.
    target_link_options(openmm2 PRIVATE -municode)
endif()
