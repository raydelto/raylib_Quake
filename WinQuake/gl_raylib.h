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
// gl_raylib.h -- the OpenGL 1.1 subset GLQuake uses, emulated on raylib's rlgl

// glquake.h includes this instead of <GL/gl.h> when GLQUAKE_RAYLIB is
// defined. GLQuake draws in immediate mode (glBegin/glVertex/glEnd) with
// fixed-function state (alpha test, texture env, depth range, a matrix
// stack). gl_raylib.c collects the vertices into a stream buffer and draws
// them with its own shader through rlgl, which runs on OpenGL 3.3 and on
// OpenGL ES 3 / WebGL 2, neither of which has the fixed-function pipeline.
//
// Plain C types only: gl_raylib.c includes rlgl.h but no Quake header, and
// the Quake side never sees rlgl.h. The functions are renamed to rgl* so
// they never resolve to a system libGL's own entry points.

#ifndef GL_RAYLIB_H
#define GL_RAYLIB_H

typedef unsigned int	GLenum;
typedef unsigned char	GLboolean;
typedef unsigned int	GLbitfield;
typedef signed char		GLbyte;
typedef short			GLshort;
typedef int				GLint;
typedef int				GLsizei;
typedef unsigned char	GLubyte;
typedef unsigned short	GLushort;
typedef unsigned int	GLuint;
typedef float			GLfloat;
typedef float			GLclampf;
typedef double			GLdouble;
typedef double			GLclampd;
typedef void			GLvoid;

#define GL_FALSE						0
#define GL_TRUE							1

// primitives
#define GL_POINTS						0x0000
#define GL_LINES						0x0001
#define GL_LINE_LOOP					0x0002
#define GL_LINE_STRIP					0x0003
#define GL_TRIANGLES					0x0004
#define GL_TRIANGLE_STRIP				0x0005
#define GL_TRIANGLE_FAN					0x0006
#define GL_QUADS						0x0007
#define GL_QUAD_STRIP					0x0008
#define GL_POLYGON						0x0009

// comparison functions
#define GL_NEVER						0x0200
#define GL_LESS							0x0201
#define GL_EQUAL						0x0202
#define GL_LEQUAL						0x0203
#define GL_GREATER						0x0204
#define GL_NOTEQUAL						0x0205
#define GL_GEQUAL						0x0206
#define GL_ALWAYS						0x0207

// blending
#define GL_ZERO							0
#define GL_ONE							1
#define GL_SRC_COLOR					0x0300
#define GL_ONE_MINUS_SRC_COLOR			0x0301
#define GL_SRC_ALPHA					0x0302
#define GL_ONE_MINUS_SRC_ALPHA			0x0303
#define GL_DST_ALPHA					0x0304
#define GL_ONE_MINUS_DST_ALPHA			0x0305
#define GL_DST_COLOR					0x0306
#define GL_ONE_MINUS_DST_COLOR			0x0307

// buffers, faces, polygon mode
#define GL_FRONT						0x0404
#define GL_BACK							0x0405
#define GL_FRONT_AND_BACK				0x0408
#define GL_POINT						0x1B00
#define GL_LINE							0x1B01
#define GL_FILL							0x1B02
#define GL_DEPTH_BUFFER_BIT				0x00000100
#define GL_COLOR_BUFFER_BIT				0x00004000

// capabilities
#define GL_CULL_FACE					0x0B44
#define GL_FOG							0x0B60
#define GL_DEPTH_TEST					0x0B71
#define GL_ALPHA_TEST					0x0BC0
#define GL_BLEND						0x0BE2
#define GL_SCISSOR_TEST					0x0C11
#define GL_TEXTURE_2D					0x0DE1
#define GL_POLYGON_OFFSET_FILL			0x8037
#define GL_SHARED_TEXTURE_PALETTE_EXT	0x81FB

// hints and shading
#define GL_PERSPECTIVE_CORRECTION_HINT	0x0C50
#define GL_DONT_CARE					0x1100
#define GL_FASTEST						0x1101
#define GL_NICEST						0x1102
#define GL_FLAT							0x1D00
#define GL_SMOOTH						0x1D01

