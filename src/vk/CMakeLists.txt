# Vulkan renderer. Standalone: `cmake -S src/vk -B build-vk && ninja -C build-vk`
# builds vkr_selftest. The game links the vkr library target.
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
    project(vkr C)
endif()

if(NINTENDO_SWITCH)
    # Switch: NVK from mesa-switch, linked in statically (no loader). NVK_SDK
    # is its install prefix (the "mesa-switch-nvk-sdk" download's
    # portlibs/switch). After OpenTrickyNX's xboxrecomp/src/d3d/CMakeLists.txt.
    set(NVK_SDK "" CACHE PATH "mesa-switch install prefix (include/vulkan, lib/libvulkan.a)")
    if(NOT EXISTS "${NVK_SDK}/lib/cmake/Vulkan/VulkanConfig.cmake")
        message(FATAL_ERROR "SSX3_VULKAN on the Switch needs NVK_SDK (mesa-switch with NVK); "
                            "got '${NVK_SDK}'. Or configure with -DSSX3_VULKAN=OFF.")
    endif()
    # its dependencies from the SDK itself when they are there, else portlibs
    foreach(dep ELF:elf EXPAT:expat ZSTD:zstd Z:z)
        string(REPLACE ":" ";" dep "${dep}")
        list(GET dep 0 var)
        list(GET dep 1 lib)
        if(EXISTS "${NVK_SDK}/lib/lib${lib}.a")
            set(_VULKAN_SWITCH_${var}_LIBRARY "${NVK_SDK}/lib/lib${lib}.a" CACHE FILEPATH "" FORCE)
        endif()
    endforeach()
    # NVK calls nothing in libelf, which devkitPro does not package
    find_library(_VULKAN_SWITCH_ELF_LIBRARY elf)
    if(NOT _VULKAN_SWITCH_ELF_LIBRARY)
        set(_VULKAN_SWITCH_ELF_LIBRARY m CACHE STRING "" FORCE)
    endif()
    # set ahead: the Switch toolchain only searches its own sysroot
    set(Vulkan_LIBRARY "${NVK_SDK}/lib/libvulkan.a" CACHE FILEPATH "" FORCE)
    set(Vulkan_INCLUDE_DIR "${NVK_SDK}/include" CACHE PATH "" FORCE)
    include("${NVK_SDK}/lib/cmake/Vulkan/VulkanConfig.cmake")
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR "NVK_SDK=${NVK_SDK}: VulkanConfig.cmake found no usable Vulkan")
    endif()
else()
    find_package(Vulkan REQUIRED)
endif()

# GLSL -> SPIR-V on the build host (SPIR-V is target-independent): glslc, or
# glslangValidator where only that is installed (the ssx3nx-build image).
find_program(GLSLC glslc)
if(GLSLC)
    set(VKR_GLSL ${GLSLC} --target-env=vulkan1.3 -O)
else()
    find_program(GLSLANGVALIDATOR glslangValidator)
    if(NOT GLSLANGVALIDATOR)
        message(FATAL_ERROR "Neither glslc nor glslangValidator found (GLSL -> SPIR-V)")
    endif()
    set(VKR_GLSL ${GLSLANGVALIDATOR} -V --target-env vulkan1.3)
endif()

set(VKR_GEN "${CMAKE_CURRENT_BINARY_DIR}/vkr_gen")
file(MAKE_DIRECTORY "${VKR_GEN}")

# vkr_shader(<stage> [<base>]): shaders/<base>.<stage> (default base xbox)
# -> vkr_gen/<name>_spv.h with the array <name>_spv, name = xbox_<stage>, or
# <base>_<stage> for another base.
function(vkr_shader stage)
    set(base "xbox")
    set(name "xbox_${stage}")
    if(ARGC GREATER 1)
        set(base "${ARGV1}")
        set(name "${ARGV1}_${stage}")
    endif()
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${base}.${stage}")
    set(spv "${VKR_GEN}/${name}.spv")
    set(hdr "${VKR_GEN}/${name}_spv.h")
    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND ${VKR_GLSL} -o "${spv}" "${src}"
        COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${hdr} -DNAME=${name}_spv
                -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_common.glsl"
        VERBATIM)
    set(VKR_HEADERS ${VKR_HEADERS} "${hdr}" PARENT_SCOPE)
endfunction()
# vkr_shader_variant(<stage> <base> <name> <define> [<define>...]): the same
# source built with -D<define> into <name>_spv.
function(vkr_shader_variant stage base name def)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${base}.${stage}")
    set(spv "${VKR_GEN}/${name}.spv")
    set(hdr "${VKR_GEN}/${name}_spv.h")
    set(defs "")
    foreach(d ${def} ${ARGN})
        list(APPEND defs "-D${d}")
    endforeach()
    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND ${VKR_GLSL} ${defs} -o "${spv}" "${src}"
        COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${hdr} -DNAME=${name}_spv
                -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_spv.cmake"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_common.glsl"
        VERBATIM)
    set(VKR_HEADERS ${VKR_HEADERS} "${hdr}" PARENT_SCOPE)
endfunction()
vkr_shader(vert)
vkr_shader(frag)
vkr_shader_variant(frag xbox xbox_frag_wdepth W_DEPTH)
# RECOMP_MBLUR rider tags: the draw's tag in the frame's alpha, the real alpha
# as the second blend source
vkr_shader_variant(frag xbox xbox_frag_tags TAGS)
vkr_shader_variant(frag xbox xbox_frag_wdepth_tags W_DEPTH TAGS)
vkr_shader(vert xbox_vsh)
vkr_shader(vert crt)
vkr_shader(frag crt)
vkr_shader(frag scene)
# Switch: the frame onto the swapchain image (gamma ramp, FPS overlay)
vkr_shader(frag present)
vkr_shader(frag speedlines)
# with MSAA the depth it reads is multisampled
vkr_shader_variant(frag scene scene_frag_ms MS_DEPTH)

add_library(vkr STATIC vkr.c ${VKR_HEADERS})
# vkr_backend.c is added by the game build (needs the toolkit headers).
target_include_directories(vkr PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${VKR_GEN}")
if(NINTENDO_SWITCH)
    # the package's own list in one link group ($<LINK_GROUP> is not available
    # to C targets with this toolchain), and the ICD entry points kept
    target_include_directories(vkr PUBLIC ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(vkr PUBLIC
        -Wl,--start-group ${_VULKAN_SWITCH_LIBRARIES} -Wl,--end-group nx stdc++ m
        -Wl,-u,vk_icdGetInstanceProcAddr -Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion -pthread)
    return()   # no selftest on the console
endif()
target_link_libraries(vkr PUBLIC Vulkan::Vulkan)

add_executable(vkr_selftest vkr_selftest.c)
target_link_libraries(vkr_selftest PRIVATE vkr)
target_sources(vkr_selftest PRIVATE ../log.c)   # vkr.c logs through log_msg
