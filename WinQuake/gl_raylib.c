/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// gl_raylib.c -- GLQuake's OpenGL 1.1 calls on raylib's rlgl (see gl_raylib.h)

// How it works:
// - glBegin/glVertex/glEnd collect vertices; glEnd turns the primitive
//   (polygon, quads, fan, strip) into triangles and adds them to a batch.
// - Any state change (texture, blend, depth, alpha test, texture env,
//   matrices, ...) first draws the batch with the state it was built under.
//   Drawing appends the batch to one stream vertex buffer, which is
//   orphaned when full and at the start of every frame.
// - One shader does the fixed-function work GLQuake needs: modulate or
//   replace texture env (or no texture), alpha test, and glDepthRange,
//   which it applies to clip-space z. It also has GL 1.3's GL_RGB_SCALE
//   (in any env mode), which overbright models use.
// - The matrix stacks are kept here; the shader gets projection*modelview.
// - The frame is drawn into an offscreen framebuffer, which RGL_EndFrame
//   copies to the window through a gamma curve: the brightness (gamma)
//   applies to the finished picture, lighting included, as the software
//   renderer's palette does, not to the textures alone as GLQuake did.
// - Textures are kept by GLQuake's own numbers (it never calls
//   glGenTextures) and mapped to rlgl texture ids. Data is converted to
//   RGBA8, since luminance/intensity/alpha formats are gone from GL 3 core
//   and GLES 3. Mip levels above 0 are generated on the GPU instead of
//   taking GLQuake's.
//
// This only includes rlgl.h, so it runs wherever rlgl does with
// OpenGL 3.3 or OpenGL ES 3 (WebGL 2). rlgl has no call for glDepthFunc,
// a depth-only glClear, glGetString or buffer orphaning; those four entry
// points (all in GLES 3) come from rlGetProcAddress.

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rlgl.h"
#include "gl_raylib.h"

#define GL_ARRAY_BUFFER			0x8892
#define GL_STREAM_DRAW			0x88E0
#define GL_CLAMP_TO_EDGE		0x812F

typedef void (*depthfunc_t) (GLenum func);
typedef void (*clear_t) (GLbitfield mask);
typedef const GLubyte *(*getstring_t) (GLenum name);
typedef void (*bufferdata_t) (GLenum target, ptrdiff_t size,
	const void *data, GLenum usage);

static depthfunc_t	qglDepthFunc;
static clear_t		qglClear;
static getstring_t	qglGetString;
static bufferdata_t	qglBufferData;

/*
=============================================================================

STATE

=============================================================================
*/

typedef struct
{
	float			x, y, z;
	float			s, t;
	unsigned char	c[4];
} rvert_t;

// stream buffer, in vertices (24 bytes each)
#define STREAM_VERTS	(1 << 17)

// batched triangles waiting for a draw
static rvert_t	batch[STREAM_VERTS];
static int		numbatch;

// the primitive between glBegin and glEnd
#define MAX_PRIM_VERTS	8192
static rvert_t	prim[MAX_PRIM_VERTS];
static int		numprim;
static int		primmode = -1;		// -1 outside glBegin/glEnd

static float			cur_s, cur_t;
static unsigned char	cur_c[4] = { 255, 255, 255, 255 };

typedef struct
{
	unsigned	id;			// rlgl texture id, 0 if never uploaded
	int			width, height;
	int			hasmips;
	int			minfilter, magfilter;	// as GLQuake asked
	int			appliedmin, appliedmag;
	int			wraps, wrapt;
} rtex_t;

static rtex_t	*textures;
static int		numtextures;

// the draw state; drawn batches use what it was when they were built
typedef struct
{
	int		texture2d;
	int		texmode;		// GL_MODULATE or GL_REPLACE
	float	rgbscale;		// GL_RGB_SCALE: 1, 2 or 4
	int		boundtex;
	int		alphatest;
	float	alpharef;		// GL_GREATER only, which is all GLQuake uses
	int		blend;
	int		blendsrc, blenddst;
	int		depthtest;
	int		depthmask;
	int		depthfunc;
	float	depthnear, depthfar;
	int		cullface;
	int		cullmode;
	int		viewport[4];
} rstate_t;

static rstate_t	state;		// what GLQuake asked for
static rstate_t	applied;	// what the context has
static int		appliedvalid;

// matrix stacks
#define MODELVIEW_DEPTH		32
#define PROJECTION_DEPTH	4

static float	modelview[MODELVIEW_DEPTH][16];
static float	projection[PROJECTION_DEPTH][16];
static int		mvdepth, projdepth;
static int		matrixmode = GL_MODELVIEW;

static unsigned	vao, vbo;
static int		streamused;		// vertices used in the stream buffer
static unsigned	program;
static int		loc_mvp, loc_depthrange, loc_texmode, loc_alpharef, loc_tex;
static int		loc_rgbscale;

static int		fbwidth, fbheight;
static int		statdraws, statverts;

// the offscreen frame (0 if it couldn't be made: then drawing goes
// straight to the window and gamma is not applied)
static unsigned	fbo, fbocolor, fbodepth;
static int		fbowidth, fboheight;
static unsigned	postprogram;
static int		loc_postgamma, loc_posttex;
static float	lastgamma = 1;
static int		insideframe;	// between RGL_BeginFrame and RGL_EndFrame

static unsigned char	clearcolor[4];

/*
=============================================================================

MATRICES

=============================================================================
*/

