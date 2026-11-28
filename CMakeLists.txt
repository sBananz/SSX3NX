cmake_minimum_required(VERSION 3.20)

# ============================================================================
# SSX 3 - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(ssx3_recomp C)

# Static C runtime: the release runs without the Visual C++ Redistributable
# (a missing VCRUNTIME140.dll would stop launcher.exe before it opens).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# WebAssembly (emcmake cmake ..., see docs/WEB.md): every guest and host
# thread is a pthread, so the whole tree is built with shared memory.
if(EMSCRIPTEN)
    add_compile_options(-pthread)
    add_link_options(-pthread)
endif()

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Nintendo Switch (SSX3NX): devkitPro's Switch toolchain file sets
# NINTENDO_SWITCH and -D__SWITCH__. See PORTING_PLAN.md and build_switch.sh.
if(NINTENDO_SWITCH)
    # Vulkan (vkr on mesa-switch NVK) needs NVK_SDK; the software rasteriser
    # does not (-DSSX3_VULKAN=OFF builds without it).
    set(NVK_SDK "" CACHE PATH "mesa-switch NVK install prefix (portlibs/switch of the NVK SDK)")
    if(EXISTS "${NVK_SDK}/lib/cmake/Vulkan/VulkanConfig.cmake")
        set(_ssx3nx_vk_default ON)
    else()
        set(_ssx3nx_vk_default OFF)
    endif()
    option(SSX3_VULKAN "Build the Vulkan renderer" ${_ssx3nx_vk_default})
    # The toolkit's input layer links SDL2. Found here first and made global so
    # every subdirectory sees the same targets, and -- with NVK -- its static
    # link list gets mesa-switch's EGL/glapi instead of devkitPro's Mesa 20.1
    # (the two clash; after OpenTrickyNX's xboxrecomp/src/d3d/CMakeLists.txt).
    find_package(SDL2 REQUIRED)
    foreach(_t SDL2::SDL2-static SDL2::SDL2 SDL2::SDL2main)
        if(TARGET ${_t})
            get_target_property(_aliased ${_t} ALIASED_TARGET)
            if(NOT _aliased)
                set_target_properties(${_t} PROPERTIES IMPORTED_GLOBAL TRUE)
            endif()
        endif()
    endforeach()
    if(SSX3_VULKAN AND TARGET SDL2::SDL2-static AND EXISTS "${NVK_SDK}/lib/libEGL.a")
        get_target_property(_sdl_libs SDL2::SDL2-static INTERFACE_LINK_LIBRARIES)
        set(_sdl_new "")
        foreach(l IN LISTS _sdl_libs)
            if(l STREQUAL "EGL" OR l STREQUAL "-lEGL")
                list(APPEND _sdl_new "${NVK_SDK}/lib/libEGL.a")
            elseif(l STREQUAL "glapi" OR l STREQUAL "-lglapi")
                list(APPEND _sdl_new "${NVK_SDK}/lib/libglapi.a")
            elseif(NOT (l STREQUAL "drm_nouveau" OR l STREQUAL "-ldrm_nouveau"))
                list(APPEND _sdl_new "${l}")
            endif()
        endforeach()
        set_target_properties(SDL2::SDL2-static PROPERTIES INTERFACE_LINK_LIBRARIES "${_sdl_new}")
    endif()
    # The game's x86 registers (eax..esp, the x87 stack top) in CPU registers
    # w19-w26 instead of __thread variables -- every access was a thread-
    # pointer call (-mtp=soft) plus a reload after each guest store. Every C
    # file of the program, the toolkit's included, gets the header first so
    # none of them uses those registers for anything else
    # (xboxrecomp/src/kernel/xbox_guest_regs.h; OpenTrickyNX's OT_GUEST_REGS).
    option(SSX3NX_GUEST_REGS "Keep the game's x86 registers in CPU registers" ON)
    if(SSX3NX_GUEST_REGS)
        add_compile_definitions(XBOX_GUEST_REGS=1)
        add_compile_options("$<$<COMPILE_LANGUAGE:C>:-include${XBOXRECOMP_DIR}/src/kernel/xbox_guest_regs.h>")
    endif()
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NINTENDO_SWITCH AND NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR
        "src/recomp/gen/ is empty: the translated game code has not been generated "
        "(scripts/regen.sh, from your own default.xbe).")
endif()

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/log.c
    src/journal.c
    src/mark.c
    src/live.c
    src/autopilot.c
    src/ps2_controls.c
    src/button_icons.c
    src/perf.c
    src/guest_prof.c
    src/recomp_manual.c
    src/save_repair.c
    src/options_autoload.c
    src/ssx3_overrides.c
    src/thread_dump.c
    src/game_state.c
    src/camera_mode.c
    src/autonav.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)
