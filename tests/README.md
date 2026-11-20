# Desktop OpenGL 1.0 smoke test

See [GL 1.x coverage](coverage.md) for the distinction between core behavior,
optional extensions and the rendering path a game actually selects.

From the repository root, build against desktop OpenGL and Xlib:

```sh
make -j2 gl-runtime
cc -std=c99 -Wall -Wextra tests/gl10-baseline.c -Lbuild -lGL -lX11 -o gl10-baseline
DISPLAY=:0 GLX_ES=1 LD_LIBRARY_PATH="$PWD/build/gl-runtime" ./gl10-baseline --show
```

Use the display and library search path for the implementation under test.
Record the selected library path as well as the printed vendor, renderer,
version, and GLX direct flag; the strings alone do not identify a library build.
The test uses a legacy GLX context. Most checks exercise GL 1.0; the named
RGB internal format and `glTexSubImage2D` update additionally exercise GL 1.1.
The complete binary is not a strict GL 1.0-only client or a conformance test.

Expected results come from the [desktop OpenGL 1.0 specification](https://registry.khronos.org/OpenGL/specs/gl/glspec10.pdf)
(1 July 1994). All samples below are taken before swapping.

| Check | Expected behavior | GL 1.0 reference |
| --- | --- | --- |
| Clear and draw | Both samples start blue. A green triangle changes the center but not the corner. | §§2.6–2.7, 4.2.3, 4.3.2 |
| Depth, `GL_LESS` | Against cleared depth 1, window depth 0.5 passes (red); 0.75 fails (still red); 0.25 passes (green). | §4.1.5 |
| Source-alpha blend | Half-alpha red over blue yields roughly half red and half blue, with no green. The corner stays blue. | §4.1.6 |
| Texture component counts | Upload counts 1–4 with matching luminance, luminance-alpha, RGB and RGBA data. Sample each over blue to check color and, where present, alpha. | §3.8, table 3.6 |
| RGB texture from RGBA bytes | With unpack alignment 8, upload two rows with different colors and source alpha. Both must render opaque over blue; the padding must not affect the second row or change the caller's alignment. | §§3.6.1, 3.8 |
| Named RGB and subimage update | Named RGB storage also drops source alpha; empty storage accepts a later RGB update. | [GL 1.1 §3.8](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf) |
| Switch unshared GLX contexts | A has blending enabled and green current color; fresh B has blending disabled and white current color. Switching back to A recovers its state. | §2.7 and chapter 6 state tables for defaults; GLX handles the switch |

The bound drawable must have at least 8 bits per RGB channel and 16 bits of depth.
The ±4 RGB readback tolerance is a smoke-test threshold, not a spec guarantee;
alpha is not scored. Exit 0 means all checks passed; 1 means a state, pixel, GL error, or identity check
failed; 2 indicates usage or an explicit setup failure. Xlib can also terminate
on an X protocol error; retain stderr.

Pass `--show` to swap after scoring and display the frame for five seconds.
Check the triangle on the actual display or capture it separately. Pre-swap
readback does not prove X11 presentation, and a returned swap does not prove
visible pixels.

Link against `build` and run with `build/gl-runtime`. Keep the unversioned
`libEGL.so` shim out of the runtime path so Android gralloc can load the system EGL library.

For sources, build commands and fullscreen launches for Mesa gears, dinoshade
and QuakeSpasm, see the [Termux demo recipes](demos.md). Record the same
details for each new demo, including any local patches and rendering defects.
