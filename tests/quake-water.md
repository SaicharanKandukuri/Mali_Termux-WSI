# Quake: white water on Mali-G72

On the Galaxy M31, the fixed-function ZenFox runtime rendered textured walls in
QuakeSpasm but showed solid white water in *The Necropolis*. Use
`+r_oldwater 1` at startup for the classic textured water path.

To reproduce with the shareware episode, enter these **Quake console** commands.
Wait for the map to finish loading before entering `setpos`:

```text
map e1m3
setpos -718 -123 -317 50 0 0
r_oldwater 0
```

Close the console and leave the camera still. Compare `r_oldwater 1`, then
`r_oldwater 0` again. On this device the same surface went **white → textured
→ white**; the brickwork stayed textured. The face uses map texture `*04water1`.
This checks the surface from above, not underwater screen effects.

The [default warp pass](https://github.com/sezero/quakespasm/blob/13f16f2b28f288ba29a0222c28bcf1e41d60cc5a/Quake/gl_warp.c#L224-L266)
draws a warped image into the framebuffer, copies it to a texture, and optionally
generates mipmaps. Classic water bypasses that pass. The comparison isolates the
rendering path, not the failing GL call; it is a workaround, not a driver fix.
