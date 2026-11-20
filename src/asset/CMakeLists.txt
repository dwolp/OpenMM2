add_library(mm2_asset STATIC
    Image.cpp
    Pkg.cpp
    Bms.cpp
    Mm1TexSheet.cpp
    Mm1Bound.cpp
    Dlp.cpp
    Mtx.cpp
    Bound.cpp
    VehicleModel.cpp
    Ped.cpp)
target_link_libraries(mm2_asset
    PUBLIC OpenMM2::data OpenMM2::core
    PRIVATE OpenMM2::miniz OpenMM2::stb)
add_library(OpenMM2::asset ALIAS mm2_asset)
