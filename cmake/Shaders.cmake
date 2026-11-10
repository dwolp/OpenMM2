# Shader build pipeline.
#
# Shaders are written once in GLSL without a #version line, using the macros
# defined in src/render/shaders/prelude_*.glsl. At build time each shader is
# combined with a backend prelude:
#   * Vulkan: prelude_vk.glsl + source -> SPIR-V (glslc / glslangValidator)
#   * OpenGL: prelude_gl.glsl + source -> GLSL 3.30 source text
# and the results are embedded into a generated C++ source file.

find_program(OPENMM2_GLSLC glslc)
find_program(OPENMM2_GLSLANG glslangValidator)

if(OPENMM2_ENABLE_VULKAN AND NOT OPENMM2_GLSLC AND NOT OPENMM2_GLSLANG)
    message(FATAL_ERROR "Vulkan renderer requires glslc or glslangValidator to compile shaders "
                        "(install shaderc/glslang or the Vulkan SDK).")
endif()

set(OPENMM2_SHADER_TOOL "${CMAKE_CURRENT_LIST_DIR}/ShaderTool.cmake")

# openmm2_embed_shaders(<target> PRELUDE_DIR <dir> SOURCES <files...>
#                       [RAY_SOURCES <files...>])
#
# Adds a generated source to <target> that defines, for each shader file
# `name.stage`, the symbols declared in render/ShaderBlobs.h. RAY_SOURCES
# are also compiled for Vulkan 1.2 with ray queries (prelude_vk_ray.glsl
# before prelude_vk.glsl: MM2_RAY_QUERY), as `name_ray_stage`.
function(openmm2_embed_shaders target)
    cmake_parse_arguments(ARG "" "PRELUDE_DIR" "SOURCES;RAY_SOURCES" ${ARGN})
    set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY "${gen_dir}")

    set(outputs)
    set(entries)
    foreach(src IN LISTS ARG_SOURCES)
        get_filename_component(abs "${src}" ABSOLUTE)
        get_filename_component(fname "${src}" NAME)
        string(REPLACE "." "_" ident "${fname}")
        string(REGEX REPLACE "^.*\\." "" stage "${fname}")

        if(OPENMM2_ENABLE_VULKAN)
            set(vk_src "${gen_dir}/${fname}.vk.glsl")
            set(spv "${gen_dir}/${fname}.spv")
            if(OPENMM2_GLSLC)
                set(compile_cmd "${OPENMM2_GLSLC}" -fshader-stage=${stage} --target-env=vulkan1.1 -O
                                -Werror -o "${spv}" "${vk_src}")
            else()
                set(compile_cmd "${OPENMM2_GLSLANG}" -S ${stage} --target-env vulkan1.1 -o "${spv}" "${vk_src}")
            endif()
            add_custom_command(
                OUTPUT "${spv}"
                COMMAND "${CMAKE_COMMAND}" -DMODE=concat "-DPRELUDE=${ARG_PRELUDE_DIR}/prelude_vk.glsl"
                        "-DINPUT=${abs}" "-DOUTPUT=${vk_src}" -P "${OPENMM2_SHADER_TOOL}"
                COMMAND ${compile_cmd}
                DEPENDS "${abs}" "${ARG_PRELUDE_DIR}/prelude_vk.glsl" "${OPENMM2_SHADER_TOOL}"
                COMMENT "Compiling SPIR-V ${fname}"
                VERBATIM)
            list(APPEND outputs "${spv}")
            list(APPEND entries "spv:${ident}:${spv}")
        endif()

        if(OPENMM2_ENABLE_OPENGL)
            set(gl_src "${gen_dir}/${fname}.gl.glsl")
            # glslangValidator (when available) checks the desktop GLSL at
            # build time, so OpenGL shader errors are not found only at runtime.
            set(validate_cmd)
            if(OPENMM2_GLSLANG)
                set(validate_cmd COMMAND "${OPENMM2_GLSLANG}" -S ${stage} "${gl_src}")
            endif()
            add_custom_command(
                OUTPUT "${gl_src}"
                COMMAND "${CMAKE_COMMAND}" -DMODE=concat "-DPRELUDE=${ARG_PRELUDE_DIR}/prelude_gl.glsl"
                        "-DINPUT=${abs}" "-DOUTPUT=${gl_src}" -P "${OPENMM2_SHADER_TOOL}"
                ${validate_cmd}
                DEPENDS "${abs}" "${ARG_PRELUDE_DIR}/prelude_gl.glsl" "${OPENMM2_SHADER_TOOL}"
                COMMENT "Preparing GLSL ${fname}"
                VERBATIM)
            list(APPEND outputs "${gl_src}")
            list(APPEND entries "glsl:${ident}:${gl_src}")
        endif()
    endforeach()

    if(OPENMM2_ENABLE_VULKAN)
        foreach(src IN LISTS ARG_RAY_SOURCES)
            get_filename_component(abs "${src}" ABSOLUTE)
            get_filename_component(fname "${src}" NAME)
            string(REGEX REPLACE "^.*\\." "" stage "${fname}")
            string(REGEX REPLACE "\\.[^.]*$" "" base "${fname}")
            set(ident "${base}_ray_${stage}")
            set(vk_src "${gen_dir}/${base}_ray.${stage}.vk.glsl")
            set(spv "${gen_dir}/${base}_ray.${stage}.spv")
            if(OPENMM2_GLSLC)
                set(compile_cmd "${OPENMM2_GLSLC}" -fshader-stage=${stage} --target-env=vulkan1.2 -O
                                -Werror -o "${spv}" "${vk_src}")
            else()
                set(compile_cmd "${OPENMM2_GLSLANG}" -S ${stage} --target-env vulkan1.2 -o "${spv}" "${vk_src}")
            endif()
            add_custom_command(
                OUTPUT "${spv}"
                COMMAND "${CMAKE_COMMAND}" -DMODE=concat
                        "-DPRELUDE=${ARG_PRELUDE_DIR}/prelude_vk_ray.glsl|${ARG_PRELUDE_DIR}/prelude_vk.glsl"
                        "-DINPUT=${abs}" "-DOUTPUT=${vk_src}" -P "${OPENMM2_SHADER_TOOL}"
                COMMAND ${compile_cmd}
                DEPENDS "${abs}" "${ARG_PRELUDE_DIR}/prelude_vk_ray.glsl" "${ARG_PRELUDE_DIR}/prelude_vk.glsl"
                        "${OPENMM2_SHADER_TOOL}"
                COMMENT "Compiling SPIR-V ${base}_ray.${stage}"
                VERBATIM)
            list(APPEND outputs "${spv}")
            list(APPEND entries "spv:${ident}:${spv}")
        endforeach()
    endif()

    set(blob_cpp "${gen_dir}/ShaderBlobs.cpp")
    string(REPLACE ";" "|" entries_arg "${entries}")
    add_custom_command(
        OUTPUT "${blob_cpp}"
        COMMAND "${CMAKE_COMMAND}" -DMODE=embed "-DENTRIES=${entries_arg}" "-DOUTPUT=${blob_cpp}"
                -P "${OPENMM2_SHADER_TOOL}"
        DEPENDS ${outputs} "${OPENMM2_SHADER_TOOL}"
        COMMENT "Embedding shaders"
        VERBATIM)
    target_sources(${target} PRIVATE "${blob_cpp}")
endfunction()
