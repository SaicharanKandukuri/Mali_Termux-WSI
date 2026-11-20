# Quake: white water on Mali-G72

On the Galaxy M31 the fixed driver renders QuakeSpasm's normal water in
*The Necropolis* with warp mipmaps enabled. Keep `r_oldwater 0` and omit
`-nowarpmipmaps`. On older builds, `-nowarpmipmaps` is a workaround.

To check the fix (or reproduce the failure with the previous driver), start
**without** `-nowarpmipmaps` and enter these **Quake console** commands.
Wait for the map to finish loading before entering `setpos`:

```text
map e1m3
setpos -718 -123 -317 50 0 0
r_oldwater 0
```

Close the console and leave the camera still. Compare `r_oldwater 1`, then
`r_oldwater 0` again. Before the fix the same surface went **white → textured
→ white**; the brickwork stayed textured. The face uses map texture `*04water1`.
This checks the surface from above, not underwater screen effects.

The [default warp pass](https://github.com/sezero/quakespasm/blob/13f16f2b28f288ba29a0222c28bcf1e41d60cc5a/Quake/gl_warp.c#L224-L266)
draws a warped image into the framebuffer, copies it to a texture, and optionally
generates mipmaps. Classic water bypasses that pass.

## Where the white surface comes from

On this M31, nine RGB samples of the 512×512 warp matched before the framebuffer
copy and after sampling the copied base level with nearest filtering.
`glCopyTexSubImage2D` returned no error. The subsequent
`glGenerateMipmapEXT(GL_TEXTURE_2D)` produced `GL_INVALID_OPERATION` (0x0502);
sampling with the game's mipmap filter gave white at all nine points.
With warp mipmaps disabled, the same camera showed textured water even with
`r_oldwater 0`.

Run the standalone regression after the [Termux setup](demos.md):

```sh
cc -std=c99 -Wall -Wextra -Werror "$W/tests/gl-copy-water.c" -L"$W/build" -lGL -lX11 -o "$D/gl-copy-water"
zen "$D/gl-copy-water"
zen "$D/gl-copy-water" --oes
```

Both runs use the same ES1-backed GLX context type, red/green framebuffer
pattern and blue pre-filled mip levels. The desktop EXT entry failed and the
minified image stayed blue (exit 1); the GLES OES entry regenerated the mips,
turning the samples red and green (exit 0). Exit 2 means the display, context
or requested entry point was unavailable. This tests the entry point through
ZenFox, not just its presence in an extension string.

After the fix the desktop EXT wrapper selects the vendor OES function when it
is exported and the calling thread has a GLES 1 context. Other contexts keep
the previous lookup. On the M31 the same probe binary failed with the old
library but passed with the fixed one (both EXT and OES). The fixed library
also passed the GL 1.0 smoke test. Quake logged `FOUND: glGenerateMipmapEXT`
and showed textured water at the camera above without either workaround.
The pixel probe uses immediate-mode drawing; it does not verify GLES 2/3.