// fog
#define GL_FOG_DENSITY					0x0B62
#define GL_FOG_START					0x0B63
#define GL_FOG_END						0x0B64
#define GL_FOG_MODE						0x0B65
#define GL_FOG_COLOR					0x0B66
#define GL_EXP							0x0800
#define GL_LINEAR						0x2601

// matrices
#define GL_MODELVIEW					0x1700
#define GL_PROJECTION					0x1701
#define GL_TEXTURE						0x1702
#define GL_MODELVIEW_MATRIX				0x0BA6
#define GL_PROJECTION_MATRIX			0x0BA7

// data types
#define GL_BYTE							0x1400
#define GL_UNSIGNED_BYTE				0x1401
#define GL_SHORT						0x1402
#define GL_UNSIGNED_SHORT				0x1403
#define GL_INT							0x1404
#define GL_UNSIGNED_INT					0x1405
#define GL_FLOAT						0x1406

// pixel formats
#define GL_COLOR_INDEX					0x1900
#define GL_ALPHA						0x1906
#define GL_RGB							0x1907
#define GL_RGBA							0x1908
#define GL_LUMINANCE					0x1909
#define GL_LUMINANCE_ALPHA				0x190A
#define GL_RGBA4						0x8056
#define GL_INTENSITY					0x8049
#define GL_COLOR_INDEX8_EXT				0x80E5

// textures
#define GL_NEAREST						0x2600
#define GL_NEAREST_MIPMAP_NEAREST		0x2700
#define GL_LINEAR_MIPMAP_NEAREST		0x2701
#define GL_NEAREST_MIPMAP_LINEAR		0x2702
#define GL_LINEAR_MIPMAP_LINEAR			0x2703
#define GL_TEXTURE_MAG_FILTER			0x2800
#define GL_TEXTURE_MIN_FILTER			0x2801
#define GL_TEXTURE_WRAP_S				0x2802
#define GL_TEXTURE_WRAP_T				0x2803
#define GL_CLAMP						0x2900
#define GL_REPEAT						0x2901
#define GL_TEXTURE_ENV					0x2300
#define GL_RGB_SCALE					0x8573	// GL 1.3; here with any env mode
#define GL_TEXTURE_ENV_MODE				0x2200
#define GL_MODULATE						0x2100
#define GL_DECAL						0x2101
#define GL_REPLACE						0x1E01

// strings
#define GL_VENDOR						0x1F00
#define GL_RENDERER						0x1F01
#define GL_VERSION						0x1F02
#define GL_EXTENSIONS					0x1F03

#define glAlphaFunc			rglAlphaFunc
#define glBegin				rglBegin
#define glBindTexture		rglBindTexture
#define glBlendFunc			rglBlendFunc
#define glClear				rglClear
#define glClearColor		rglClearColor
#define glColor3f			rglColor3f
#define glColor3ubv			rglColor3ubv
#define glColor4f			rglColor4f
#define glColor4fv			rglColor4fv
#define glColor4ubv			rglColor4ubv
#define glCullFace			rglCullFace
#define glDepthFunc			rglDepthFunc
#define glDepthMask			rglDepthMask
#define glDepthRange		rglDepthRange
#define glDisable			rglDisable
#define glDrawBuffer		rglDrawBuffer
#define glEnable			rglEnable
#define glEnd				rglEnd
#define glFinish			rglFinish
#define glFlush				rglFlush
#define glFogf				rglFogf
#define glFogfv				rglFogfv
#define glFogi				rglFogi
#define glFrustum			rglFrustum
#define glGetFloatv			rglGetFloatv
#define glGetString			rglGetString
#define glHint				rglHint
#define glLoadIdentity		rglLoadIdentity
#define glLoadMatrixf		rglLoadMatrixf
#define glMatrixMode		rglMatrixMode
#define glOrtho				rglOrtho
#define glPolygonMode		rglPolygonMode
#define glPopMatrix			rglPopMatrix
#define glPushMatrix		rglPushMatrix
#define glReadBuffer		rglReadBuffer
#define glReadPixels		rglReadPixels
#define glRotatef			rglRotatef
#define glScalef			rglScalef
#define glShadeModel		rglShadeModel
#define glTexCoord2f		rglTexCoord2f
#define glTexCoord2fv		rglTexCoord2fv
#define glTexEnvf			rglTexEnvf
#define glTexImage2D		rglTexImage2D
#define glTexParameterf		rglTexParameterf
#define glTexSubImage2D		rglTexSubImage2D
#define glTranslatef		rglTranslatef
#define glVertex2f			rglVertex2f
#define glVertex3f			rglVertex3f
#define glVertex3fv			rglVertex3fv
#define glViewport			rglViewport