static void MatIdentity (float *m)
{
	memset (m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1;
}

// out = a * b, column-major as OpenGL keeps them
static void MatMul (float *out, const float *a, const float *b)
{
	float	r[16];
	int		i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			r[j*4+i] = a[0*4+i]*b[j*4+0] + a[1*4+i]*b[j*4+1]
				+ a[2*4+i]*b[j*4+2] + a[3*4+i]*b[j*4+3];
	memcpy (out, r, sizeof(r));
}

static void Flush (void);

static float *CurrentMatrix (void)
{
	if (matrixmode == GL_PROJECTION)
		return projection[projdepth];
	return modelview[mvdepth];
}

static void MultCurrent (const float *m)
{
	float	*cur;

	Flush ();
	cur = CurrentMatrix ();
	MatMul (cur, cur, m);
}

void rglMatrixMode (GLenum mode)
{
	matrixmode = mode;
}

void rglLoadIdentity (void)
{
	Flush ();
	MatIdentity (CurrentMatrix ());
}

void rglLoadMatrixf (const GLfloat *m)
{
	Flush ();
	memcpy (CurrentMatrix (), m, 16 * sizeof(float));
}

void rglPushMatrix (void)
{
	if (matrixmode == GL_PROJECTION)
	{
		if (projdepth + 1 >= PROJECTION_DEPTH)
			return;
		memcpy (projection[projdepth+1], projection[projdepth], sizeof(projection[0]));
		projdepth++;
	}
	else
	{
		if (mvdepth + 1 >= MODELVIEW_DEPTH)
			return;
		memcpy (modelview[mvdepth+1], modelview[mvdepth], sizeof(modelview[0]));
		mvdepth++;
	}
}

void rglPopMatrix (void)
{
	Flush ();
	if (matrixmode == GL_PROJECTION)
	{
		if (projdepth > 0)
			projdepth--;
	}
	else if (mvdepth > 0)
		mvdepth--;
}

void rglTranslatef (GLfloat x, GLfloat y, GLfloat z)
{
	float	m[16];

	MatIdentity (m);
	m[12] = x;
	m[13] = y;
	m[14] = z;
	MultCurrent (m);
}

void rglScalef (GLfloat x, GLfloat y, GLfloat z)
{
	float	m[16];

	MatIdentity (m);
	m[0] = x;
	m[5] = y;
	m[10] = z;
	MultCurrent (m);
}

void rglRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
	float	m[16];
	float	len, c, s, ic;

	len = sqrtf (x*x + y*y + z*z);
	if (len == 0)
		return;
	x /= len;
	y /= len;
	z /= len;
	c = cosf (angle * (float)M_PI / 180.0f);
	s = sinf (angle * (float)M_PI / 180.0f);
	ic = 1.0f - c;

	MatIdentity (m);
	m[0] = x*x*ic + c;
	m[1] = y*x*ic + z*s;
	m[2] = x*z*ic - y*s;
	m[4] = x*y*ic - z*s;
	m[5] = y*y*ic + c;
	m[6] = y*z*ic + x*s;
	m[8] = x*z*ic + y*s;
	m[9] = y*z*ic - x*s;
	m[10] = z*z*ic + c;
	MultCurrent (m);
}

void rglFrustum (GLdouble l, GLdouble r, GLdouble b, GLdouble t,
	GLdouble n, GLdouble f)
{
	float	m[16];

	memset (m, 0, sizeof(m));
	m[0] = (float)(2*n / (r - l));
	m[5] = (float)(2*n / (t - b));
	m[8] = (float)((r + l) / (r - l));
	m[9] = (float)((t + b) / (t - b));
	m[10] = (float)(-(f + n) / (f - n));
	m[11] = -1;
	m[14] = (float)(-2*f*n / (f - n));
	MultCurrent (m);
}

void rglOrtho (GLdouble l, GLdouble r, GLdouble b, GLdouble t,
	GLdouble n, GLdouble f)
{
	float	m[16];

	MatIdentity (m);
	m[0] = (float)(2 / (r - l));
	m[5] = (float)(2 / (t - b));
	m[10] = (float)(-2 / (f - n));
	m[12] = (float)(-(r + l) / (r - l));
	m[13] = (float)(-(t + b) / (t - b));
	m[14] = (float)(-(f + n) / (f - n));
	MultCurrent (m);
}

void rglGetFloatv (GLenum pname, GLfloat *params)
{
	if (pname == GL_MODELVIEW_MATRIX)
		memcpy (params, modelview[mvdepth], 16 * sizeof(float));
	else if (pname == GL_PROJECTION_MATRIX)
		memcpy (params, projection[projdepth], 16 * sizeof(float));
}

/*
=============================================================================

DRAWING

=============================================================================
*/

static void ApplyBlend (void)
{
	rlSetBlendFactors (state.blendsrc, state.blenddst, RL_FUNC_ADD);
	rlSetBlendMode (RL_BLEND_CUSTOM);
}