if(NINTENDO_SWITCH)
    # The libnx host layer (src/switch/). No SIGSEGV MMIO traps: guest device
    # accesses are checked through the page map (XBOX_PAGEMAP), as on the web.
    list(APPEND GAME_SOURCES src/switch/nx_platform.c)
else()
    list(APPEND GAME_SOURCES src/mmio_posix.c src/web_platform.c)
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
else()
    add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
endif()

# The build's version for the session log (src/journal.c)
add_custom_target(ssx3_build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/build_info.h
            -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/build_info.h
    VERBATIM)
add_dependencies(${PROJECT_NAME} ssx3_build_info)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR})

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        xinput
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
elseif(NINTENDO_SWITCH)
    # libnx (and its newlib): no libdl, no -rdynamic (symbols for crash reports
    # come from SSX3NX.sym beside the .nro, written below).
    # libpng / zlib (devkitPro portlibs): a user controller picture,
    # icons/pro_controller.png (src/button_icons.c).
    target_include_directories(${PROJECT_NAME} PRIVATE $ENV{DEVKITPRO}/portlibs/switch/include /opt/devkitpro/portlibs/switch/include)
    target_link_directories(${PROJECT_NAME} PRIVATE $ENV{DEVKITPRO}/portlibs/switch/lib /opt/devkitpro/portlibs/switch/lib)
    target_link_libraries(${PROJECT_NAME} PRIVATE png z nx m)
    # The generated code and the host sources were written for x86 hosts;
    # these keep their warnings from stopping the build.
    target_compile_options(${PROJECT_NAME} PRIVATE -Wno-int-conversion
        -Wno-incompatible-pointer-types -fno-strict-aliasing -fwrapv)
    # The translated game code at -O3 (after the build type's -O2, so it wins).
    # The CPU flags are devkitA64's: -march=armv8-a+crc+crypto
    # -mtune=cortex-a57 (-mcpu=cortex-a57 would drop the crypto extension and
    # conflict with them). No -ffast-math: float stays IEEE-754.
    option(SSX3NX_GEN_O3 "Build src/recomp/gen/*.c at -O3" ON)
    if(SSX3NX_GEN_O3)
        set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3")
    endif()
elseif(EMSCRIPTEN)
    # main() runs on a worker (PROXY_TO_PTHREAD): guest code blocks freely and
    # the browser thread stays free for input, audio and presenting. Guest
    # call chains are native call chains, so threads get deep stacks.
    # One fixed 2 GB memory: growing a shared memory is slow, and every
    # thread's view would have to follow. Guest memory is ~155 MB of it.
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".js")
    target_link_options(${PROJECT_NAME} PRIVATE
        --profiling-funcs
        -sPROXY_TO_PTHREAD=1
        -sINITIAL_MEMORY=2GB
        -sALLOW_MEMORY_GROWTH=0
        -sSTACK_SIZE=8MB
        -sDEFAULT_PTHREAD_STACK_SIZE=4MB
        -sPTHREAD_POOL_SIZE=40
        -sWASMFS=1
        -sENVIRONMENT=web,worker
        -sMODULARIZE=1
        -sEXPORT_NAME=SSX3
        -sEXIT_RUNTIME=0
        -sINVOKE_RUN=0
        -sEXPORTED_FUNCTIONS=_main,_web_key,_web_pad,_web_frame,_web_audio_ring,_malloc,_free
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,ENV,callMain
    )
    # The page next to the module: build-web/ is what tools/serve_web.py serves.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
            ${CMAKE_CURRENT_SOURCE_DIR}/web/main.js
            ${CMAKE_CURRENT_SOURCE_DIR}/web/audio-worklet.js
            ${CMAKE_CURRENT_SOURCE_DIR}/web/gpu.js
            ${CMAKE_CURRENT_SOURCE_DIR}/web/xbox.wgsl
            $<TARGET_FILE_DIR:${PROJECT_NAME}>)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE m pthread dl)
    # Symbol names (sub_XXXXXXXX) in the crash handler's backtrace.
    target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
endif()

# Vulkan renderer for the NV2A executor (RECOMP_GPU=vk), see docs/VULKAN.html.
# Both hosts; on Windows it needs the Vulkan SDK (tools\build.bat finds it).
if(EMSCRIPTEN)
    set(SSX3_VULKAN_DEFAULT OFF)
else()
    set(SSX3_VULKAN_DEFAULT ON)
