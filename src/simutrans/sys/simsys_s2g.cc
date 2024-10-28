/*
 * This file is part of the Simutrans project under the Artistic License.
 * (see LICENSE.txt)
 */

#if !defined __APPLE__ && !defined __ANDROID__
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#else
#include <SDL.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <stdio.h>
#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glx.h>

#ifdef __CYGWIN__
extern int __argc;
extern char **__argv;
#endif

#include "simsys.h"

#include "../macros.h"
#include "../simversion.h"
#include "../simevent.h"
#include "../display/simgraph.h"
#include "../simdebug.h"
#include "../dataobj/environment.h"
#include "../gui/simwin.h"
#include "../gui/components/gui_component.h"
#include "../gui/components/gui_textinput.h"
#include "../simintr.h"
#include "../world/simworld.h"
#include "../music/music.h"
#include "../utils/unicode.h"


// Maybe Linux is not fine too, had critical bugs...
#if !defined(__linux__)
#define USE_SDL_TEXTEDITING
#else
#endif

// threshold for zooming in/out with multitouch
#define DELTA_PINCH (0.033)

static Uint8 hourglass_cursor[] = {
	0x3F, 0xFE, //   *************
	0x30, 0x06, //   **         **
	0x3F, 0xFE, //   *************
	0x10, 0x04, //    *         *
	0x10, 0x04, //    *         *
	0x12, 0xA4, //    *  * * *  *
	0x11, 0x44, //    *  * * *  *
	0x18, 0x8C, //    **   *   **
	0x0C, 0x18, //     **     **
	0x06, 0xB0, //      ** * **
	0x03, 0x60, //       ** **
	0x03, 0x60, //       **H**
	0x06, 0x30, //      ** * **
	0x0C, 0x98, //     **     **
	0x18, 0x0C, //    **   *   **
	0x10, 0x84, //    *    *    *
	0x11, 0x44, //    *   * *   *
	0x12, 0xA4, //    *  * * *  *
	0x15, 0x54, //    * * * * * *
	0x3F, 0xFE, //   *************
	0x30, 0x06, //   **         **
	0x3F, 0xFE  //   *************
};

static Uint8 hourglass_cursor_mask[] = {
	0x3F, 0xFE, //   *************
	0x3F, 0xFE, //   *************
	0x3F, 0xFE, //   *************
	0x1F, 0xFC, //    ***********
	0x1F, 0xFC, //    ***********
	0x1F, 0xFC, //    ***********
	0x1F, 0xFC, //    ***********
	0x1F, 0xFC, //    ***********
	0x0F, 0xF8, //     *********
	0x07, 0xF0, //      *******
	0x03, 0xE0, //       *****
	0x03, 0xE0, //       **H**
	0x07, 0xF0, //      *******
	0x0F, 0xF8, //     *********
	0x1F, 0xFC, //    ***********
	0x1F, 0xFC, //    ***********
	0x1F, 0xFC, //    ***********
	0x1F, 0xFC, //    ***********
	0x1F, 0xFC, //    ***********
	0x3F, 0xFE, //   *************
	0x3F, 0xFE, //   *************
	0x3F, 0xFE  //   *************
};

static Uint8 blank_cursor[] = {
	0x0,
	0x0,
};

static SDL_Window *window;
static SDL_GLContext glcontext;

static int width = 16;
static int height = 16;

static int sync_blit = 0;
static sint16 fullscreen = WINDOWED;

static SDL_Cursor *arrow;
static SDL_Cursor *hourglass;
static SDL_Cursor *blank;

// Number of fractional bits for screen scaling
#define SCALE_SHIFT_X 5
#define SCALE_SHIFT_Y 5

#define SCALE_NEUTRAL_X (1 << SCALE_SHIFT_X)
#define SCALE_NEUTRAL_Y (1 << SCALE_SHIFT_Y)

// Multiplier when converting from texture to screen coords, fixed point format
// Example: If x_scale==2*SCALE_NEUTRAL_X && y_scale==2*SCALE_NEUTRAL_Y,
// then things on screen are 2*2 = 4 times as big by area
static sint32 x_scale = SCALE_NEUTRAL_X;
static sint32 y_scale = SCALE_NEUTRAL_Y;

// When using -autodpi, attempt to scale things on screen to this DPI value
#ifdef __ANDROID__
#define TARGET_DPI (192)
#else
#define TARGET_DPI (96)
#endif

// make sure we have at least so much pixel in y-direction
#define MIN_SCALE_HEIGHT (640)

// Most Android devices are underpowered to handle larger screens
#define MAX_AUTOSCALE_WIDTH (1280)

// screen -> texture coords
#define SCREEN_TO_TEX_X(x) (((x) * SCALE_NEUTRAL_X) / x_scale)
#define SCREEN_TO_TEX_Y(y) (((y) * SCALE_NEUTRAL_Y) / y_scale)

// texture -> screen coords
#define TEX_TO_SCREEN_X(x) (((x) * x_scale) / SCALE_NEUTRAL_X)
#define TEX_TO_SCREEN_Y(y) (((y) * y_scale) / SCALE_NEUTRAL_Y)

static int tex_max_size;

/**
 * Checks for the extensions this backend can use
 */
static void check_for_extensions()
{

	// Initialize GLEW
	GLenum err = glewInit();
	if(  GLEW_OK != err  ) {
		dbg->fatal( "check_for_extensions()", "glew failed to initialize" );
		return;
	}

	//see simsys_opengl.cc for how to check things
}

/**
 * Detects the biggest texture the system supports, and sets tex_max_size
 */
