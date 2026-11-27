# Dojo, the PC port of Bushido Blade: the decompiled game, unchanged except where TARGET_PC says so, over the port's
# implementation of the console's libraries and hardware.
#
# The game is 32-bit code through and through (pointers in disc records, 24-bit ordering-table links), so the
# port builds a 32-bit executable linked at a fixed base low enough that every address fits 24 bits.
#
# It builds with the self-contained toolchain in toolchain/ (tools/fetch_toolchain.py fetches it; the release
# ships it): for Windows, llvm-mingw for 32-bit x86; for Linux, Zig (Clang, the linker and glibc's headers and
# stubs for 32-bit x86); and CMake, Ninja, Python (Windows; Linux has its own), SDL3, the Vulkan headers and
# glslang. Everyone building with the same tools makes the same program, which online play relies on.
#
#   toolchain/cmake/bin/cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   toolchain/cmake/bin/cmake --build build
#
# DOJO_PLATFORM picks the system the game is built for: the one building it, unless set (a Windows machine
# can build the Linux game with a Linux toolchain's Zig, SDL3 and glslang swapped for their Windows builds).

cmake_minimum_required(VERSION 3.20)

set(DOJO_TOOLCHAIN "${CMAKE_CURRENT_SOURCE_DIR}/toolchain" CACHE PATH "The self-contained toolchain")
if(NOT EXISTS "${DOJO_TOOLCHAIN}/TOOLCHAIN_VERSION")
    message(FATAL_ERROR "No toolchain in ${DOJO_TOOLCHAIN}: run tools/fetch_toolchain.py")
endif()
if(CMAKE_HOST_WIN32)
    set(HOST_EXE ".exe")
    set(DOJO_DEFAULT_PLATFORM windows)
else()
    set(HOST_EXE "")
    set(DOJO_DEFAULT_PLATFORM linux)
endif()
set(DOJO_PLATFORM ${DOJO_DEFAULT_PLATFORM} CACHE STRING "The system the game is built for: windows or linux")
if(NOT CMAKE_MAKE_PROGRAM)
    set(CMAKE_MAKE_PROGRAM "${DOJO_TOOLCHAIN}/ninja/ninja${HOST_EXE}" CACHE FILEPATH "")
endif()
if(DOJO_PLATFORM STREQUAL "windows")
    set(CMAKE_C_COMPILER "${DOJO_TOOLCHAIN}/llvm-mingw/bin/i686-w64-mingw32-clang.exe" CACHE FILEPATH "" FORCE)
elseif(DOJO_PLATFORM STREQUAL "linux")
    # Zig's C compiler for 32-bit x86 Linux, through small scripts CMake can take as its tools. The glibc it
    # links against is an old one, so the game runs on any distribution since; the CPU is fixed rather than
    # the building machine's. Zig's own caches stay in the build folder.
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR i686)
    set(ZIG "${DOJO_TOOLCHAIN}/zig/zig${HOST_EXE}")
    set(ZIG_CACHE "${CMAKE_BINARY_DIR}/zig-cache")
    foreach(tool cc ar ranlib)
        if(tool STREQUAL "cc")
            set(tool_args "cc -target x86-linux-gnu.2.31 -mcpu=pentium4 -fno-sanitize=all")
        else()
            set(tool_args "${tool}")
        endif()
        if(CMAKE_HOST_WIN32)
            set(wrapper "${CMAKE_BINARY_DIR}/zig-${tool}.cmd")
            file(WRITE ${wrapper} "@set ZIG_GLOBAL_CACHE_DIR=${ZIG_CACHE}\r\n@set ZIG_LOCAL_CACHE_DIR=${ZIG_CACHE}\r\n"
                "@\"${ZIG}\" ${tool_args} %*\r\n")
        else()
            set(wrapper "${CMAKE_BINARY_DIR}/zig-${tool}")
            file(WRITE ${wrapper} "#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR='${ZIG_CACHE}' ZIG_LOCAL_CACHE_DIR='${ZIG_CACHE}'\n"
                "exec '${ZIG}' ${tool_args} \"$@\"\n")
            file(CHMOD ${wrapper} PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                 WORLD_READ WORLD_EXECUTE)
        endif()
        set(ZIG_${tool} ${wrapper})
    endforeach()
    set(CMAKE_C_COMPILER "${ZIG_cc}" CACHE FILEPATH "" FORCE)
    set(CMAKE_AR "${ZIG_ar}" CACHE FILEPATH "" FORCE)
    set(CMAKE_RANLIB "${ZIG_ranlib}" CACHE FILEPATH "" FORCE)
else()
    message(FATAL_ERROR "DOJO_PLATFORM is windows or linux, not ${DOJO_PLATFORM}")
endif()
project(dojo_pc C)

set(DOJO_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "The Bushido Blade decompilation the port builds the game from (not part of this repository)")
if(NOT EXISTS "${DOJO_DECOMP_DIR}/src/main.c" OR NOT EXISTS "${DOJO_DECOMP_DIR}/include/substep.h")
    message(FATAL_ERROR "DOJO_DECOMP_DIR (${DOJO_DECOMP_DIR}) is not the decompilation with the port's changes: "
        "it needs src/main.c and include/substep.h")
endif()
set(REPO_ROOT ${DOJO_DECOMP_DIR})
set(DOJO_DISC "${REPO_ROOT}/game_files/Bushido Blade (USA).bin" CACHE FILEPATH
    "The player's disc image; the build takes the loading-screen picture from its executable")

