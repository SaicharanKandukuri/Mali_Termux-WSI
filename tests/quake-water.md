# Quake: white water on Mali-G72

For textured, warped water in *The Necropolis*, keep `r_oldwater 0` and omit
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

The framebuffer copy populated mip level 0 correctly. The failure was in
`glGenerateMipmapEXT(GL_TEXTURE_2D)`: it returned `GL_INVALID_OPERATION`,
leaving the texture white when sampled with the game's mipmap filter.

Run the standalone regression after the [Termux setup](demos.md):

```sh
cc -std=c99 -Wall -Wextra -Werror "$W/tests/gl-copy-water.c" -L"$W/build" -lGL -lX11 -o "$D/gl-copy-water"
zen "$D/gl-copy-water"
zen "$D/gl-copy-water" --oes
```

Both commands should turn the blue pre-filled mip levels red and green
(exit 0). Exit 1 indicates a failed check; exit 2 means the display, context
or entry point was unavailable. On GLES1, the desktop EXT wrapper uses the
vendor OES function when exported. This probe does not verify GLES 2/3.