static void check_max_texture_size()
{

	GLint width,curr_width;

	curr_width = 32;

	do {
		curr_width=curr_width<<1;

		glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGB, curr_width, curr_width, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
		glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	}
	while (width!=0 && width==curr_width);

	curr_width=curr_width>>1;
	tex_max_size=curr_width;

	fprintf(stderr, "Renderer supports textures up to %dx%d.\n",curr_width,curr_width);
	DBG_MESSAGE("check_max_texture_size(OpenGL)", "Renderer supports textures up to %dx%d",curr_width,curr_width);
}

bool has_soft_keyboard = false;


bool dr_set_screen_scale(sint16 scale_percent)
{
	const sint32 old_x_scale = x_scale;
	const sint32 old_y_scale = y_scale;

	if (scale_percent == -1) {
		float hdpi, vdpi;
		SDL_DisplayMode mode;
		SDL_GetCurrentDisplayMode(0, &mode);
		DBG_MESSAGE("dr_auto_scale", "screen resolution width=%d, height=%d", mode.w, mode.h);

#if SDL_VERSION_ATLEAST(2,0,4)
		// auto scale only for high enough screens
		if (mode.h > 1.5 * MIN_SCALE_HEIGHT && SDL_GetDisplayDPI(0, NULL, &hdpi, &vdpi) == 0) {

			x_scale = ((sint64)hdpi * SCALE_NEUTRAL_X + 1) / TARGET_DPI;
			y_scale = ((sint64)vdpi * SCALE_NEUTRAL_Y + 1) / TARGET_DPI;
			DBG_MESSAGE("auto_dpi_scaling", "x=%i, y=%i", x_scale, y_scale);
		}

#ifdef __ANDROID__
		// most Android are underpowered to run more than 1280 pixel horizontal
		sint32 current_x = SCREEN_TO_TEX_X(mode.w);
		if (current_x > MAX_AUTOSCALE_WIDTH) {
			sint32 new_x_scale = ((sint64)mode.w * SCALE_NEUTRAL_X + 1) / MAX_AUTOSCALE_WIDTH;
			y_scale = (y_scale * new_x_scale) / x_scale;
			x_scale = new_x_scale;
			DBG_MESSAGE("new scaling (max 1280)", "x=%i, y=%i", x_scale, y_scale);
	}
#endif

		// ensure minimum height
		sint32 current_y = SCREEN_TO_TEX_Y(mode.h);
		if (current_y < MIN_SCALE_HEIGHT) {
			DBG_MESSAGE("dr_auto_scale", "virtual height=%d < %d", current_y, MIN_SCALE_HEIGHT);
			x_scale = (x_scale * current_y) / MIN_SCALE_HEIGHT;
			y_scale = (y_scale * current_y) / MIN_SCALE_HEIGHT;
			DBG_MESSAGE("new scaling (min 640)", "x=%i, y=%i", x_scale, y_scale);
		}
#else
#pragma message "SDL version must be at least 2.0.4 to support autoscaling."
		// 1.5 scale up by default
		x_scale = (150*SCALE_NEUTRAL_X)/100;
		y_scale = (150*SCALE_NEUTRAL_Y)/100;
#endif
	}
	else if (scale_percent == 0) {
		x_scale = SCALE_NEUTRAL_X;
		y_scale = SCALE_NEUTRAL_Y;
	}
	else {
		x_scale = (scale_percent*SCALE_NEUTRAL_X)/100;
		y_scale = (scale_percent*SCALE_NEUTRAL_Y)/100;
	}

	if (window  &&  (x_scale != old_x_scale || y_scale != old_y_scale)  ) {
		// force window resize
		int w, h;
		SDL_GetWindowSize(window, &w, &h);

		SDL_Event ev;
		ev.type = SDL_WINDOWEVENT;
		ev.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
		ev.window.data1 = w;
		ev.window.data2 = h;

		if (SDL_PushEvent(&ev) != 1) {
			return false;
		}
	}

	return true;
}


sint16 dr_get_screen_scale()
{
	return (x_scale*100)/SCALE_NEUTRAL_X;
}


static int SDLCALL my_event_filter(void* /*userdata*/, SDL_Event* event)
{
	DBG_DEBUG4("my_event_filter", "%i", event->type);
	switch (event->type)
	{
	case SDL_APP_DIDENTERBACKGROUND:
		intr_disable();
		// save settings
		{
			dr_chdir(env_t::user_dir);
			loadsave_t settings_file;
			if (settings_file.wr_open("settings.xml", loadsave_t::xml, 0, "settings only/", SAVEGAME_VER_NR) == loadsave_t::FILE_STATUS_OK) {
				env_t::rdwr(&settings_file);
				env_t::default_settings.rdwr(&settings_file);
				settings_file.close();
			}
		}
		dr_stop_midi();
		return 0;

	case SDL_APP_TERMINATING:
		// quitting immediate, save settings and game without visual feedback
		intr_disable();
		DBG_DEBUG("SDL_APP_TERMINATING", "env_t::reload_and_save_on_quit=%d", env_t::reload_and_save_on_quit);
		world()->stop(true);
		// save settings
		{
			dr_chdir(env_t::user_dir);
			loadsave_t settings_file;
			if (settings_file.wr_open("settings.xml", loadsave_t::xml, 0, "settings only/", SAVEGAME_VER_NR) == loadsave_t::FILE_STATUS_OK) {
				env_t::rdwr(&settings_file);
				env_t::default_settings.rdwr(&settings_file);
				settings_file.close();
			}
		}
		// at this point there is no UI active anymore, and we have no time to die, so just exit and leeve the cleanup to the OS
		dr_stop_midi();
		SDL_Quit();
		dr_os_close();
		exit(0);
		// we never reach here tough ...
		return 0;

	}
	return 1;  // let all events be added to the queue since we always return 1.
}


/**
 * Detects if we have hardware acceleration available or not
 */
