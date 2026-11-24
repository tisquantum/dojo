# Dojo, the PC port of Bushido Blade: the decompiled game, unchanged except where TARGET_PC says so, over the port's
# implementation of the console's libraries and hardware.
#
# The game is 32-bit code through and through (pointers in disc records, 24-bit ordering-table links), so the
# port builds a 32-bit executable linked at a fixed base low enough that every address fits 24 bits.
#
#   cmake -S port -B port/build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build port/build

cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_COMPILER_TARGET i686-pc-windows-msvc CACHE STRING "")
project(dojo_pc C)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(DOJO_DISC "${REPO_ROOT}/game_files/Bushido Blade (USA).bin" CACHE FILEPATH
    "The player's disc image; the build takes the loading-screen picture from its executable")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The loading-screen picture the source includes from the player's own executable.
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${GENERATED_DIR}/content/now_loading.inc
    COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_assets.py ${DOJO_DISC}
            ${GENERATED_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_assets.py ${REPO_ROOT}/config/bushidoblade/assets.tsv
    COMMENT "Extracting the loading-screen picture from the disc image")
add_custom_target(game_assets DEPENDS ${GENERATED_DIR}/content/now_loading.inc)

# The console compiler's semantics the decompiled code relies on.
set(PSX_C_FLAGS
    -std=gnu89
    -fno-omit-frame-pointer
    -fno-strict-aliasing
    -fwrapv
    -fno-builtin
    -Wno-everything
    -Werror=implicit-function-declaration
    -Werror=incompatible-function-pointer-types)

# Units whose data is simulation state put it in the sections a save state copies (src/sim_sections.h).
set(SIM_SECTIONS -include ${CMAKE_CURRENT_SOURCE_DIR}/src/sim_sections.h)

option(DOJO_TRACE "Keep a shadow call stack of the game's functions for crash reports" OFF)
if(DOJO_TRACE)
    list(APPEND PSX_C_FLAGS -finstrument-functions)
endif()

