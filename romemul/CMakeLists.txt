# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.12)

# Include build functions from Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include($ENV{PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

# Set name of project (as PROJECT_NAME) and C/C   standards
project(romemul C CXX ASM)
set(CMAKE_C_STANDARD 11)

# If not specified, use pico_w as default board type
set(BOARD_TYPE $ENV{BOARD_TYPE})
if(NOT BOARD_TYPE)
    set(BOARD_TYPE "pico_w")
endif()
set(PICO_BOARD ${BOARD_TYPE})

# Creates a pico-sdk subdirectory in our project for the libraries
pico_sdk_init()

# Fase 12: the makefsdata step and the fs/ web assets are gone with the web
# configurator; nothing generates or compiles my_fsdata.c any more.

# Tell CMake where to find the executable source file
add_executable(${PROJECT_NAME} 
        main.c
)

# Needed to include lwipopts.h properly
target_include_directories(romemul PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/
)
        
# Create map/bin/hex/uf2 files
pico_add_extra_outputs(${PROJECT_NAME})

# by default the header is generated into the build dir
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/romemul.pio)

target_sources(${PROJECT_NAME} PRIVATE lwipopts.h)
target_sources(${PROJECT_NAME} PRIVATE hw_config.c)
target_sources(${PROJECT_NAME} PRIVATE commands.c)
target_sources(${PROJECT_NAME} PRIVATE constants.c)
target_sources(${PROJECT_NAME} PRIVATE firmware_gemdrvemul.c)
target_sources(${PROJECT_NAME} PRIVATE romemul.c)
target_sources(${PROJECT_NAME} PRIVATE tprotocol.c)
target_sources(${PROJECT_NAME} PRIVATE config.c)
target_sources(${PROJECT_NAME} PRIVATE network.c)
target_sources(${PROJECT_NAME} PRIVATE filesys.c)
target_sources(${PROJECT_NAME} PRIVATE scfs.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_probe.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_update_check.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_resolve.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_config.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_system_config.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_netconfig.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_rtcconfig.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_config_drive.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_config_drive_backend.c)
target_sources(${PROJECT_NAME} PRIVATE sidetnfs_sd_service.c)
target_sources(${PROJECT_NAME} PRIVATE rtcemul.c)
target_sources(${PROJECT_NAME} PRIVATE gemdrvemul.c)

target_sources(${PROJECT_NAME} PRIVATE usb_descriptors.c)
target_sources(${PROJECT_NAME} PRIVATE usb_mass.c)

# Tell CMake where to find other source code
add_subdirectory($ENV{FATFS_SDK_PATH}/src build)

# Link to pico_stdlib (gpio, time, etc. functions)
target_link_libraries(${PROJECT_NAME} PRIVATE
        FatFs_SPI                # for SD card access
        pico_stdlib              # for core functionality
        hardware_pio             # for GPIO
        hardware_dma             # for DMA
        pico_cyw43_arch_lwip_poll
        tinyusb_device           # for USB
        pico_mbedtls             # TLS 1.2 client -- firmware-update version check (raw.githubusercontent.com)
        pico_lwip_mbedtls        # lwIP's altcp_tls glue on top of pico_mbedtls
        )

# Link custom memmap with reserved memory for ROMs
set_target_properties(${PROJECT_NAME} PROPERTIES
        PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_romemul.ld
)

# Use reduced space for ROMs
pico_set_binary_type(${PROJECT_NAME} romemul)

# Fetch the values from the environment variables
set(RELEASE_TYPE $ENV{RELEASE_TYPE})
set(RELEASE_VERSION $ENV{RELEASE_VERSION})
set(RELEASE_DATE $ENV{RELEASE_DATE})
set(_DEBUG $ENV{DEBUG_MODE})

