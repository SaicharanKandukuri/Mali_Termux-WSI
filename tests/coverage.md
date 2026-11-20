# Desktop GL 1.x coverage

Target: native Termux, SDL/X11 or GLX, ZenFox's desktop-on-GLES1 runtime
(`GLX_ES=1`). An exported
symbol or reported GL version is not a behavior test. Anything not covered
by a named check below is **untested**, not a pass or an inferred failure.

## Core behavior

| Desktop spec | Tested | Limits |
| --- | --- | --- |
| [1.0](https://registry.khronos.org/OpenGL/specs/gl/glspec10.pdf) | [Pixel/state smoke tests](README.md): immediate triangle, depth, blending, texture upload and unpack, switching unshared contexts | Not a full test of these features; e.g. lighting, display lists, evaluators, selection/feedback and error rules are not covered by this baseline. |
| [1.1 §2.8](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf) | [Array probe](gl11-array-element.c): indexed draw, array-element strips, interleaved attributes and subsequent draws | Not the whole 1.1 API. Mixed immediate/array blocks have [documented limits](demos.md#openarena-088-ioquake3-desktop-gl-11). Buffer-object and multitexture cases are additional compatibility tests, not 1.1 requirements. |
| 1.2–1.5 | No version-wide verdict | Do not promote individual extension or game results to a core-version pass. |

For new core entries, start from the version's specification and the
[Khronos command registry](https://github.com/KhronosGroup/OpenGL-Registry/blob/main/xml/gl.xml).
Record the expected result and add a check for it; a callable name by itself
doesn't establish correct pixels, state transitions or GL errors.

### Piglit desktop baseline

On Mali-G72 with Waffle/GLX and `GLX_ES=1`, a [14-case subset](piglit-gl1-baseline.tsv)
of [Piglit at c8388b5](https://gitlab.freedesktop.org/mesa/piglit/-/tree/c8388b5932ddd582444b7a71a1d4cc84d3bc119b)
reported 5 passes and 9 failures, with no timeouts. This is not conformance:
`gl-1.1-draw-arrays-start` reported a pass despite logging pixel mismatches.
The two vertex-count cases used `100000 varray` with the indicated primitive.

The [readback control](gl-readback-control.c) isolates a shared failure: after a green clear,
RGBA/unsigned-byte reads green, but RGB/float and RGBA/float return `GL_INVALID_ENUM`
and leave the destination untouched. Piglit uses float reads for desktop color probes;
a failed comparison alone does not prove the draw was wrong. Separately, Piglit
could not resolve `glEdgeFlag` or `glColorMaterial`.

With the [Termux environment](demos.md#one-time-setup), run:

```sh
clang "$W/tests/gl-readback-control.c" -L"$W/build" -lGL -lX11 -o "$D/readback-control"
zen "$D/readback-control"
```

This diagnostic prints the GL error and destination for each call; exit zero
means the probe completed, not that all reads succeeded.

## Optional extensions

Record the **advertised name**, the **functions resolved by the application**,
and **behavior in that context** separately. Never call an unadvertised
extension merely because the loader returned an address. A missing optional
extension is a skip; an advertised extension with missing functions or
incorrect behavior is a failure. This table is not a list of all advertised extensions.

| Extension | Evidence on ZenFox | Application consequence |
| --- | --- | --- |
| [EXT_compiled_vertex_array](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_compiled_vertex_array.txt) | On desktop GL over GLES1, GL and SDL advertise it and SDL resolves both functions. The locked draw, per-context range, four query types, error cases and display-list checks pass. No vertex cache or extra geometry copy is used. | Unmodified OpenArena now selects indexed triangles on Auto. |
| EXT_framebuffer_object | Advertised; only the desktop mipmap-generation entry is verified by the [Quake water regression](quake-water.md), not the whole extension. | QuakeSpasm's default water samples textured mip levels after the fix. |
| [ARB_multitexture](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_multitexture.txt) | SDL resolves OpenArena's three ARB names through GLX. The array probe selects both texture units and draws distinct texture coordinates through those names. Other coordinate variants are not behavior-tested here. | OpenArena can combine a surface texture and its lightmap in a single pass. |

Use [Mesa llvmpipe as a reference](demos.md#openarena-088-ioquake3-desktop-gl-11) for the lock and error checks.

## Game paths: OpenArena 0.8.8

Before the extension (revision `1a14de0`), the [stock ioquake3 selection rule](https://github.com/ioquake/ioq3/blob/e505e34d79d40585a5e2731c459404aefb0b2d1b/code/renderergl1/tr_shade.c#L163-L190)
in the classic GL1 renderer on Auto (`r_primitives 0`) logged
`GL_EXT_compiled_vertex_array not found` and `rendering primitives: multiple glArrayElement`.
With the extension, stock settings (`r_primitives 0`, `r_ext_compiled_vertex_array 1`)
log `compiled vertex arrays: enabled` and `rendering primitives: single glDrawElements`.

**Mali-G72, 1080×1997 fullscreen:** 51–52 FPS on Auto with indexed triangles; about 8 FPS on the strip path used by the old Auto setting (same 296-frame replay, swap interval 0, sound off).

With indexed triangles in both cases, the same replay measured **55.6–56.1 FPS
with multitexturing**, versus **52.8–53.6 FPS without**. To compare, leave
`r_primitives 0` and launch with `+set r_ext_multitexture 1` or `0`.
Check the renderer log for `multitexture: enabled` or `disabled`.

To repeat: [build, launch fullscreen and record a match](demos.md#openarena-088-ioquake3-desktop-gl-11).
From the **main menu** (disconnect from the live match first), run these in the game console:

```text
\r_primitives 0
\timedemo 1
\cl_timedemoLog auto-1.log
\demo zen-oa
```

Wait for each replay to finish. Repeat Auto with a new filename. For a forced
comparison run the same recording with modes 1 and 2, saving distinct logs.
Read the frame count, elapsed time
and frame durations in the profile's `baseoa/` logs. Restore `timedemo 0`,
`cl_timedemoLog ""` and your original `r_primitives` afterwards.
These are engine frame times, not display refresh rates; different recordings
need not reproduce the same absolute numbers.
