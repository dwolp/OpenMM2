configure_file(Version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/core/Version.h" @ONLY)

add_library(mm2_core STATIC
    Log.cpp
    Ini.cpp
    Paths.cpp
    StringUtil.cpp
    File.cpp
    Math.cpp
    Libm.cpp
    Trace.cpp)
target_include_directories(mm2_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mm2_core PUBLIC OpenMM2::options Threads::Threads)
if(WIN32)
    target_link_libraries(mm2_core PUBLIC shell32 ole32)
endif()
add_library(OpenMM2::core ALIAS mm2_core)