# If the environment variables are not set, use default values
if(NOT RELEASE_VERSION)

    # Check if RELEASE_TYPE is set and not empty, otherwise use default "FINAL"
    if(NOT RELEASE_TYPE OR RELEASE_TYPE STREQUAL "")
        set(RELEASE_TYPE "final")
    endif()

    # Determine the file name based on RELEASE_TYPE
    if(RELEASE_TYPE STREQUAL "final")
        set(VERSION_FILE_NAME "version.txt")
    else()
        set(VERSION_FILE_NAME "version-${RELEASE_TYPE}.txt")
    endif()
    
    # Remove newline character if present
    string(STRIP "${RELEASE_VERSION}" RELEASE_VERSION)

    # In case version.txt is empty or does not exist, use a default version
    if("${RELEASE_VERSION}" STREQUAL "")
        set(RELEASE_VERSION "v0.0.1dev")
    endif()
endif()

# Firmware-updatefunctionaliteit verwijderd: SideTNFS does no version check
# and no firmware download, so there is no GitHub release URL to derive any
# more. LATEST_RELEASE_URL is kept as an empty string purely because
# config.c's defaultEntries[] still has the (now inert, never-read)
# PARAM_LASTEST_RELEASE_URL slot -- removing that entry would change
# MAX_ENTRIES and the legacy ConfigEntry flash layout, which is out of scope.
set(LATEST_RELEASE_URL "")

if(NOT RELEASE_DATE)
        string(TIMESTAMP CURRENT_DATE_TIME "%Y-%m-%d %H:%M:%S")
        set(RELEASE_DATE ${CURRENT_DATE_TIME})
endif()

if (NOT _DEBUG)
        set(_DEBUG 0)
endif()

# Two officially supported build variants, selected by build.sh's build-type
# argument alone (`./build.sh pico_w` -> Production, `./build.sh pico_w
# debug` -> Debug -- see build.sh, which sets _DEBUG from that same
# argument): SIDETNFS_ENABLE_DEBUG and SIDETNFS_ENABLE_DIAG both default to
# _DEBUG's own value, so a plain build of either variant needs no other
# flag. Each can still be overridden independently via its own env var, same
# passthrough pattern as _DEBUG above, e.g. to get the eventlog without the
# heavier snapshot instrumentation:
#   SIDETNFS_ENABLE_DEBUG=1 SIDETNFS_ENABLE_DIAG=0 ./build.sh pico_w debug
# NOTE: deliberately checked against the empty string, not `if(NOT ...)` --
# CMake treats the literal string "0" itself as boolean-false, so
# `if(NOT _VAR)` would be true (and clobber an explicit "0" back to the
# default) exactly when the env var is set to the value we most need to
# respect.
if("$ENV{SIDETNFS_ENABLE_DEBUG}" STREQUAL "")
        set(_SIDETNFS_ENABLE_DEBUG ${_DEBUG})
else()
        set(_SIDETNFS_ENABLE_DEBUG $ENV{SIDETNFS_ENABLE_DEBUG})
endif()

if("$ENV{SIDETNFS_ENABLE_DIAG}" STREQUAL "")
        set(_SIDETNFS_ENABLE_DIAG ${_DEBUG})
else()
        set(_SIDETNFS_ENABLE_DIAG $ENV{SIDETNFS_ENABLE_DIAG})
endif()

if(${_SIDETNFS_ENABLE_DIAG} STREQUAL "0")
        set(_SIDETNFS_BUILD_VARIANT "Production")
else()
        set(_SIDETNFS_BUILD_VARIANT "Debug")
endif()

# SIDETNFS_ENABLE_SD_SLOT_DUMP -- same passthrough pattern, default 0.
# Independent of SIDETNFS_ENABLE_DIAG/the EVENTLOG.TXT/SNAPSHOT.TXT dumps
# above -- writes the same kind of information (plus file-handle/DTA/
# call-counter detail) to <hd_folder>/SLOTDIAG.TXT on SD instead, triggered
# by a SHORT SELECT press during normal operation, never at boot, never the
# 10s factory-reset long-press. E.g.:
#   SIDETNFS_ENABLE_SD_SLOT_DUMP=1 ./build.sh pico_w debug
if("$ENV{SIDETNFS_ENABLE_SD_SLOT_DUMP}" STREQUAL "")
        set(_SIDETNFS_ENABLE_SD_SLOT_DUMP 0)
