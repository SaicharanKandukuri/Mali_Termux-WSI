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

Use [Mesa llvmpipe as a reference](demos.md#openarena-088-ioquake3-desktop-gl-11) for the lock and error checks.

## Game paths: OpenArena 0.8.8

Before the extension (revision `1a14de0`), the [stock ioquake3 selection rule](https://github.com/ioquake/ioq3/blob/e505e34d79d40585a5e2731c459404aefb0b2d1b/code/renderergl1/tr_shade.c#L163-L190)
in the classic GL1 renderer on Auto (`r_primitives 0`) logged
`GL_EXT_compiled_vertex_array not found` and `rendering primitives: multiple glArrayElement`.
With the extension, stock settings (`r_primitives 0`, `r_ext_compiled_vertex_array 1`)
log `compiled vertex arrays: enabled` and `rendering primitives: single glDrawElements`.

**Mali-G72, 1080×1997 fullscreen:** 51–52 FPS on Auto with indexed triangles; about 8 FPS on the strip path used by the old Auto setting (same 296-frame replay, swap interval 0, sound off).

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
