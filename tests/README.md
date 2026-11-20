# Desktop OpenGL 1.0 smoke test

From the repository root, build against desktop OpenGL and Xlib:

```sh
make -j2 gl-runtime
cc -std=c99 -Wall -Wextra tests/gl10-baseline.c -Lbuild -lGL -lX11 -o gl10-baseline
DISPLAY=:0 GLX_ES=1 LD_LIBRARY_PATH="$PWD/build/gl-runtime" ./gl10-baseline --show
```

Use the display and library search path for the implementation under test.
Record the selected library path as well as the printed vendor, renderer,
version, and GLX direct flag; the strings alone do not identify a library build.
The test uses a legacy GLX context and only GL 1.0 rendering calls. A newer
reported version is fine. This is not a conformance or performance test.

| Stage (before any swap) | Center (64,64) | Corner (8,8) |
| --- | --- | --- |
| Clear | blue (0,0,255) | blue (0,0,255) |
| Immediate-mode triangle | green (0,255,0) | blue (0,0,255) |

Actual RGB values may differ by at most 4 per channel; alpha is not scored.
Exit 0 means all checks passed; 1 means a pixel, GL error, or identity check
failed; 2 indicates usage or an explicit setup failure. Xlib can also terminate
on an X protocol error; retain stderr.

Pass `--show` to swap after scoring and display the frame for five seconds.
Check the triangle on the actual display or capture it separately. Pre-swap
readback does not prove X11 presentation, and a returned swap does not prove
visible pixels.

On a Galaxy M31 (Android 13, Mali-G72), all four readbacks passed and the
swapped triangle was visible in Termux:X11. Adding only the unversioned
`libEGL.so` shim to the runtime directory made gralloc abort before the first
readback; removing it restored the pass. Use `build` for linking and
`build/gl-runtime` for running the test. No root is required.