static bool check_hardware_accelerated()
{
	int result;
	const GLubyte* vendor = glGetString(GL_VENDOR);
	const GLubyte* renderer = glGetString(GL_RENDERER);
	const char * glxvendor = "Unknown";
	{
		SDL_SysWMinfo wminfo;
		SDL_VERSION(&wminfo.version);
		if(  SDL_GetWindowWMInfo(window, &wminfo)  ) {
			glxvendor = glXGetClientString(wminfo.info.x11.display,
						       GLX_VENDOR);
		}
	}

	SDL_GL_GetAttribute(SDL_GL_ACCELERATED_VISUAL,&result);

	if(  result==1  ) {
		fprintf(stderr, "Hardware acceleration available, vendor: %s, renderer: %s, glx vendor: %s.\n",vendor,renderer,glxvendor);
		DBG_MESSAGE("check_hardware_accelerated(OpenGL)", "Hardware acceleration available, vendor: %s",vendor);
	}
	else {
		fprintf(stderr, "Hardware acceleration NOT available, vendor: %s, renderer: %s, glx vendor: %s.\n",vendor,renderer,glxvendor);
		DBG_MESSAGE("check_hardware_accelerated(OpenGL)", "Hardware acceleration NOT available, vendor: %s",vendor);
	}
	return result==1;
}

/*
 * Hier sind die Basisfunktionen zur Initialisierung der
 * Schnittstelle untergebracht
 * -> init,open,close
 */
bool dr_os_init(const int* parameter)
{
	if(  SDL_Init( SDL_INIT_VIDEO ) != 0  ) {
		dbg->error("dr_os_init(SDL2)", "Could not initialize SDL: %s", SDL_GetError() );
		return false;
	}

	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");

	dbg->message("dr_os_init(SDL2)", "SDL Driver: %s", SDL_GetCurrentVideoDriver() );

	// disable event types not interested in
#ifndef USE_SDL_TEXTEDITING
	SDL_EventState( SDL_TEXTEDITING, SDL_DISABLE );
#endif
	SDL_EventState( SDL_FINGERDOWN, SDL_ENABLE );
	SDL_EventState( SDL_FINGERUP, SDL_ENABLE );
	SDL_EventState( SDL_FINGERMOTION, SDL_ENABLE );
	SDL_EventState( SDL_DOLLARGESTURE, SDL_DISABLE );
	SDL_EventState( SDL_DOLLARRECORD, SDL_DISABLE );
	SDL_EventState( SDL_MULTIGESTURE, SDL_ENABLE );
	SDL_EventState( SDL_CLIPBOARDUPDATE, SDL_DISABLE );
	SDL_EventState( SDL_DROPFILE, SDL_DISABLE );

	// termination event: save current map and settings
	SDL_SetEventFilter(my_event_filter, 0);

	has_soft_keyboard = SDL_HasScreenKeyboardSupport();
	if (has_soft_keyboard  &&  !env_t::hide_keyboard) {
		env_t::hide_keyboard = true;
	}
	if (!env_t::hide_keyboard) {
		SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
	}

	sync_blit = parameter[0];  // hijack SDL1 -async flag for SDL2 vsync

	// prepare for next event
	sys_event.type = SIM_NOEVENT;
	sys_event.code = 0;

	if (!env_t::hide_keyboard) {
		SDL_StartTextInput();
		DBG_MESSAGE("SDL_StartTextInput", "");
	}

	atexit( SDL_Quit ); // clean up on exit
	return true;
}


resolution dr_query_screen_resolution()
{
	resolution res;
	SDL_DisplayMode mode;
	SDL_GetCurrentDisplayMode( 0, &mode );
	DBG_MESSAGE("dr_query_screen_resolution(SDL2)", "screen resolution width=%d, height=%d", mode.w, mode.h );
	res.w = SCREEN_TO_TEX_X(mode.w);
	res.h = SCREEN_TO_TEX_Y(mode.h);
	return res;
}

// open the window
int dr_os_open(const scr_size window_size, sint16 fs)
{
	// scale up
	resolution res = dr_query_screen_resolution();
	const int tex_w = clamp( res.w, 1, SCREEN_TO_TEX_X(window_size.w) );
	const int tex_h = clamp( res.h, 1, SCREEN_TO_TEX_Y(window_size.h) );

	DBG_MESSAGE("dr_os_open()", "Screen requested %i,%i, available max %i,%i", tex_w, tex_h, res.w, res.h);

	fullscreen = fs ? BORDERLESS : WINDOWED;	// SDL2 has no real fullscreen mode

	// some cards need those alignments
	// especially 64bit want a border of 8bytes
	const int tex_pitch = (tex_w + 15) & 0x7FF0;
	width = tex_pitch;
	height = tex_h;

	// SDL2 only works with borderless fullscreen (SDL_WINDOW_FULLSCREEN_DESKTOP)
	Uint32 flags = fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE;
	flags |= SDL_WINDOW_ALLOW_HIGHDPI; // apparently needed for Apple retina displays
#ifdef __ANDROID__
	// needed for landscape apparently
	flags |= SDL_WINDOW_RESIZABLE;
#endif
	flags |= SDL_WINDOW_OPENGL;

	SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 16 );
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 1 );
	SDL_GL_SetAttribute( SDL_GL_ACCELERATED_VISUAL, 1 );

	window = SDL_CreateWindow( SIM_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_size.w, window_size.h, flags );
	if(  window == NULL  ) {
		dbg->error("dr_os_open(SDL2)", "Could not open the window: %s", SDL_GetError() );
		return 0;
	}

	glcontext = SDL_GL_CreateContext(window);
	DBG_MESSAGE("dr_os_open(SDL2G)", "SDL realized screen size width=%d, height=%d (internal w=%d, h=%d)", width, height, tex_w, tex_h );

	SDL_ShowCursor(0);
	arrow = SDL_GetCursor();
	hourglass = SDL_CreateCursor( hourglass_cursor, hourglass_cursor_mask, 16, 22, 8, 11 );
	blank = SDL_CreateCursor( blank_cursor, blank_cursor, 8, 2, 0, 0 );
	SDL_ShowCursor(1);