if(CMAKE_HOST_WIN32)
    set(Python3_EXECUTABLE "${DOJO_TOOLCHAIN}/python/python.exe" CACHE FILEPATH "")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The loading-screen picture the source includes from the player's own executable.
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${GENERATED_DIR}/content/now_loading.inc
    COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_assets.py ${DOJO_DISC}
            ${GENERATED_DIR} ${REPO_ROOT}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_assets.py ${REPO_ROOT}/config/bushidoblade/assets.tsv
            ${DOJO_DISC}
    COMMENT "Extracting the loading-screen picture from the disc image")
add_custom_target(game_assets DEPENDS ${GENERATED_DIR}/content/now_loading.inc)

if(DOJO_PLATFORM STREQUAL "windows")
    # Debug information as a PDB (CodeView), which crash reports (llvm-symbolizer) and tools/statediff.py read.
    add_compile_options($<$<CONFIG:RelWithDebInfo,Debug>:-gcodeview>)
    add_link_options($<$<CONFIG:RelWithDebInfo,Debug>:-Wl,--pdb=> "LINKER:-Xlink=-pdbaltpath:%_PDB%")
    # Reproducible: the same program wherever it is built (no timestamp, no folder names), so two players'
    # builds are the same bytes.
    add_link_options(-Wl,--no-insert-timestamp)
endif()
add_compile_options(-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=dojo -ffile-prefix-map=${DOJO_DECOMP_DIR}=decomp
    -ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=build)

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

# SDL3, the window, input and audio (the toolchain's mingw development package, or for Linux Debian's 32-bit
# one), the Vulkan headers (the entry points are loaded at run time through SDL, so nothing more is needed to
# link), and glslang for the shaders.
add_library(SDL3 SHARED IMPORTED)
if(DOJO_PLATFORM STREQUAL "windows")
    set(SDL3_DIR ${DOJO_TOOLCHAIN}/deps/SDL3/i686-w64-mingw32)
    set(SDL3_RUNTIME ${SDL3_DIR}/bin/SDL3.dll)
    set_target_properties(SDL3 PROPERTIES
        IMPORTED_LOCATION ${SDL3_RUNTIME}
        IMPORTED_IMPLIB ${SDL3_DIR}/lib/libSDL3.dll.a
        INTERFACE_INCLUDE_DIRECTORIES ${SDL3_DIR}/include)
else()
    set(SDL3_DIR ${DOJO_TOOLCHAIN}/deps/SDL3/i686-linux-gnu)
    set(SDL3_RUNTIME ${SDL3_DIR}/lib/libSDL3.so.0)
    set_target_properties(SDL3 PROPERTIES
        IMPORTED_LOCATION ${SDL3_RUNTIME}
        IMPORTED_SONAME libSDL3.so.0
        INTERFACE_INCLUDE_DIRECTORIES ${SDL3_DIR}/include)
endif()
set(VULKAN_HEADERS_DIR ${DOJO_TOOLCHAIN}/deps/Vulkan-Headers)
set(SHADER_COMPILER ${DOJO_TOOLCHAIN}/deps/glslang/bin/glslang${HOST_EXE})

# Each shader becomes a header holding its SPIR-V as an array, spv_<name>_<stage>.
set(SHADER_HEADERS)
foreach(shader draw.vert draw.frag quad.vert paint.frag downsample.frag present.frag overlay.frag)
    string(REPLACE "." "_" shader_name ${shader})
    set(shader_header ${GENERATED_DIR}/shaders/${shader_name}.h)
    set(shader_command ${SHADER_COMPILER} -V --target-env vulkan1.1 --vn spv_${shader_name} -o ${shader_header}
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader})
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
add_custom_command(TARGET dojo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL3_RUNTIME} $<TARGET_FILE_DIR:dojo>)
if(DOJO_PLATFORM STREQUAL "windows")
    target_link_libraries(dojo PRIVATE dojo_game dojo_lib SDL3 ws2_32)
    # Fixed at 0x400000 with no relocations (every address the game sees fits 24 bits), above 2 GB too (the
    # console's memory is mapped at 0x80000000), and the runtime linked in, so the program needs only Windows
    # and SDL3.dll.
    target_link_options(dojo PRIVATE
        -Wl,--image-base,0x400000
        -Wl,--disable-dynamicbase
        -Wl,--disable-reloc-section
        -Wl,--large-address-aware
        -Wl,--error-limit=0
        -static)
else()
    target_link_libraries(dojo PRIVATE dojo_game dojo_lib SDL3 pthread)
    # Fixed at 0x400000, not position-independent (every address the game sees fits 24 bits; a 32-bit program
    # on a 64-bit kernel has all 4 GB, the console's memory at 0x80000000 among them), the simulation's
    # sections gathered (linux.ld), and libSDL3.so.0 found beside the program.
    set_target_properties(dojo PROPERTIES INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH TRUE)
    target_compile_options(dojo PRIVATE -fno-pie)
    target_link_options(dojo PRIVATE
        -no-pie
        -Wl,--image-base=0x400000
        -T ${CMAKE_CURRENT_SOURCE_DIR}/linux.ld
        # (Zig's C runtime carries debug information naming the folder Zig is in: a release build leaves it
        # out, so the program is the same bytes wherever it was built)
        $<$<CONFIG:Release>:-Wl,--strip-debug>)
    target_compile_options(dojo_game PRIVATE -fno-pie)
    target_compile_options(dojo_lib PRIVATE -fno-pie)
endif()
