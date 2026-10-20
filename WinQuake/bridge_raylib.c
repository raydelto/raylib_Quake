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
// bridge_raylib.c -- the only file that includes raylib.h (see bridge_raylib.h)

#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "rlgl.h"
#include "bridge_raylib.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>

// The browser build: the page (web/shell.html) sizes the canvas, owns
// fullscreen and asks for pointer lock on a click, since browsers allow
// those only from inside a user's event handler. The game says when it
// wants the pointer through Module.quakeWantsPointer.
static float	webdx, webdy;

// raylib only keeps the last movement of a poll; add them all up.
static EM_BOOL WebMouseMove (int type, const EmscriptenMouseEvent *ev, void *user)
{
	if (EM_ASM_INT ({ return document.pointerLockElement === Module.canvas; }))
	{
		webdx += ev->movementX;
		webdy += ev->movementY;
	}
	return 0;
}

static int WebPointerLocked (void)
{
	return EM_ASM_INT ({ return document.pointerLockElement === Module.canvas; });
}

// The canvas's CSS size in device pixels, packed as width << 16 | height;
// 0 if it is not laid out.
static int WebDrawableSize (void)
{
	return EM_ASM_INT ({
		var r = Module.canvas.getBoundingClientRect ();
		var d = window.devicePixelRatio || 1;
		var w = Math.round (r.width * d);
		var h = Math.round (r.height * d);
		if (w < 1 || h < 1)
			return 0;
		return Math.min (w, 32767) << 16 | Math.min (h, 32767);
	});
}
#endif

static int			windowopen;
static int			fbwidth, fbheight;
static Texture2D	screentex;
static int			texlinear;
static int			quitrequested;

/*
=============================================================================

WINDOW AND VIDEO

=============================================================================
*/

int RL_OpenWindow (const char *title, int width, int height,
	int winwidth, int winheight, int fullscreen, int vsync)
{
	Image	img;

	SetTraceLogLevel (LOG_WARNING);

	// HIGHDPI gives a Retina (macOS) or scaled (Wayland) window a framebuffer
	// of real pixels; sizes and mouse positions stay in window units.
#ifdef __EMSCRIPTEN__
	// The canvas keeps the size given here and the page scales it; frames
	// come with the display's refresh, so there is no vsync to ask for.
	SetConfigFlags (0);
#else
	SetConfigFlags (FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI
		| (vsync ? FLAG_VSYNC_HINT : 0));
#endif
	InitWindow (winwidth, winheight, title);
	if (!IsWindowReady ())
		return 0;

	// Esc is Quake's menu key, not a quit key.
	SetExitKey (KEY_NULL);
#ifdef __EMSCRIPTEN__
	emscripten_set_mousemove_callback (EMSCRIPTEN_EVENT_TARGET_DOCUMENT,
		NULL, 0, WebMouseMove);
#else
	SetWindowMinSize (320, 200);
#endif

	fbwidth = width;
	fbheight = height;

	img = GenImageColor (width, height, BLACK);
	screentex = LoadTextureFromImage (img);
	UnloadImage (img);
	SetTextureFilter (screentex, TEXTURE_FILTER_POINT);
	texlinear = 0;

	windowopen = 1;

	if (fullscreen)
		RL_SetFullscreen (1);

	return 1;
}

void RL_CloseWindow (void)
{
	if (!windowopen)
		return;
	UnloadTexture (screentex);
	CloseWindow ();
	windowopen = 0;
}

int RL_WindowOpen (void)
{
	return windowopen;
}