file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${REPO_ROOT}/src/*.c)

add_library(dojo_game STATIC ${GAME_SOURCES})
add_dependencies(dojo_game game_assets)
target_include_directories(dojo_game PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include ${REPO_ROOT}/include ${GENERATED_DIR})
target_compile_definitions(dojo_game PRIVATE TARGET_PC main=GameMain _CRT_SECURE_NO_WARNINGS)
target_compile_options(dojo_game PRIVATE ${PSX_C_FLAGS} ${SIM_SECTIONS})

# Assembly translated to C (tools/mips2c.py): the game's hand-written renderer, and LIBGTE, which the
# decompilation holds as assembly. LIBGTE's InitGeom (msc00.s) patches the BIOS and is written in C instead.
function(translate_asm output)
    set(sources ${ARGN})
    file(GLOB includes ${REPO_ROOT}/include/*.inc)
    add_custom_command(
        OUTPUT ${GENERATED_DIR}/${output}
        COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/mips2c.py ${GENERATED_DIR}/${output}
                ${sources} -I ${REPO_ROOT}/include -I ${REPO_ROOT}/src/lib/gte
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/mips2c.py ${sources} ${includes}
        COMMENT "Translating ${output} from assembly")
    set_source_files_properties(${GENERATED_DIR}/${output} PROPERTIES COMPILE_OPTIONS "-Wno-everything")
endfunction()

translate_asm(asm_rec.c
    ${REPO_ROOT}/src/render.s ${REPO_ROOT}/src/pov_flat.s ${REPO_ROOT}/src/pov_tex.s
    ${REPO_ROOT}/src/cuttable_fade.s ${REPO_ROOT}/src/call_thunk.s)

file(GLOB LIBGTE_ASM ${REPO_ROOT}/src/lib/gte/*.s)
list(FILTER LIBGTE_ASM EXCLUDE REGEX "(patchgte|msc00)\.s$")
translate_asm(libgte_rec.c ${LIBGTE_ASM})
# LIBPRESS's VLC decoder.
translate_asm(press_rec.c ${REPO_ROOT}/src/lib/press/vlc.s)

# The decompiled library modules that are C and run as they are.
set(LIB_SOURCES
    ${REPO_ROOT}/src/lib/gte/cstbl.c ${REPO_ROOT}/src/lib/gte/geo_00.c ${REPO_ROOT}/src/lib/gte/geo_01.c
    ${REPO_ROOT}/src/lib/gte/ratan.c ${REPO_ROOT}/src/lib/gte/sincos.c
    ${REPO_ROOT}/src/lib/gpu/prim.c ${REPO_ROOT}/src/lib/gpu/ext.c)
# LIBSPU and LIBSND whole: they drive the port's emulated sound processor through its registers.
file(GLOB SOUND_LIB_SOURCES ${REPO_ROOT}/src/lib/spu/*.c ${REPO_ROOT}/src/lib/snd/*.c ${REPO_ROOT}/src/lib/snd/vm/*.c)
list(APPEND LIB_SOURCES ${SOUND_LIB_SOURCES})
# LIBPRESS, the movie decoder's library: it drives the port's emulated MDEC (src/mdec.c) through its registers.
list(APPEND LIB_SOURCES ${REPO_ROOT}/src/lib/press/libpress.c ${REPO_ROOT}/src/lib/press/table.c)
set_property(SOURCE ${REPO_ROOT}/src/lib/press/table.c APPEND PROPERTY COMPILE_OPTIONS
    -include ${CMAKE_CURRENT_SOURCE_DIR}/src/table_sections.h)
add_library(dojo_lib STATIC ${LIB_SOURCES})
target_include_directories(dojo_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include ${REPO_ROOT}/include ${REPO_ROOT}/src/lib/gte ${REPO_ROOT}/src/lib/gpu
    ${REPO_ROOT}/src/lib/spu ${REPO_ROOT}/src/lib/snd ${REPO_ROOT}/src/lib/snd/vm ${REPO_ROOT}/src/lib/press)
target_compile_definitions(dojo_lib PRIVATE TARGET_PC)
# The libraries write the hardware's registers, some through plain pointers; kept as single stores, every
# write reaches the emulated device (mmio.c) rather than as vector stores it would have to take apart. None
# of them uses floating point, so without SSE a block copy is plain 32-bit moves.
target_compile_options(dojo_lib PRIVATE ${PSX_C_FLAGS} ${SIM_SECTIONS} -fno-vectorize -fno-slp-vectorize -mno-sse)

# SDL3, the window, input and audio: the official Visual C++ development package, checked by its SHA-256.
set(SDL3_VERSION 3.2.10)
set(SDL3_ZIP ${CMAKE_CURRENT_BINARY_DIR}/_deps/SDL3-devel-${SDL3_VERSION}-VC.zip)
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/_deps/SDL3-${SDL3_VERSION})
    file(DOWNLOAD
        https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/SDL3-devel-${SDL3_VERSION}-VC.zip
        ${SDL3_ZIP}
        EXPECTED_HASH SHA256=afcfafc7e389e048c3693e11fe645ea84392c35673e4f201fe21ac513719935d)
    file(ARCHIVE_EXTRACT INPUT ${SDL3_ZIP} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/_deps)
endif()
set(SDL3_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/SDL3-${SDL3_VERSION})
add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES
    IMPORTED_LOCATION ${SDL3_DIR}/lib/x86/SDL3.dll
    IMPORTED_IMPLIB ${SDL3_DIR}/lib/x86/SDL3.lib
    INTERFACE_INCLUDE_DIRECTORIES ${SDL3_DIR}/include)

# Vulkan: the official headers (the entry points are loaded at run time through SDL, so no SDK is needed to
# link), and a GLSL compiler for the shaders: the Vulkan SDK's glslc when VULKAN_SDK is set, otherwise the
# official glslang release, both checked by SHA-256.
set(VULKAN_HEADERS_TAG vulkan-sdk-1.4.341.0)
set(VULKAN_HEADERS_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/Vulkan-Headers-${VULKAN_HEADERS_TAG})
if(NOT EXISTS ${VULKAN_HEADERS_DIR})
    file(DOWNLOAD https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${VULKAN_HEADERS_TAG}.zip
        ${CMAKE_CURRENT_BINARY_DIR}/_deps/vulkan-headers.zip
        EXPECTED_HASH SHA256=1a0ea8ce1f1f08bcf9a7e4962646ba6b2a09b40108ca516fc6a006e45c77cb65)
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_BINARY_DIR}/_deps/vulkan-headers.zip
        DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/_deps)
endif()

if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/glslc.exe")
    set(SHADER_COMPILER "$ENV{VULKAN_SDK}/Bin/glslc.exe")
    set(SHADER_COMPILER_KIND glslc)
else()
    set(GLSLANG_VERSION 16.5.0)
    set(GLSLANG_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/glslang-${GLSLANG_VERSION})
    if(NOT EXISTS ${GLSLANG_DIR})
        file(DOWNLOAD
            https://github.com/KhronosGroup/glslang/releases/download/${GLSLANG_VERSION}/glslang-${GLSLANG_VERSION}-windows-x86_64-release.zip
            ${CMAKE_CURRENT_BINARY_DIR}/_deps/glslang.zip
            EXPECTED_HASH SHA256=06b71298b750268c127f2ee7ae0ef7525e2068120c6c8a3a08b2f58ca6f325ce)
        file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_BINARY_DIR}/_deps/glslang.zip DESTINATION ${GLSLANG_DIR})
    endif()
    set(SHADER_COMPILER ${GLSLANG_DIR}/bin/glslang.exe)
    set(SHADER_COMPILER_KIND glslang)
endif()

# Each shader becomes a header holding its SPIR-V as an array, spv_<name>_<stage>.
set(SHADER_HEADERS)
foreach(shader draw.vert draw.frag quad.vert paint.frag present.frag overlay.frag)
    string(REPLACE "." "_" shader_name ${shader})
    set(shader_header ${GENERATED_DIR}/shaders/${shader_name}.h)
    if(SHADER_COMPILER_KIND STREQUAL glslc)
        # glslc writes the words alone; a header written here names them.
        set(shader_words ${GENERATED_DIR}/shaders/${shader_name}.inc)
        file(WRITE ${GENERATED_DIR}/shaders/${shader_name}_wrap.h
            "static const uint32_t spv_${shader_name}[] = {
#include \"${shader_name}.inc\"
};
")
        set(shader_command ${SHADER_COMPILER} --target-env=vulkan1.3 -mfmt=num -o ${shader_words}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
            COMMAND ${CMAKE_COMMAND} -E copy ${GENERATED_DIR}/shaders/${shader_name}_wrap.h ${shader_header})
    else()
        set(shader_command ${SHADER_COMPILER} -V --target-env vulkan1.3 --vn spv_${shader_name} -o ${shader_header}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader})
    endif()
    add_custom_command(OUTPUT ${shader_header}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_DIR}/shaders
        COMMAND ${shader_command}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}
        COMMENT "Compiling the ${shader} shader")
    list(APPEND SHADER_HEADERS ${shader_header})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_HEADERS})

file(GLOB PORT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_executable(dojo ${PORT_SOURCES} ${GENERATED_DIR}/asm_rec.c ${GENERATED_DIR}/libgte_rec.c
    ${GENERATED_DIR}/press_rec.c)
add_dependencies(dojo shaders)
# The port's emulated hardware is simulation state too; the host, renderer and context are not.
foreach(sim_source gte.c kernel.c libcd.c libgpu.c gpu.c interim.c spu.c xa.c memory.c substep.c mdec.c)
    set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/${sim_source} APPEND PROPERTY COMPILE_OPTIONS ${SIM_SECTIONS})
endforeach()
set_property(SOURCE ${GENERATED_DIR}/asm_rec.c ${GENERATED_DIR}/libgte_rec.c ${GENERATED_DIR}/press_rec.c
    APPEND PROPERTY COMPILE_OPTIONS
    ${SIM_SECTIONS})
target_include_directories(dojo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include ${REPO_ROOT}/include
    ${GENERATED_DIR} ${VULKAN_HEADERS_DIR}/include)
target_compile_definitions(dojo PRIVATE TARGET_PC _CRT_SECURE_NO_WARNINGS)
target_compile_options(dojo PRIVATE -std=gnu11 -fno-strict-aliasing -fwrapv -fno-omit-frame-pointer)
target_link_libraries(dojo PRIVATE dojo_game dojo_lib SDL3 ws2_32)
add_custom_command(TARGET dojo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL3_DIR}/lib/x86/SDL3.dll $<TARGET_FILE_DIR:dojo>)
target_link_options(dojo PRIVATE
    -fuse-ld=lld
    -Wl,/BASE:0x400000
    -Wl,/FIXED
    -Wl,/DYNAMICBASE:NO
    -Wl,/LARGEADDRESSAWARE
    -Wl,/ERRORLIMIT:0)
