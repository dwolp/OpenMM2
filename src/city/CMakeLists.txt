add_library(mm2_city STATIC
    Psdl.cpp
    SdlCollect.cpp
    SdlDraw.cpp
    CityMesh.cpp
    Inst.cpp
    Pvs.cpp
    AiMap.cpp
    PathSet.cpp
    Race.cpp
    Environment.cpp
    CityData.cpp
    RoomLocator.cpp
    RoomInfo.cpp
    Mm1Instances.cpp)
target_link_libraries(mm2_city PUBLIC OpenMM2::data OpenMM2::vfs OpenMM2::core)
add_library(OpenMM2::city ALIAS mm2_city)