// Brings the context in line with state, touching only what changed.
static void ApplyState (void)
{
	int		force = !appliedvalid;
	int		*v;

	if (force || state.blend != applied.blend)
	{
		if (state.blend)
			rlEnableColorBlend ();
		else
			rlDisableColorBlend ();
	}
	if (force || state.blendsrc != applied.blendsrc
		|| state.blenddst != applied.blenddst)
		ApplyBlend ();

	if (force || state.depthtest != applied.depthtest)
	{
		if (state.depthtest)
			rlEnableDepthTest ();
		else
			rlDisableDepthTest ();
	}
	if (force || state.depthmask != applied.depthmask)
	{
		if (state.depthmask)
			rlEnableDepthMask ();
		else
			rlDisableDepthMask ();
	}
	if ((force || state.depthfunc != applied.depthfunc) && qglDepthFunc)
		qglDepthFunc (state.depthfunc);

	if (force || state.cullface != applied.cullface)
	{
		if (state.cullface)
			rlEnableBackfaceCulling ();
		else
			rlDisableBackfaceCulling ();
	}
	if (force || state.cullmode != applied.cullmode)
		rlSetCullFace (state.cullmode == GL_FRONT ? RL_CULL_FACE_FRONT
			: RL_CULL_FACE_BACK);

	v = state.viewport;
	if (force || memcmp (v, applied.viewport, sizeof(state.viewport)))
		rlViewport (v[0], v[1], v[2], v[3]);

	applied = state;
	appliedvalid = 1;
}

static void SetUniformMatrix (int loc, const float *m)
{
	Matrix	mat;

	// raylib's Matrix fields are named by row; m0..m3 is the first column
	mat.m0 = m[0];  mat.m1 = m[1];  mat.m2 = m[2];  mat.m3 = m[3];
	mat.m4 = m[4];  mat.m5 = m[5];  mat.m6 = m[6];  mat.m7 = m[7];
	mat.m8 = m[8];  mat.m9 = m[9];  mat.m10 = m[10]; mat.m11 = m[11];
	mat.m12 = m[12]; mat.m13 = m[13]; mat.m14 = m[14]; mat.m15 = m[15];
	rlSetUniformMatrix (loc, mat);
}

// Draws the batch with the current state.
static void Flush (void)
{
	float	mvp[16];
	float	depthrange[2];
	float	alpharef;
	int		texmode;
	int		zero = 0;
	rtex_t	*tex = NULL;

	if (!numbatch)
		return;

	ApplyState ();

	if (state.texture2d && state.boundtex > 0 && state.boundtex < numtextures
		&& textures[state.boundtex].id)
		tex = &textures[state.boundtex];

	// Out of room: orphan the buffer, so the driver gives us fresh memory
	// instead of waiting for draws still reading the old one.
	if (streamused + numbatch > STREAM_VERTS)
	{
		rlEnableVertexBuffer (vbo);
		qglBufferData (GL_ARRAY_BUFFER, STREAM_VERTS * sizeof(rvert_t),
			NULL, GL_STREAM_DRAW);
		streamused = 0;
	}

	rlUpdateVertexBuffer (vbo, batch, numbatch * sizeof(rvert_t),
		streamused * sizeof(rvert_t));

	MatMul (mvp, projection[projdepth], modelview[mvdepth]);
	depthrange[0] = state.depthnear;
	depthrange[1] = state.depthfar;
	// texture unit disabled: the vertex colour alone
	texmode = !tex ? 0 : state.texmode == GL_REPLACE ? 2 : 1;
	alpharef = state.alphatest ? state.alpharef : -1.0f;

	rlEnableShader (program);
	SetUniformMatrix (loc_mvp, mvp);
	rlSetUniform (loc_depthrange, depthrange, RL_SHADER_UNIFORM_VEC2, 1);
	rlSetUniform (loc_texmode, &texmode, RL_SHADER_UNIFORM_INT, 1);
	rlSetUniform (loc_alpharef, &alpharef, RL_SHADER_UNIFORM_FLOAT, 1);
	rlSetUniform (loc_rgbscale, &state.rgbscale, RL_SHADER_UNIFORM_FLOAT, 1);
	rlSetUniform (loc_tex, &zero, RL_SHADER_UNIFORM_INT, 1);

	rlActiveTextureSlot (0);
	rlEnableTexture (tex ? tex->id : rlGetTextureIdDefault ());

	rlEnableVertexArray (vao);
	rlDrawVertexArray (streamused, numbatch);
	rlDisableVertexArray ();

	streamused += numbatch;
	statdraws++;
	statverts += numbatch;
	numbatch = 0;
}

void rglBegin (GLenum mode)
{
	primmode = mode;
	numprim = 0;
}

static void EmitVertex (float x, float y, float z)
{
	rvert_t	*v;

	if (primmode < 0 || numprim >= MAX_PRIM_VERTS)
		return;
	v = &prim[numprim++];
	v->x = x;
	v->y = y;
	v->z = z;
	v->s = cur_s;
	v->t = cur_t;
	memcpy (v->c, cur_c, 4);
}

void rglVertex2f (GLfloat x, GLfloat y)
{
	EmitVertex (x, y, 0);
}

void rglVertex3f (GLfloat x, GLfloat y, GLfloat z)
{
	EmitVertex (x, y, z);
}

void rglVertex3fv (const GLfloat *v)
{
	EmitVertex (v[0], v[1], v[2]);
}

void rglTexCoord2f (GLfloat s, GLfloat t)
{
	cur_s = s;
	cur_t = t;
}

void rglTexCoord2fv (const GLfloat *v)
{
	cur_s = v[0];
	cur_t = v[1];
}

static unsigned char ColorByte (float f)
{
	if (f <= 0)
		return 0;
	if (f >= 1)
		return 255;
	return (unsigned char)(f * 255.0f + 0.5f);
}

void rglColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	cur_c[0] = ColorByte (r);
	cur_c[1] = ColorByte (g);
	cur_c[2] = ColorByte (b);
	cur_c[3] = ColorByte (a);
}