#if SDL_VERSION_ATLEAST(2, 0, 10)
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); // no mouse emulation for touch
#endif

	glEnable(GL_TEXTURE_2D);

	check_for_extensions();

	if(  !env_t::hide_keyboard  ) {
		// enable keyboard input at all times unless requested otherwise
	    SDL_StartTextInput();
	}
	check_max_texture_size();
	if(  !check_hardware_accelerated()  ){
		DBG_MESSAGE("dr_os_open(OpenGL)", "No hardware renderer available, exiting...");
		fprintf(stderr, "No hardware renderer available, exiting...");
		return 0;
	}

	display_set_actual_width( window_size.w );
	display_set_height( window_size.h );
	return window_size.w;
}


// shut down SDL
void dr_os_close()
{
	SDL_FreeCursor( blank );
	SDL_FreeCursor( hourglass );
	SDL_GL_DeleteContext( glcontext );
	SDL_DestroyWindow( window );
	SDL_StopTextInput();
}

static void setupGL()
{
	glViewport(0,0,width,height);
	//map x[0,width] to x[-1,1]: X(x) = x/width*2-1
	//map y[0,height] to y[1,-1]: Y(x) = -x/height*2+1
	double mat[16] = {
	  2.0/width, 0,      0, -1,
	  0,     -2.0/height, 0, 1,
	  0,     0,      1, 0,
	  0,     0,      0, 1};
	glMatrixMode(GL_PROJECTION);
	glLoadTransposeMatrixd(mat);

	//this is needed (at least on mesa/i965) to get the first frame into
	//the back buffer
	glDrawBuffer(GL_FRONT);
	glDrawBuffer(GL_BACK);
	glClear(GL_COLOR_BUFFER_BIT);

	glFlush();
	glFinish();
}


// resizes screen
int dr_textur_resize(unsigned short** const textur, int tex_w, int const tex_h)
{
	// enforce multiple of 16 pixels, or there are likely mismatches
//	w = (w + 15 ) & 0x7FF0;

	// w, h are the width in pixel, we calculate now the scree size
	width = TEX_TO_SCREEN_X(tex_w);
	height = TEX_TO_SCREEN_Y(tex_h);
	display_set_actual_width( width );
	display_set_height( height );
	if(  textur  ) {
		*textur = dr_textur_init();
	}
	setupGL();
	return width;
}


unsigned short *dr_textur_init()
{
	setupGL();
	return NULL;
}


/**
 * Transform a 24 bit RGB color into the system format.
 * @return converted color value
 */
PIXVAL get_system_color(rgb888_t col)
{
	SDL_PixelFormat *fmt = SDL_AllocFormat( SDL_PIXELFORMAT_RGB565 );
	unsigned int ret = SDL_MapRGB(fmt, col.r, col.g, col.b);
	SDL_FreeFormat( fmt );
	assert((ret & 0xFFFF0000u) == 0);
	return ret;
}


void dr_prepare_flush()
{
	return;
}


void dr_flush()
{
	display_flush_buffer();

	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glReadBuffer(GL_BACK);
	glDrawBuffer(GL_FRONT);
	glRasterPos2i(0,height);
	glCopyPixels(0,0,width,height,GL_COLOR);
	glDrawBuffer(GL_BACK);
	glFlush();
	glFinish();
}


void dr_textur(int /*xp*/, int /*yp*/, int /*w*/, int /*h*/)
{
  //we don't do partial updates
}
static bool in_finger_handling = false;

// move cursor to the specified location
bool move_pointer(int x, int y)
{
	if (in_finger_handling) {
		return false;
	}
	SDL_WarpMouseInWindow( window, TEX_TO_SCREEN_X(x), TEX_TO_SCREEN_Y(y) );
	return true;
}


// set the mouse cursor (pointer/load)
void set_pointer(int loading)
{
	SDL_SetCursor( loading ? hourglass : arrow );
}


/*
 * Hier sind die Funktionen zur Messageverarbeitung
 */


static inline unsigned int ModifierKeys()
{
	const SDL_Keymod mod = SDL_GetModState();

	return
		  ((mod & KMOD_SHIFT) ? SIM_MOD_SHIFT : SIM_MOD_NONE)
		| ((mod & KMOD_CTRL)  ? SIM_MOD_CTRL  : SIM_MOD_NONE)
#ifdef __APPLE__
		// Treat the Command key as a control key.
		| ((mod & KMOD_GUI)   ? SIM_MOD_CTRL  : SIM_MOD_NONE)
#endif
		;
}


static uint16 conv_mouse_buttons(Uint8 const state)
{
	return
		(state & SDL_BUTTON_LMASK ? MOUSE_LEFTBUTTON  : 0) |
		(state & SDL_BUTTON_MMASK ? MOUSE_MIDBUTTON   : 0) |
		(state & SDL_BUTTON_RMASK ? MOUSE_RIGHTBUTTON : 0);
}