void RL_SetFrameSize (int width, int height)
{
	Image	img;

	if (!windowopen || (width == fbwidth && height == fbheight))
		return;
	UnloadTexture (screentex);
	fbwidth = width;
	fbheight = height;
	img = GenImageColor (width, height, BLACK);
	screentex = LoadTextureFromImage (img);
	UnloadImage (img);
	SetTextureFilter (screentex,
		texlinear ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
}

int RL_GetDrawableSize (int *width, int *height)
{
#ifdef __EMSCRIPTEN__
	int		size;

	size = WebDrawableSize ();
	if (!size)
		return 0;
	*width = size >> 16;
	*height = size & 0xffff;
	return 1;
#else
	return 0;
#endif
}

#ifdef __EMSCRIPTEN__
// The browser: the canvas's backing store is kept at the size the page
// shows it at, and raylib's own window size is left alone (GLFW would size
// the canvas in CSS pixels); the viewport and projection are set here.
static void WebPresent (int integer)
{
	int		size, cw, ch, n;
	float	scale;
	Rectangle	src, dst;

	size = WebDrawableSize ();
	emscripten_get_canvas_element_size ("#canvas", &cw, &ch);
	if (size && ((size >> 16) != cw || (size & 0xffff) != ch))
	{
		cw = size >> 16;
		ch = size & 0xffff;
		emscripten_set_canvas_element_size ("#canvas", cw, ch);
	}

	// 1:1, or whole multiples, when the frame was sized for that; else the
	// largest fit, nearest (or linear) sampled
	scale = (float)cw / fbwidth;
	if ((float)ch / fbheight < scale)
		scale = (float)ch / fbheight;
	if (integer)
	{
		n = (int)scale;
		if (n >= 1)
			scale = (float)n;
	}
	src = (Rectangle) { 0, 0, (float)fbwidth, (float)fbheight };
	dst.width = fbwidth * scale;
	dst.height = fbheight * scale;
	dst.x = (float)(int)((cw - dst.width) / 2);
	dst.y = (float)(int)((ch - dst.height) / 2);

	BeginDrawing ();
	rlDrawRenderBatchActive ();
	rlViewport (0, 0, cw, ch);
	rlMatrixMode (RL_PROJECTION);
	rlLoadIdentity ();
	rlOrtho (0, cw, ch, 0, 0.0, 1.0);
	rlMatrixMode (RL_MODELVIEW);
	rlLoadIdentity ();
	ClearBackground (BLACK);
	DrawTexturePro (screentex, src, dst, (Vector2) { 0, 0 }, 0.0f, WHITE);
	EndDrawing ();
}
#endif

void RL_Present (const unsigned *rgba, int integer, int linear)
{
	float		winw, winh;
	float		scale, dpi;
	Rectangle	src, dst;

	if (!windowopen)
		return;

	UpdateTexture (screentex, rgba);

	if (linear != texlinear)
	{
		SetTextureFilter (screentex,
			linear ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
		texlinear = linear;
	}

#ifdef __EMSCRIPTEN__
	WebPresent (integer);
	return;
#endif

	// Keep the aspect ratio, centred, black bars on the long side.
	winw = (float)GetScreenWidth ();
	winh = (float)GetScreenHeight ();
	scale = winw / fbwidth;
	if (winh / fbheight < scale)
		scale = winh / fbheight;
	if (integer)
	{
		// whole multiples of device pixels, not window units: 2x on a
		// Retina screen is 1 window unit per frame pixel
		dpi = GetWindowScaleDPI ().x;
		if (dpi < 1.0f)
			dpi = 1.0f;
		if (scale * dpi >= 1.0f)
			scale = (float)(int)(scale * dpi) / dpi;
	}

	src = (Rectangle) { 0, 0, (float)fbwidth, (float)fbheight };
	dst.width = fbwidth * scale;
	dst.height = fbheight * scale;
	dst.x = (winw - dst.width) / 2;
	dst.y = (winh - dst.height) / 2;
	if (integer)
	{
		// start on a device pixel so the texels stay square
		dpi = GetWindowScaleDPI ().x;
		if (dpi < 1.0f)
			dpi = 1.0f;
		dst.x = (float)(int)(dst.x * dpi) / dpi;
		dst.y = (float)(int)(dst.y * dpi) / dpi;
	}

	BeginDrawing ();
	ClearBackground (BLACK);
	DrawTexturePro (screentex, src, dst, (Vector2) { 0, 0 }, 0.0f, WHITE);
	EndDrawing ();		// also polls input, see RL_PumpEvents
}

void RL_BeginGLFrame (int *width, int *height)
{
#ifdef __EMSCRIPTEN__
	int		size, cw, ch;

	// GLQuake draws straight into the canvas, so its backing store is kept
	// at the size the page shows it at, in device pixels (see WebPresent).
	size = WebDrawableSize ();
	emscripten_get_canvas_element_size ("#canvas", &cw, &ch);
	if (size && ((size >> 16) != cw || (size & 0xffff) != ch))
	{
		cw = size >> 16;
		ch = size & 0xffff;
		emscripten_set_canvas_element_size ("#canvas", cw, ch);
	}
	BeginDrawing ();
	*width = cw;
	*height = ch;
#else
	BeginDrawing ();
	*width = GetRenderWidth ();
	*height = GetRenderHeight ();
#endif
}

void RL_EndGLFrame (void)
{
	if (windowopen)
		EndDrawing ();		// also polls input, see RL_PumpEvents
}

int RL_IsFullscreen (void)
{
#ifdef __EMSCRIPTEN__
	return 0;	// the page's business, see above
#else
	return windowopen && IsWindowState (FLAG_BORDERLESS_WINDOWED_MODE);
#endif
}

// Borderless fullscreen on the current monitor: no video mode change, so
// Alt+Tab and the desktop keep working.
void RL_SetFullscreen (int fullscreen)
{
#ifdef __EMSCRIPTEN__
	return;
#endif
	if (!windowopen || !fullscreen == !RL_IsFullscreen ())
		return;
	ToggleBorderlessWindowed ();
}

void RL_SetVsync (int vsync)
{
#ifdef __EMSCRIPTEN__
	return;
#endif
	if (!windowopen || !vsync == !IsWindowState (FLAG_VSYNC_HINT))
		return;
	if (vsync)
		SetWindowState (FLAG_VSYNC_HINT);
	else
		ClearWindowState (FLAG_VSYNC_HINT);
}

int RL_QuitRequested (void)
{
	return quitrequested;
}

/*
=============================================================================

INPUT

=============================================================================
*/

#define MAXEVENTS	256

static rl_keyevent_t	events[MAXEVENTS];
static int				eventhead, eventtail;

// raylib keys we track, with the code reported for each. Several raylib keys
// can give one code (left and right Shift, the keypad arrows).
typedef struct
{
	int		rlkey;
	int		code;
} keymap_t;

static const keymap_t	keymap[] =
{
	{ KEY_TAB, RLK_TAB },
	{ KEY_ENTER, RLK_ENTER },
	{ KEY_KP_ENTER, RLK_ENTER },
	{ KEY_ESCAPE, RLK_ESCAPE },
	{ KEY_SPACE, RLK_SPACE },
	{ KEY_BACKSPACE, RLK_BACKSPACE },

	{ KEY_UP, RLK_UP },
	{ KEY_DOWN, RLK_DOWN },
	{ KEY_LEFT, RLK_LEFT },
	{ KEY_RIGHT, RLK_RIGHT },
	{ KEY_LEFT_ALT, RLK_ALT },
	{ KEY_RIGHT_ALT, RLK_ALT },
	{ KEY_LEFT_CONTROL, RLK_CTRL },
	{ KEY_RIGHT_CONTROL, RLK_CTRL },
	{ KEY_LEFT_SHIFT, RLK_SHIFT },
	{ KEY_RIGHT_SHIFT, RLK_SHIFT },
	{ KEY_F1, RLK_F1 }, { KEY_F2, RLK_F2 }, { KEY_F3, RLK_F3 },
	{ KEY_F4, RLK_F4 }, { KEY_F5, RLK_F5 }, { KEY_F6, RLK_F6 },
	{ KEY_F7, RLK_F7 }, { KEY_F8, RLK_F8 }, { KEY_F9, RLK_F9 },
	{ KEY_F10, RLK_F10 }, { KEY_F11, RLK_F11 }, { KEY_F12, RLK_F12 },
	{ KEY_INSERT, RLK_INS },
	{ KEY_DELETE, RLK_DEL },
	{ KEY_PAGE_DOWN, RLK_PGDN },
	{ KEY_PAGE_UP, RLK_PGUP },
	{ KEY_HOME, RLK_HOME },
	{ KEY_END, RLK_END },
	{ KEY_PAUSE, RLK_PAUSE },

	// the keypad, as the old X11 driver mapped it
	{ KEY_KP_8, RLK_UP },
	{ KEY_KP_2, RLK_DOWN },
	{ KEY_KP_4, RLK_LEFT },
	{ KEY_KP_6, RLK_RIGHT },
	{ KEY_KP_7, RLK_HOME },
	{ KEY_KP_1, RLK_END },
	{ KEY_KP_9, RLK_PGUP },
	{ KEY_KP_3, RLK_PGDN },
	{ KEY_KP_0, RLK_INS },
	{ KEY_KP_DECIMAL, RLK_DEL },
	{ KEY_KP_5, '5' },
	{ KEY_KP_ADD, '+' },
	{ KEY_KP_SUBTRACT, '-' },
	{ KEY_KP_MULTIPLY, '*' },
	{ KEY_KP_DIVIDE, '/' },
	{ KEY_KP_EQUAL, '=' },
};

#define NUMKEYMAP	(int)(sizeof(keymap) / sizeof(keymap[0]))

// Highest raylib key code (KEY_KB_MENU is 348).
#define MAXRLKEYS	512

// Last state of each raylib key, and how many raylib keys hold each code.
static unsigned char	rlkeydown[MAXRLKEYS];
static unsigned char	codedown[RLK_NUMCODES];

// Keys whose press was kept from Quake (Enter of Alt+Enter); their release
// is kept from it too.
static unsigned char	swallowed[MAXRLKEYS];

static int		mousewanted;
static int		mousegrabbed;
#ifndef __EMSCRIPTEN__
static Vector2	lastmouse;
#endif
static float	mousedx, mousedy;
static int		mousebuttons;
static const int	rlbuttons[3] =
	{ MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE };
static float	wheel;

static void PostEvent (int code, int down)
{
	int next = (eventhead + 1) % MAXEVENTS;

	if (next == eventtail)
		return;		// full, drop it

	events[eventhead].key = code;
	events[eventhead].down = down;
	eventhead = next;
}

// The code for a raylib key, 0 for keys Quake has no use for.
static int KeyCode (int rlkey)
{
	int		i;

	// Letters, digits, space and punctuation are ASCII in raylib.
	if (rlkey >= KEY_A && rlkey <= KEY_Z)
		return rlkey - KEY_A + 'a';
	if (rlkey > KEY_SPACE && rlkey <= KEY_GRAVE)
		return rlkey;

	for (i = 0; i < NUMKEYMAP; i++)
		if (keymap[i].rlkey == rlkey)
			return keymap[i].code;

	return 0;
}

static void SetKeyState (int rlkey, int down)
{
	int		code;

	if (rlkey <= 0 || rlkey >= MAXRLKEYS || rlkeydown[rlkey] == down)
		return;
	rlkeydown[rlkey] = down;

	if (swallowed[rlkey])
	{
		if (!down)
			swallowed[rlkey] = 0;
		return;
	}

	code = KeyCode (rlkey);
	if (!code)
		return;

	// Report the code going down with the first key and up with the last.
	if (down)
	{
		if (codedown[code]++ == 0)
			PostEvent (code, 1);
	}
	else if (codedown[code] > 0)
	{
		if (--codedown[code] == 0)
			PostEvent (code, 0);
	}
}

// Reads what the last poll collected. Called before and after our own poll,
// because EndDrawing polls too, and each poll resets the pressed-key queue,
// the auto-repeat flags and the wheel.
static void ReadPoll (void)
{
	int		key;
	int		i;
	int		code;

	// A key pressed and released between two polls is in the pressed
	// queue but never in IsKeyDown: report a down and an up. The queue
	// holds presses only, in order.
	while ((key = GetKeyPressed ()) != 0)
	{
		if (key <= 0 || key >= MAXRLKEYS)
			continue;

		if ((key == KEY_ENTER || key == KEY_KP_ENTER)
			&& (IsKeyDown (KEY_LEFT_ALT) || IsKeyDown (KEY_RIGHT_ALT)
			|| IsKeyDown (KEY_LEFT_SUPER) || IsKeyDown (KEY_RIGHT_SUPER)))
		{
			// Alt+Enter (Option+Enter, Cmd+Enter on a Mac) toggles fullscreen and is not passed on.
			// (In a browser the page does it, from the key event.)
			RL_SetFullscreen (!RL_IsFullscreen ());
			if (!rlkeydown[key])
			{
				rlkeydown[key] = 1;
				swallowed[key] = 1;
			}
			continue;
		}

		// Already down: it was let go and pressed again since (a
		// fast "pp"), so give Quake that release too.
		if (rlkeydown[key])
			SetKeyState (key, 0);
		SetKeyState (key, 1);
		if (!IsKeyDown (key))
			SetKeyState (key, 0);
	}

	// Releases, and presses the queue missed (it holds 16).
	for (key = 1; key < MAXRLKEYS; key++)
		SetKeyState (key, IsKeyDown (key));

	// Auto-repeat: Quake wants another down event, as X11 sent. GLFW
	// also reports a quick release and press of the same key (under
	// 20 ms apart) as a repeat; if the key is up again by now, that
	// press gets its own down and up.
	for (key = 1; key < MAXRLKEYS; key++)
	{
		if (swallowed[key] || !IsKeyPressedRepeat (key))
			continue;
		code = KeyCode (key);
		if (!code)
			continue;
		if (rlkeydown[key])
			PostEvent (code, 1);
		else if (!codedown[code])
		{
			PostEvent (code, 1);
			PostEvent (code, 0);
		}
	}

	if (mousegrabbed)
	{
		Vector2		pos;

#ifdef __EMSCRIPTEN__
		(void)pos;
		mousedx += webdx;
		mousedy += webdy;
		webdx = webdy = 0;
#else
		// GetMouseDelta only covers the last poll; with the cursor
		// disabled the position is unbounded, so track it ourselves.
		pos = GetMousePosition ();
		mousedx += pos.x - lastmouse.x;
		mousedy += pos.y - lastmouse.y;
		lastmouse = pos;
#endif

		wheel += GetMouseWheelMove ();

		for (i = 0; i < 3; i++)
		{
			int		bit = 1 << i;
			int		down = IsMouseButtonDown (rlbuttons[i]);

			if (down != !!(mousebuttons & bit))
			{
				mousebuttons ^= bit;
				PostEvent (RLK_MOUSE1 + i, down);
			}
		}
	}

	// One wheel notch is a down and an up, as the X11 driver did it.
	while (wheel >= 1.0f)
	{
		PostEvent (RLK_MWHEELUP, 1);
		PostEvent (RLK_MWHEELUP, 0);
		wheel -= 1.0f;
	}
	while (wheel <= -1.0f)
	{
		PostEvent (RLK_MWHEELDOWN, 1);
		PostEvent (RLK_MWHEELDOWN, 0);
		wheel += 1.0f;
	}

	// raylib reports a close request until the next poll only. A page has
	// no close button (and raylib's web WindowShouldClose sleeps, which
	// only works with ASYNCIFY).
#ifndef __EMSCRIPTEN__
	if (WindowShouldClose ())
		quitrequested = 1;
#endif
}

static void UpdateGrab (void)
{
	int		grab;
	int		i;

#ifdef __EMSCRIPTEN__
	// The page captures the pointer on a click while the game wants it;
	// Esc (the browser's own) lets it go, and a click takes it again.
	EM_ASM ({
		Module.quakeWantsPointer = $0;
		if (!$0 && document.pointerLockElement === Module.canvas)
			document.exitPointerLock ();
	}, mousewanted);
	grab = mousewanted && WebPointerLocked ();
#else
	grab = mousewanted && IsWindowFocused ();
#endif
	if (grab == mousegrabbed)
		return;

	mousegrabbed = grab;
#ifdef IN_RAYLIB_KEYLOG
	printf ("keylog: pointer %s\n", grab ? "captured" : "released");
#endif
#ifdef __EMSCRIPTEN__
	webdx = webdy = 0;
#else
	if (grab)
		DisableCursor ();	// relative, raw (unaccelerated) motion
	else
		EnableCursor ();
#endif

	// Moving the cursor around is not player movement, and buttons
	// held when the grab ends are released.
#ifndef __EMSCRIPTEN__
	lastmouse = GetMousePosition ();
#endif
	mousedx = mousedy = 0;
	wheel = 0;
	for (i = 0; i < 3; i++)
		if (mousebuttons & (1 << i))
			PostEvent (RLK_MOUSE1 + i, 0);
	mousebuttons = 0;
}

void RL_PumpEvents (void)
{
	// WindowShouldClose is true when there is no window, and the
	// dedicated server never opens one.
	if (!windowopen)
		return;

	ReadPoll ();
	PollInputEvents ();
	ReadPoll ();

	// GLFW releases every key when the window loses focus (Alt+Tab),
	// so nothing stays stuck down; the pointer is let go and captured
	// again when focus comes back.
	UpdateGrab ();
}

int RL_GetKeyEvent (rl_keyevent_t *ev)
{
	if (eventtail == eventhead)
		return 0;

	*ev = events[eventtail];
	eventtail = (eventtail + 1) % MAXEVENTS;
	return 1;
}

void RL_SetMouseGrab (int grab)
{
	mousewanted = grab;
	if (windowopen)
		UpdateGrab ();
}

void RL_GetMouseMotion (float *dx, float *dy)
{
	*dx = mousedx;
	*dy = mousedy;
	mousedx = mousedy = 0;
}

/*
=============================================================================

BROWSER TOUCH CONTROLS

=============================================================================
*/

#ifdef __EMSCRIPTEN__
// The page's touch buttons, one RL_WebTouch bit each, in bit order
// (web/shell.html has the same list).
static const int	touchcodes[] =
{
	RLK_TOUCH_FIRE,			// 0x001
	RLK_TOUCH_JUMP,			// 0x002
	RLK_TOUCH_NEXTWEAPON,	// 0x004
	RLK_ESCAPE,				// 0x008
	RLK_ENTER,				// 0x010
	'y',					// 0x020
	'n',					// 0x040
	RLK_UP,					// 0x080
	RLK_DOWN,				// 0x100
	RLK_LEFT,				// 0x200
	RLK_RIGHT,				// 0x400
	'`',					// 0x800, the console
};

#define NUMTOUCHCODES	(int)(sizeof(touchcodes) / sizeof(touchcodes[0]))

static int		touchused;
static int		touchbits;
static float	touchside, touchforward;
static float	touchdx, touchdy;
static int		playingsent = -1;

// Called by the page on every touch change: the buttons held, the stick
// (-1000 to 1000 each way, screen directions: y is down) and the look drag
// since the last call, in mouse counts. A button going down or up is a key
// event, queued with the keyboard's.
EMSCRIPTEN_KEEPALIVE void RL_WebTouch (int bits, int stickx, int sticky,
	int dx, int dy)
{
	int		i;
	int		bit;

	touchused = 1;
	for (i = 0; i < NUMTOUCHCODES; i++)
	{
		bit = 1 << i;
		if ((bits ^ touchbits) & bit)
			PostEvent (touchcodes[i], (bits & bit) != 0);
	}
	touchbits = bits;
	touchside = stickx / 1000.0f;
	touchforward = -sticky / 1000.0f;
	touchdx += dx;
	touchdy += dy;
}

// A key typed on the phone's keyboard (console, player name): a press and a
// release, queued behind the other key events, so a whole word typed
// between two frames arrives in order. ASCII; 8 is Backspace.
EMSCRIPTEN_KEEPALIVE void RL_WebTouchKey (int key)
{
	if (key == 8)
		key = RLK_BACKSPACE;
	if (key != RLK_ENTER && key != RLK_ESCAPE && key != RLK_BACKSPACE
		&& (key < 32 || key > 126))
		return;
	touchused = 1;
	PostEvent (key, 1);
	PostEvent (key, 0);
}
#endif

void RL_SetPlaying (int playing)
{
#ifdef __EMSCRIPTEN__
	if (playing == playingsent)
		return;
	playingsent = playing;
	EM_ASM ({
		Module.quakePlaying = $0;
		if (Module.quakeOnPlaying)
			Module.quakeOnPlaying ($0);
	}, playing);
#else
	(void)playing;
#endif
}

int RL_GetTouchMove (float *side, float *forward, float *dx, float *dy)
{
#ifdef __EMSCRIPTEN__
	*side = touchside;
	*forward = touchforward;
	*dx = touchdx;
	*dy = touchdy;
	touchdx = touchdy = 0;
	return touchused;
#else
	*side = *forward = *dx = *dy = 0;
	return 0;
#endif
}
