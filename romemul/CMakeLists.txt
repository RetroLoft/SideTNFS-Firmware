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

# Fase 8B: SIDETNFS_ENABLE_SD_SUPPORT/SIDETNFS_ENABLE_DEBUG -- same
# env-var-to-add_definitions() passthrough as _DEBUG above. Both default to
# 1 (unchanged behavior) unless explicitly overridden, e.g.:
#   SIDETNFS_ENABLE_SD_SUPPORT=0 SIDETNFS_ENABLE_DEBUG=0 ./build.sh pico_w
# NOTE: deliberately checked against the empty string, not `if(NOT ...)` --
# CMake treats the literal string "0" itself as boolean-false, so
# `if(NOT _VAR)` would be true (and clobber an explicit "0" back to "1")
# exactly when the env var is set to the value we most need to respect.
# _DEBUG above gets away with the simpler NOT-check only because its own
# default already happens to be 0.
if("$ENV{SIDETNFS_ENABLE_SD_SUPPORT}" STREQUAL "")
        set(_SIDETNFS_ENABLE_SD_SUPPORT 1)
else()
        set(_SIDETNFS_ENABLE_SD_SUPPORT $ENV{SIDETNFS_ENABLE_SD_SUPPORT})
endif()

if("$ENV{SIDETNFS_ENABLE_DEBUG}" STREQUAL "")
        set(_SIDETNFS_ENABLE_DEBUG 1)
else()
        set(_SIDETNFS_ENABLE_DEBUG $ENV{SIDETNFS_ENABLE_DEBUG})
endif()

# Diagnostic build: SIDETNFS_ENABLE_DIAG_UART -- same passthrough
# pattern, default 0 (unchanged behavior/no code compiled in) unless
# explicitly overridden, e.g.:
#   SIDETNFS_ENABLE_DIAG_UART=1 SIDETNFS_ENABLE_DEBUG=0 ./build.sh pico_w
# Deliberately independent of _DEBUG/SIDETNFS_ENABLE_DEBUG: this build keeps
# DPRINTF (and the DEBUG.TXT dump-on-select) exactly as they'd otherwise be,
# but still needs the physical UART wired up for the new, separate
# SELECT-triggered snapshot dump -- see the pico_enable_stdio_uart() call
# below.
if("$ENV{SIDETNFS_ENABLE_DIAG_UART}" STREQUAL "")
        set(_SIDETNFS_ENABLE_DIAG_UART 0)
else()
        set(_SIDETNFS_ENABLE_DIAG_UART $ENV{SIDETNFS_ENABLE_DIAG_UART})
endif()

# Buildvarianten vereenvoudigen en logisch hernoemen: SIDETNFS_BUILD_DIAGNOSTIC
# is the new, user-facing name for asking for the "Diagnostic" build
# variant (see build.sh/report). It is a pure alias on top of the
# existing SIDETNFS_ENABLE_DIAG_UART macro above, not a second,
# independent switch -- union/OR semantics only:
# SIDETNFS_BUILD_DIAGNOSTIC=1 always turns Diagnostic mode on (forcing
# _SIDETNFS_ENABLE_DIAG_UART to 1), even if SIDETNFS_ENABLE_DIAG_UART
# itself was left at its 0 default. There is no way to use this alias to
# force Diagnostic mode back OFF -- if either switch says "on", the build
# is Diagnostic -- so the two can never contradict each other. All
# existing SIDETNFS_ENABLE_DIAG_UART-gated code (see gemdrvemul.c,
# sidetnfs_probe.c, main.c) is completely unaffected: it still only ever
# checks _SIDETNFS_ENABLE_DIAG_UART, never this alias directly.
if("$ENV{SIDETNFS_BUILD_DIAGNOSTIC}" STREQUAL "1")
        set(_SIDETNFS_ENABLE_DIAG_UART 1)
endif()

if(${_SIDETNFS_ENABLE_DIAG_UART} STREQUAL "0")
        set(_SIDETNFS_BUILD_VARIANT "Production")
else()
        set(_SIDETNFS_BUILD_VARIANT "Diagnostic")
endif()

