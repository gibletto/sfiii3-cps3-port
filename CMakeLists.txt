# sfiii3-cps3-port: Street Fighter III 3rd Strike (games/sf3, the decomp) on PC and 3DS
#
#   PC (MSYS2 mingw32):
#     cmake -G Ninja -B build -DCMAKE_C_COMPILER=gcc
#     ninja -C build
#
#   3DS (devkitARM):
#     cmake -G Ninja -B build-3ds -DCMAKE_TOOLCHAIN_FILE=port/cmake/3ds-native.cmake
#     ninja -C build-3ds
cmake_minimum_required(VERSION 3.20)
project(sfiii3_cps3_port C)

set(GAME ${CMAKE_SOURCE_DIR}/games/sf3)
set(PORT ${CMAKE_SOURCE_DIR}/port)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "the game stores addresses in 32 bits: build for a 32-bit target (MSYS2 mingw32)")
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# the game's data files, laid out the way the original compiler laid them out (port/tools/host_data.py)
file(GLOB GAME_DATA ${GAME}/data/*.c)
set(HOST_DATA_DIR ${CMAKE_BINARY_DIR}/host_data)
set(HOST_DATA)
foreach(f ${GAME_DATA})
    get_filename_component(n ${f} NAME)
    list(APPEND HOST_DATA ${HOST_DATA_DIR}/${n})
endforeach()
# the lists of the 16- and 32-bit tables, for reads past the end of a table (port/cps3/host_board.c)
list(APPEND HOST_DATA ${HOST_DATA_DIR}/host_data_ranges.c)
# the 16- and 32-bit tables the game reads a byte at a time, with their element size: byte-swapped on
# little-endian hosts so the bytes come out in the board's order. host_data.py checks the list against the
# data files and the code.
set(BYTE_IMAGES sys_font_cg:2,sc_chr_data:2,gill_sa_number_chr:2)
file(GLOB_RECURSE GAME_CODE ${GAME}/src/*.c)
add_custom_command(OUTPUT ${HOST_DATA}
    COMMAND Python3::Interpreter ${PORT}/tools/host_data.py --images ${BYTE_IMAGES} --src ${GAME}/src
            ${HOST_DATA_DIR} ${GAME_DATA}
    DEPENDS ${GAME_DATA} ${GAME_CODE} ${PORT}/tools/host_data.py
    COMMENT "Laying out the game data for the host")

file(GLOB GAME_SOURCES ${GAME}/src/*.c)
# The board links the game's objects in the order of its .sub file, which gives tables and RAM variables in
# different files the neighbours they have there: the game reads past the end of some tables. Link them in that
# order too, as an object library (a static library's members come in whatever order the linker takes them).
file(STRINGS ${GAME}/sf3.sub GAME_SUB REGEX "^INPUT obj")
set(GAME_LINKED)
foreach(line ${GAME_SUB})
    string(REGEX REPLACE "^INPUT obj.(.+)\\.obj.*$" "\\1" obj "${line}")
    foreach(f ${HOST_DATA_DIR}/${obj}.c ${GAME}/src/${obj}.c ${GAME}/src/nonmatching/${obj}.c)
        if(f IN_LIST GAME_SOURCES OR f IN_LIST HOST_DATA)
            list(APPEND GAME_LINKED ${f})
        endif()
    endforeach()
endforeach()
# then anything the .sub does not list
foreach(f ${GAME_SOURCES} ${HOST_DATA})
    if(NOT f IN_LIST GAME_LINKED)
        list(APPEND GAME_LINKED ${f})
    endif()
endforeach()
add_library(sf3_game OBJECT ${GAME_LINKED})
target_include_directories(sf3_game PUBLIC ${GAME}/include ${PORT}/include)
target_compile_definitions(sf3_game PUBLIC HOST_PORT=1 HOST_LITTLE_ENDIAN=1)
# compile the game code the way the arcade compiler treated it: K&R, signed char, wrapping, type punning
target_compile_options(sf3_game PRIVATE
    -std=gnu17 -fpermissive -fwrapv -fno-strict-aliasing -fsigned-char -w
    -include ${PORT}/include/port_prefix.h
    # keep data in definition order (checked after linking by port/tools/check_layout.py)
    -fno-toplevel-reorder
    # variables set to 0 stay with the other initialised ones, as the board's compiler keeps them
    -fno-zero-initialized-in-bss)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i686|AMD64|i386")
    target_compile_options(sf3_game PRIVATE -malign-data=abi)
endif()
# Optimised (-O2). Where the game reads the value of a function that can end without returning one, the
# function returns what the board leaves in R0 there (#ifdef HOST_PORT, from the program's disassembly).
target_compile_options(sf3_game PRIVATE -O2)

# the board: memory, kernel, video, sound, EEPROM
set(CPS3_SOURCES
    ${PORT}/cps3/cps3_hw.c
    ${PORT}/cps3/cps3_kernel.c
    ${PORT}/cps3/cps3_video.c
    ${PORT}/cps3/cps3_video_dma.c
    ${PORT}/cps3/cps3_eeprom.c
    ${PORT}/cps3/cps3_sound.c
    ${PORT}/cps3/simmram.c
    ${PORT}/cps3/sh2_sub.c
    ${PORT}/cps3/host_board.c)
# 3rd Strike reads the BIOS's CD flag, so the region setting has NO CD versions (cps3_region.c)
set_source_files_properties(${PORT}/cps3/cps3_region.c PROPERTIES COMPILE_DEFINITIONS CPS3_REGION_NO_CD=1)

if(NINTENDO_3DS)
    add_executable(sf3
        ${CPS3_SOURCES}
        ${PORT}/sys/coro_arm.c
        ${PORT}/sys/divzero_arm.c
        ${PORT}/ctr/main_3ds.c
        ${PORT}/ctr/ctr_video.c
        ${PORT}/ctr/ctr_prefetch.c
        ${PORT}/ctr/ctr_sound.c
        ${PORT}/ctr/ctr_bottom.c
        ${PORT}/ctr/ctr_controls.c
        ${PORT}/cps3/cps3_romset.c
        ${PORT}/cps3/cps3_region.c
        ${PORT}/ctr/ctr_depth.c)
    target_link_libraries(sf3 PRIVATE sf3_game citro3d ctru z m)
    # ctr_depth.c reads the game's state through the game's headers
    set_source_files_properties(${PORT}/ctr/ctr_depth.c PROPERTIES
        COMPILE_OPTIONS "-include;${PORT}/include/port_prefix.h;-Wno-error;-w")
    # division by zero returns 0, as on the arcade (divzero_arm.c)
    foreach(fn __aeabi_idiv __aeabi_uidiv __aeabi_idivmod __aeabi_uidivmod)
        target_link_options(sf3 PRIVATE "-Wl,--wrap=${fn}")
    endforeach()
    set(SYM_PREFIX "")
else()
    find_package(SDL2 REQUIRED)
    add_executable(sf3 WIN32
        ${CPS3_SOURCES}
        ${PORT}/cps3/cps3_romset.c
        ${PORT}/cps3/cps3_region.c
        ${PORT}/sys/coro_win32.c
        ${PORT}/sys/divzero_win32.c
        ${PORT}/sdl/main_sdl.c)
    # zlib linked in, so the game needs only SDL2.dll
    target_link_libraries(sf3 PRIVATE sf3_game SDL2::SDL2main SDL2::SDL2 -l:libz.a)
    set(SYM_PREFIX "_")     # i686 C symbols have a leading underscore
endif()
target_include_directories(sf3 PRIVATE ${PORT}/sys)
target_compile_options(sf3 PRIVATE -O2 -std=gnu17 -fwrapv -fno-strict-aliasing -fsigned-char -Wall -Wno-unused-variable)

# symbol aliases from the original link script (sf3.sub)
set(SF3_DEFSYMS
    asstbl_lv_5000=asstbl_lv_2000   asstbl_lv_5010=asstbl_lv_2010
    asstbl_lv_6000=asstbl_lv_3000   asstbl_lv_6010=asstbl_lv_3010
    asstbl_lv_7000=asstbl_lv_4000   asstbl_lv_7010=asstbl_lv_4010
    asstbl_lv_8000=asstbl_lv_2000   asstbl_lv_8010=asstbl_lv_2010
    asstbl_lv_9000=asstbl_lv_3000   asstbl_lv_9010=asstbl_lv_3010
    asstbl_lv_10000=asstbl_lv_4000  asstbl_lv_10010=asstbl_lv_4010)
foreach(sym ${SF3_DEFSYMS})
    string(REPLACE "=" "=${SYM_PREFIX}" sym "${sym}")
    target_link_options(sf3 PRIVATE "-Wl,--defsym,${SYM_PREFIX}${sym}")
endforeach()

add_custom_command(TARGET sf3 POST_BUILD
    COMMAND Python3::Interpreter ${PORT}/tools/check_layout.py $<TARGET_FILE:sf3> ${CMAKE_NM}
    COMMENT "Checking the game's data layout")

if(WIN32)
    # a fixed load address, so runs are repeatable
    target_link_options(sf3 PRIVATE "-Wl,--disable-dynamicbase")
endif()

# -DOLD_3DS_TEST=ON makes the Old 3DS test build: on a New 3DS it runs the way an Old 3DS runs (the Old 3DS memory
# mode, 268 MHz, no New 3DS speed-up and so no L2 cache), and it installs beside the normal build as its own title,
# with the same SD folder and settings
option(OLD_3DS_TEST "3DS: make the Old 3DS test build" OFF)

# The version on the 3DS builds, after the description the HOME Menu and Homebrew Launcher show: the release workflow
# passes its tag, otherwise it is what git describe says
set(PORT_VERSION "" CACHE STRING "Version shown on the 3DS builds (default: git describe)")
if(NOT PORT_VERSION)
    execute_process(COMMAND git describe --tags --always WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE PORT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

if(NINTENDO_3DS)
    if(OLD_3DS_TEST)
        target_compile_definitions(sf3 PRIVATE OLD_3DS_TEST=1)
        # the RSF with its own title, the Old 3DS memory mode and the Old 3DS CPU speed
        set(CIA_DIR ${CMAKE_CURRENT_BINARY_DIR}/cia_o3ds)
        file(COPY ${PORT}/ctr/cia/icon.png ${PORT}/ctr/cia/banner.png ${PORT}/ctr/cia/banner.wav DESTINATION ${CIA_DIR})
        file(READ ${PORT}/ctr/cia/sf3.rsf rsf)
        foreach(pair "Title;SFIII3DG" "ProductCode;CTR-H-SF3O" "UniqueId;0xF5F3E" "SystemModeExt;Legacy" "CpuSpeed;268MHz")
            list(GET pair 0 key)
            list(GET pair 1 value)
            string(REGEX REPLACE "\n(  ${key} +: )[^\r\n]*" "\n\\1${value}" rsf "${rsf}")
        endforeach()
        file(WRITE ${CIA_DIR}/sf3.rsf "${rsf}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PORT}/ctr/cia/sf3.rsf)
        ctr_create_3dsx(sf3 "SF III 3rd Strike O3DS" "Old 3DS test build ${PORT_VERSION}" ${PORT}/ctr/cia/icon.png ${PORT}/ctr/romfs)
        ctr_create_cia(sf3 "SF III 3rd Strike O3DS" "Street Fighter III 3rd Strike (O3DS test) ${PORT_VERSION}" ${CIA_DIR} ${PORT}/ctr/romfs)
    else()
        ctr_create_3dsx(sf3 "SF III 3rd Strike" "CPS3 port ${PORT_VERSION}" ${PORT}/ctr/cia/icon.png ${PORT}/ctr/romfs)
        ctr_create_cia(sf3 "SF III 3rd Strike" "Street Fighter III 3rd Strike (CPS3 port) ${PORT_VERSION}" ${PORT}/ctr/cia ${PORT}/ctr/romfs)
    endif()

    # PICA200 shaders (vertex, then geometry), assembled and embedded as a C header (<name>_shbin)
    function(ctr_shader target pica)
        get_filename_component(name ${pica} NAME_WE)
        set(shbin ${CMAKE_CURRENT_BINARY_DIR}/${name}.shbin)
        set(header ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}_shbin.h)
        add_custom_command(OUTPUT ${header}
            COMMAND ${CTR_PICASSO} -o ${shbin} ${pica} ${ARGN}
            COMMAND Python3::Interpreter ${PORT}/tools/bin2c.py ${shbin} ${header} ${name}_shbin
            DEPENDS ${pica} ${ARGN} ${PORT}/tools/bin2c.py
            COMMENT "Assembling ${name} shaders")
        target_sources(${target} PRIVATE ${header})
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    endfunction()

    ctr_shader(sf3 ${PORT}/ctr/ctr_video.v.pica ${PORT}/ctr/ctr_video.g.pica)
endif()
