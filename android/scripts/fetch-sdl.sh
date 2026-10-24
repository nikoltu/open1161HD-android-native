#!/usr/bin/env bash
# Downloads the SDL2 source tree into android/app/jni/SDL and copies the
# org.libsdl.app Java support classes into the app source set. Both are
# build inputs that are deliberately not committed to the repository.
set -euo pipefail

SDL_VERSION="${SDL_VERSION:-2.30.9}"
SDL_MIXER_VERSION="${SDL_MIXER_VERSION:-2.8.1}"

cd "$(dirname "$0")/.."  # android/

if [ -f "app/jni/SDL/CMakeLists.txt" ]; then
    echo "SDL source already present, skipping download."
else
    echo "Fetching SDL2 ${SDL_VERSION}..."
    tmp="$(mktemp -d)"
    curl -fsSL -o "${tmp}/sdl.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz" \
    || curl -fsSL -o "${tmp}/sdl.tar.gz" \
        "https://www.libsdl.org/release/SDL2-${SDL_VERSION}.tar.gz"
    mkdir -p app/jni/SDL
    tar -xzf "${tmp}/sdl.tar.gz" -C app/jni/SDL --strip-components=1
    rm -rf "${tmp}"
fi

if [ -f "app/jni/SDL_mixer/CMakeLists.txt" ]; then
    echo "SDL2_mixer source already present, skipping download."
else
    echo "Fetching SDL2_mixer ${SDL_MIXER_VERSION}..."
    tmp="$(mktemp -d)"
    curl -fsSL -o "${tmp}/sdl_mixer.tar.gz" \
        "https://github.com/libsdl-org/SDL_mixer/releases/download/release-${SDL_MIXER_VERSION}/SDL2_mixer-${SDL_MIXER_VERSION}.tar.gz" \
    || curl -fsSL -o "${tmp}/sdl_mixer.tar.gz" \
        "https://www.libsdl.org/projects/SDL_mixer/release/SDL2_mixer-${SDL_MIXER_VERSION}.tar.gz"
    mkdir -p app/jni/SDL_mixer
    tar -xzf "${tmp}/sdl_mixer.tar.gz" -C app/jni/SDL_mixer --strip-components=1
    rm -rf "${tmp}"
fi

echo "Copying org.libsdl.app Java classes..."
mkdir -p app/src/main/java/org/libsdl/app
cp app/jni/SDL/android-project/app/src/main/java/org/libsdl/app/*.java \
   app/src/main/java/org/libsdl/app/

# The SDL android-project template ships its own gradle wrapper jar, which
# trips gradle/actions wrapper validation in CI; we only need the Java
# classes copied above, so drop the template's build files.
rm -rf app/jni/SDL/android-project/gradle app/jni/SDL/android-project/gradlew \
       app/jni/SDL/android-project/gradlew.bat

echo "Done."
