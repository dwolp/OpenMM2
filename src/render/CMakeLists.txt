add_library(mm2_render STATIC
    Types.cpp
    DisplaySettings.cpp
    Projection.cpp
    Renderer.cpp
    ImageUtil.cpp
    ImGuiRenderer.cpp
    Overlay2D.cpp)
target_link_libraries(mm2_render
    PUBLIC OpenMM2::platform
    PRIVATE OpenMM2::miniz)

if(OPENMM2_ENABLE_VULKAN)
    target_sources(mm2_render PRIVATE vulkan/VulkanDevice.cpp vulkan/VmaImpl.cpp)
    target_link_libraries(mm2_render PRIVATE volk::volk VulkanMemoryAllocator)
    target_compile_definitions(mm2_render PUBLIC OPENMM2_HAS_VULKAN)
endif()

if(OPENMM2_ENABLE_OPENGL)
    target_sources(mm2_render PRIVATE opengl/GlApi.cpp opengl/GlDevice.cpp)
    target_compile_definitions(mm2_render PUBLIC OPENMM2_HAS_OPENGL)
endif()

openmm2_embed_shaders(mm2_render
    PRELUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    SOURCES
        shaders/mesh.vert
        shaders/mesh.frag
        shaders/overlay.vert
        shaders/overlay.frag
        shaders/composite.vert
        shaders/composite.frag
        shaders/bloom.frag
        shaders/depthcopy.frag
        shaders/depthcopy_ms.frag
        shaders/ao.frag
        shaders/aoblur.frag
        shaders/ssr.frag
        shaders/shafts.frag
        shaders/motionblur.frag)

add_library(OpenMM2::render ALIAS mm2_render)