static void internal_GetEvents()
{
	// Apparently Cocoa SDL posts key events that meant to be used by IM...
	// Ignoring SDL_KEYDOWN during preedit seems to work fine.
	static bool composition_is_underway = false;
	static bool ignore_previous_number = false;
	static int previous_multifinger_touch = 0;
	static SDL_FingerID FirstFingerId = 0;
	static double dLastDist = 0.0;

	static bool has_queued_finger_release = false;
	static bool has_queued_zero_mouse_move = false;
	static sint32 last_mx, last_my; // last finger down pos

	if (has_queued_finger_release) {
		// we need to send a finger release, which was not done yet
		has_queued_finger_release = false;
		sys_event.type = SIM_MOUSE_BUTTONS;
		sys_event.code = SIM_MOUSE_LEFTUP;
		sys_event.mb = 0;
		sys_event.mx = last_mx;
		sys_event.my = last_my;
		sys_event.key_mod = ModifierKeys();
		DBG_MESSAGE("SDL_FINGERUP for queue", "SIM_MOUSE_LEFTUP at %i,%i", sys_event.mx, sys_event.my);
		return;
	}

	if (has_queued_zero_mouse_move) {
		// we need to send a finger release, which was not done yet
		has_queued_zero_mouse_move = false;
		sys_event.type = SIM_MOUSE_MOVE;
		sys_event.code = 0;
		sys_event.mb = 0;
		sys_event.mx = last_mx;
		sys_event.my = last_my;
		sys_event.key_mod = ModifierKeys();
		DBG_MESSAGE("SDL_FINGERUP for queue", "SIM_MOUSE_MOVE at %i,%i", sys_event.mx, sys_event.my);
		return;
	}

	SDL_Event event;
	event.type = 1;
	if (SDL_PollEvent(&event) == 0) {
		return;
	}

	static char textinput[SDL_TEXTINPUTEVENT_TEXT_SIZE];
	DBG_DEBUG("SDL_EVENT", "0x%X", event.type);

	if (in_finger_handling && event.type == SDL_FINGERMOTION) {
		// swallow the millons of fingermotion events
		do {
		} while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FINGERMOTION, SDL_FINGERMOTION) == 1);
	}

	switch(  event.type  ) {

		case SDL_APP_DIDENTERFOREGROUND:
			dr_stop_textinput();
			intr_enable();
			//reenable midi
			break;

		case SDL_WINDOWEVENT:
			if(  event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED  ) {
				sys_event.new_window_size_w = max(1, SCREEN_TO_TEX_X(event.window.data1));
				sys_event.new_window_size_h = max(1, SCREEN_TO_TEX_Y(event.window.data2));
				sys_event.type = SIM_SYSTEM;
				sys_event.code = SYSTEM_RESIZE;
			}
			// Ignore other window events.
			break;

		case SDL_MOUSEBUTTONDOWN:
			dLastDist = 0.0;
			if (event.button.which != SDL_TOUCH_MOUSEID) {
				sys_event.type    = SIM_MOUSE_BUTTONS;
				switch(  event.button.button  ) {
					case SDL_BUTTON_LEFT:   sys_event.code = SIM_MOUSE_LEFTBUTTON;  break;
					case SDL_BUTTON_MIDDLE: sys_event.code = SIM_MOUSE_MIDBUTTON;   break;
					case SDL_BUTTON_RIGHT:  sys_event.code = SIM_MOUSE_RIGHTBUTTON; break;
					case SDL_BUTTON_X1:     sys_event.code = SIM_MOUSE_WHEELUP;     break;
					case SDL_BUTTON_X2:     sys_event.code = SIM_MOUSE_WHEELDOWN;   break;
				}
				sys_event.mx      = SCREEN_TO_TEX_X(event.button.x);
				sys_event.my      = SCREEN_TO_TEX_Y(event.button.y);
				sys_event.mb      = conv_mouse_buttons( SDL_GetMouseState(0, 0) );
				sys_event.key_mod = ModifierKeys();
			}
			break;

		case SDL_MOUSEBUTTONUP:
			if (!previous_multifinger_touch  &&  !in_finger_handling) {
				// we try to only handle mouse events
				sys_event.type    = SIM_MOUSE_BUTTONS;
				switch(  event.button.button  ) {
					case SDL_BUTTON_LEFT:   sys_event.code = SIM_MOUSE_LEFTUP;  break;
					case SDL_BUTTON_MIDDLE: sys_event.code = SIM_MOUSE_MIDUP;   break;
					case SDL_BUTTON_RIGHT:  sys_event.code = SIM_MOUSE_RIGHTUP; break;
				}
				sys_event.mx      = SCREEN_TO_TEX_X(event.button.x);
				sys_event.my      = SCREEN_TO_TEX_Y(event.button.y);
				sys_event.mb      = conv_mouse_buttons( SDL_GetMouseState(0, 0) );
				sys_event.key_mod = ModifierKeys();
				previous_multifinger_touch = false;
			}
			break;

		case SDL_MOUSEWHEEL:
			sys_event.type    = SIM_MOUSE_BUTTONS;
			sys_event.code    = event.wheel.y > 0 ? SIM_MOUSE_WHEELUP : SIM_MOUSE_WHEELDOWN;
			sys_event.key_mod = ModifierKeys();
			break;

		case SDL_MOUSEMOTION:
			if (!in_finger_handling) {
				sys_event.type = SIM_MOUSE_MOVE;
				sys_event.code = SIM_MOUSE_MOVED;
				sys_event.mx = SCREEN_TO_TEX_X(event.motion.x);
				sys_event.my = SCREEN_TO_TEX_Y(event.motion.y);
				sys_event.mb = conv_mouse_buttons( SDL_GetMouseState(0, 0) );
				sys_event.key_mod = ModifierKeys();
			}
			break;

		case SDL_FINGERDOWN:
			/* just reset scroll state, since another finger may touch down next
			 * The button down events will be from fingr move and the coordinate will be set from mouse up: enough
			 */
	DBG_MESSAGE("SDL_FINGERDOWN", "fingerID=%x FirstFingerId=%x Finger %i", (int)event.tfinger.fingerId, (int)FirstFingerId, SDL_GetNumTouchFingers(event.tfinger.touchId));

			if (!in_finger_handling) {
				dLastDist = 0.0;
				FirstFingerId = event.tfinger.fingerId;
				DBG_MESSAGE("SDL_FINGERDOWN", "FirstfingerID=%x", FirstFingerId);
				in_finger_handling = true;
				previous_multifinger_touch = 0;
			}
			else if (FirstFingerId != event.tfinger.fingerId) {
				previous_multifinger_touch = 2;
			}
			break;

		case SDL_FINGERMOTION:
			// move whatever
			if(   previous_multifinger_touch==0  &&  FirstFingerId==event.tfinger.fingerId) {
				if (dLastDist == 0.0) {
					// not yet a finger down event before => we send one
					dLastDist = 1e-99;
					sys_event.type = SIM_MOUSE_BUTTONS;
					sys_event.code = SIM_MOUSE_LEFTBUTTON;
					sys_event.mx = event.tfinger.x * display_get_width();
					sys_event.my = event.tfinger.y * display_get_height();
					previous_multifinger_touch = 0;
	DBG_MESSAGE("SDL_FINGERMOTION", "SIM_MOUSE_LEFTBUTTON at %i,%i", sys_event.mx, sys_event.my);
				}
				else {

					sys_event.type = SIM_MOUSE_MOVE;
					sys_event.code = SIM_MOUSE_MOVED;
					sys_event.mx = event.tfinger.x * display_get_width();
					sys_event.my = event.tfinger.y * display_get_height();
	DBG_MESSAGE("SDL_FINGERMOTION", "SIM_MOUSE_MOVED at %i,%i", sys_event.mx, sys_event.my);
				}
				sys_event.mb = MOUSE_LEFTBUTTON;
				sys_event.key_mod = ModifierKeys();
			}
			in_finger_handling = true;
			break;

		case SDL_FINGERUP:
			if (in_finger_handling) {
				if (FirstFingerId==event.tfinger.fingerId  ||  SDL_GetNumTouchFingers(event.tfinger.touchId)==0) {
					if(!previous_multifinger_touch) {
						if (dLastDist == 0.0) {
							dLastDist = 1e-99;
							// return a press event
							sys_event.type = SIM_MOUSE_BUTTONS;
							sys_event.code = SIM_MOUSE_LEFTBUTTON;
							sys_event.mb = MOUSE_LEFTBUTTON;
							sys_event.key_mod = ModifierKeys();
							last_mx = sys_event.mx = event.tfinger.x * display_get_width();
							last_my = sys_event.my = event.tfinger.y * display_get_height();
							// not yet moved -> set click origin or click will be at last position ...
							set_click_xy(sys_event.mx, sys_event.my);

							has_queued_finger_release = true;
		DBG_MESSAGE("SDL_FINGERUP", "SIM_MOUSE_LEFTDOWN+UP at %i,%i", sys_event.mx, sys_event.my);
						}
						else {
							sys_event.type = SIM_MOUSE_BUTTONS;
							sys_event.code = SIM_MOUSE_LEFTUP;
							sys_event.mb = 0;
							sys_event.mx = (event.tfinger.x + event.tfinger.dx) * display_get_width();
							sys_event.my = (event.tfinger.y + event.tfinger.dy) * display_get_height();
							sys_event.key_mod = ModifierKeys();
		DBG_MESSAGE("SDL_FINGERUP", "SIM_MOUSE_LEFTUP at %i,%i", sys_event.mx, sys_event.my);
						}
					}
					else {
		DBG_MESSAGE("SDL_FINGERUP", "ignored at %i,%i because previous_multifinger_touch>0", sys_event.mx, sys_event.my);
					}
					previous_multifinger_touch = 0;
					in_finger_handling = 0;
					FirstFingerId = 0xFFFF;
				}
				else {
		DBG_MESSAGE("SDL_FINGERUP", "ignored at %i,%i beacuse FirstFingerId(%xd)!=event.tfinger.fingerId(%xd) &&  SDL_GetNumTouchFinger()=%d", sys_event.mx, sys_event.my,FirstFingerId, event.tfinger.fingerId,SDL_GetNumTouchFingers(event.tfinger.touchId));
				}
			}
			break;

		case SDL_MULTIGESTURE:
			DBG_MESSAGE("SDL_FINGERUP", "Finger %i", SDL_GetNumTouchFingers(event.tfinger.touchId));
			in_finger_handling = true;
			if( event.mgesture.numFingers == 2 ) {
				int num_events;
				do {
					dLastDist += event.mgesture.dDist;
					num_events = SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_MULTIGESTURE, SDL_MULTIGESTURE);
				} while (num_events == 1);

				// any multitouch is intepreted as pinch zoom
				if (dLastDist < -DELTA_PINCH) {
					sys_event.type = SIM_MOUSE_BUTTONS;
					sys_event.code = SIM_MOUSE_WHEELDOWN;
					sys_event.key_mod = ModifierKeys();
					//dLastDist += DELTA_PINCH;
					dLastDist = 0;
				}
				else if (dLastDist > DELTA_PINCH) {
					sys_event.type = SIM_MOUSE_BUTTONS;
					sys_event.code = SIM_MOUSE_WHEELUP;
					sys_event.key_mod = ModifierKeys();
					//dLastDist -= DELTA_PINCH;
					dLastDist = 0;
				}

				previous_multifinger_touch = 2;
			}
			else if (event.mgesture.numFingers == 3) {
				// any three finger touch is scrolling the map

				if (previous_multifinger_touch != 3) {
					// just started scrolling
					set_click_xy(SCREEN_TO_TEX_X(event.mgesture.x* width), SCREEN_TO_TEX_Y(event.mgesture.y* height));
				}

				do {
				} while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_MULTIGESTURE, SDL_MULTIGESTURE) == 1);
				sys_event.type = SIM_MOUSE_MOVE;
				sys_event.code = SIM_MOUSE_MOVED;
				sys_event.mb = MOUSE_RIGHTBUTTON;
				sys_event.key_mod = ModifierKeys();
				sys_event.mx = SCREEN_TO_TEX_X(event.mgesture.x * width);
				sys_event.my = SCREEN_TO_TEX_Y(event.mgesture.y * height);
				previous_multifinger_touch = 3;
			}
			break;

		case SDL_KEYDOWN: {
			// Hack: when 2 byte character composition is under way, we have to leave the key processing with the IME
			// BUT: if not, we have to do it ourselves, or the cursor or return will not be recognised
			if(  composition_is_underway  ) {
				if(  gui_component_t *c = win_get_focus()  ) {
					if(  gui_textinput_t *tinp = dynamic_cast<gui_textinput_t *>(c)  ) {
						if(  tinp->get_composition()[0]  ) {
							// pending string, handled by IME
							break;
						}
					}
				}
			}

			unsigned long code;
#ifdef _WIN32
			// SDL doesn't set numlock state correctly on startup. Revert to win32 function as workaround.
			const bool key_numlock = ((GetKeyState( VK_NUMLOCK ) & 1) != 0);
#else
			const bool key_numlock = (SDL_GetModState() & KMOD_NUM);
#endif
			const bool numlock = key_numlock  ||  (env_t::numpad_always_moves_map  &&  !win_is_textinput());
			sys_event.key_mod = ModifierKeys();
			SDL_Keycode sym = event.key.keysym.sym;
			bool np = false; // to indicate we converted a numpad key

			switch(  sym  ) {
				case SDLK_BACKSPACE:  code = SIM_KEY_BACKSPACE;             break;
				case SDLK_TAB:        code = SIM_KEY_TAB;                   break;
				case SDLK_RETURN:     code = SIM_KEY_ENTER;                 break;
				case SDLK_ESCAPE:     code = SIM_KEY_ESCAPE;                break;
				case SDLK_AC_BACK:
				case SDLK_DELETE:     code = SIM_KEY_DELETE;                break;
				case SDLK_DOWN:       code = SIM_KEY_DOWN;                  break;
				case SDLK_END:        code = SIM_KEY_END;                   break;
				case SDLK_HOME:       code = SIM_KEY_HOME;                  break;
				case SDLK_F1:         code = SIM_KEY_F1;                    break;
				case SDLK_F2:         code = SIM_KEY_F2;                    break;
				case SDLK_F3:         code = SIM_KEY_F3;                    break;
				case SDLK_F4:         code = SIM_KEY_F4;                    break;
				case SDLK_F5:         code = SIM_KEY_F5;                    break;
				case SDLK_F6:         code = SIM_KEY_F6;                    break;
				case SDLK_F7:         code = SIM_KEY_F7;                    break;
				case SDLK_F8:         code = SIM_KEY_F8;                    break;
				case SDLK_F9:         code = SIM_KEY_F9;                    break;
				case SDLK_F10:        code = SIM_KEY_F10;                   break;
				case SDLK_F11:        code = SIM_KEY_F11;                   break;
				case SDLK_F12:        code = SIM_KEY_F12;                   break;
				case SDLK_F13:        code = SIM_KEY_F13;                   break;
				case SDLK_F14:        code = SIM_KEY_F14;                   break;
				case SDLK_F15:        code = SIM_KEY_F15;                   break;
				case SDLK_KP_0:       np = true; code = (numlock ? '0' : (unsigned long)SIM_KEY_NUMPAD_BASE); break;
				case SDLK_KP_1:       np = true; code = (numlock ? '1' : (unsigned long)SIM_KEY_DOWNLEFT); break;
				case SDLK_KP_2:       np = true; code = (numlock ? '2' : (unsigned long)SIM_KEY_DOWN); break;
				case SDLK_KP_3:       np = true; code = (numlock ? '3' : (unsigned long)SIM_KEY_DOWNRIGHT); break;
				case SDLK_KP_4:       np = true; code = (numlock ? '4' : (unsigned long)SIM_KEY_LEFT); break;
				case SDLK_KP_5:       np = true; code = (numlock ? '5' : (unsigned long)SIM_KEY_CENTER); break;
				case SDLK_KP_6:       np = true; code = (numlock ? '6' : (unsigned long)SIM_KEY_RIGHT); break;
				case SDLK_KP_7:       np = true; code = (numlock ? '7' : (unsigned long)SIM_KEY_UPLEFT); break;
				case SDLK_KP_8:       np = true; code = (numlock ? '8' : (unsigned long)SIM_KEY_UP); break;
				case SDLK_KP_9:       np = true; code = (numlock ? '9' : (unsigned long)SIM_KEY_UPRIGHT); break;
				case SDLK_KP_ENTER:   code = SIM_KEY_ENTER;                 break;
				case SDLK_LEFT:       code = SIM_KEY_LEFT;                  break;
				case SDLK_PAGEDOWN:   code = '<';                           break;
				case SDLK_PAGEUP:     code = '>';                           break;
				case SDLK_RIGHT:      code = SIM_KEY_RIGHT;                 break;
				case SDLK_UP:         code = SIM_KEY_UP;                    break;
				case SDLK_PAUSE:      code = SIM_KEY_PAUSE;                 break;
				case SDLK_SCROLLLOCK: code = SIM_KEY_SCROLLLOCK;            break;
				default: {
					// Handle CTRL-keys. SDL_TEXTINPUT event handles regular input
					if(  (sys_event.key_mod & SIM_MOD_CTRL)  &&  SDLK_a <= sym  &&  sym <= SDLK_z  ) {
						code = event.key.keysym.sym & 31;
					}
					else {
						code = 0;
					}
					break;
				}
			}
			ignore_previous_number = (np  &&  key_numlock);
			sys_event.type    = SIM_KEYBOARD;
			sys_event.code    = code;
			break;
		}

		case SDL_TEXTINPUT: {
			size_t in_pos = 0;
			utf32 uc = utf8_decoder_t::decode((utf8 const*)event.text.text, in_pos);
			if(  event.text.text[in_pos]==0  ) {
				// single character
				if( ignore_previous_number ) {
					ignore_previous_number = false;
					break;
				}
				sys_event.type    = SIM_KEYBOARD;
				sys_event.code    = (unsigned long)uc;
			}
			else {
				// string
				strcpy( textinput, event.text.text );
				sys_event.type    = SIM_STRING;
				sys_event.ptr     = (void*)textinput;
			}
			sys_event.key_mod = ModifierKeys();
			composition_is_underway = false;
			break;
		}
