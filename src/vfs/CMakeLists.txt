add_library(mm2_vfs STATIC
    DirectoryFs.cpp
    IsoImage.cpp
    DaveArchive.cpp
    AresArchive.cpp
    Vfs.cpp
    GameSource.cpp)
target_link_libraries(mm2_vfs
    PUBLIC OpenMM2::core
    PRIVATE OpenMM2::miniz)
add_library(OpenMM2::vfs ALIAS mm2_vfs)
