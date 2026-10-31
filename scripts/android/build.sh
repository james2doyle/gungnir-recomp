#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SDK_DIR=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
JAVA_DIR=${JAVA_HOME:-}
DEPS_DIR="$ROOT/build/android-deps"
GRADLE_PROPS=()
BUILD_TYPE=debug
BUILD_TASK=:app:assembleDebug
CLEAN=0
OFFLINE=0

usage() {
    cat <<'EOF'
usage: scripts/android/build.sh [options] [-P<property>=<value> ...]

Builds the Android APK. Machine paths are passed as -P properties (or
ANDROID_HOME/JAVA_HOME); no local.properties is written.

options:
  --help                  show this help
  --release               assembleRelease instead of assembleDebug
  --clean                 run :app:clean first
  --offline               require the pinned SDL3 archive to be already present
  -Pgungnir.sdkDir=DIR    Android SDK root (default: ANDROID_HOME)
  -Pgungnir.javaHome=DIR  JDK 17-23 (default: JAVA_HOME, else java on PATH)
  -Pgungnir.depsDir=DIR   fetched-dependency cache (default: build/android-deps)
  -P<name>=<value>        any other Gradle project property
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --help)
            usage
            exit 0
            ;;
        --release)
            BUILD_TYPE=release
            BUILD_TASK=:app:assembleRelease
            ;;
        --clean)
            CLEAN=1
            ;;
        --offline)
            OFFLINE=1
            ;;
        -Pgungnir.sdkDir=*)
            SDK_DIR=${1#*=}
            ;;
        -Pgungnir.javaHome=*)
            JAVA_DIR=${1#*=}
            ;;
        -Pgungnir.depsDir=*)
            DEPS_DIR=${1#*=}
            ;;
        -P*)
            GRADLE_PROPS+=("$1")
            ;;
        *)
            echo "build.sh: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

TASKS=()
if [ "$CLEAN" -eq 1 ]; then
    TASKS+=(:app:clean)
fi
TASKS+=("$BUILD_TASK")

if [ -z "$SDK_DIR" ] || [ ! -d "$SDK_DIR" ]; then
    echo "missing_sdk: pass -Pgungnir.sdkDir=DIR or set ANDROID_HOME" >&2
    exit 1
fi
for component in \
    platforms/android-36 \
    build-tools/36.0.0 \
    ndk/28.2.13676358 \
    cmake/3.22.1; do
    if [ ! -d "$SDK_DIR/$component" ]; then
        echo "missing_sdk_component: $SDK_DIR/$component" >&2
        exit 1
    fi
done

if [ -z "$JAVA_DIR" ]; then
    JAVA_BIN=$(command -v java || true)
    if [ -n "$JAVA_BIN" ]; then
        JAVA_DIR=$(dirname "$(dirname "$JAVA_BIN")")
    fi
fi
if [ -z "$JAVA_DIR" ] || [ ! -x "$JAVA_DIR/bin/java" ]; then
    echo "missing_jdk: pass -Pgungnir.javaHome=DIR or set JAVA_HOME" >&2
    exit 1
fi
JAVA_MAJOR=$("$JAVA_DIR/bin/java" -version 2>&1 | sed -n 's/.*version "\([0-9][0-9]*\).*/\1/p' | head -1)
if [ -z "$JAVA_MAJOR" ] || [ "$JAVA_MAJOR" -lt 17 ] || [ "$JAVA_MAJOR" -gt 23 ]; then
    echo "unsupported_jdk: need JDK 17-23, found ${JAVA_MAJOR:-unknown}; pass -Pgungnir.javaHome=DIR" >&2
    exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "missing_cmake: cmake 3.22+ is required to materialise SDL3" >&2
    exit 1
fi
CMAKE_ARGS=("-DGUNGNIR_DEPS_DIR=$DEPS_DIR")
if [ "$OFFLINE" -eq 1 ]; then
    CMAKE_ARGS+=("-DGUNGNIR_OFFLINE=ON")
fi
cmake "${CMAKE_ARGS[@]}" -P "$ROOT/cmake/Deps.cmake"
if [ ! -d "$DEPS_DIR/sdl3-src/android-project/app/src/main/java" ]; then
    echo "missing_sdl3_java: $DEPS_DIR/sdl3-src" >&2
    exit 1
fi
GRADLE_PROPS+=("-PsdlSource=$DEPS_DIR/sdl3-src")

export ANDROID_HOME="$SDK_DIR"
export ANDROID_SDK_ROOT="$SDK_DIR"
export JAVA_HOME="$JAVA_DIR"
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$HOME/.gradle}"

"$ROOT/android/gradlew" "${GRADLE_PROPS[@]}" "-Pgungnir.depsDir=$DEPS_DIR" "${TASKS[@]}"

APK=$(find "$ROOT/android/app/build/outputs/apk/$BUILD_TYPE" -type f -name '*.apk' -print -quit 2>/dev/null)
if [ -z "$APK" ]; then
    echo "missing_apk: Gradle finished without an APK" >&2
    exit 1
fi
echo "APK: $APK"