#ifdef USE_SDL_TEXTEDITING
		case SDL_TEXTEDITING: {
			//printf( "SDL_TEXTEDITING {timestamp=%d, \"%s\", start=%d, length=%d}\n", event.edit.timestamp, event.edit.text, event.edit.start, event.edit.length );
			strcpy( textinput, event.edit.text );
			if(  !textinput[0]  ) {
				composition_is_underway = false;
			}
			int i = 0;
			int start = 0;
			for(  ; i<event.edit.start; ++i  ) {
				start = utf8_get_next_char( (utf8 *)event.edit.text, start );
			}
			int end = start;
			for(  ; i<event.edit.start+event.edit.length; ++i  ) {
				end = utf8_get_next_char( (utf8*)event.edit.text, end );
			}

			if(  gui_component_t *c = win_get_focus()  ) {
				if(  gui_textinput_t *tinp = dynamic_cast<gui_textinput_t *>(c)  ) {
					tinp->set_composition_status( textinput, start, end-start );
				}
			}
			composition_is_underway = true;
			break;
		}
#endif
		case SDL_KEYUP: {
			sys_event.type = SIM_KEYBOARD;
			sys_event.code = 0;
			break;
		}
		case SDL_QUIT: {
			sys_event.type = SIM_SYSTEM;
			sys_event.code = SYSTEM_QUIT;
			break;
		}
		default: {
			sys_event.type = SIM_IGNORE_EVENT;
			sys_event.code = 0;
			break;
		}
	}
}