void rglAlphaFunc (GLenum func, GLclampf ref);
void rglBegin (GLenum mode);
void rglBindTexture (GLenum target, GLuint texture);
void rglBlendFunc (GLenum sfactor, GLenum dfactor);
void rglClear (GLbitfield mask);
void rglClearColor (GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void rglColor3f (GLfloat r, GLfloat g, GLfloat b);
void rglColor3ubv (const GLubyte *v);
void rglColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void rglColor4fv (const GLfloat *v);
void rglColor4ubv (const GLubyte *v);
void rglCullFace (GLenum mode);
void rglDepthFunc (GLenum func);
void rglDepthMask (GLboolean flag);
void rglDepthRange (GLclampd znear, GLclampd zfar);
void rglDisable (GLenum cap);
void rglDrawBuffer (GLenum mode);
void rglEnable (GLenum cap);
void rglEnd (void);
void rglFinish (void);
void rglFlush (void);
void rglFogf (GLenum pname, GLfloat param);
void rglFogfv (GLenum pname, const GLfloat *params);
void rglFogi (GLenum pname, GLint param);
void rglFrustum (GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
	GLdouble znear, GLdouble zfar);
void rglGetFloatv (GLenum pname, GLfloat *params);
const GLubyte *rglGetString (GLenum name);
void rglHint (GLenum target, GLenum mode);
void rglLoadIdentity (void);
void rglLoadMatrixf (const GLfloat *m);
void rglMatrixMode (GLenum mode);
void rglOrtho (GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
	GLdouble znear, GLdouble zfar);
void rglPolygonMode (GLenum face, GLenum mode);
void rglPopMatrix (void);
void rglPushMatrix (void);
void rglReadBuffer (GLenum mode);
void rglReadPixels (GLint x, GLint y, GLsizei width, GLsizei height,
	GLenum format, GLenum type, GLvoid *pixels);
void rglRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void rglScalef (GLfloat x, GLfloat y, GLfloat z);
void rglShadeModel (GLenum mode);
void rglTexCoord2f (GLfloat s, GLfloat t);
void rglTexCoord2fv (const GLfloat *v);
void rglTexEnvf (GLenum target, GLenum pname, GLfloat param);
void rglTexImage2D (GLenum target, GLint level, GLint internalformat,
	GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
	const GLvoid *pixels);
void rglTexParameterf (GLenum target, GLenum pname, GLfloat param);
void rglTexSubImage2D (GLenum target, GLint level, GLint xoffset,
	GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
	const GLvoid *pixels);
void rglTranslatef (GLfloat x, GLfloat y, GLfloat z);
void rglVertex2f (GLfloat x, GLfloat y);
void rglVertex3f (GLfloat x, GLfloat y, GLfloat z);
void rglVertex3fv (const GLfloat *v);
void rglViewport (GLint x, GLint y, GLsizei width, GLsizei height);

//
// beyond OpenGL 1.1, for vid_glraylib.c
//

// Sets up the stream buffer, the shader and the default state. Call once
// the window (and so the context) exists. Returns 0 on failure.
int RGL_Init (void);
void RGL_Shutdown (void);

// The frame's size in pixels, given by the driver before drawing a frame;
// glReadPixels flips rows with it. Also restarts the stream buffer, and
// points drawing at the offscreen frame (see RGL_EndFrame).
void RGL_BeginFrame (int width, int height);

// Draws whatever is still batched and copies the frame to the window
// through out = in ^ gamma; call before the buffers are swapped.
void RGL_EndFrame (float gamma);

// Draw calls and vertices since the last call (for r_speeds style stats).
void RGL_GetStats (int *draws, int *verts);

#endif