# Fase 10B: SIDETNFS_CONFIG_DRIVE_ONLY -- same passthrough pattern, default
# 0 (unchanged behavior) unless explicitly overridden, e.g.:
#   SIDETNFS_CONFIG_DRIVE_ONLY=1 SIDETNFS_ENABLE_SD_SUPPORT=0 SIDETNFS_ENABLE_DEBUG=0 ./build.sh pico_w
if("$ENV{SIDETNFS_CONFIG_DRIVE_ONLY}" STREQUAL "")
        set(_SIDETNFS_CONFIG_DRIVE_ONLY 0)
else()
        set(_SIDETNFS_CONFIG_DRIVE_ONLY $ENV{SIDETNFS_CONFIG_DRIVE_ONLY})
endif()

# Fase 12B2: SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM -- same passthrough
# pattern, default 0 (unchanged behavior). When 1: sidetnfs_config_init()
# still reads and (if SIDETNFS_ENABLE_DIAG_UART) dumps the REAL flash
# config exactly as normal, but main.c then overwrites the RAM config
# with factory defaults via sidetnfs_config_force_factory_ram() before
# any TNFS/runtime-drive code runs -- never writes to flash. Diagnostic
# recovery build only, e.g.:
#   SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM=1 SIDETNFS_ENABLE_DIAG_UART=1 SIDETNFS_ENABLE_DEBUG=0 ./build.sh pico_w
if("$ENV{SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM}" STREQUAL "")
        set(_SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM 0)
else()
        set(_SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM $ENV{SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM})
endif()

# Fase 12B3: SIDETNFS_ENABLE_SLOT_DIAG -- same passthrough pattern,
# default 0. Independent of the (larger, verbose) Fase 12B2
# sidetnfs_config_dump_uart()/runtime-table dump -- when this is 1, that
# older, more verbose dump is automatically suppressed (see
# gemdrvemul.c/main.c) so boot output stays compact, and this phase's own
# three-block Dump A/B/C fires instead. Still requires
# SIDETNFS_ENABLE_DIAG_UART=1 for the physical UART/stdio to actually be
# enabled -- this flag alone only controls whether the dump CODE is
# compiled in, e.g.:
#   SIDETNFS_ENABLE_DIAG_UART=1 SIDETNFS_ENABLE_SLOT_DIAG=1 SIDETNFS_ENABLE_DEBUG=0 ./build.sh pico_w
if("$ENV{SIDETNFS_ENABLE_SLOT_DIAG}" STREQUAL "")
        set(_SIDETNFS_ENABLE_SLOT_DIAG 0)
else()
        set(_SIDETNFS_ENABLE_SLOT_DIAG $ENV{SIDETNFS_ENABLE_SLOT_DIAG})
endif()

# Fase 12B4: SIDETNFS_ENABLE_SD_SLOT_DUMP -- same passthrough pattern,
# default 0. Completely independent of SIDETNFS_ENABLE_DIAG_UART/the
# UART-based SLOTDIAG A/B/C dumps above -- writes the same kind of
# information (plus file-handle/DTA/call-counter detail) to
# <hd_folder>/SLOTDIAG.TXT on SD instead, triggered by a SHORT SELECT
# press during normal operation (same edge-triggered pattern as the
# existing DEBUG.TXT dump), never at boot, never the 10s factory-reset
# long-press. No UART/stdio code is compiled in because of this flag --
# see gemdrvemul.c/sidetnfs_probe.c for the exact gating, e.g.:
#   SIDETNFS_ENABLE_DIAG_UART=0 SIDETNFS_ENABLE_SLOT_DIAG=1 SIDETNFS_ENABLE_SD_SLOT_DUMP=1 SIDETNFS_ENABLE_SD_SUPPORT=1 SIDETNFS_ENABLE_DEBUG=0 ./build.sh pico_w
if("$ENV{SIDETNFS_ENABLE_SD_SLOT_DUMP}" STREQUAL "")
        set(_SIDETNFS_ENABLE_SD_SLOT_DUMP 0)
else()
        set(_SIDETNFS_ENABLE_SD_SLOT_DUMP $ENV{SIDETNFS_ENABLE_SD_SLOT_DUMP})
endif()