void GetEvents()
{
	internal_GetEvents();
}


void show_pointer(int yesno)
{
	SDL_SetCursor( (yesno != 0) ? arrow : blank );
}


void ex_ord_update_mx_my()
{
	SDL_PumpEvents();
}


uint32 dr_time()
{
	return SDL_GetTicks();
}


void dr_sleep(uint32 usec)
{
	SDL_Delay( usec );
}


void dr_start_textinput()
{
	if(  env_t::hide_keyboard  ) {
	    SDL_StartTextInput();
		DBG_MESSAGE("SDL_StartTextInput", "");
	}
}


void dr_stop_textinput()
{
	if(  env_t::hide_keyboard  ) {
	    SDL_StopTextInput();
		DBG_MESSAGE("SDL_StoptTextInput", "");
	}
	else {
		SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
	}
}

void dr_notify_input_pos(scr_coord pos)
{
	SDL_Rect rect = { int( TEX_TO_SCREEN_X( pos.x ) ),
	                  int( TEX_TO_SCREEN_Y( pos.y + LINESPACE ) ),
	                  1, 1
	                };
	SDL_SetTextInputRect( &rect );
}


const char* dr_get_locale()
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
	static char LanguageCode[5] = "";
	SDL_Locale *loc = SDL_GetPreferredLocales();
	if( loc ) {
		strncpy( LanguageCode, loc->language, 2 );
		LanguageCode[2] = 0;
		DBG_MESSAGE( "dr_get_locale()", "%2s", LanguageCode );
		return LanguageCode;
	}