void rglColor3f (GLfloat r, GLfloat g, GLfloat b)
{
	rglColor4f (r, g, b, 1);
}

void rglColor4fv (const GLfloat *v)
{
	rglColor4f (v[0], v[1], v[2], v[3]);
}

void rglColor3ubv (const GLubyte *v)
{
	cur_c[0] = v[0];
	cur_c[1] = v[1];
	cur_c[2] = v[2];
	cur_c[3] = 255;
}

void rglColor4ubv (const GLubyte *v)
{
	memcpy (cur_c, v, 4);
}

static void AddTriangle (int a, int b, int c)
{
	batch[numbatch++] = prim[a];
	batch[numbatch++] = prim[b];
	batch[numbatch++] = prim[c];
}

void rglEnd (void)
{
	int		i, n, tris;

	n = numprim;
	switch (primmode)
	{
	case GL_TRIANGLES:
		tris = n / 3;
		break;
	case GL_QUADS:
		tris = n / 4 * 2;
		break;
	case GL_TRIANGLE_STRIP:
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		tris = n >= 3 ? n - 2 : 0;
		break;
	default:
		tris = 0;	// points and lines: only the gl_test.c debug code
		break;
	}

	if (numbatch + tris * 3 > STREAM_VERTS)
		Flush ();

	switch (primmode)
	{
	case GL_TRIANGLES:
		for (i = 0; i + 2 < n; i += 3)
			AddTriangle (i, i+1, i+2);
		break;
	case GL_QUADS:
		for (i = 0; i + 3 < n; i += 4)
		{
			AddTriangle (i, i+1, i+2);
			AddTriangle (i, i+2, i+3);
		}
		break;
	case GL_TRIANGLE_STRIP:
		// keep every triangle's winding the same as the first one's
		for (i = 2; i < n; i++)
		{
			if (i & 1)
				AddTriangle (i-1, i-2, i);
			else
				AddTriangle (i-2, i-1, i);
		}
		break;
	case GL_TRIANGLE_FAN:
	case GL_POLYGON:
		for (i = 2; i < n; i++)
			AddTriangle (0, i-1, i);
		break;
	}

	primmode = -1;
	numprim = 0;
}

/*
=============================================================================

STATE CALLS

=============================================================================
*/

// Draws what was batched under the old state if this call changes it.
#define SETSTATE(field, value)	do { \
		if (state.field != (value)) { Flush (); state.field = (value); } \
	} while (0)

static void SetCap (GLenum cap, int on)
{
	switch (cap)
	{
	case GL_TEXTURE_2D:		SETSTATE (texture2d, on); break;
	case GL_ALPHA_TEST:		SETSTATE (alphatest, on); break;
	case GL_BLEND:			SETSTATE (blend, on); break;
	case GL_DEPTH_TEST:		SETSTATE (depthtest, on); break;
	case GL_CULL_FACE:		SETSTATE (cullface, on); break;
	default:
		break;		// fog, polygon offset, shared palette: not used
	}
}

void rglEnable (GLenum cap)
{
	SetCap (cap, 1);
}

void rglDisable (GLenum cap)
{
	SetCap (cap, 0);
}

void rglAlphaFunc (GLenum func, GLclampf ref)
{
	(void)func;
	SETSTATE (alpharef, ref);
}

void rglBlendFunc (GLenum sfactor, GLenum dfactor)
{
	SETSTATE (blendsrc, (int)sfactor);
	SETSTATE (blenddst, (int)dfactor);
}

void rglCullFace (GLenum mode)
{
	SETSTATE (cullmode, (int)mode);
}

void rglDepthFunc (GLenum func)
{
	SETSTATE (depthfunc, (int)func);
}

void rglDepthMask (GLboolean flag)
{
	SETSTATE (depthmask, flag ? 1 : 0);
}

void rglDepthRange (GLclampd znear, GLclampd zfar)
{
	SETSTATE (depthnear, (float)znear);
	SETSTATE (depthfar, (float)zfar);
}

void rglTexEnvf (GLenum target, GLenum pname, GLfloat param)
{
	(void)target;
	if (pname == GL_TEXTURE_ENV_MODE)
		SETSTATE (texmode, (int)param);
	else if (pname == GL_RGB_SCALE)
		SETSTATE (rgbscale, param);
}

void rglViewport (GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (state.viewport[0] == x && state.viewport[1] == y
		&& state.viewport[2] == width && state.viewport[3] == height)
		return;
	Flush ();
	state.viewport[0] = x;
	state.viewport[1] = y;
	state.viewport[2] = width;
	state.viewport[3] = height;
}

void rglClearColor (GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	clearcolor[0] = ColorByte (r);
	clearcolor[1] = ColorByte (g);
	clearcolor[2] = ColorByte (b);
	clearcolor[3] = ColorByte (a);
}