# Fase 12B5: SIDETNFS_FORCE_CONFIG_RECOVERY -- same passthrough pattern,
# default 0. One-shot recovery build: as early as possible after minimal
# init, forcibly rewrites the SideTNFS drive-list flash sector to clean
# native-v3 factory defaults (S:/N: only) IF it doesn't already exactly
# match those defaults -- see sidetnfs_config_flash_matches_factory_defaults()
# and main.c's own recovery function for the full contract, including the
# reset-loop-avoidance check. No UART, no SD diagnosis of any kind -- see
# main.c for the exact zero-UART guarantee. E.g.:
#   SIDETNFS_FORCE_CONFIG_RECOVERY=1 SIDETNFS_ENABLE_SD_SUPPORT=1 SIDETNFS_ENABLE_DEBUG=0 ./build.sh pico_w
if("$ENV{SIDETNFS_FORCE_CONFIG_RECOVERY}" STREQUAL "")
        set(_SIDETNFS_FORCE_CONFIG_RECOVERY 0)
else()
        set(_SIDETNFS_FORCE_CONFIG_RECOVERY $ENV{SIDETNFS_FORCE_CONFIG_RECOVERY})
endif()

# Debug outputs
pico_enable_stdio_usb(${PROJECT_NAME} 0)
# Workaround to disable USB output in release builds. SIDETNFS_ENABLE_DIAG_UART
# additionally forces the physical UART on even at _DEBUG=0, for the
# temporary diagnostic build's SELECT-triggered snapshot dump -- DPRINTF
# itself stays gated on _DEBUG alone (unchanged), so a diag-UART build with
# _DEBUG=0 gets a live UART port but zero DPRINTF chatter during GEMDOS/bus
# handling.
if(${_DEBUG} STREQUAL "0" AND ${_SIDETNFS_ENABLE_DIAG_UART} STREQUAL "0")
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

# Pass the _DEBUG flag to the C compiler
add_definitions(-D_DEBUG=${_DEBUG})

# Fase 8B: pass SIDETNFS_ENABLE_SD_SUPPORT/SIDETNFS_ENABLE_DEBUG to the C
# compiler, same as _DEBUG above.
message("SIDETNFS_ENABLE_SD_SUPPORT: " ${_SIDETNFS_ENABLE_SD_SUPPORT})
message("SIDETNFS_ENABLE_DEBUG: " ${_SIDETNFS_ENABLE_DEBUG})
message("SIDETNFS_CONFIG_DRIVE_ONLY: " ${_SIDETNFS_CONFIG_DRIVE_ONLY})
message("SIDETNFS_ENABLE_DIAG_UART: " ${_SIDETNFS_ENABLE_DIAG_UART})
message("SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM: " ${_SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM})
message("SIDETNFS_ENABLE_SLOT_DIAG: " ${_SIDETNFS_ENABLE_SLOT_DIAG})
message("SIDETNFS_ENABLE_SD_SLOT_DUMP: " ${_SIDETNFS_ENABLE_SD_SLOT_DUMP})
message("SIDETNFS_FORCE_CONFIG_RECOVERY: " ${_SIDETNFS_FORCE_CONFIG_RECOVERY})
add_definitions(-DSIDETNFS_ENABLE_SD_SUPPORT=${_SIDETNFS_ENABLE_SD_SUPPORT})
add_definitions(-DSIDETNFS_ENABLE_DEBUG=${_SIDETNFS_ENABLE_DEBUG})
add_definitions(-DSIDETNFS_CONFIG_DRIVE_ONLY=${_SIDETNFS_CONFIG_DRIVE_ONLY})
add_definitions(-DSIDETNFS_ENABLE_DIAG_UART=${_SIDETNFS_ENABLE_DIAG_UART})
add_definitions(-DSIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM=${_SIDETNFS_FORCE_FACTORY_CONFIG_IN_RAM})
add_definitions(-DSIDETNFS_ENABLE_SLOT_DIAG=${_SIDETNFS_ENABLE_SLOT_DIAG})
add_definitions(-DSIDETNFS_ENABLE_SD_SLOT_DUMP=${_SIDETNFS_ENABLE_SD_SLOT_DUMP})
add_definitions(-DSIDETNFS_FORCE_CONFIG_RECOVERY=${_SIDETNFS_FORCE_CONFIG_RECOVERY})

# Optimization Flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}  -ffunction-sections -fdata-sections")