#endif
	return NULL;
}

bool dr_has_fullscreen()
{
	return false;
}

sint16 dr_get_fullscreen()
{
	return fullscreen ? BORDERLESS : WINDOWED;
}

sint16 dr_toggle_borderless()
{
	if ( fullscreen ) {
		SDL_SetWindowFullscreen(window, 0);
		SDL_SetWindowPosition(window, 10, 10);
		fullscreen = WINDOWED;
	}
	else {
		SDL_SetWindowPosition(window, 0, 0);
		SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
		fullscreen = BORDERLESS;
	}
	return fullscreen;
}

sint16 dr_suspend_fullscreen()
{
	int was_fullscreen = fullscreen;
	if (fullscreen) {
		SDL_SetWindowFullscreen(window, 0);
		fullscreen = WINDOWED;
	}
	SDL_MinimizeWindow(window);
	return was_fullscreen;
}

void dr_restore_fullscreen(sint16 was_fullscreen)
{
	SDL_RestoreWindow(window);
	if(was_fullscreen) {
		SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
		fullscreen = BORDERLESS;
	}
}

#ifdef _MSC_VER
// Needed for MS Visual C++ with /SUBSYSTEM:CONSOLE to work , if /SUBSYSTEM:WINDOWS this function is compiled but unreachable
#undef main
int main()
{
	return WinMain(NULL,NULL,NULL,NULL);
}
#endif


#ifdef _WIN32
int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main(int argc, char **argv)
#endif
{
#ifdef _WIN32
	int    const argc = __argc;
	char** const argv = __argv;
#endif
	return sysmain(argc, argv);
}
