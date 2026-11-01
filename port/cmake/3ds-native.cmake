# devkitARM toolchain for the 3DS, usable from any CMake (Windows or Linux)
#
#   cmake -G Ninja -B build-3ds -DCMAKE_TOOLCHAIN_FILE=port/cmake/3ds-native.cmake [-DDEVKITPRO=C:/devkitPro]
#
# devkitPro's own 3DS.cmake needs its MSYS2 CMake on Windows, which breaks on the game's long link command.
# The flags are the same as devkitPro's.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv6k)

if(NOT DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO} AND NOT "$ENV{DEVKITPRO}" MATCHES "^/opt")
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    elseif(CMAKE_HOST_WIN32)
        set(DEVKITPRO "C:/devkitPro")
    else()
        set(DEVKITPRO "/opt/devkitpro")
    endif()
endif()
set(DEVKITPRO "${DEVKITPRO}" CACHE PATH "devkitPro install")

if(CMAKE_HOST_WIN32)
    set(EXE ".exe")
endif()
set(TOOLS ${DEVKITPRO}/devkitARM/bin)
set(CMAKE_C_COMPILER ${TOOLS}/arm-none-eabi-gcc${EXE})
set(CMAKE_ASM_COMPILER ${TOOLS}/arm-none-eabi-gcc${EXE})
set(CMAKE_AR ${TOOLS}/arm-none-eabi-gcc-ar${EXE} CACHE FILEPATH "")
set(CMAKE_RANLIB ${TOOLS}/arm-none-eabi-gcc-ranlib${EXE} CACHE FILEPATH "")
set(CMAKE_NM ${TOOLS}/arm-none-eabi-nm${EXE} CACHE FILEPATH "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(NINTENDO_3DS TRUE)
set(CTR_ROOT ${DEVKITPRO}/libctru)
set(CTR_3DSXTOOL ${DEVKITPRO}/tools/bin/3dsxtool${EXE})
set(CTR_SMDHTOOL ${DEVKITPRO}/tools/bin/smdhtool${EXE})
set(CTR_PICASSO ${DEVKITPRO}/tools/bin/picasso${EXE})

set(CMAKE_C_FLAGS_INIT "-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -ffunction-sections -D__3DS__")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=3dsx.specs -L${CTR_ROOT}/lib -L${DEVKITPRO}/portlibs/3ds/lib")
include_directories(SYSTEM ${CTR_ROOT}/include ${DEVKITPRO}/portlibs/3ds/include)

set(CMAKE_FIND_ROOT_PATH ${DEVKITPRO}/devkitARM ${CTR_ROOT} ${DEVKITPRO}/portlibs/3ds)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# makes <target>.3dsx with a title and icon, and a romfs if a directory is given
function(ctr_create_3dsx target title description)
    set(smdh ${CMAKE_CURRENT_BINARY_DIR}/${target}.smdh)
    set(romfs "")
    if(ARGC GREATER 3)
        set(romfs "--romfs=${ARGV3}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CTR_SMDHTOOL} --create ${title} ${description} "sfiii3-cps3-port" ${CTR_ROOT}/default_icon.png ${smdh}
        COMMAND ${CTR_3DSXTOOL} $<TARGET_FILE:${target}> ${CMAKE_CURRENT_BINARY_DIR}/${target}.3dsx --smdh=${smdh} ${romfs}
        COMMENT "Making ${target}.3dsx")
endfunction()