endif()
option(SSX3_VULKAN "Build the Vulkan renderer" ${SSX3_VULKAN_DEFAULT})   # Switch: set above
if(EMSCRIPTEN)
    # The same renderer interface (src/vk/vkr.h, driven by vkr_backend.c) on
    # WebGPU: src/web/wgr.c records frames, web/gpu.js replays them on the
    # page thread. RECOMP_GPU=sw keeps the software rasteriser.
    target_sources(${PROJECT_NAME} PRIVATE src/vk/vkr_backend.c src/web/wgr.c)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vk ${XBOXRECOMP_DIR}/src/kernel)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SSX3_VULKAN=1)
elseif(SSX3_VULKAN)
    add_subdirectory(src/vk ${CMAKE_BINARY_DIR}/vk)
    target_sources(${PROJECT_NAME} PRIVATE src/vk/vkr_backend.c src/vk/vkr_hdtex.c)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vk ${XBOXRECOMP_DIR}/src/kernel)
    target_link_libraries(${PROJECT_NAME} PRIVATE vkr)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SSX3_VULKAN=1)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # build/ssx3_recomp.map: every generated function is a symbol in it, so
        # xboxrecomp/tools/symbolize.py turns a crash RIP into sub_XXXXXXXX.
        /MAP
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    # A few generated functions are thousands of lines of gotos (CRT/XAPI and
    # game code); MSVC's full /O2 on them is superlinear -- recomp_0064.c
    # alone took ~1000 s and was the whole build. This internal switch limits
    # optimization inside such huge functions only: that file now compiles in
    # ~70 s, everything else is optimized as before.
    target_compile_options(${PROJECT_NAME} PRIVATE /d2ReducedOptimizeHugeFunctions)
endif()

# ── launcher.exe ─────────────────────────────────────────────────
# Settings GUI that starts the game (src/launcher/launcher.c): game folder
# picker with a check of default.xbe, display options, controllers.
if(WIN32)
    add_executable(ssx3_launcher WIN32 src/launcher/launcher.c)
    set_target_properties(ssx3_launcher PROPERTIES OUTPUT_NAME launcher)
    target_link_libraries(ssx3_launcher PRIVATE comctl32 ole32 shell32 xinput winhttp)
    target_include_directories(ssx3_launcher PRIVATE ${CMAKE_BINARY_DIR})   # build_info.h
    add_dependencies(ssx3_launcher ssx3_build_info)
else()
    # The same launcher on SDL2 (src/launcher/launcher_sdl.c): text through
    # SDL2_ttf in the desktop's font (fontconfig). Optional: without them
    # the game builds as before.
    find_package(SDL2 QUIET)
    find_package(SDL2_ttf QUIET)
    find_package(Fontconfig QUIET)
    if(TARGET SDL2::SDL2 AND TARGET SDL2_ttf::SDL2_ttf AND TARGET Fontconfig::Fontconfig)
        add_executable(ssx3_launcher src/launcher/launcher_sdl.c)
        set_target_properties(ssx3_launcher PROPERTIES OUTPUT_NAME launcher)
        target_link_libraries(ssx3_launcher PRIVATE SDL2::SDL2 SDL2_ttf::SDL2_ttf
                              Fontconfig::Fontconfig)
        target_include_directories(ssx3_launcher PRIVATE ${CMAKE_BINARY_DIR})   # build_info.h
        add_dependencies(ssx3_launcher ssx3_build_info)
    elseif(NOT NINTENDO_SWITCH)
        message(STATUS "launcher: SDL2_ttf or fontconfig not found, not built")
    endif()
endif()

# ── Nintendo Switch: SSX3NX.nro ─────────────────────────────────
# No launcher on the console: "SSX 3 env.txt" beside the .nro replaces it
# (src/switch/nx_platform.c). Title, author and version on the Homebrew Menu;
# SSX3NX.sym (addresses, sizes, names) for reading crash addresses.
if(NINTENDO_SWITCH)
    set(SSX3NX_VERSION "0.1.0" CACHE STRING "SSX3NX version on the Homebrew Menu")
    nx_generate_nacp(ssx3nx.nacp
        NAME "SSX 3 (SSX3NX)"
        AUTHOR "SSX3NX contributors"
        VERSION "${SSX3NX_VERSION}")
    set(_nro_icon "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/switch/icon.jpg")
        set(_nro_icon ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/switch/icon.jpg")
    endif()
    nx_create_nro(${PROJECT_NAME}
        OUTPUT "${CMAKE_BINARY_DIR}/SSX3NX.nro"
        NACP ssx3nx.nacp
        ${_nro_icon})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND sh -c "\"$0\" -n -S --defined-only \"$1\" | grep ' [TtWw] ' > \"$2\""
                "${CMAKE_NM}" "$<TARGET_FILE:${PROJECT_NAME}>" "${CMAKE_BINARY_DIR}/SSX3NX.sym"
        VERBATIM)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