else()
        set(_SIDETNFS_ENABLE_SD_SLOT_DUMP $ENV{SIDETNFS_ENABLE_SD_SLOT_DUMP})
endif()

# SIDETNFS_FORCE_CONFIG_RECOVERY -- same passthrough pattern,
# default 0. One-shot recovery build: as early as possible after minimal
# init, forcibly rewrites the SideTNFS drive-list flash sector to clean
# native-v3 factory defaults (S:/N: only) IF it doesn't already exactly
# match those defaults -- see sidetnfs_config_flash_matches_factory_defaults()
# and main.c's own recovery function for the full contract, including the
# reset-loop-avoidance check. No UART, no SD diagnosis of any kind -- see
# main.c for the exact zero-UART guarantee. E.g.:
#   SIDETNFS_FORCE_CONFIG_RECOVERY=1 ./build.sh pico_w
if("$ENV{SIDETNFS_FORCE_CONFIG_RECOVERY}" STREQUAL "")
        set(_SIDETNFS_FORCE_CONFIG_RECOVERY 0)
else()
        set(_SIDETNFS_FORCE_CONFIG_RECOVERY $ENV{SIDETNFS_FORCE_CONFIG_RECOVERY})
endif()

# Debug outputs
pico_enable_stdio_usb(${PROJECT_NAME} 0)
# Workaround to disable USB output in release builds. SIDETNFS_ENABLE_DIAG
# additionally forces the physical UART on even at _DEBUG=0 (reachable only
# via the independent-override form above), so the SELECT-triggered
# snapshot dump's own instrumentation has a live stdio backend to sit
# behind -- DPRINTF itself stays gated on _DEBUG alone (unchanged).
if(${_DEBUG} STREQUAL "0" AND ${_SIDETNFS_ENABLE_DIAG} STREQUAL "0")
    pico_enable_stdio_uart(${PROJECT_NAME} 0)
else()
    pico_enable_stdio_uart(${PROJECT_NAME} 1)
endif()

# Print the variables on screen
message("Build variant: " ${_SIDETNFS_BUILD_VARIANT})
message("RELEASE_TYPE: " ${RELEASE_TYPE})
message("RELEASE_VERSION: " ${RELEASE_VERSION})
message("RELEASE_DATE: " ${RELEASE_DATE})
message("DEBUG_MODE: " ${_DEBUG})

# Pass these values to the C compiler
add_definitions(-DRELEASE_TYPE="${RELEASE_TYPE}")
add_definitions(-DRELEASE_VERSION="${RELEASE_VERSION}")
add_definitions(-DRELEASE_DATE="${RELEASE_DATE}")
add_definitions(-DLATEST_RELEASE_URL="${LATEST_RELEASE_URL}")

# Pass the _DEBUG flag to the C compiler
add_definitions(-D_DEBUG=${_DEBUG})

message("SIDETNFS_ENABLE_DEBUG: " ${_SIDETNFS_ENABLE_DEBUG})
message("SIDETNFS_ENABLE_DIAG: " ${_SIDETNFS_ENABLE_DIAG})
message("SIDETNFS_ENABLE_SD_SLOT_DUMP: " ${_SIDETNFS_ENABLE_SD_SLOT_DUMP})
message("SIDETNFS_FORCE_CONFIG_RECOVERY: " ${_SIDETNFS_FORCE_CONFIG_RECOVERY})
add_definitions(-DSIDETNFS_ENABLE_DEBUG=${_SIDETNFS_ENABLE_DEBUG})
add_definitions(-DSIDETNFS_ENABLE_DIAG=${_SIDETNFS_ENABLE_DIAG})
add_definitions(-DSIDETNFS_ENABLE_SD_SLOT_DUMP=${_SIDETNFS_ENABLE_SD_SLOT_DUMP})
add_definitions(-DSIDETNFS_FORCE_CONFIG_RECOVERY=${_SIDETNFS_FORCE_CONFIG_RECOVERY})

# Optimization Flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}  -ffunction-sections -fdata-sections")