void rglClear (GLbitfield mask)
{
	Flush ();
	ApplyState ();		// a clear honours the depth mask
	rlClearColor (clearcolor[0], clearcolor[1], clearcolor[2], clearcolor[3]);
	if (qglClear)
		qglClear (mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
	else
		rlClearScreenBuffers ();
}

// Nothing to do: one shader, filled triangles, smooth shading (flat
// shading only ever got colours that were the same anyway), no fog.
void rglShadeModel (GLenum mode) { (void)mode; }
void rglPolygonMode (GLenum face, GLenum mode) { (void)face; (void)mode; }
void rglHint (GLenum target, GLenum mode) { (void)target; (void)mode; }
void rglFogf (GLenum pname, GLfloat param) { (void)pname; (void)param; }
void rglFogfv (GLenum pname, const GLfloat *params) { (void)pname; (void)params; }
void rglFogi (GLenum pname, GLint param) { (void)pname; (void)param; }

// Always the back buffer: R_TimeRefresh_f's front-buffer drawing just
// isn't shown until the swap.
void rglDrawBuffer (GLenum mode) { (void)mode; }
void rglReadBuffer (GLenum mode) { (void)mode; }

void rglFlush (void)
{
	Flush ();
}

void rglFinish (void)
{
	Flush ();
}

const GLubyte *rglGetString (GLenum name)
{
	const GLubyte	*s = NULL;

	// GL_EXTENSIONS is an error in a core profile; GLQuake only looks
	// for extensions this shim doesn't offer anyway.
	if (name != GL_EXTENSIONS && qglGetString)
		s = qglGetString (name);
	return s ? s : (const GLubyte *)"";
}

/*
=============================================================================

TEXTURES

=============================================================================
*/

static rtex_t *BoundTexture (void)
{
	int		n = state.boundtex;
	int		newcount;

	if (n <= 0)
		return NULL;
	if (n >= numtextures)
	{
		newcount = numtextures ? numtextures : 1024;
		while (newcount <= n)
			newcount *= 2;
		textures = realloc (textures, newcount * sizeof(*textures));
		if (!textures)
		{
			fprintf (stderr, "gl_raylib: out of memory for textures\n");
			exit (1);
		}
		memset (textures + numtextures, 0,
			(newcount - numtextures) * sizeof(*textures));
		numtextures = newcount;
	}
	return &textures[n];
}

void rglBindTexture (GLenum target, GLuint texture)
{
	(void)target;
	SETSTATE (boundtex, (int)texture);
}

// A mipmap minification filter needs mip levels; without them use the
// plain filter it starts from, as the texture would be incomplete.
static int MinFilter (const rtex_t *t)
{
	if (t->hasmips)
		return t->minfilter;
	switch (t->minfilter)
	{
	case GL_NEAREST_MIPMAP_NEAREST:
	case GL_NEAREST_MIPMAP_LINEAR:
		return GL_NEAREST;
	case GL_LINEAR_MIPMAP_NEAREST:
	case GL_LINEAR_MIPMAP_LINEAR:
		return GL_LINEAR;
	}
	return t->minfilter;
}

static void ApplyTexParams (rtex_t *t)
{
	int		f;

	if (!t->id)
		return;
	f = MinFilter (t);
	if (f != t->appliedmin)
	{
		rlTextureParameters (t->id, RL_TEXTURE_MIN_FILTER, f);
		t->appliedmin = f;
	}
	if (t->magfilter != t->appliedmag)
	{
		rlTextureParameters (t->id, RL_TEXTURE_MAG_FILTER, t->magfilter);
		t->appliedmag = t->magfilter;
	}
}

void rglTexParameterf (GLenum target, GLenum pname, GLfloat param)
{
	rtex_t	*t;
	int		p = (int)param;

	(void)target;
	t = BoundTexture ();
	if (!t)
		return;
	Flush ();

	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER:
		t->minfilter = p;
		break;
	case GL_TEXTURE_MAG_FILTER:
		t->magfilter = p;
		break;
	case GL_TEXTURE_WRAP_S:
	case GL_TEXTURE_WRAP_T:
		if (p == GL_CLAMP)
			p = GL_CLAMP_TO_EDGE;
		if (pname == GL_TEXTURE_WRAP_S)
			t->wraps = p;
		else
			t->wrapt = p;
		if (t->id)
			rlTextureParameters (t->id, pname == GL_TEXTURE_WRAP_S
				? RL_TEXTURE_WRAP_S : RL_TEXTURE_WRAP_T, p);
		return;
	default:
		return;
	}
	ApplyTexParams (t);
}

// Converts width x height pixels of format to RGBA8 in out. internalformat
// decides what one channel means: an intensity texture has it in alpha too,
// and an RGB (3 component) texture is opaque whatever alpha it was given.
static void ToRGBA (unsigned char *out, const unsigned char *in, int count,
	GLint internalformat, GLenum format)
{
	int		i;
	int		opaque = internalformat == 3 || internalformat == GL_RGB;

	for (i = 0; i < count; i++, out += 4)
	{
		switch (format)
		{
		case GL_RGBA:
			out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3];
			in += 4;
			break;
		case GL_RGB:
			out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = 255;
			in += 3;
			break;
		case GL_LUMINANCE_ALPHA:
			out[0] = out[1] = out[2] = in[0]; out[3] = in[1];
			in += 2;
			break;
		case GL_ALPHA:
			out[0] = out[1] = out[2] = 255; out[3] = in[0];
			in++;
			break;
		case GL_INTENSITY:
			out[0] = out[1] = out[2] = out[3] = in[0];
			in++;
			break;
		default:	// GL_LUMINANCE
			out[0] = out[1] = out[2] = in[0];
			out[3] = internalformat == GL_INTENSITY ? in[0] : 255;
			in++;
			break;
		}
		if (opaque)
			out[3] = 255;
	}
}

static unsigned char	*convbuf;
static int				convsize;

