# Serious Sam Classic: The First Encounter

Native ARM64 Termux, SDL2/X11, ZenFox desktop GL over GLES1. Uses the
[free demo and 1.05 patch](https://github.com/tx00100xt/SeriousSamClassic/blob/80b9893e5b74e5a2160eaf63e6d6b3f3981dfbbd/build-linux64demo.sh), not the HD edition or a retail install.

## Build and stage on macOS

Use a fresh directory. Set `PATCH` to the absolute path of the accompanying
[Termux build patch](serious-sam-tfe.patch). Install CMake, Ninja, flex, bison,
patchelf and the Android NDK. Set `N` to the NDK's
`toolchains/llvm/prebuilt/darwin-x86_64` directory (tested with r27d).
In Termux, install `sdl2 libogg libvorbis pulseaudio`. Export its headers
and SDL library **from Termux**; copy this archive into the Mac build directory:

```sh
tar -C "$PREFIX" -czf "$HOME/termux-sdl.tgz" include/SDL2 include/ogg include/vorbis lib/libSDL2.so lib/libSDL2-2.0.so
```

```sh
set -e
base=$PWD
mkdir deps
tar xf termux-sdl.tgz -C deps
git clone --filter=blob:none https://github.com/tx00100xt/SeriousSamClassic src
git -C src checkout --detach 80b9893e5b74e5a2160eaf63e6d6b3f3981dfbbd
git -C src apply "$PATCH"
cd src/SamTFE/Sources
bison -o Ecc/Parser.cpp -d Ecc/Parser.y
cp Ecc/Parser.hpp Ecc/Parser.h
flex -oEcc/Scanner.cpp Ecc/Scanner.l
xcrun clang++ -DPLATFORM_UNIX -I. Ecc/Main.cpp Ecc/Parser.cpp Ecc/Scanner.cpp -o "$base/ecc"
cd "$base"
cmake -S src/SamTFE/Sources -B build -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DUSE_ASM=OFF -DTFE=ON \
  -DCMAKE_BUILD_TYPE=Release -DECC="$base/ecc" \
  -DCMAKE_C_COMPILER="$N/bin/aarch64-linux-android29-clang" \
  -DCMAKE_CXX_COMPILER="$N/bin/aarch64-linux-android29-clang++" \
  -DCMAKE_CXX_FLAGS="-I$base/deps/include -Wno-enum-constexpr-conversion" \
  -DSDL2_INCLUDE_DIR="$base/deps/include/SDL2" -DSDL2_LIBRARY="$base/deps/lib/libSDL2.so" \
  -DZLIB_INCLUDE_DIR="$N/sysroot/usr/include" -DZLIB_LIBRARY="$N/sysroot/usr/lib/aarch64-linux-android/29/libz.so" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,--allow-shlib-undefined"
cmake --build build --target SeriousSam Entities Game Shaders amp11lib
mkdir -p stage/Bin demo update
cp build/SeriousSam build/Debug/*.so stage/Bin/
# Bionic plugins need explicit dependencies; these symbols are not global by default.
for f in stage/Bin/*; do patchelf --set-rpath /data/data/com.termux/files/usr/lib "$f"; done
patchelf --add-needed libSDL2-2.0.so --add-needed libz.so stage/Bin/libEngine.so
for f in libEntities.so libGame.so libShaders.so; do patchelf --add-needed libEngine.so "stage/Bin/$f"; done
patchelf --add-needed libEntities.so stage/Bin/libGame.so

curl -fL https://archive.org/download/SeriousSamDemo/SeriousSamDemo.exe -o demo.exe
curl -fL https://archive.org/download/SeriousSamPatches/serioussampatch105_usa.exe -o patch.exe
echo '029ec109adba4445e8c4365c88230c03fdad6abf98262a14c6fe88efdd686cd0  demo.exe' | shasum -a 256 -c -
echo '57efedff534f4cc429924ae031016919bbf0ab8760eb00bf62419885c5e2429b  patch.exe' | shasum -a 256 -c -
bsdtar -xf demo.exe -C demo
bsdtar -xf patch.exe -C update
cp -R src/SamTFE/{Controls,Data,Help,Levels,Players,Scripts,SE1_10b.gro} stage/
for dir in demo update; do
  rsync -r --exclude 'Bin/' --exclude 'Scripts/GLSettings/' \
    --include '*/' --include '*.gro' --include '*/*' --exclude '*' "$dir/Disk1/" stage/
done
COPYFILE_DISABLE=1 tar czf samtfe.tgz -C stage .
```

## Play fullscreen in Termux

Transfer `samtfe.tgz` to Termux and extract under its private home, not shared
storage. Use the [ZenFox runtime directory and active X display](demos.md#one-time-setup).

```sh
mkdir -p "$HOME/zen-samtfe-demo"
tar xf samtfe.tgz -C "$HOME/zen-samtfe-demo"
cd "$HOME/zen-samtfe-demo"
ln -sf "$PREFIX/lib/libvorbisfile.so" Bin/libvorbisfile.so
pulseaudio --check || pulseaudio --start
cat >Scripts/ZenReplay.ini <<'EOF'
sam_bFullScreen = 1;
sam_iScreenSizeI = 1280;
sam_iScreenSizeJ = 720;
sam_strIntroLevel = "";
sam_bAutoPlayDemos = 1;
hud_iStats = 1;
dem_bProfile = 1;
dem_strPostExec = "Quit();";
EOF
sam() {
  env GLX_ES=1 SDL_VIDEODRIVER=x11 SDL_VIDEO_GL_DRIVER="$R/libGL.so.1" \
    LD_LIBRARY_PATH="$R:$PWD/Bin" ./Bin/SeriousSam +script Scripts/ZenReplay.ini "$@"
}
sam +level 'Levels\KarnakDemo.wld'
```

## Replay and log FPS

`hud_iStats = 1;` enables the live counter. Without `+level`, the script skips
the intro and autoplays the first sorted demo. The engine profiles frame times
during playback; its final report distinguishes all frames from a filtered
average that omits peaks. This is real-time replay, not ioquake3's timedemo.

```sh
logs=$(mktemp -d "$HOME/samtfe-fps.XXXXXX")
for run in warmup 1 2 3; do
  name="ZenReplay-$run"
  timeout 180 env GLX_ES=1 SDL_VIDEODRIVER=x11 SDL_VIDEO_GL_DRIVER="$R/libGL.so.1" \
    LD_LIBRARY_PATH="$R:$PWD/Bin" ./Bin/SeriousSam +script Scripts/ZenReplay.ini \
    +logfile "$name" >"$logs/$run.stdout" 2>&1
  rc=$?
  cp "$HOME/.local/share/Serious-Engine/serioussam/$name.log" "$logs/$run.log" || break
  grep -E 'Starting display mode|ARM, Mali|Started playing demo|Originally recorded:|Without excessive peaks:' "$logs/$run.log"
  grep -q 'Originally recorded:.*FPS average' "$logs/$run.log" && [ "$rc" = 0 ] || { echo "Incomplete: $logs/$run.log"; break; }
done
```

Only compare completed runs of the same demo and settings; exclude warm-up.
On Mali-G72 the game starts, but the replay showed black geometry and tried
to join `serveraddress` before logging its final average. Do not use this run
as a performance baseline.
