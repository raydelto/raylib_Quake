# GLQuake on raylib (`quake-gl`)

`quake-gl` is id's GLQuake renderer (`WinQuake/gl_*.c`) running in a raylib
window, next to the software `quake`. Input, sound, CD audio, networking and
the main loop are the same raylib drivers the software build uses; only the
video driver differs.

## Build and run

```sh
cmake -S . -B build -G Ninja      # quake-gl is on by default (-DQUAKE_GL=OFF skips it)
cmake --build build
./build/quake-gl -basedir "$PWD/data" -width 1280 -height 720
```

- `-width`/`-height`: the window. GLQuake renders at the window's real size,
  and the window can be resized afterwards.
- The 2D layer (console, menus, status bar) is the window divided by a
  whole-number scale, so its pixels stay sharp and square: `scr_scale`
  (saved; 0, the default, picks it as the software build does: 1 at
  640x480, 2 at 1280x720, 3 at 1920x1080). The 2D pictures are drawn
  nearest-sampled. `-conwidth N` / `-conheight N` fix the 2D size instead.
- `fov_adapt` (saved, default 1): Hor+, as in the software build (QUAK-38):
  `fov` is the 4:3 horizontal fov, wider screens see more at the sides and
  portrait ones keep the 4:3 vertical view.
- Brightness: the frame is drawn offscreen and shown through
  `out = in ^ (-gamma x gamma)`. `-gamma G` (default 0.7, the value GLQuake
  used on every card but a 3Dfx) is the display curve, and the `gamma` cvar
  is the Options → Brightness slider, so the slider works in GL too. Both
  apply to the finished picture, lighting included, as the software
  build's palette does (GLQuake applied -gamma to the textures only).
- Lighting (QUAK-45): `gl_overbright 1` (saved, default; read at map load)
  lets the lightmaps reach twice the texture's colour, as the software
  renderer's colormap does, and models are lit the same way through
  `GL_RGB_SCALE` 2. `gl_flashblend 0` (saved, default here) makes rockets,
  explosions and the muzzle flash light the walls through the lightmaps
  instead of drawing glow balls. `gl_keeptjunctions 1` removes the
  sparkling cracks between polygons. Textures are trilinear by default
  (`gl_texturemode GL_LINEAR_MIPMAP_LINEAR`; any GLQuake mode works).
- Cvars: `vid_vsync` (saved), `vid_fullscreen` / Alt+Enter, `r_wateralpha`
  (saved; the shareware maps aren't vised for see-through water), plus
  GLQuake's own (`gl_polyblend`, `r_shadows`, ...).
- Needs OpenGL 3.3 (desktop) or OpenGL ES 3 / WebGL 2.
- Browser build: `quake-gl.js` next to `quake.js`, loaded by the page for
  `?renderer=gl` (see docs/WEB.md).

## How it works: GL 1.1 on rlgl

GLQuake draws in OpenGL 1.1 immediate mode with fixed-function state.
`gl_raylib.c` implements the subset it calls on top of raylib's **rlgl**
(`gl_raylib.h` replaces `<GL/gl.h>`; the functions are renamed `rgl*` so
they can never bind to a system libGL):

- `glBegin`/`glVertex`/`glEnd` collect vertices; `glEnd` turns polygons,
  quads, fans and strips into triangles and adds them to a batch.
- Any state change first draws the batch with the state it was built under.
  Batches are appended to one stream vertex buffer, orphaned when full and
  every frame.
- One shader (GLSL 330 or GLSL ES 300, chosen at run time) does what the
  fixed-function pipeline did: texture env modulate / replace / untextured,
  the alpha test (`GL_GREATER`), and `glDepthRange` (applied to clip-space z).
- The modelview and projection stacks are kept on the CPU.
- Textures keep GLQuake's own numbers and map to rlgl ids. All formats are
  converted to RGBA8 (GL 3 core and GLES 3 have no luminance, intensity or
  alpha textures); mip levels are generated on the GPU.
- `glDepthFunc`, `glClear` (depth only), `glGetString` and `glBufferData`
  (orphaning) have no rlgl call and come from `rlGetProcAddress`. All four
  are core GLES 3 / WebGL 2 (the browser build links with
  `-sGL_ENABLE_GET_PROC_ADDRESS`), and `RGL_Init` fails if one is missing.
- `GL_RGB_SCALE` (GL 1.3) is honoured in any texture env mode, for
  overbright models.
- The frame goes to an offscreen framebuffer (colour texture + depth
  renderbuffer); `RGL_EndFrame (gamma)` draws it to the window with a
  one-triangle gamma shader. `glReadPixels` (screenshots) reads the
  offscreen frame and applies the same curve. Without a framebuffer it
  draws straight to the window, without gamma.
- The status bar is drawn every frame: GLQuake drew it into `vid.numpages`
  buffers and relied on swaps keeping them, which a WebGL canvas and most
  compositors don't (it vanished in the browser).

`gl_raylib.c` includes `rlgl.h` and no Quake header; `raylib.h` stays in
`bridge_raylib.c`, which gains `RL_BeginGLFrame`/`RL_EndGLFrame`.

Decision (QUAK-43): rlgl works for this, with one caveat. Its `rlBegin`
batch can't be used directly, because it has no alpha test, texture env or
depth range, and it applies matrices when the batch is drawn. So the shim
keeps its own batch and shader and uses rlgl for the context, buffers,
shaders, textures and state. Nothing in it needs desktop compatibility-profile
GL, so the same code should build for WebGL 2 (Stage 3, not tried yet).

## What doesn't match GLQuake yet

Tested on Mesa llvmpipe under Xvfb only.

- **Multitexture** (`GL_SGIS_multitexture`): off, so lightmaps are a second
  blended pass, as on cards without it. That's the same picture, but more
  draw calls.
- **8-bit paletted textures** (`GL_EXT_shared_texture_palette`, 3Dfx): not
  offered, so `VID_Is8bit` is false.
- **Fog** (`glFog*`): ignored. GLQuake's fog code is commented out anyway.
- **`gl_ztrick`** defaults to 0 (a depth clear every frame). With 1, the
  alternating depth ranges are emulated but not clipped at the far plane.
- **Points and lines** (`gl_test.c` debug drawing): dropped.
- **`glShadeModel(GL_FLAT)`**: drawn smooth. GLQuake only uses flat shading
  where all vertex colours are equal.
- **`.lit` coloured light**: not supported; lightmaps are luminance only.
- **Sky, water warp, particles, dynamic lights, 2D/HUD**: these render like
  GLQuake (see the PR screenshots).
- Fixed in QUAK-45: the crucified zombies in `start.bsp` were nearly black
  (GLQuake lit models at light/200 and clamped at 1).