static unsigned char *Convert (const void *pixels, int width, int height,
	GLint internalformat, GLenum format)
{
	int		size = width * height * 4;

	if (size > convsize)
	{
		free (convbuf);
		convbuf = malloc (size);
		if (!convbuf)
		{
			fprintf (stderr, "gl_raylib: out of memory for a %dx%d texture\n",
				width, height);
			exit (1);
		}
		convsize = size;
	}
	ToRGBA (convbuf, pixels, width * height, internalformat, format);
	return convbuf;
}

void rglTexImage2D (GLenum target, GLint level, GLint internalformat,
	GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
	const GLvoid *pixels)
{
	rtex_t			*t;
	unsigned char	*data;
	int				mips;

	(void)target;
	(void)border;
	t = BoundTexture ();
	if (!t || type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0)
		return;
	Flush ();

	// GLQuake uploads every mip level it makes. The GPU makes them from
	// level 0 instead (a box filter too), once level 1 shows up.
	if (level > 0)
	{
		if (level == 1 && t->id && !t->hasmips)
		{
			rlGenTextureMipmaps (t->id, t->width, t->height,
				RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, &mips);
			t->hasmips = 1;
			ApplyTexParams (t);
		}
		return;
	}

	data = pixels ? Convert (pixels, width, height, internalformat, format)
		: NULL;

	if (t->id && t->width == width && t->height == height)
	{
		if (data)
			rlUpdateTexture (t->id, 0, 0, width, height,
				RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, data);
	}
	else
	{
		if (t->id)
			rlUnloadTexture (t->id);
		if (!data)
		{
			data = calloc (width * height, 4);
			t->id = rlLoadTexture (data, width, height,
				RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
			free (data);
		}
		else
			t->id = rlLoadTexture (data, width, height,
				RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
		t->width = width;
		t->height = height;
		if (!t->minfilter)
		{
			// GL's defaults
			t->minfilter = GL_NEAREST_MIPMAP_LINEAR;
			t->magfilter = GL_LINEAR;
			t->wraps = t->wrapt = GL_REPEAT;
		}
		t->appliedmin = t->appliedmag = -1;
		rlTextureParameters (t->id, RL_TEXTURE_WRAP_S, t->wraps);
		rlTextureParameters (t->id, RL_TEXTURE_WRAP_T, t->wrapt);
	}
	// a new level 0 makes the old mip levels stale
	t->hasmips = 0;
	ApplyTexParams (t);
}

void rglTexSubImage2D (GLenum target, GLint level, GLint xoffset,
	GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
	const GLvoid *pixels)
{
	rtex_t			*t;
	unsigned char	*data;

	(void)target;
	t = BoundTexture ();
	if (!t || !t->id || level != 0 || type != GL_UNSIGNED_BYTE
		|| width <= 0 || height <= 0)
		return;
	// polygons drawn before this must see the old texels
	Flush ();

	// the lightmaps: one byte is luminance, as GLQuake allocated them
	data = Convert (pixels, width, height,
		format == GL_INTENSITY ? GL_INTENSITY : 4, format);
	rlUpdateTexture (t->id, xoffset, yoffset, width, height,
		RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, data);
}

/*
=============================================================================

READING PIXELS

=============================================================================
*/

void rglReadPixels (GLint x, GLint y, GLsizei width, GLsizei height,
	GLenum format, GLenum type, GLvoid *pixels)
{
	unsigned char	*screen, *src, *dst;
	int				row, col, bytes;

	Flush ();
	if (type != GL_UNSIGNED_BYTE || fbwidth <= 0 || fbheight <= 0)
		return;
	bytes = format == GL_RGBA ? 4 : 3;
	memset (pixels, 0, (size_t)width * height * bytes);

	// whole frame, top row first; the offscreen one gets the gamma curve
	// the window shows it through
	if (fbo)
		rlEnableFramebuffer (fbo);
	screen = rlReadScreenPixels (fbwidth, fbheight);
	if (fbo && !insideframe)
		rlDisableFramebuffer ();
	if (!screen)
		return;
	if (fbo && lastgamma != 1)
	{
		unsigned char	curve[256];
		int				i;

		for (i = 0; i < 256; i++)
			curve[i] = (unsigned char)(255 * pow (i / 255.0, lastgamma) + 0.5);
		for (i = 0; i < fbwidth * fbheight * 4; i++)
			screen[i] = curve[screen[i]];
	}

	// GL's rows go bottom up from y
	for (row = 0; row < height; row++)
	{
		int		sy = fbheight - 1 - (y + row);

		if (sy < 0 || sy >= fbheight)
			continue;
		dst = (unsigned char *)pixels + (size_t)row * width * bytes;
		for (col = 0; col < width; col++, dst += bytes)
		{
			int		sx = x + col;

			if (sx < 0 || sx >= fbwidth)
				continue;
			src = screen + ((size_t)sy * fbwidth + sx) * 4;
			memcpy (dst, src, bytes);
		}
	}
	free (screen);
}

/*
=============================================================================

SETUP

=============================================================================
*/

static const char vs_body[] =
	"in vec3 vertexPosition;\n"
	"in vec2 vertexTexCoord;\n"
	"in vec4 vertexColor;\n"
	"uniform mat4 mvp;\n"
	"uniform vec2 depthRange;\n"
	"out vec2 fragTexCoord;\n"
	"out vec4 fragColor;\n"
	"void main ()\n"
	"{\n"
	"    vec4 p = mvp * vec4 (vertexPosition, 1.0);\n"
	"    // glDepthRange (n, f): window z = n + (f - n) * (ndc z + 1) / 2\n"
	"    p.z = p.z * (depthRange.y - depthRange.x)\n"
	"        + p.w * (depthRange.x + depthRange.y - 1.0);\n"
	"    fragTexCoord = vertexTexCoord;\n"
	"    fragColor = vertexColor;\n"
	"    gl_Position = p;\n"
	"}\n";

static const char fs_body[] =
	"in vec2 fragTexCoord;\n"
	"in vec4 fragColor;\n"
	"uniform sampler2D texture0;\n"
	"uniform int texMode;\n"		// 0 no texture, 1 modulate, 2 replace
	"uniform float alphaRef;\n"		// GL_GREATER; -1 is off
	"uniform float rgbScale;\n"		// GL_RGB_SCALE
	"out vec4 finalColor;\n"
	"void main ()\n"
	"{\n"
	"    vec4 c = fragColor;\n"
	"    if (texMode != 0)\n"
	"    {\n"
	"        vec4 t = texture (texture0, fragTexCoord);\n"
	"        c = texMode == 1 ? t * c : t;\n"
	"    }\n"
	"    if (c.a <= alphaRef)\n"
	"        discard;\n"
	"    finalColor = vec4 (c.rgb * rgbScale, c.a);\n"
	"}\n";

// The copy to the window: one triangle that covers it, no vertex data.
static const char post_vs_body[] =
	"out vec2 fragTexCoord;\n"
	"void main ()\n"
	"{\n"
	"    vec2 p = vec2 (float ((gl_VertexID << 1) & 2), float (gl_VertexID & 2));\n"
	"    fragTexCoord = p;\n"
	"    gl_Position = vec4 (p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char post_fs_body[] =
	"in vec2 fragTexCoord;\n"
	"uniform sampler2D texture0;\n"
	"uniform float gamma;\n"
	"out vec4 finalColor;\n"
	"void main ()\n"
	"{\n"
	"    vec3 c = texture (texture0, fragTexCoord).rgb;\n"
	"    finalColor = vec4 (pow (c, vec3 (gamma)), 1.0);\n"
	"}\n";

static unsigned LoadProgram (const char *header, const char *vsbody,
	const char *fsbody)
{
	char		*vs, *fs;
	unsigned	id;

	vs = malloc (strlen (header) + strlen (vsbody) + 1);
	fs = malloc (strlen (header) + strlen (fsbody) + 1);
	if (!vs || !fs)
	{
		free (vs);
		free (fs);
		return 0;
	}
	strcpy (vs, header);
	strcat (vs, vsbody);
	strcpy (fs, header);
	strcat (fs, fsbody);
	id = rlLoadShaderProgram (vs, fs);
	free (vs);
	free (fs);
	return id;
}

static void FreeFrame (void)
{
	if (fbo)
		rlUnloadFramebuffer (fbo);	// also deletes its attachments
	fbo = fbocolor = fbodepth = 0;
	fbowidth = fboheight = 0;
}

// (Re)makes the offscreen frame at width x height; 0 if it can't.
static int MakeFrame (int width, int height)
{
	FreeFrame ();
	fbo = rlLoadFramebuffer ();
	if (!fbo)
		return 0;
	fbocolor = rlLoadTexture (NULL, width, height,
		RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
	fbodepth = rlLoadTextureDepth (width, height, true);
	if (!fbocolor || !fbodepth)
	{
		FreeFrame ();
		return 0;
	}
	rlTextureParameters (fbocolor, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_NEAREST);
	rlTextureParameters (fbocolor, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);
	rlTextureParameters (fbocolor, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
	rlTextureParameters (fbocolor, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
	rlFramebufferAttach (fbo, fbocolor, RL_ATTACHMENT_COLOR_CHANNEL0,
		RL_ATTACHMENT_TEXTURE2D, 0);
	rlFramebufferAttach (fbo, fbodepth, RL_ATTACHMENT_DEPTH,
		RL_ATTACHMENT_RENDERBUFFER, 0);
	if (!rlFramebufferComplete (fbo))
	{
		FreeFrame ();
		return 0;
	}
	fbowidth = width;
	fboheight = height;
	return 1;
}

int RGL_Init (void)
{
	const char	*header;
	int			version;

	version = rlGetVersion ();
	if (version == RL_OPENGL_ES_30)
		header = "#version 300 es\nprecision highp float;\n";
	else if (version == RL_OPENGL_33 || version == RL_OPENGL_43)
		header = "#version 330\n";
	else
	{
		fprintf (stderr, "gl_raylib: needs OpenGL 3.3 or OpenGL ES 3"
			" (rlgl version %d)\n", version);
		return 0;
	}

	qglDepthFunc = (depthfunc_t)rlGetProcAddress ("glDepthFunc");
	qglClear = (clear_t)rlGetProcAddress ("glClear");
	qglGetString = (getstring_t)rlGetProcAddress ("glGetString");
	qglBufferData = (bufferdata_t)rlGetProcAddress ("glBufferData");
	// All four are core OpenGL 3.3 / ES 3 (the browser build needs
	// -sGL_ENABLE_GET_PROC_ADDRESS for them); without one the picture
	// would be wrong, so don't start.
	if (!qglDepthFunc || !qglClear || !qglGetString || !qglBufferData)
	{
		fprintf (stderr, "gl_raylib: rlGetProcAddress has no%s%s%s%s\n",
			qglDepthFunc ? "" : " glDepthFunc", qglClear ? "" : " glClear",
			qglGetString ? "" : " glGetString",
			qglBufferData ? "" : " glBufferData");
		return 0;
	}

	program = LoadProgram (header, vs_body, fs_body);
	postprogram = LoadProgram (header, post_vs_body, post_fs_body);
	if (!program || !postprogram)
	{
		fprintf (stderr, "gl_raylib: couldn't build the shaders\n");
		return 0;
	}
	loc_postgamma = rlGetLocationUniform (postprogram, "gamma");
	loc_posttex = rlGetLocationUniform (postprogram, "texture0");
	loc_mvp = rlGetLocationUniform (program, "mvp");
	loc_depthrange = rlGetLocationUniform (program, "depthRange");
	loc_texmode = rlGetLocationUniform (program, "texMode");
	loc_alpharef = rlGetLocationUniform (program, "alphaRef");
	loc_rgbscale = rlGetLocationUniform (program, "rgbScale");
	loc_tex = rlGetLocationUniform (program, "texture0");

	vao = rlLoadVertexArray ();
	rlEnableVertexArray (vao);
	vbo = rlLoadVertexBuffer (NULL, STREAM_VERTS * sizeof(rvert_t), true);
	rlSetVertexAttribute (rlGetLocationAttrib (program, "vertexPosition"),
		3, RL_FLOAT, false, sizeof(rvert_t), 0);
	rlEnableVertexAttribute (rlGetLocationAttrib (program, "vertexPosition"));
	rlSetVertexAttribute (rlGetLocationAttrib (program, "vertexTexCoord"),
		2, RL_FLOAT, false, sizeof(rvert_t), 12);
	rlEnableVertexAttribute (rlGetLocationAttrib (program, "vertexTexCoord"));
	rlSetVertexAttribute (rlGetLocationAttrib (program, "vertexColor"),
		4, RL_UNSIGNED_BYTE, true, sizeof(rvert_t), 20);
	rlEnableVertexAttribute (rlGetLocationAttrib (program, "vertexColor"));
	rlDisableVertexArray ();
	if (!vbo)
	{
		fprintf (stderr, "gl_raylib: couldn't make the vertex buffer\n");
		return 0;
	}

	// OpenGL's initial state, not rlgl's (which culls back faces)
	memset (&state, 0, sizeof(state));
	state.texmode = GL_MODULATE;
	state.rgbscale = 1;
	state.blendsrc = GL_ONE;
	state.blenddst = GL_ZERO;
	state.depthmask = 1;
	state.depthfunc = GL_LESS;
	state.depthnear = 0;
	state.depthfar = 1;
	state.cullmode = GL_BACK;
	appliedvalid = 0;

	MatIdentity (modelview[0]);
	MatIdentity (projection[0]);
	mvdepth = projdepth = 0;

	return 1;
}

void RGL_Shutdown (void)
{
	int		i;

	for (i = 0; i < numtextures; i++)
		if (textures[i].id)
			rlUnloadTexture (textures[i].id);
	free (textures);
	textures = NULL;
	numtextures = 0;
	free (convbuf);
	convbuf = NULL;
	convsize = 0;
	FreeFrame ();
	if (vbo)
		rlUnloadVertexBuffer (vbo);
	if (vao)
		rlUnloadVertexArray (vao);
	if (program)
		rlUnloadShaderProgram (program);
	if (postprogram)
		rlUnloadShaderProgram (postprogram);
	vbo = vao = program = postprogram = 0;
}

void RGL_BeginFrame (int width, int height)
{
	static int	noframe;

	fbwidth = width;
	fbheight = height;

	if (!noframe && (width != fbowidth || height != fboheight)
		&& !MakeFrame (width, height))
	{
		fprintf (stderr, "gl_raylib: no offscreen frame, so no gamma\n");
		noframe = 1;
	}
	if (fbo)
		rlEnableFramebuffer (fbo);
	insideframe = 1;

	// raylib may have touched the state between frames
	appliedvalid = 0;

	// start the stream buffer over in fresh memory
	rlEnableVertexBuffer (vbo);
	qglBufferData (GL_ARRAY_BUFFER, STREAM_VERTS * sizeof(rvert_t), NULL,
		GL_STREAM_DRAW);
	rlDisableVertexBuffer ();
	streamused = 0;
}

void RGL_EndFrame (float gamma)
{
	int		zero = 0;

	Flush ();

	// the frame to the window, through the gamma curve
	if (fbo)
	{
		rlDisableFramebuffer ();
		rlViewport (0, 0, fbwidth, fbheight);
		rlDisableDepthTest ();
		rlDisableColorBlend ();
		rlDisableBackfaceCulling ();
		rlEnableShader (postprogram);
		rlSetUniform (loc_postgamma, &gamma, RL_SHADER_UNIFORM_FLOAT, 1);
		rlSetUniform (loc_posttex, &zero, RL_SHADER_UNIFORM_INT, 1);
		rlActiveTextureSlot (0);
		rlEnableTexture (fbocolor);
		rlEnableVertexArray (vao);
		rlDrawVertexArray (0, 3);
		rlDisableVertexArray ();
		lastgamma = gamma;
	}
	insideframe = 0;

	// leave rlgl the state it expects for its own drawing
	rlDisableShader ();
	rlDisableTexture ();
	rlSetBlendMode (RL_BLEND_ALPHA);
	rlEnableDepthMask ();
	if (qglDepthFunc)
		qglDepthFunc (GL_LEQUAL);
	appliedvalid = 0;
}

void RGL_GetStats (int *draws, int *verts)
{
	*draws = statdraws;
	*verts = statverts;
	statdraws = statverts = 0;
}
