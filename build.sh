#!/bin/bash

# Install SDK needed for building
echo "Installing SDKs..."
git submodule init
git submodule update --init --recursive

# Pin the building versions
echo "Pinning the SDK versions..."
cd fatfs-sdk
#git checkout tags/v1.1.1
#git checkout v1.2.4
#git checkout v2.6.0
git checkout v1.2.4

cd ../pico-sdk
git checkout tags/1.5.1

cd ../pico-extras
git checkout tags/sdk-1.5.1
cd ..

# This is a dirty hack to guarantee that I can use the fatfs-sdk submodule
echo "Patching the fatfs-sdk... to use chmod"
sed -i.bak 's/#define FF_USE_CHMOD[[:space:]]*0/#define FF_USE_CHMOD 1/' fatfs-sdk/src/ff15/source/ffconf.h && mv fatfs-sdk/src/ff15/source/ffconf.h.bak .

# Set the environment variables of the SDKs
export FATFS_SDK_PATH=$PWD/fatfs-sdk
export PICO_SDK_PATH=$PWD/pico-sdk
export PICO_EXTRAS_PATH=$PWD/pico-extras

# Check if the third parameter is provided
export RELEASE_TYPE=${3:-""}
echo "Release type: $RELEASE_TYPE"

# Determine the file to use based on RELEASE_TYPE
if [ -z "$RELEASE_TYPE" ] || [ "$RELEASE_TYPE" = "final" ]; then
    VERSION_FILE="version.txt"
else
    VERSION_FILE="version-$RELEASE_TYPE.txt"
fi

# Read the release version from the version.txt file
export RELEASE_VERSION=$(cat "$VERSION_FILE" | tr -d '\r\n ')
echo "Release version: $RELEASE_VERSION"

# Get the release date and time from the current date
export RELEASE_DATE=$(date +"%Y-%m-%d %H:%M:%S")
echo "Release date: $RELEASE_DATE"

# Set the board type to be used for building
# If nothing passed as first argument, use pico w
export BOARD_TYPE=${1:-pico_w}
echo "Board type: $BOARD_TYPE"

# Set the release or debug build type
# If nothing passed as second argument, use release
export BUILD_TYPE=${2:-release}
echo "Build type: $BUILD_TYPE"

# If the build type is release, set DEBUG_MODE environment variable to 0
# Otherwise set it to 1
if [ "$BUILD_TYPE" = "release" ]; then
    export DEBUG_MODE=0
else
    export DEBUG_MODE=1
fi

# Buildvarianten vereenvoudigen en logisch hernoemen: the two officially
# supported, everyday build variants are "Production" (the default -- no
# extra diagnostic logging, meant to actually be flashed) and
# "Diagnostic" (explicit opt-in only -- extra debug/snapshot/serial
# logging). Never both from a single invocation, and the plain/default
# command below always produces Production only.
#
# SIDETNFS_BUILD_DIAGNOSTIC=1 is the new, preferred way to ask for the
# Diagnostic variant; the older, still-fully-supported
# SIDETNFS_ENABLE_DIAG_UART=1 internal macro works exactly as before too
# (CMakeLists.txt treats the two as a union -- either one alone is enough
# to select Diagnostic, so they can never contradict each other; there is
# no way to use SIDETNFS_BUILD_DIAGNOSTIC to force Diagnostic back off).
# This variable only affects artifact naming/labeling here in build.sh --
# the actual CMake definitions is where SIDETNFS_ENABLE_DIAG_UART itself
# gets its value (see CMakeLists.txt).
#   Production (default):  ./build.sh pico_w
#   Diagnostic (explicit):  SIDETNFS_BUILD_DIAGNOSTIC=1 ./build.sh pico_w
if [ "${SIDETNFS_BUILD_DIAGNOSTIC:-0}" = "1" ] || [ "${SIDETNFS_ENABLE_DIAG_UART:-0}" = "1" ]; then
    BUILD_VARIANT="Diagnostic"
    ARTIFACT_SUFFIX="diagnostic"
else
    BUILD_VARIANT="Production"
    ARTIFACT_SUFFIX="production"
fi
echo "Build variant: $BUILD_VARIANT"

# Fase 10B: SIDETNFS_CONFIG_DRIVE_ONLY (the old CONFIG_DRIVE_ONLY/
# SETTINGS-only legacy build) is untouched and still fully supported --
# but it is deliberately never part of the normal Production/Diagnostic
# workflow above: no dedicated artifact name, no separate reporting here.
# Build it only by setting the environment variable yourself, same as
# before this change, e.g.:
#   SIDETNFS_CONFIG_DRIVE_ONLY=1 SIDETNFS_ENABLE_SD_SUPPORT=0 ./build.sh pico_w

# Set the build directory. Delete previous contents if any
rm -rf build
mkdir build

# We assume that the last firmware was built for the same board type
# And previously pushed to the repo version

# Build the project
cd build
cmake ../romemul -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j4

# Copy the built firmware to the /dist folder (unchanged, historical
# release-packaging path/naming -- kept as-is)
cd ..
mkdir -p dist
if [ "$BUILD_TYPE" = "release" ]; then
    cp build/romemul.uf2 dist/sidecart-$BOARD_TYPE.uf2
else
    cp build/romemul.uf2 dist/sidecart-$BOARD_TYPE-$BUILD_TYPE.uf2
fi

# Copy the built firmware to build_artifacts/ using the new official
# Production/Diagnostic naming -- exactly one file per invocation, never
# both. Explicitly skipped for a SIDETNFS_CONFIG_DRIVE_ONLY=1 (legacy)
# build: that variant must never be copied under the "production" or
# "diagnostic" name (a CONFIG_DRIVE_ONLY/SETTINGS-only image silently
# named "sidetnfs_production.uf2" would be a real, dangerous mix-up --
# someone could flash it believing it is the normal, everyday firmware).
# build/romemul.uf2 (and the unchanged dist/ copy above) are still there
# for whoever explicitly asked for this legacy build to grab by hand.
if [ "${SIDETNFS_CONFIG_DRIVE_ONLY:-0}" = "1" ]; then
    echo "SIDETNFS_CONFIG_DRIVE_ONLY=1 (legacy build) -- not copied to build_artifacts/ under the Production/Diagnostic name; use build/romemul.uf2 directly."
else
    mkdir -p build_artifacts
    cp build/romemul.uf2 build_artifacts/sidetnfs_$ARTIFACT_SUFFIX.uf2
    echo "Artifact: build_artifacts/sidetnfs_$ARTIFACT_SUFFIX.uf2 ($BUILD_VARIANT)"
fi
