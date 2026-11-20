# Run the ZenFox demos in native Termux

Run the setup and one demo at a time in the same shell. This uses a private
build directory, not replacement libraries in `$PREFIX/lib`. Tested graphics
stack: Galaxy M31, Android 13, Mali-G72. The original demos used revision
`471e044`; the default Quake water needs the subsequent ES1 mipmap fix.

## One-time setup

Install the [Termux:X11 Android app](https://github.com/termux/termux-x11)
and its matching Termux companion. In native 64-bit Termux:

```sh
pkg install x11-repo
pkg install termux-x11-nightly clang make git patch curl tar bsdtar coreutils libx11 libxcb libglvnd vulkan-headers glu freeglut sdl2
git clone -b test/gl10-baseline https://github.com/SaicharanKandukuri/Mali_Termux-WSI.git
cd Mali_Termux-WSI
W=$PWD
D="$W/build/demos"
R="$W/build/gl-runtime"
mkdir -p "$D"
```

If your Termux headers omit desktop GL/GLX or GLES, stage the missing header
families privately. Do not put desktop EGL headers ahead of Android's.

```sh
if ! printf '#include <GL/glx.h>\n#include <GL/glext.h>\n#include <GLES2/gl2.h>\n' | clang -E -x c - >/dev/null 2>&1; then
  git clone --depth 1 -b v1.7.0 https://github.com/NVIDIA/libglvnd.git "$D/glvnd-headers"
  mkdir -p "$D/include"
  for h in GL GLES2 GLES3 KHR; do cp -R "$D/glvnd-headers/include/$h" "$D/include/"; done
  export CPATH="$D/include${CPATH:+:$CPATH}"
fi
make -j2 gl-runtime glu glut
ln -sf ../libGLU.so.1 ../libglut.so.3 "$R/"
for n in libGL.so libOpenGL.so.0 libGLX.so.0; do ln -sf ../libGL.so.1 "$R/$n"; done

export DISPLAY=${DISPLAY:-:0}
# If no Termux:X11 server is running for this DISPLAY:
# termux-x11 "$DISPLAY" -noreset &
am start -n com.termux.x11/com.termux.x11.MainActivity
zen() { env GLX_ES=1 LD_LIBRARY_PATH="$R" "$@"; }
```

Use `-L"$W/build"` to link, `zen` to run. Neither `$W/build` nor
`$PREFIX/lib` belongs in the runtime override. In particular, exposing
ZenFox's unversioned `libEGL.so` broke Android gralloc on the M31.

## GL 1.0 baseline (automated, not a fullscreen demo)

```sh
clang -std=c99 -Wall -Wextra "$W/tests/gl10-baseline.c" -L"$W/build" -lGL -lX11 -o "$D/baseline"
zen "$D/baseline" --show
```

Expect `PASS pre-swap readback`; the 128×128 sample is intentionally fixed
for [pixel checks](README.md). It exits after showing the triangle.

## Mesa gears

```sh
cd "$D"
curl -fLO https://archive.mesa3d.org/demos/mesa-demos-8.4.0.tar.bz2
echo '01e99c94a0184e63e796728af89bfac559795fb2a0d6f506fa900455ca5fff7d  mesa-demos-8.4.0.tar.bz2' | sha256sum -c - &&
tar -xjf mesa-demos-8.4.0.tar.bz2 mesa-demos-8.4.0/src/xdemos/glxgears.c &&
clang mesa-demos-8.4.0/src/xdemos/glxgears.c -L"$W/build" -lGL -lX11 -lm -o gears &&
zen ./gears -fullscreen -info
```

Look for three lit, rotating gears and `GL_RENDERER = Mali-G72` on the M31.
The original M31 capture was windowed; the command above requests fullscreen.
The printed frame counter is not the display refresh rate.

## Dinoshade

```sh
mkdir -p "$D/dino" && cd "$D/dino"
curl -fLO https://www.opengl.org/archives/resources/code/samples/mjktips/dinoshade.c
echo '6e0bc34b38ad6f12ae718d16bdcea0ea27f89213f6451615848b91b36f10eec8  dinoshade.c' | sha256sum -c - &&
patch -p1 < "$W/tests/dinoshade-fullscreen.patch" &&
clang -Wno-incompatible-function-pointer-types dinoshade.c -L"$W/build" -lglut -lGLU -lGL -lm -o dinoshade &&
zen ./dinoshade
```

The patch uses the screen size for the window and projection and the core
polygon-offset entry instead of the unavailable EXT symbol. Use the driver's
GLU/GLUT: Termux's packaged GLUT required an unexported `glPushClientAttrib`.
The dinosaur's dark faces can look like gaps. At the same fixed camera and
light angle, desktop OpenGL showed the same shading. With lighting, reflection
and shadows off, the solid-color dinosaur covered exactly the same 5,000 pixels
on both renderers in a 270×500 sample, with or without face culling.

The confirmed M31 mismatch is the **floor**. In the default (non-mipmapped)
run, the demo uploads RGB bytes with the desktop internal format `3`.
Desktop GL accepted it; ZenFox on the M31 returned `GL_INVALID_VALUE` and
the green rings were absent. To reproduce the app-side comparison after
building the original above, change **only** that argument in a diagnostic copy:

```sh
cd "$D/dino"
sed 's/glTexImage2D(GL_TEXTURE_2D, 0, 3, 16, 16/glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16/' dinoshade.c > dinoshade-rgb.c
clang -Wno-incompatible-function-pointer-types dinoshade-rgb.c -L"$W/build" -lglut -lGLU -lGL -lm -o dinoshade-rgb
zen ./dinoshade-rgb
```

The same upload then returned no error and the rings reappeared. This is a
workaround for the demo, not a fix for desktop numeric internal formats.

## QuakeSpasm (Quake shareware)

```sh
cd "$D"
git clone https://github.com/sezero/quakespasm.git
cd quakespasm
git checkout --detach 13f16f2b28f288ba29a0222c28bcf1e41d60cc5a
git apply "$W/tests/quakespasm-es1.patch"
make -C Quake -j4 USE_SDL2=1 CC=clang USE_CODEC_MP3=0 USE_CODEC_VORBIS=0 LDFLAGS="-L$R"

mkdir -p "$D/shareware/id1" && cd "$D/shareware"
curl -fLO https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake/quake106.zip
echo 'ec6c9d34b1ae0252ac0066045b6611a7919c2a0d78a3a66d9387a8f597553239  quake106.zip' | sha256sum -c - &&
bsdtar -xf quake106.zip resource.1 &&
bsdtar -xOf resource.1 ID1/PAK0.PAK > id1/pak0.pak &&
echo '35a9c55e5e5a284a159ad2a62e0e8def23d829561fe2f54eb402dbc0a9a946af  id1/pak0.pak' | sha256sum -c -
cp "$D/quakespasm/Quake/quakespasm.pak" .

zen env SDL_VIDEODRIVER=x11 SDL_VIDEO_GL_DRIVER="$R/libGL.so.1" \
  "$D/quakespasm/Quake/quakespasm" -basedir "$PWD" -current -fullscreen \
  -nostdin -nojoy -novbo -noglsl -condebug +r_lightmapwide 0 +r_oldwater 0 +playdemo demo1
```

The engine patch uses `glFogf` and explicit RGBA internal formats. The original
numeric texture formats left walls white on this driver. Check `qconsole.log`
for the video mode, ARM renderer and `Playing demo from demo1.dem`. For the
[water regression](quake-water.md), keep warp mipmaps enabled and check the
same surface in E1M3. To measure throughput instead of showing normal playback,
replace `+playdemo demo1` with `+timedemo demo1`.
