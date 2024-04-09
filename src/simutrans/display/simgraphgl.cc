/*
 * This file is part of the Simutrans project under the Artistic License.
 * (see LICENSE.txt)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <map>
#include <unordered_map>
#include <vector>

#include "../simtypes.h"

 /*
  * Zoom factor (must be done before including simgraph)
  */
#define MAX_ZOOM_FACTOR (10)
#define ZOOM_NEUTRAL (3)
uint32 zoom_factor = ZOOM_NEUTRAL;
extern const sint32 zoom_num[MAX_ZOOM_FACTOR + 1] = { 2, 3, 4, 1, 3, 5, 1, 3, 1, 3, 1 };
extern const sint32 zoom_den[MAX_ZOOM_FACTOR + 1] = { 1, 2, 3, 1, 4, 8, 2, 8, 4, 16, 8 };

#include "../macros.h"
#include "font.h"
#include "../pathes.h"
#include "../simconst.h"
#include "../sys/simsys.h"
#include "../simmem.h"
#include "../simdebug.h"
#include "../descriptor/image.h"
#include "../dataobj/environment.h"
#include "../dataobj/translator.h"
#include "../utils/unicode.h"
#include "../simticker.h"
#include "../utils/simstring.h"
#include "../utils/unicode.h"
#include "../io/raw_image.h"

#include "../gui/simwin.h"
#include "../dataobj/environment.h"

#include "../obj/roadsign.h" // for signal status indicator

#include "simgraph.h"

#include <GL/glew.h>
#include <GL/gl.h>


#ifdef _MSC_VER
#	include <io.h>
#	define W_OK 2
#else
#	include <unistd.h>
#endif

#ifdef MULTI_THREAD
#error simgraphgl does not support MULTI_THREAD drawing
#endif
// to pass the extra clipnum when not needed use this
#ifdef MULTI_THREAD
#define CLIPNUM_IGNORE , 0
#else
#define CLIPNUM_IGNORE
#endif


#include "simgraph.h"

#ifdef USE_SOFTPOINTER
#error SOFTPOINTER not supported
#endif
static int standard_pointer = -1;

static void build_stencil_for(int _x0, int _y0, int _x1, int _y1,
                              int min_x, int min_y, int max_x, int max_y);

namespace simgraphgl {
class clip_line_t {
private:
	// line from (x0,y0) to (x1 y1)
	// clip (do not draw) everything right from the ray (x0,y0)->(x1,y1)
	// pixels on the ray are not drawn
	// (not_convex) if y0>=y1 then clip along the path (x0,-inf)->(x0,y0)->(x1,y1)
	// (not_convex) if y0<y1  then clip along the path (x0,y0)->(x1,y1)->(x1,+inf)
	scr_coord_val x0, y0;
	scr_coord_val x1, y1;
	bool non_convex;

public:
	void clip_from_to(scr_coord_val x0_, scr_coord_val y0_, scr_coord_val x1_, scr_coord_val y1_, bool non_convex_)
	{
		x0 = x0_;
		x1 = x1_;
		y0 = y0_;
		y1 = y1_;
		non_convex = non_convex_;
	}

	void build_stencil(scr_coord_val min_x, scr_coord_val min_y, scr_coord_val max_x, scr_coord_val max_y) const
	{
		if(  non_convex  ) {
			if(  y1 < y0  ) {
				build_stencil_for(
				                   x0, y0,
				                   x1, y1,
				                   min_x, y1,
				                   max_x, max_y);
				if(  min_y < y1  ) {
					build_stencil_for(
					                   x1,y1,
					                   x1, min_y,
					                   min_x, min_y,
					                   max_x, y1);
				}
			}
			else {
				if(  min_y < y0  ) {
					build_stencil_for(
					                   x0, min_y,
					                   x0, y0,
					                   min_x, min_y,
					                   max_x, y0);
				}
				build_stencil_for(
				                   x0, y0,
				                   x1, y1,
				                   min_x, y0,
				                   max_x, max_y);
			}
		}
		else {
			build_stencil_for(
			                   x0, y0,
			                   x1, y1,
			                   min_x, min_y,
			                   max_x, max_y);
		}
	}
};

#define MAX_POLY_CLIPS 6

MSVC_ALIGN(64) struct clipping_info_t {
	// current clipping rectangle
	clip_dimension clip_rect;
	// clipping rectangle to be swapped by display_clip_wh_toggle
	clip_dimension clip_rect_swap;
	bool swap_active;
	// poly clipping variables
	int number_of_clips;
	uint8 active_ribi;
	uint8 clip_ribi[MAX_POLY_CLIPS];
	clip_line_t poly_clips[MAX_POLY_CLIPS];
} GCC_ALIGN(64); // aligned to separate cachelines

//todo: use char pages and cache used glyphs there
struct CharInfo {
	GLuint texture;
	GLfloat x, y, w, h;
};
struct CharPageInfo {
	GLuint texture;
	GLuint width, height;
	GLuint curx, cury, lineheight;
};

struct PIX32
{
	uint8_t R;
	uint8_t G;
	uint8_t B;
	uint8_t A;
};
struct ArrayInfo
{
	GLuint tex;
	uint64_t hash;
	int change_ctr;
	int use_ctr;
};
}

using namespace simgraphgl;

#ifdef MULTI_THREAD
clipping_info_t clips[MAX_THREADS];
#define CR0 clips[0]
#else
clipping_info_t clips;
#define CR0 clips
#endif

#define CR clips CLIP_NUM_INDEX

static font_t default_font;

// needed for resizing gui
int default_font_ascent = 0;
int default_font_linespace = 0;
static int default_font_numberwidth = 0;


#define LIGHT_COUNT (15)
#define MAX_PLAYER_COUNT (16)

#define RGBMAPSIZE (0x8000+LIGHT_COUNT+MAX_PLAYER_COUNT+1024 /* 343 transparent */)

// RGB 555/565 specific functions

// different masks needed for RGB 555 and RGB 565 for blending
#ifdef RGB555
#define ONE_OUT (0x3DEF) // mask out bits after applying >>1
#define TWO_OUT (0x1CE7) // mask out bits after applying >>2
#define MASK_32 (0x03e0f81f) // mask out bits after transforming to 32bit
inline PIXVAL rgb(PIXVAL r, PIXVAL g, PIXVAL b) { return (r << 10) | (g << 5) | b; }
inline PIXVAL red(PIXVAL rgb) { return  rgb >> 10; }
inline PIXVAL green(PIXVAL rgb) { return (rgb >> 5) & 0x1F; }
#else
#define ONE_OUT (0x7bef) // mask out bits after applying >>1
#define TWO_OUT (0x39E7) // mask out bits after applying >>2
#define MASK_32 (0x07e0f81f) // mask out bits after transforming to 32bit
inline PIXVAL rgb(PIXVAL r, PIXVAL g, PIXVAL b) { return (r << 11) | (g << 5) | b; }
inline PIXVAL red(PIXVAL rgb) { return rgb >> 11; }
inline PIXVAL green(PIXVAL rgb) { return (rgb >> 5) & 0x3F; }
#endif
inline PIXVAL blue(PIXVAL rgb) { return  rgb & 0x1F; }

/**
 * Implement shift-and-mask for rgb values:
 * shift-right by 1 or 2, and mask it to a valid rgb number.
 */
inline PIXVAL rgb_shr1(PIXVAL c) { return (c >> 1) & ONE_OUT; }
inline PIXVAL rgb_shr2(PIXVAL c) { return (c >> 2) & TWO_OUT; }


/*
 * mapping tables for RGB 555 to actual output format
 * plus the special (player, day&night) colors appended
 *
 * 0x0000 - 0x7FFF: RGB 555 colors
 * 0x8000 - 0x800F: Player colors
 * 0x8010 - 0x001F: Day&Night special colors
 * The following transparent colors are not in the colortable
 * 0x8020 - 0x83E0: special colors in 31 transparency levels
 * 0x83E1 - 0xFFE1: 3 4 3 RGB transparent colors in 31 transparency levels
 * transparency levels are in 1/32nds to 31/32nds
 * the 31 transparency levels for each color occupy consecutive indexes
 *
 * see also descriptor/writer/image_writer.cc: pixrgb_to_pixval() for the generator
 */
static PIXVAL rgbmap_day_night[RGBMAPSIZE];
static GLuint rgbmap_day_night_tex;


/*
 * same as rgbmap_day_night, but always daytime colors
 */
static PIXVAL rgbmap_all_day[RGBMAPSIZE];
static GLuint rgbmap_all_day_tex;

/*
 * used by pixel copy functions, is one of rgbmap_day_night
 * rgbmap_all_day
 */
static PIXVAL *rgbmap_current = 0;
static GLuint rgbmap_current_tex;


/*
 * mapping table for special-colors (AI player colors)
 * to actual output format - day&night mode
 * 16 sets of 16 colors
 */
static PIXVAL specialcolormap_day_night[256];


/*
 * mapping table for special-colors (AI player colors)
 * to actual output format - all day mode
 * 16 sets of 16 colors
 */
PIXVAL specialcolormap_all_day[256];

// offsets of first and second company color
static uint8 player_offsets[MAX_PLAYER_COUNT][2];


/*
 * Image map descriptor structure
 */
struct imd {
	sint16 base_x; // min x offset
	sint16 base_y; // min y offset
	sint16 base_w; // width
	sint16 base_h; // height

	PIXVAL* base_data; // original image data

	GLuint base_tex;
	GLuint index_tex;

	sint32 zoom_num;
	sint32 zoom_den;
	float zoom;
	uint32 flags;
};

#define FLAG_ZOOMABLE (4)

#define TRANSPARENT_RUN (0x8000u)

static scr_coord_val disp_width  = 640;
static scr_coord_val disp_actual_width  = 640;
static scr_coord_val disp_height = 480;


/*
 * Image table
 */
static std::vector<imd> images;

static std::unordered_map<uint64_t, GLuint> rgbmap_cache;
static std::unordered_map<void const *,ArrayInfo> arrayInfo;
static std::map<uint32_t,CharInfo> chartex;
static std::vector<CharPageInfo> charpage;

static uint8 player_night=0xFF;
static uint8 player_day=0xFF;

static int light_level = 0;
static int night_shift = -1;


/*
 * special colors during daytime
 */
rgb888_t display_day_lights[LIGHT_COUNT] = {
	{ 0x57, 0x65, 0x6F }, // Dark windows, lit yellowish at night
	{ 0x7F, 0x9B, 0xF1 }, // Lighter windows, lit blueish at night
	{ 0xFF, 0xFF, 0x53 }, // Yellow light
	{ 0xFF, 0x21, 0x1D }, // Red light
	{ 0x01, 0xDD, 0x01 }, // Green light
	{ 0x6B, 0x6B, 0x6B }, // Non-darkening grey 1 (menus)
	{ 0x9B, 0x9B, 0x9B }, // Non-darkening grey 2 (menus)
	{ 0xB3, 0xB3, 0xB3 }, // non-darkening grey 3 (menus)
	{ 0xC9, 0xC9, 0xC9 }, // Non-darkening grey 4 (menus)
	{ 0xDF, 0xDF, 0xDF }, // Non-darkening grey 5 (menus)
	{ 0xE3, 0xE3, 0xFF }, // Nearly white light at day, yellowish light at night
	{ 0xC1, 0xB1, 0xD1 }, // Windows, lit yellow
	{ 0x4D, 0x4D, 0x4D }, // Windows, lit yellow
	{ 0xE1, 0x00, 0xE1 }, // purple light for signals
	{ 0x01, 0x01, 0xFF }  // blue light
};


/*
 * special colors during nighttime
 */
rgb888_t display_night_lights[LIGHT_COUNT] = {
	{ 0xD3, 0xC3, 0x80 }, // Dark windows, lit yellowish at night
	{ 0x80, 0xC3, 0xD3 }, // Lighter windows, lit blueish at night
	{ 0xFF, 0xFF, 0x53 }, // Yellow light
	{ 0xFF, 0x21, 0x1D }, // Red light
	{ 0x01, 0xDD, 0x01 }, // Green light
	{ 0x6B, 0x6B, 0x6B }, // Non-darkening grey 1 (menus)
	{ 0x9B, 0x9B, 0x9B }, // Non-darkening grey 2 (menus)
	{ 0xB3, 0xB3, 0xB3 }, // non-darkening grey 3 (menus)
	{ 0xC9, 0xC9, 0xC9 }, // Non-darkening grey 4 (menus)
	{ 0xDF, 0xDF, 0xDF }, // Non-darkening grey 5 (menus)
	{ 0xFF, 0xFF, 0xE3 }, // Nearly white light at day, yellowish light at night
	{ 0xD3, 0xC3, 0x80 }, // Windows, lit yellow
	{ 0xD3, 0xC3, 0x80 }, // Windows, lit yellow
	{ 0xE1, 0x00, 0xE1 }, // purple light for signals
	{ 0x01, 0x01, 0xFF }  // blue light
};


// the players colors and colors for simple drawing operations
// each 8 colors are a player color
static const rgb888_t special_pal[SPECIAL_COLOR_COUNT] =
{
	{  36,  75, 103 }, {  57,  94, 124 }, {  76, 113, 145 }, {  96, 132, 167 }, { 116, 151, 189 }, { 136, 171, 211 }, { 156, 190, 233 }, { 176, 210, 255 },
	{  88,  88,  88 }, { 107, 107, 107 }, { 125, 125, 125 }, { 144, 144, 144 }, { 162, 162, 162 }, { 181, 181, 181 }, { 200, 200, 200 }, { 219, 219, 219 },
	{  17,  55, 133 }, {  27,  71, 150 }, {  37,  86, 167 }, {  48, 102, 185 }, {  58, 117, 202 }, {  69, 133, 220 }, {  79, 149, 237 }, {  90, 165, 255 },
	{ 123,  88,   3 }, { 142, 111,   4 }, { 161, 134,   5 }, { 180, 157,   7 }, { 198, 180,   8 }, { 217, 203,  10 }, { 236, 226,  11 }, { 255, 249,  13 },
	{  86,  32,  14 }, { 110,  40,  16 }, { 134,  48,  18 }, { 158,  57,  20 }, { 182,  65,  22 }, { 206,  74,  24 }, { 230,  82,  26 }, { 255,  91,  28 },
	{  34,  59,  10 }, {  44,  80,  14 }, {  53, 101,  18 }, {  63, 122,  22 }, {  77, 143,  29 }, {  92, 164,  37 }, { 106, 185,  44 }, { 121, 207,  52 },
	{   0,  86,  78 }, {   0, 108,  98 }, {   0, 130, 118 }, {   0, 152, 138 }, {   0, 174, 158 }, {   0, 196, 178 }, {   0, 218, 198 }, {   0, 241, 219 },
	{  74,   7, 122 }, {  95,  21, 139 }, { 116,  37, 156 }, { 138,  53, 173 }, { 160,  69, 191 }, { 181,  85, 208 }, { 203, 101, 225 }, { 225, 117, 243 },
	{  59,  41,   0 }, {  83,  55,   0 }, { 107,  69,   0 }, { 131,  84,   0 }, { 155,  98,   0 }, { 179, 113,   0 }, { 203, 128,   0 }, { 227, 143,   0 },
	{  87,   0,  43 }, { 111,  11,  69 }, { 135,  28,  92 }, { 159,  45, 115 }, { 183,  62, 138 }, { 230,  74, 174 }, { 245, 121, 194 }, { 255, 156, 209 },
	{  20,  48,  10 }, {  44,  74,  28 }, {  68,  99,  45 }, {  93, 124,  62 }, { 118, 149,  79 }, { 143, 174,  96 }, { 168, 199, 113 }, { 193, 225, 130 },
	{  54,  19,  29 }, {  82,  44,  44 }, { 110,  69,  58 }, { 139,  95,  72 }, { 168, 121,  86 }, { 197, 147, 101 }, { 226, 173, 115 }, { 255, 199, 130 },
	{   8,  11, 100 }, {  14,  22, 116 }, {  20,  33, 139 }, {  26,  44, 162 }, {  41,  74, 185 }, {  57, 104, 208 }, {  76, 132, 231 }, {  96, 160, 255 },
	{  43,  30,  46 }, {  68,  50,  85 }, {  93,  70, 110 }, { 118,  91, 130 }, { 143, 111, 170 }, { 168, 132, 190 }, { 193, 153, 210 }, { 219, 174, 230 },
	{  63,  18,  12 }, {  90,  38,  30 }, { 117,  58,  42 }, { 145,  78,  55 }, { 172,  98,  67 }, { 200, 118,  80 }, { 227, 138,  92 }, { 255, 159, 105 },
	{  11,  68,  30 }, {  33,  94,  56 }, {  54, 120,  81 }, {  76, 147, 106 }, {  98, 174, 131 }, { 120, 201, 156 }, { 142, 228, 181 }, { 164, 255, 207 },
	{  64,   0,   0 }, {  96,   0,   0 }, { 128,   0,   0 }, { 192,   0,   0 }, { 255,   0,   0 }, { 255,  64,  64 }, { 255,  96,  96 }, { 255, 128, 128 },
	{   0, 128,   0 }, {   0, 196,   0 }, {   0, 225,   0 }, {   0, 240,   0 }, {   0, 255,   0 }, {  64, 255,  64 }, {  94, 255,  94 }, { 128, 255, 128 },
	{   0,   0, 128 }, {   0,   0, 192 }, {   0,   0, 224 }, {   0,   0, 255 }, {   0,  64, 255 }, {   0,  94, 255 }, {   0, 106, 255 }, {   0, 128, 255 },
	{ 128,  64,   0 }, { 193,  97,   0 }, { 215, 107,   0 }, { 235, 118,   0 }, { 255, 128,   0 }, { 255, 149,  43 }, { 255, 170,  85 }, { 255, 193, 132 },
	{   8,  52,   0 }, {  16,  64,   0 }, {  32,  80,   4 }, {  48,  96,   4 }, {  64, 112,  12 }, {  84, 132,  20 }, { 104, 148,  28 }, { 128, 168,  44 },
	{ 164, 164,   0 }, { 180, 180,   0 }, { 193, 193,   0 }, { 215, 215,   0 }, { 235, 235,   0 }, { 255, 255,   0 }, { 255, 255,  64 }, { 255, 255, 128 },
	{  32,   4,   0 }, {  64,  20,   8 }, {  84,  28,  16 }, { 108,  44,  28 }, { 128,  56,  40 }, { 148,  72,  56 }, { 168,  92,  76 }, { 184, 108,  88 },
	{  64,   0,   0 }, {  96,   8,   0 }, { 112,  16,   0 }, { 120,  32,   8 }, { 138,  64,  16 }, { 156,  72,  32 }, { 174,  96,  48 }, { 192, 128,  64 },
	{  32,  32,   0 }, {  64,  64,   0 }, {  96,  96,   0 }, { 128, 128,   0 }, { 144, 144,   0 }, { 172, 172,   0 }, { 192, 192,   0 }, { 224, 224,   0 },
	{  64,  96,   8 }, {  80, 108,  32 }, {  96, 120,  48 }, { 112, 144,  56 }, { 128, 172,  64 }, { 150, 210,  68 }, { 172, 238,  80 }, { 192, 255,  96 },
	{  32,  32,  32 }, {  48,  48,  48 }, {  64,  64,  64 }, {  80,  80,  80 }, {  96,  96,  96 }, { 172, 172, 172 }, { 236, 236, 236 }, { 255, 255, 255 },
	{  41,  41,  54 }, {  60,  45,  70 }, {  75,  62, 108 }, {  95,  77, 136 }, { 113, 105, 150 }, { 135, 120, 176 }, { 165, 145, 218 }, { 198, 191, 232 }
};


/*
 * tile raster width
 */
scr_coord_val tile_raster_width = 16;      // zoomed
scr_coord_val base_tile_raster_width = 16; // original

// variables for storing currently used image procedure set and tile raster width
display_image_proc display_normal = NULL;
display_image_proc display_color = NULL;
display_blend_proc display_blend = NULL;
display_alpha_proc display_alpha = NULL;
signed short current_tile_raster_width = 0;


// Pierre : glsl shaders for img_alpha

// combined shader
// note about gl_Color.a: selects between indexed color(0.0),
// texture color(0.5) and gl_Color.rgb(1.0)
static char const combined_fragmentShaderText[] =
	"uniform sampler2D texColor,texAlpha,texRGBMap;\n"
	"uniform vec4 alphaMask;\n"
	"void main () {\n"
	"   vec4 alpha = texture2D(texAlpha,gl_TexCoord[0].st);\n"
	"   vec4 index = texture2D(texColor,gl_TexCoord[0].st);\n"
	"   vec3 indexedrgb = texture2D(texRGBMap,index.st).rgb;\n"
	"   vec3 rgb = indexedrgb;\n"
	"   rgb = mix(rgb,gl_Color.rgb,gl_Color.a);\n" //handle gl_Color.a = 0 and 1
	"   rgb = mix(index.rgb,rgb,abs(2.0*gl_Color.a-1.0));\n" //handle gl_Color.a = 0.5
	"   gl_FragColor.rgb = rgb;\n"
	"   gl_FragColor.a = clamp(alpha.r * alphaMask.r +\n"
	"                          alpha.g * alphaMask.g +\n"
	"                          alpha.b * alphaMask.b +\n"
	"                          index.a * alphaMask.a, 0.0, 1.0);\n"
	"}\n";

//vertex shader
static char const vertexShaderText[] =
	"void main () {\n"
	"   gl_Position = ftransform();\n"
	"   gl_TexCoord[0] = gl_TextureMatrix[0] * gl_MultiTexCoord0;\n"
	"   gl_FrontColor = gl_Color;\n"
	"}\n";

static GLuint combined_program;
static GLuint combined_texColor_Location;
static GLuint combined_texRGBMap_Location;
static GLuint combined_texAlpha_Location;
static GLuint combined_alphaMask_Location;

static inline rgb888_t pixval_to_rgb888(PIXVAL colour)
{
	// Scale each colour channel from 5 or 6 bits to 8 bits
#ifdef RGB555
	return {
		uint8( ( ( colour >> 10 ) & 0x1F ) * 0xFF / 0x1F ), // R
		uint8( ( ( colour >>  5 ) & 0x1F ) * 0xFF / 0x1F ), // G
		uint8( ( ( colour >>  0 ) & 0x1F ) * 0xFF / 0x1F ) // B
	};
#else
	return {
		uint8( ( ( colour >> 11 ) & 0x1F ) * 0xFF / 0x1F ), // R
		uint8( ( ( colour >>  5 ) & 0x3F ) * 0xFF / 0x3F ), // G
		uint8( ( ( colour >>  0 ) & 0x1F ) * 0xFF / 0x1F ) // B
	};
#endif
}


static inline PIXVAL pixval_to_rgb343(PIXVAL rgb)
{
	//         msb          lsb
	// rgb555: xrrrrrgggggbbbbb
	// rgb565: rrrrrggggggbbbbb
	// rgb343:       rrrggggbbb
#ifdef RGB555
	return ( ( rgb >> 5 ) & 0x0380 ) | ( ( rgb >>  3 ) & 0x0078 ) | ( ( rgb >> 2 ) & 0x07 );
#else
	return ( ( rgb >> 6 ) & 0x0380 ) | ( ( rgb >>  4 ) & 0x0078 ) | ( ( rgb >> 2 ) & 0x07 );
#endif
}


/*
 * Gets a colour index and returns RGB888
 */
rgb888_t get_color_rgb(uint8 idx)
{
	// special_pal has 224 rgb colors
	if(  idx < SPECIAL_COLOR_COUNT  ) {
		return special_pal[idx];
	}

	// if it uses one of the special light colours it's under display_day_lights
	if(  idx < SPECIAL_COLOR_COUNT + LIGHT_COUNT  ) {
		return display_day_lights[idx - SPECIAL_COLOR_COUNT];
	}

	// Return black for anything else
	return rgb888_t{0, 0, 0};
}

/**
 * Convert indexed colors to rgb and back
 */
PIXVAL color_idx_to_rgb(PIXVAL idx)
{
	return specialcolormap_all_day[ idx  & 0x00FF];
}

PIXVAL color_rgb_to_idx(PIXVAL color)
{
	for(  PIXVAL i = 0; i <= 0xff; i++  ) {
		if(  specialcolormap_all_day[i] == color  ) {
			return i;
		}
	}
	return 0;
}


/*
 * Convert env_t colours from RGB888 to the system format
 */
void env_t_rgb_to_system_colors()
{
	// get system colours for the default colours or settings.xml
	env_t::default_window_title_color = get_system_color( env_t::default_window_title_color_rgb );
	env_t::front_window_text_color    = get_system_color( env_t::front_window_text_color_rgb );
	env_t::bottom_window_text_color   = get_system_color( env_t::bottom_window_text_color_rgb );
	env_t::tooltip_color              = get_system_color( env_t::tooltip_color_rgb );
	env_t::tooltip_textcolor          = get_system_color( env_t::tooltip_textcolor_rgb );
	env_t::cursor_overlay_color       = get_system_color( env_t::cursor_overlay_color_rgb );
	env_t::background_color           = get_system_color( env_t::background_color_rgb );
}


/* changes the raster width after loading */
scr_coord_val display_set_base_raster_width(scr_coord_val new_raster)
{
	scr_coord_val old = base_tile_raster_width;
	base_tile_raster_width = new_raster;
	tile_raster_width = ( new_raster *  zoom_num[zoom_factor] ) / zoom_den[zoom_factor];
	return old;
}


// ----------------------------------- clipping routines ------------------------------------------


scr_coord_val display_get_width()
{
	return disp_actual_width;
}


// only use, if you are really really sure!
void display_set_actual_width(scr_coord_val w)
{
	disp_actual_width = w;
}


scr_coord_val display_get_height()
{
	return disp_height;
}


void display_set_height(scr_coord_val const h)
{
	disp_height = h;
}


/**
 * Clips intervall [x,x+w] such that left <= x and x+w <= right.
 * If @p w is negative, it stays negative.
 * @returns difference between old and new value of @p x.
 */
static inline int clip_intv(scr_coord_val &x, scr_coord_val &w, const scr_coord_val left, const scr_coord_val right)
{
	//exit early if it cannot fit
	if(  x + w < left || x > right  ) {
		w = -1;
		return 0;
	}
	scr_coord_val xx = min( x + w, right );
	scr_coord_val xoff = left - x;
	if(  xoff > 0  ) { // equivalent to x < left
		x = left;
	}
	else {
		xoff = 0;
	}
	w = xx - x;
	return xoff;
}

/// wrapper for clip_intv
static inline int clip_wh(scr_coord_val *x, scr_coord_val *w, const scr_coord_val left, const scr_coord_val right)
{
	return clip_intv( *x, *w, left, right );
}


/// wrapper for clip_intv, @returns whether @p w is positive
static inline bool clip_lr(scr_coord_val *x, scr_coord_val *w, const scr_coord_val left, const scr_coord_val right)
{
	clip_intv( *x, *w, left, right );
	return *w > 0;
}


/**
 * Get the clipping rectangle dimensions
 */
clip_dimension display_get_clip_wh(CLIP_NUM_DEF0)
{
	return CR.clip_rect;
}


/**
 * Set the clipping rectangle dimensions
 *
 * here, a pixel at coordinate xp is displayed if
 *  clip. x <= xp < clip.xx
 * the right-most pixel of an image located at xp with width w is displayed if
 *  clip.x < xp+w <= clip.xx
 * analogously for the y coordinate
 */
void display_set_clip_wh(scr_coord_val x, scr_coord_val y, scr_coord_val w, scr_coord_val h  CLIP_NUM_DEF, bool fit)
{
	if(  !fit  ) {
		clip_wh( &x, &w, 0, disp_width );
		clip_wh( &y, &h, 0, disp_height );
	}
	else {
		clip_wh( &x, &w, CR.clip_rect.x, CR.clip_rect.xx );
		clip_wh( &y, &h, CR.clip_rect.y, CR.clip_rect.yy );
	}

	CR.clip_rect.x = x;
	CR.clip_rect.y = y;
	CR.clip_rect.w = w;
	CR.clip_rect.h = h;
	CR.clip_rect.xx = x + w; // watch out, clips to scr_coord_val max
	CR.clip_rect.yy = y + h; // watch out, clips to scr_coord_val max
}

void display_push_clip_wh(scr_coord_val x, scr_coord_val y, scr_coord_val w, scr_coord_val h  CLIP_NUM_DEF)
{
	clip_dimension &rect = CR.clip_rect;
	clip_dimension &swap = CR.clip_rect_swap;
	bool &active = CR.swap_active;
	assert(!active);
	swap = rect;
	display_set_clip_wh(x, y, w, h  CLIP_NUM_PAR);
	active = true;
}

void display_swap_clip_wh(CLIP_NUM_DEF0)
{
	clip_dimension &rect = CR.clip_rect;
	clip_dimension &swap = CR.clip_rect_swap;
	bool &active = CR.swap_active;
	if(  active  ) {
		clip_dimension save = rect;
		rect = swap;
		swap = save;
	}
}

void display_pop_clip_wh(CLIP_NUM_DEF0)
{
	clip_dimension &rect = CR.clip_rect;
	clip_dimension &swap = CR.clip_rect_swap;
	bool &active = CR.swap_active;
	if(  active  ) {
		rect = swap;
		active = false;
	}
}


static void build_stencil_for(int _x0, int _y0, int _x1, int _y1,
                              int min_x, int min_y, int max_x, int max_y)
{
	float x0 = _x0;
	float y0 = _y0;
	float x1 = _x1;
	float y1 = _y1;
	int dir0 = 0;//0 => west, 1 => nord, …
	int dir1 = 0;//0 => ost, 1 => süd, …
	float const extra = 0;
	//find intersections with the bounding box (min_x,min_y),(max_x,max_y)
	if(  _x0 != _x1 && _y0 != _y1  ) {
		if(  x0 < x1  ) {
			float xt0, yt0, xt1, yt1;
			xt0 = min_x;
			yt0 = (float)( min_x - x0 ) / ( x1 - x0 ) * ( y1 - y0 ) + y0;
			xt1 = max_x;
			yt1 = (float)( max_x - x0 ) / ( x1 - x0 ) * ( y1 - y0 ) + y0;
			x0 = xt0;
			y0 = yt0 - extra;
			x1 = xt1;
			y1 = yt1 + extra;
			dir0 = 3;
			dir1 = 1;
		}
		else {
			float xt0, yt0, xt1, yt1;
			xt0 = max_x;
			yt0 = (float)( max_x - x0 ) / ( x1 - x0 ) * ( y1 - y0 ) + y0;
			xt1 = min_x;
			yt1 = (float)( min_x - x0 ) / ( x1 - x0 ) * ( y1 - y0 ) + y0;
			x0 = xt0;
			y0 = yt0 + extra;
			x1 = xt1;
			y1 = yt1 - extra;
			dir0 = 1;
			dir1 = 3;
		}
		if(  y0 < min_y && y1 < min_y  ) {
			y0 = min_y;
			y1 = min_y;
		}
		else if(  y0 > max_y && y1 > max_y  ) {
			y0 = max_y;
			y1 = max_y;
		}
		else if(  y0 < y1  ) {
			float xt0, yt0, xt1, yt1;
			xt0 = (float)( min_y - y0 ) / ( y1 - y0 ) * ( x1 - x0 ) + x0;
			yt0 = min_y;
			xt1 = (float)( max_y - y0 ) / ( y1 - y0 ) * ( x1 - x0 ) + x0;
			yt1 = max_y;
			if(  yt0 > y0 || ( yt0 == y0 && dir0 == 1 )  ) {
				x0 = xt0 + extra;
				y0 = yt0;
				dir0 = 0;
			}
			if(  yt1 < y1 || ( yt1 == y1 && dir0 == 3 )  ) {
				x1 = xt1 - extra;
				y1 = yt1;
				dir1 = 2;
			}
		}
		else {
			float xt0, yt0, xt1, yt1;
			xt0 = (float)( max_y - y0 ) / ( y1 - y0 ) * ( x1 - x0 ) + x0;
			yt0 = max_y;
			xt1 = (float)( min_y - y0 ) / ( y1 - y0 ) * ( x1 - x0 ) + x0;
			yt1 = min_y;
			if(  yt0 < y0 || ( yt0 == y0 && dir0 == 3 )  ) {
				x0 = xt0 - extra;
				y0 = yt0;
				dir0 = 2;
			}
			if(  yt1 > y1 || ( yt1 == y1 && dir0 == 1 )  ) {
				x1 = xt1 + extra;
				y1 = yt1;
				dir1 = 0;
			}
		}
		if(  x0 < min_x && x1 < min_x  ) {
			x0 = min_x;
			x1 = min_x;
		}
		if(  x0 > max_x && x1 > max_x  ) {
			x0 = max_x;
			x1 = max_x;
		}
		/*
		xt = t * ( x1 - x0 ) + x0
		yt = t * ( y1 - y0 ) + y0
		*/
	}
	else if(  _x0 != _x1  ) {
		if(  _x0 < _x1  ) {
			x0 = min_x;
			x1 = max_x;
			dir0 = 3;
			dir1 = 1;
		}
		else {
			x0 = max_x;
			x1 = min_x;
			dir0 = 1;
			dir1 = 3;
		}
		if(  y0 < min_y && y1 < min_y  ) {
			y0 = min_y;
			y1 = min_y;
		}
		if(  y0 > max_y && y1 > max_y  ) {
			y0 = max_y;
			y1 = max_y;
		}
	}
	else if(  _y0 != _y1  ) {
		if(  _y0 < _y1  ) {
			y0 = min_y;
			y1 = max_y;
			dir0 = 0;
			dir1 = 2;
		}
		else {
			y0 = max_y;
			y1 = min_y;
			dir0 = 2;
			dir1 = 0;
		}
		if(  x0 < min_x && x1 < min_x  ) {
			x0 = min_x;
			x1 = min_x;
		}
		if(  x0 > max_x && x1 > max_x  ) {
			x0 = max_x;
			x1 = max_x;
		}
	}
	else {
		return;
	}
	if(  dir0 == dir1  ) {
		return;
	}

	//okay. each point is on one of the boundaries.
	//put out the original edge
	glBegin( GL_POLYGON );
	glVertex2f( x0, y0 );
	glVertex2f( x1, y1 );
	//then follow the edges clockwise. maximum count is 5.
	while(  dir0 != dir1  )
	{
		switch(dir1)
		{
		case 0:
			dir1++;
			glVertex2i( max_x, min_y );
			break;
		case 1:
			dir1++;
			glVertex2i( max_x, max_y );
			break;
		case 2:
			dir1++;
			glVertex2i( min_x, max_y );
			break;
		case 3:
			dir1 = 0;
			glVertex2i( min_x, min_y );
			break;
		}
	}
	glEnd();
}

static void build_stencil(int min_x, int min_y, int max_x, int max_y, clipping_info_t const &cr)
{
	glColorMask( 0, 0, 0, 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glEnable( GL_STENCIL_TEST );
	glStencilFunc( GL_ALWAYS, 0, 1 );
	glStencilOp( GL_KEEP, GL_KEEP, GL_REPLACE );

	glClear( GL_STENCIL_BUFFER_BIT );

	glStencilFunc( GL_ALWAYS, 1, 1 );
	for(  uint8 i = 0; i < cr.number_of_clips; i++  ) {
		if(  cr.clip_ribi[i] & cr.active_ribi  ) {
			cr.poly_clips[i].build_stencil( min_x, min_y, max_x, max_y );
		}
	}
	glDisable( GL_STENCIL_TEST );
	glColorMask( 1, 1, 1, 1 );
}

/*
 * Add clipping line through (x0,y0) and (x1,y1)
 * with associated ribi
 * if ribi & 16 then non-convex clipping.
 */
void add_poly_clip(int x0,int y0, int x1, int y1, int ribi  CLIP_NUM_DEF)
{
	if(  CR.number_of_clips < MAX_POLY_CLIPS  ) {
		CR.poly_clips[CR.number_of_clips].clip_from_to( x0, y0, x1, y1, ribi & 16 );
		CR.clip_ribi[CR.number_of_clips] = ribi & 15;
		CR.number_of_clips++;
	}
}


/*
 * Clears all clipping lines
 */
void clear_all_poly_clip(CLIP_NUM_DEF0)
{
	CR.number_of_clips = 0;
	CR.active_ribi = 15; // set all to active
}


/*
 * Activates clipping lines associated with ribi
 * ie if clip_ribi[i] & active_ribi
 */
void activate_ribi_clip(int ribi  CLIP_NUM_DEF)
{
	CR.active_ribi = ribi;
}



// ------------------------------ dirty tile stuff --------------------------------


/**
 * Mark tile as dirty, with clipping
 */
void mark_rect_dirty_wc(scr_coord_val x1, scr_coord_val y1, scr_coord_val x2, scr_coord_val y2)
{
	(void)x1;
	(void)y1;
	(void)x2;
	(void)y2;
}


void mark_rect_dirty_clip(scr_coord_val x1, scr_coord_val y1, scr_coord_val x2, scr_coord_val y2  CLIP_NUM_DEF)
{
	(void)x1;
	(void)y1;
	(void)x2;
	(void)y2;
	(void)CR;
}


/**
 * Mark the whole screen as dirty.
 *
 */
void mark_screen_dirty()
{
}


/**
 * the area of this image need update
 */
void display_mark_img_dirty(image_id image, scr_coord_val xp, scr_coord_val yp)
{
	(void)image;
	(void)xp;
	(void)yp;
	//TODO can we use this to determine if we need to reupload to texture?
}


// ------------------------- rendering images for display --------------------------------

/*
 * Simutrans caches player colored images, to allow faster drawing of them
 * They are derived from a base image, which may need zooming too
 */

void set_zoom_factor(int z)
{
	// do not zoom beyond 4 pixels
	if(  ( base_tile_raster_width * zoom_num[z] ) / zoom_den[z] > 4  ) {
		zoom_factor = z;
		tile_raster_width = ( base_tile_raster_width * zoom_num[zoom_factor] ) / zoom_den[zoom_factor];
		dbg->message( "set_zoom_factor()", "Zoom level now %d (%i/%i)", zoom_factor, zoom_num[zoom_factor], zoom_den[zoom_factor] );
	}
}


int zoom_factor_up()
{
	// zoom out, if size permits
	if(  zoom_factor > 0  ) {
		set_zoom_factor( zoom_factor - 1 );
		return true;
	}
	return false;
}


int zoom_factor_down()
{
	if(  zoom_factor < MAX_ZOOM_FACTOR  ) {
		set_zoom_factor( zoom_factor + 1 );
		return true;
	}
	return false;
}

static void updateRGBMap(GLuint &tex, PIXVAL *rgbmap, uint64_t code)
{
	if(  rgbmap_cache[code] != 0  ) {
		tex = rgbmap_cache[code];
		return;
	}
	glGenTextures( 1, &tex );

	scr_coord_val w = 256;
	scr_coord_val h = 256;

	glBindTexture( GL_TEXTURE_2D, tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	//now upload the array
	glPixelStorei( GL_UNPACK_ROW_LENGTH, w );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );

	std::vector<PIX32> tmp;
	tmp.resize( w * h );
	memset( tmp.data(), 0, w * h * sizeof(PIX32) );

	/* the reference for these conversions must be
	 * descriptor/writer/image_writer.cc: pixrgb_to_pixval() */
	PIX32 *dst = tmp.data();
	PIXVAL *src = rgbmap;
	/* the rgbmap is converted straight to opaque colors */
	for(  unsigned i = 0; i < RGBMAPSIZE; i++  ) {
		PIXVAL col = *src++;
		dst->R = ( ( ( col & 0xf800 ) * 0x21 ) >> 13 ) & 0xff;
		dst->G = ( ( ( col & 0x07e0 ) * 0x41 ) >> 9 ) & 0xff;
		dst->B = ( ( ( col & 0x001f ) * 0x21 ) >> 2 ) & 0xff;
		dst->A = 0xff;
		dst++;
	}
	/* todo: transparent color handling should be moved to the callers.(still?) */
	/* transparent special colors */
	src = rgbmap + 0x8000;
	dst = tmp.data() + 0x8020;
	for(  unsigned i = 0; i < SPECIAL; i++  ) {
		PIXVAL col = *src++;
		for(  unsigned a = 0; a < 31; a++  ) {
			dst->R = ( ( ( col & 0xf800 ) * 0x21 ) >> 13 ) & 0xff;
			dst->G = ( ( ( col & 0x07e0 ) * 0x41 ) >> 9 ) & 0xff;
			dst->B = ( ( ( col & 0x001f ) * 0x21 ) >> 2 ) & 0xff;
			dst->A = ( a + 1 ) * 255 / 32;
			dst++;
		}
	}
	//these probably are not supported in simgraph16.cc, but image_writer.cc: pixrgb_to_pixval() generates them.
	dst = tmp.data() + 0x8020 + 31 * 31;
	/* regular transparent colors. mapping by replicating bits. */
	for(  unsigned i = 0; i < 0x400; i++  ) {
		//convert from RGB 343 to RGB 555 to index into rgbmap
		int r = ( ( ( i & 0x380 ) * 0x9 ) << 4 ) & 0x7c00;
		int g = ( ( ( i & 0x078 ) * 0x11 ) >> 1 ) & 0x03e0;
		int b = ( ( ( i & 0x007 ) * 0x9 ) >> 1 ) & 0x001f;
		PIXVAL col = rgbmap[r | g | b];
		//convert from RGB 555 to RGB 888, add alpha
		for(  unsigned a = 0; a < 31; a++  ) {
			dst->R = ( ( ( col & 0xf800 ) * 0x21 ) >> 13 ) & 0xff;
			dst->G = ( ( ( col & 0x07e0 ) * 0x41 ) >> 9 ) & 0xff;
			dst->B = ( ( ( col & 0x001f ) * 0x21 ) >> 2 ) & 0xff;
			dst->A = ( a + 1 ) * 255 / 32;
			dst++;
		}
	}

	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE,
	              tmp.data() );

	rgbmap_cache[code] = tex;
}

static void activate_player_color(sint8 player_nr, bool daynight)
{
	// caches the last settings
	//specialcolormap_all_day is constant
	//specialcolormap_day_night depends on light_level, night_shift
	if(  !daynight  ) {
		if(  player_day != player_nr  ) {
			int i;
			player_day = player_nr;
			for(  i = 0; i < 8; i++  ) {
				rgbmap_all_day[0x8000 + i] = specialcolormap_all_day[player_offsets[player_day][0] + i];
				rgbmap_all_day[0x8008 + i] = specialcolormap_all_day[player_offsets[player_day][1] + i];
			}
			updateRGBMap( rgbmap_all_day_tex, rgbmap_all_day,
			              0x8000000000000000ULL |
			              player_offsets[player_day][0] |
			              ( player_offsets[player_day][1] << 8 )
			            );
		}
		rgbmap_current = rgbmap_all_day;
		rgbmap_current_tex = rgbmap_all_day_tex;
	}
	else {
		// changing color table
		if(  player_night != player_nr  ) {
			int i;
			player_night = player_nr;
			for(  i = 0; i < 8; i++  ) {
				rgbmap_day_night[0x8000 + i] = specialcolormap_day_night[player_offsets[player_night][0] + i];
				rgbmap_day_night[0x8008 + i] = specialcolormap_day_night[player_offsets[player_night][1] + i];
			}
			updateRGBMap( rgbmap_day_night_tex, rgbmap_day_night,
			              0x4000000000000000ULL |
			              ( ( light_level & 0xffULL ) << 16 ) |
			              ( ( night_shift & 0xffULL ) << 24 ) |
			              player_offsets[player_night][0] |
			              ( player_offsets[player_night][1] << 8 )
			            );
		}
		rgbmap_current = rgbmap_day_night;
		rgbmap_current_tex = rgbmap_day_night_tex;
	}
}


image_id get_image_count()
{
	return images.size();
}


/**
 * Convert base image data to actual image size
 * Uses averages of all sampled points to get the "real" value
 * Blurs a bit
 */
static void rezoom_img(const image_id n)
{
	// may this image be zoomed
	if(  n >= images.size() || images[n].base_h == 0  ) {
		return;
	}
	if(  images[n].flags & FLAG_ZOOMABLE  ) {
		if(  images[n].zoom_den == zoom_den[zoom_factor] &&
		                images[n].zoom_num == zoom_num[zoom_factor]  ) {
			return;
		}
		images[n].zoom_den = zoom_den[zoom_factor];
		images[n].zoom_num = zoom_num[zoom_factor];

		float q = images[n].zoom_num * 1.f / images[n].zoom_den;
		if(  images[n].zoom_num >= images[n].zoom_den  ) {
			//check the denominator is power of two
			if(  ( images[n].zoom_den & ( images[n].zoom_den - 1 ) ) == 0  ) {
				images[n].zoom = q;
			}
			else {
				images[n].zoom = q * ( 33.f / 32.f );
			}
		}
		else {
			/*
				 *
				 *  0.75    1.0416666666667   0.78125     0.03125
				 *  0.625   1.0375            0.6484375   0.0234375
				 *  0.5     1.0625            0.53125     0.03125
				 *  0.375   1.0625            0.3984375   0.0234375
				 *  0.25    1.125             0.28125     0.03125
				 *  0.125   1.125             0.140625    0.015625
				 *
				 */
			if(  images[n].zoom_num * 4 >= images[n].zoom_den  ) {
				images[n].zoom = q + 1.f / 64.f;
			}
			else {
				images[n].zoom = q * ( 17.f / 16.f );
			}
		}
	}
	else {
		images[n].zoom_den = 1;
		images[n].zoom_num = 1;
		images[n].zoom = 1.0f;
	}
}

static float get_img_zoom(image_id n) {
	if(  n == IMG_EMPTY  ) {
		return 1.0;
	}
	if(  n >= images.size()  ) {
		return 1.0;
	}
	return images[n].zoom;
}


// get next smallest size when scaling to percent
scr_size display_get_best_matching_size(const image_id n, sint16 zoom_percent)
{
	if (n < images.size()  &&  images[n].base_h > 0) {
		int new_w = (images[n].base_w * zoom_percent + 1) / 100;
		for (int i = 0; i <= MAX_ZOOM_FACTOR; i++) {
			int zoom_w = (images[n].base_w * zoom_num[i]) / zoom_den[i];
			int zoom_h = (images[n].base_h * zoom_num[i]) / zoom_den[i];
			if (zoom_w <= new_w) {
				// first size smaller or equal to requested
				return scr_size(zoom_w, zoom_h);
			}
		}
	}
	return scr_size(32, 32); // default size
}



// force a certain size on a image (for rescaling tool images)
void display_fit_img_to_width(const image_id n, sint16 new_w)
{
	float zoom = get_img_zoom( n );
	if(  n < images.size() && images[n].base_h > 0 &&
	                ceil( images[n].base_w * zoom ) != new_w  ) {
		int old_zoom_factor = zoom_factor;
		for(  int i = 0; i <= MAX_ZOOM_FACTOR; i++  ) {
			int zoom_w = ( images[n].base_w * zoom_num[i] ) / zoom_den[i];
			if(  zoom_w <= new_w  ) {
				uint8 old_zoom_flag = images[n].flags & FLAG_ZOOMABLE;
				images[n].flags |= FLAG_ZOOMABLE;
				zoom_factor = i;
				rezoom_img( n );
				images[n].flags &= ~FLAG_ZOOMABLE;
				images[n].flags |= old_zoom_flag;
				zoom_factor = old_zoom_factor;
				return;
			}
		}
	}
}


static void calc_base_pal_from_night_shift(const int night)
{
	const int night2 = min( night, 4 );
	const int day = 4 - night2;
	unsigned int i;

	// constant multiplier 0,66 - dark night  255 will drop to 49, 55 to 10
	//                     0,7  - dark, but all is visible     61        13
	//                     0,73                                72        15
	//                     0,75 - quite bright                 80        17
	//                     0,8    bright                      104        22

	const double RG_night_multiplier = pow( 0.75, night ) * ( ( light_level + 8.0 ) / 8.0 );
	const double B_night_multiplier  = pow( 0.83, night ) * ( ( light_level + 8.0 ) / 8.0 );

	for(  i = 0; i < 0x8000; i++  ) {
		// (1<<15) this is total no of all possible colors in RGB555)
		// RGB 555 input
		int R = ( i & 0x7C00 ) >> 7;
		int G = ( i & 0x03E0 ) >> 2;
		int B = ( i & 0x001F ) << 3;
		// lines generate all possible colors in 555RGB code - input
		// however the result is in 888RGB - 8bit per channel

		R = (int)( R * RG_night_multiplier );
		G = (int)( G * RG_night_multiplier );
		B = (int)( B * B_night_multiplier );

		rgbmap_day_night[i] = get_system_color( { (uint8)R, (uint8)G, (uint8)B } );
	}

	// again the same but for transparent colors
	for(  i = 0; i < 0x0400; i++  ) {
		// RGB 343 input
		int R = ( i & 0x0380 ) >> 2;
		int G = ( i & 0x0078 ) << 1;
		int B = ( i & 0x0007 ) << 5;

		// lines generate all possible colors in 343RGB code - input
		// however the result is in 888RGB - 8bit per channel
		R = (int)( R * RG_night_multiplier );
		G = (int)( G * RG_night_multiplier );
		B = (int)( B *  B_night_multiplier );

		PIXVAL color = get_system_color( { (uint8)R, (uint8)G, (uint8)B } );
		rgbmap_day_night[0x8000 + MAX_PLAYER_COUNT + LIGHT_COUNT + i] = color;
	}

	// player color map (and used for map display etc.)
	for(  i = 0; i < SPECIAL_COLOR_COUNT; i++  ) {
		const int R = (int)( special_pal[i].r * RG_night_multiplier );
		const int G = (int)( special_pal[i].g * RG_night_multiplier );
		const int B = (int)( special_pal[i].b *  B_night_multiplier );

		specialcolormap_day_night[i] = get_system_color( { (uint8)R, (uint8)G, (uint8)B } );
	}

	// special light colors (actually, only non-darkening greys should be used)
	for(  i = 0; i < LIGHT_COUNT; i++  ) {
		specialcolormap_day_night[SPECIAL_COLOR_COUNT + i] = get_system_color( display_day_lights[i] );
	}

	// init with black for forbidden colors
	for(  i = SPECIAL_COLOR_COUNT + LIGHT_COUNT; i < 256; i++  ) {
		specialcolormap_day_night[i] = 0;
	}

	// default player colors
	for(  i = 0; i < 8; i++  ) {
		rgbmap_day_night[0x8000 + i] = specialcolormap_day_night[player_offsets[0][0] + i];
		rgbmap_day_night[0x8008 + i] = specialcolormap_day_night[player_offsets[0][1] + i];
	}
	player_night = 0;

	// Lights
	for(  i = 0; i < LIGHT_COUNT; i++  ) {
		const int day_R = display_day_lights[i].r;
		const int day_G = display_day_lights[i].g;
		const int day_B = display_day_lights[i].b;

		const int night_R = display_night_lights[i].r;
		const int night_G = display_night_lights[i].g;
		const int night_B = display_night_lights[i].b;

		const int R = ( day_R * day + night_R * night2 ) >> 2;
		const int G = ( day_G * day + night_G * night2 ) >> 2;
		const int B = ( day_B * day + night_B * night2 ) >> 2;

		PIXVAL color = get_system_color( { (uint8)max( R, 0 ), (uint8)max( G, 0 ), (uint8)max( B, 0 ) } );
		rgbmap_day_night[0x8000 + MAX_PLAYER_COUNT + i] = color;
	}
	updateRGBMap( rgbmap_day_night_tex,  rgbmap_day_night,
	              0x4000000000000000ULL |
	              ( ( light_level & 0xffULL ) << 16 ) |
	              ( ( night_shift & 0xffULL ) << 24 ) |
	              player_offsets[0][0] |
	              ( player_offsets[0][1] << 8 )
	            );
}


void display_day_night_shift(int night)
{
	if(  night != night_shift  ) {
		night_shift = night;
		calc_base_pal_from_night_shift( night );
		mark_screen_dirty();
	}
}


// set first and second company color for player
void display_set_player_color_scheme(const int player, const uint8 col1, const uint8 col2 )
{
	if(  player_offsets[player][0] != col1 || player_offsets[player][1] != col2  ) {
		// set new player colors
		player_offsets[player][0] = col1;
		player_offsets[player][1] = col2;
		if(  player == player_day || player == player_night  ) {
			// and recalculate map (and save it)
			calc_base_pal_from_night_shift( 0 );
			memcpy( rgbmap_all_day, rgbmap_day_night, RGBMAPSIZE * sizeof(PIXVAL) );
			if(  night_shift != 0  ) {
				calc_base_pal_from_night_shift( night_shift );
			}
			// calc_base_pal_from_night_shift resets player_night to 0
			player_day = player_night;
			updateRGBMap( rgbmap_all_day_tex, rgbmap_all_day,
			              0x8000000000000000ULL |
			              player_offsets[player][0] |
			              ( player_offsets[player][1] << 8 )
			            );
		}
		mark_screen_dirty();
	}
}


static GLuint getIndexImgTex(struct imd &image,
                             const PIXVAL *sp)
{
	scr_coord_val w = image.base_w;
	scr_coord_val h = image.base_h;

	if(  image.index_tex != 0  ) {
		return image.index_tex;
	}
	if(  h <= 0 || w <= 0  ) {
		return 0;
	}

	GLuint ret;
	glGenTextures( 1, &ret );
	glBindTexture( GL_TEXTURE_2D, ret );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	//now upload the array
	glPixelStorei( GL_UNPACK_ROW_LENGTH, w );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );

	std::vector<PIX32> tmp;
	tmp.resize( w * h );
	memset( tmp.data(), 0, w * h * sizeof(PIX32) );
	PIX32 *p = tmp.data();
	scr_coord_val y;
	for(  y = 0; y < h; y++  ) {
		scr_coord_val xpos = 0;
		uint16 runlen = *sp++;
		do {
			// we start with a clear run
			runlen &= ~TRANSPARENT_RUN;
			xpos += runlen;

			// now get colored pixels
			runlen = *sp++;
			runlen &= ~TRANSPARENT_RUN;

			scr_coord_val x;
			for(  x = 0; x < runlen; x++  ) {
				/* directly using the indexed colors.
				 could extract the transparency information
				 here and map the the correct colors or do
				 it in or around updateRGBMap() */
				PIXVAL col = sp[x];
				p[x + xpos].R = col & 0xff;
				p[x + xpos].G = col >> 8;
				p[x + xpos].B = 0;
				p[x + xpos].A = 0xff;
			}
			sp += runlen;
			xpos += runlen;
		} while(  ( runlen = *sp++ )  );
		p += w;
	}

	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE,
	              tmp.data() );

	image.index_tex = ret;

	return ret;
}

static GLuint getBaseImgTex(struct imd &image,
                            const PIXVAL *sp)
{
	scr_coord_val w = image.base_w;
	scr_coord_val h = image.base_h;
	if(  image.base_tex != 0  ) {
		return image.base_tex;
	}
	if(  h <= 0 || w <= 0  ) {
		return 0;
	}

	GLuint ret;
	glGenTextures( 1, &ret );
	glBindTexture( GL_TEXTURE_2D, ret );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	//now upload the array
	glPixelStorei( GL_UNPACK_ROW_LENGTH, w );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );

	std::vector<PIX32> tmp;
	tmp.resize( w * h );
	memset( tmp.data(), 0, w * h * sizeof(PIX32) );
	PIX32 *p = tmp.data();
	scr_coord_val y;
	for(  y = 0; y < h; y++  ) {
		scr_coord_val xpos = 0;
		uint16 runlen = *sp++;
		do {
			// we start with a clear run
			runlen &= ~TRANSPARENT_RUN;
			xpos += runlen;

			// now get colored pixels
			runlen = *sp++;

			runlen &= ~TRANSPARENT_RUN;

			scr_coord_val x;
			for(  x = 0; x < runlen; x++  ) {
				/* RGB565 to ARGB8888. no transparency encoded. */
				PIXVAL col = sp[x];
				p[x + xpos].R = ( ( ( col & 0xf800 ) * 0x21 ) >> 13 )
				                & 0xff;
				p[x + xpos].G = ( ( ( col & 0x07e0 ) * 0x41 ) >> 9 )
				                & 0xff;
				p[x + xpos].B = ( ( ( col & 0x001f ) * 0x21 ) >> 2 )
				                & 0xff;
				p[x + xpos].A = 0xff;
			}
			sp += runlen;
			xpos += runlen;
		} while(  ( runlen = *sp++ )  );
		p += w;
	}

	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE,
	              tmp.data() );

	image.base_tex = ret;

	return ret;
}


void register_image(image_t *image_in)
{
	struct imd *image;

	/* valid image? */
	if(  image_in->len == 0 || image_in->h == 0  ) {
		dbg->warning( "register_image", "Warning: ignoring image %lu because of missing data", images.size() );
		image_in->imageid = IMG_EMPTY;
		return;
	}

	image_in->imageid = images.size();
	images.resize( images.size() + 1 );
	image = &images.back();

	image->base_x = image_in->x;
	image->base_w = image_in->w;
	image->base_y = image_in->y;
	image->base_h = image_in->h;

	// since we do not recode them, we can work with the original data
	image->base_data = image_in->data;

	image->base_tex = 0;
	image->index_tex = 0;

	image->zoom_num = 1;
	image->zoom_den = 1;
	image->zoom = 1.0f;
	image->flags = 0;
	if(  image_in->zoomable  ) {
		image->flags |= FLAG_ZOOMABLE;
	}
}


// delete all images above a certain number ...
// (mostly needed when changing climate zones)
void display_free_all_images_above(image_id above)
{
	images.resize( above );
}


// query offsets
void display_get_image_offset(image_id image, scr_coord_val *xoff, scr_coord_val *yoff, scr_coord_val *xw, scr_coord_val *yw)
{
	if(  image < images.size()  ) {
		float zoom = get_img_zoom( image );
		*xoff = floor( images[image].base_x * zoom );
		*yoff = floor( images[image].base_y * zoom );
		*xw   = ceil( images[image].base_w * zoom );
		*yw   = ceil( images[image].base_h * zoom );
	}
}


// query un-zoomed offsets
void display_get_base_image_offset(image_id image, scr_coord_val *xoff, scr_coord_val *yoff, scr_coord_val *xw, scr_coord_val *yw)
{
	if(  image < images.size()  ) {
		*xoff = images[image].base_x;
		*yoff = images[image].base_y;
		*xw   = images[image].base_w;
		*yw   = images[image].base_h;
	}
}

// ------------------ display all kind of images from here on ------------------------------

// forward declaration, implementation is further below
PIXVAL colors_blend_alpha32(PIXVAL background, PIXVAL foreground, int alpha);

static uint64_t tex_hash(const void *ptr, size_t size)
{
	//take max max_samples samples
	const char *p = (const char*)ptr;
	unsigned int smps = size / 8;
	unsigned int off = 8;
	if(  smps > 5000  ) {
		off = ( size + 4999 ) / 5000;
		if(  off & 7  ) {
			off &= ~7;
			off += 8;
		}
		smps = size / off;
	}
	uint64_t h = uint64_t( ptr );
	while(  smps > 0  ) {
		h ^= h >> 3;
		h ^= h << 61;
		h ^= *(const uint64_t*)p;
		p += off;
		smps--;
	}
	return h;
}


//these do change relatively often and keep their data around, so we can
//key our internal data off their data pointer
static GLuint getArrayTex(const PIXVAL *arr, scr_coord_val w, scr_coord_val h)
{
	if(  w * h == 0  ) {
		return 0;
	}
	size_t byte_size = w * h * sizeof(PIXVAL);
	auto it = arrayInfo.find( (void const *)arr );
	if(  it != arrayInfo.end()  ) {
		//check if it has been changed, but only if it is not
		//changing often(then we avoid the hash function overhead
		//and go straight to reuploading)
		if(  it->second.use_ctr > 100 &&
		                it->second.use_ctr / 3 < it->second.change_ctr  ) {
			uint64_t hash = tex_hash( arr, byte_size );
			if(  hash == it->second.hash  ) {
				it->second.use_ctr++;
				return it->second.tex;
			}
			else {
				it->second.hash = hash;
			}
			it->second.change_ctr++;
		}
	}
	else {
		it = arrayInfo.insert( std::make_pair
		                       ( (void const *)arr, ArrayInfo() ) ).first;
		GLuint texname;
		glGenTextures( 1, &texname );
		it->second.tex = texname;
		it->second.hash = tex_hash( arr, byte_size );
		it->second.use_ctr = 1;
		it->second.change_ctr = 1;
	}

	GLuint texname = arrayInfo[(const void *)arr].tex;
	glBindTexture( GL_TEXTURE_2D, texname );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	//now upload the array
	glPixelStorei( GL_UNPACK_ROW_LENGTH, w );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 2 );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );

	//this already uses raw RGB 565 as defined by get_system_color
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB, w, h, 0,
	              GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
	              arr );

	return texname;
}


static GLuint getGlyphTex(uint32_t c, const font_t *fnt,
                          GLfloat &tcx, GLfloat &tcy,
                          GLfloat &tcw, GLfloat &tch)
{
	auto it = chartex.find( c );
	if(  it != chartex.end()  ) {
		tcx = it->second.x;
		tcy = it->second.y;
		tcw = it->second.w;
		tch = it->second.h;
		return it->second.texture;
	}

	const font_t::glyph_t &glyph = fnt->get_glyph( c );
	sint16 glyph_width = glyph.width;
	sint16 glyph_height = glyph.height;
	//we ignore the y_offset.

	//check if we have space in the current charpage
	if(  charpage.empty()  ||
	                !( /* does it fit at the current position? */
	                                ( charpage.back().curx + glyph_width <= charpage.back().width  &&
	                                  charpage.back().cury + glyph_height <= charpage.back().height )  ||
	                                /* does it fit at the first positon of the next line? */
	                                ( charpage.back().cury + charpage.back().lineheight +
	                                  unsigned( glyph_height ) <= charpage.back().height  &&
	                                  unsigned( glyph_width ) <= charpage.back().width ) )  ) {
		GLuint texname;
		glGenTextures( 1, &texname );
		glBindTexture( GL_TEXTURE_2D, texname );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
		                 GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
		                 GL_CLAMP_TO_EDGE );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_ALPHA, 256, 256, 0,
		              GL_ALPHA, GL_UNSIGNED_BYTE,
		              NULL );
		CharPageInfo pi = {texname,
		                   256, 256,
		                   0, 0, 0
		                  };
		charpage.emplace_back( pi );
	}
	else {
		glBindTexture( GL_TEXTURE_2D, charpage.back().texture );
	}

	/* does it fit on the current line? */
	if(  !( charpage.back().curx + glyph_width <= charpage.back().width  &&
	                charpage.back().cury + glyph_height <= charpage.back().height )  ) {
		/* move to next line */
		charpage.back().curx = 0;
		charpage.back().cury += charpage.back().lineheight;
		charpage.back().lineheight = glyph_height;
	}

	//now upload the array
	glPixelStorei( GL_UNPACK_ROW_LENGTH, glyph_width );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
	glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );

	const uint8 *d = glyph.bitmap;
	uint8_t tmp[glyph_width * glyph_height];
	uint8_t *p = tmp;
	unsigned int i;
	for(  i = 0;  i < unsigned(glyph_height * glyph_width);  i++  ) {
		int alpha = *d++;
		if(  alpha > 31  ) {
			alpha = 0xff;
		}
		else {
			alpha = ( alpha * 0x21 ) / 4;
		}
		*p++ = alpha;
	}

	glTexSubImage2D( GL_TEXTURE_2D, 0,
	                 charpage.back().curx, charpage.back().cury,
	                 glyph_width, glyph_height, GL_ALPHA, GL_UNSIGNED_BYTE,
	                 tmp );

	CharInfo ci = { charpage.back().texture,
	                charpage.back().curx / float( charpage.back().width ),
	                charpage.back().cury / float( charpage.back().height ),
	                glyph_width / float( charpage.back().width ),
	                glyph_height / float( charpage.back().height )
	              };
	chartex[c] = ci;

	if(  charpage.back().lineheight < unsigned( glyph_height )  ) {
		charpage.back().lineheight = glyph_height;
	}
	charpage.back().curx += glyph_width;
	if(  charpage.back().curx >= charpage.back().width  ) {
		charpage.back().curx = 0;
		charpage.back().cury += charpage.back().lineheight;
		charpage.back().lineheight = 0;
	}

	tcx = ci.x;
	tcy = ci.y;
	tcw = ci.w;
	tch = ci.h;
	return ci.texture;
}


static void display_img_pc(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h,
                           GLuint tex, GLuint rgbmap_tex  CLIP_NUM_DEF)
{
	scr_coord_val rw = w;
	scr_coord_val rh = h;

	const scr_coord_val xoff = clip_wh( &xp, &w, CR.clip_rect.x, CR.clip_rect.xx );
	const scr_coord_val yoff = clip_wh( &yp, &h, CR.clip_rect.y, CR.clip_rect.yy );

	if(  w > 0 && h > 0  ) {
		if(  CR.number_of_clips > 0  )
		{
			build_stencil( xp, yp, xp + w, yp + h, CR );
			glEnable( GL_STENCIL_TEST );
			glStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
			glStencilFunc( GL_NOTEQUAL, 1, 1 );
		}

		glActiveTextureARB( GL_TEXTURE0_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, tex );
		glActiveTextureARB( GL_TEXTURE1_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, rgbmap_tex );

		glUseProgram( combined_program );

		glUniform1i( combined_texColor_Location, 0 );
		glUniform1i( combined_texRGBMap_Location, 1 );
		glUniform1i( combined_texAlpha_Location, 2 );

		glUniform4f( combined_alphaMask_Location,
		             0.0, 0.0, 0.0, 1.0 );
		glColor4f( 0, 0, 0, 0.0 );

		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glBegin( GL_QUADS );
		glTexCoord2f( xoff   / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp,     yp );
		glTexCoord2f( ( xoff + w ) / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp + w,   yp );
		glTexCoord2f( ( xoff + w ) / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp + w,   yp + h );
		glTexCoord2f( xoff   / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp,     yp + h );
		glEnd();

		glUseProgram( 0 );

		glActiveTextureARB( GL_TEXTURE1_ARB );
		glDisable( GL_TEXTURE_2D );
		glActiveTextureARB( GL_TEXTURE0_ARB );

		if(  CR.number_of_clips > 0  ) {
			glDisable( GL_STENCIL_TEST );
			glStencilFunc( GL_ALWAYS, 1, 1 );
		}
	}
}


// only used for GUI
void display_img_aligned(const image_id n, scr_rect area, int align, const bool dirty)
{
	if(  n < images.size()  ) {
		scr_coord_val x, y;

		float zoom = get_img_zoom( n );
		// either the image is not touced or moved to middle or right of the rect
		x = area.x;
		if(  ( align & ALIGN_RIGHT ) == ALIGN_CENTER_H  ) {
			x -= floor( images[n].base_x * zoom );
			x += ( area.w - ceil( images[n].base_w * zoom ) ) / 2;
		}
		else if(  ( align & ALIGN_RIGHT ) == ALIGN_RIGHT  ) {
			x = area.get_right() - ceil( ( images[n].base_x + images[n].base_w ) * zoom );
		}

		// either the image is not touced or moved to middle or bottom of the rect
		y = area.y;
		if(  ( align & ALIGN_BOTTOM ) == ALIGN_CENTER_V  ) {
			y -= floor( images[n].base_y * zoom );
			y += ( area.h - ceil( images[n].base_h * zoom ) ) / 2;
		}
		else if(  ( align & ALIGN_BOTTOM ) == ALIGN_BOTTOM  ) {
			y = area.get_bottom() - ceil( ( images[n].base_y + images[n].base_h ) * zoom );
		}

		display_color_img( n, x, y, 0, false, dirty  CLIP_NUM_DEFAULT );
	}
}


/**
 * Draw image with vertical clipping (quickly) and horizontal (slowly)
 */
void display_img_aux(const image_id n, scr_coord_val xp, scr_coord_val yp, const sint8 player_nr_raw, const bool /*daynight*/, const bool /*dirty*/  CLIP_NUM_DEF)
{
	if(  n < images.size()  ) {
		// only use player images if needed
		const sint8 use_player = player_nr_raw;
		// need to go to nightmode and or re-zoomed?
		PIXVAL *sp;
		GLuint tex;

		rezoom_img( n );
		sp = images[n].base_data;
		tex = getIndexImgTex( images[n], sp );

		activate_player_color( use_player, true );
		// now, since zooming may have change this image
		float zoom = get_img_zoom( n );

		yp += floor( images[n].base_y * zoom );
		xp += floor( images[n].base_x * zoom );

		display_img_pc( xp, yp,
		                ceil( images[n].base_w * zoom ),
		                ceil( images[n].base_h * zoom ),
		                tex, rgbmap_day_night_tex  CLIP_NUM_PAR );
	}
}


// local helper function for tiles buttons
static void display_three_image_row(image_id i1, image_id i2, image_id i3, scr_rect row, FLAGGED_PIXVAL)
{
	if(  i1 != IMG_EMPTY  ) {
		float zoom1 = get_img_zoom( i1 );
		scr_coord_val w = ceil( images[i1].base_w * zoom1 );
		display_color_img( i1, row.x, row.y, 0, false, true  CLIP_NUM_DEFAULT );
		row.x += w;
		row.w -= w;
	}
	// right
	if(  i3 != IMG_EMPTY  ) {
		float zoom3 = get_img_zoom( i3 );
		scr_coord_val w = ceil( images[i3].base_w * zoom3 );
		display_color_img( i3, row.get_right() - w, row.y, 0, false, true  CLIP_NUM_DEFAULT );
		row.w -= w;
	}
	// middle
	if(  i2 != IMG_EMPTY  ) {
		float zoom2 = get_img_zoom( i2 );
		scr_coord_val w = ceil( images[i2].base_w * zoom2 );
		// tile it wide
		while(  w <= row.w  ) {
			display_color_img( i2, row.x, row.y, 0, false, true  CLIP_NUM_DEFAULT );
			row.x += w;
			row.w -= w;
		}
		// for the rest we have to clip the rectangle
		if(  row.w > 0  ) {
			clip_dimension const cl = display_get_clip_wh();
			display_set_clip_wh( cl.x, cl.y, max( 0, min( row.get_right(), cl.xx ) - cl.x ), cl.h );
			display_color_img( i2, row.x, row.y, 0, false, true  CLIP_NUM_DEFAULT );
			display_set_clip_wh( cl.x, cl.y, cl.w, cl.h );
		}
	}
}

static scr_coord_val get_img_width(image_id img)
{
	if(  img == IMG_EMPTY  ) {
		return 0;
	}
	float zoom = get_img_zoom( img );
	return ceil( images[img].base_w * zoom );
}
static scr_coord_val get_img_height(image_id img)
{
	if(  img == IMG_EMPTY  ) {
		return 0;
	}
	float zoom = get_img_zoom( img );
	return ceil( images[img].base_h * zoom );
}

typedef void (*DISP_THREE_ROW_FUNC)(image_id, image_id, image_id, scr_rect, FLAGGED_PIXVAL);

/**
 * Base method to display a 3x3 array of images to fit the scr_rect.
 * Special cases:
 * - if images[*][1] are empty, display images[*][0] vertically aligned
 * - if images[1][*] are empty, display images[0][*] horizontally aligned
 */
static void display_img_stretch_intern(const stretch_map_t &imag, scr_rect area, DISP_THREE_ROW_FUNC display_three_image_rowf, FLAGGED_PIXVAL color)
{
	scr_coord_val h_top    = max( max( get_img_height( imag[0][0] ), get_img_height( imag[1][0] ) ), get_img_height( imag[2][0] ) );
	scr_coord_val h_middle = max( max( get_img_height( imag[0][1] ), get_img_height( imag[1][1] ) ), get_img_height( imag[2][1] ) );
	scr_coord_val h_bottom = max( max( get_img_height( imag[0][2] ), get_img_height( imag[1][2] ) ), get_img_height( imag[2][2] ) );

	// center vertically if images[*][1] are empty, display images[*][0]
	if(  imag[0][1] == IMG_EMPTY && imag[1][1] == IMG_EMPTY && imag[2][1] == IMG_EMPTY  ) {
		scr_coord_val h = max( h_top, get_img_height( imag[1][1] ) );
		// center vertically
		area.y += ( area.h - h ) / 2;
	}

	// center horizontally if images[1][*] are empty, display images[0][*]
	if(  imag[1][0] == IMG_EMPTY && imag[1][1] == IMG_EMPTY && imag[1][2] == IMG_EMPTY  ) {
		scr_coord_val w_left = max( max( get_img_width( imag[0][0] ), get_img_width( imag[0][1] ) ), get_img_width( imag[0][2] ) );
		// center vertically
		area.x += ( area.w - w_left ) / 2;
	}

	// top row
	display_three_image_rowf( imag[0][0], imag[1][0], imag[2][0], area, color );

	// bottom row
	if(  h_bottom > 0  ) {
		scr_rect row( area.x, area.y + area.h - h_bottom, area.w, h_bottom );
		display_three_image_rowf( imag[0][2], imag[1][2], imag[2][2], row, color );
	}

	// now stretch the middle
	if(  h_middle > 0  ) {
		scr_rect row( area.x, area.y + h_top, area.w, area.h - h_top - h_bottom );
		// tile it wide
		while(  h_middle <= row.h  ) {
			display_three_image_rowf( imag[0][1], imag[1][1], imag[2][1], row, color );
			row.y += h_middle;
			row.h -= h_middle;
		}
		// for the rest we have to clip the rectangle
		if(  row.h > 0  ) {
			clip_dimension const cl = display_get_clip_wh();
			display_set_clip_wh( cl.x, cl.y, cl.w, max( 0, min( row.get_bottom(), cl.yy ) - cl.y ) );
			display_three_image_rowf( imag[0][1], imag[1][1], imag[2][1], row, color );
			display_set_clip_wh( cl.x, cl.y, cl.w, cl.h );
		}
	}
}

void display_img_stretch(const stretch_map_t &imag, scr_rect area)
{
	display_img_stretch_intern( imag, area, display_three_image_row, 0 );
}

static void display_three_blend_row(image_id i1, image_id i2, image_id i3, scr_rect row, FLAGGED_PIXVAL color)
{
	if(  i1 != IMG_EMPTY  ) {
		float zoom1 = get_img_zoom( i1 );
		scr_coord_val w = ceil( images[i1].base_w * zoom1 );
		display_rezoomed_img_blend( i1, row.x, row.y, 0, color, false, true CLIPNUM_IGNORE );
		row.x += w;
		row.w -= w;
	}
	// right
	if(  i3 != IMG_EMPTY  ) {
		float zoom3 = get_img_zoom( i3 );
		scr_coord_val w = ceil( images[i3].base_w * zoom3 );
		display_rezoomed_img_blend( i3, row.get_right() - w, row.y, 0, color, false, true CLIPNUM_IGNORE );
		row.w -= w;
	}
	// middle
	if(  i2 != IMG_EMPTY  ) {
		float zoom2 = get_img_zoom( i2 );
		scr_coord_val w = ceil( images[i2].base_w * zoom2 );
		// tile it wide
		while(  w <= row.w  ) {
			display_rezoomed_img_blend( i2, row.x, row.y, 0, color, false, true CLIPNUM_IGNORE );
			row.x += w;
			row.w -= w;
		}
		// for the rest we have to clip the rectangle
		if(  row.w > 0  ) {
			clip_dimension const cl = display_get_clip_wh();
			display_set_clip_wh( cl.x, cl.y, max( 0, min( row.get_right(), cl.xx ) - cl.x ), cl.h );
			display_rezoomed_img_blend( i2, row.x, row.y, 0, color, false, true CLIPNUM_IGNORE );
			display_set_clip_wh( cl.x, cl.y, cl.w, cl.h );
		}
	}
}


// this displays a 3x3 array of images to fit the scr_rect like above, but blend the color
void display_img_stretch_blend(const stretch_map_t &imag, scr_rect area, FLAGGED_PIXVAL color)
{
	display_img_stretch_intern( imag, area, display_three_blend_row, color );
}


/**
 * Draw Image, replaced player color
 */
void display_color_img(const image_id n, scr_coord_val xp, scr_coord_val yp, sint8 player_nr_raw, const bool daynight, const bool dirty  CLIP_NUM_DEF)
{
	if(  n < images.size()  ) {
		// do we have to use a player nr?
		const sint8 player_nr = player_nr_raw;

		// first: size check
		rezoom_img( n );

		if(  daynight || night_shift == 0  ) {
			display_img_aux( n, xp, yp, player_nr, true, dirty  CLIP_NUM_PAR );
			return;
		}
		else {
			// do player colour substitution but not daynight - can't use cached images. Do NOT call multithreaded.
			// now test if visible and clipping needed
			const scr_coord_val x = images[n].base_x + xp;
			const scr_coord_val y = images[n].base_y + yp;
			const scr_coord_val w = images[n].base_w;
			const scr_coord_val h = images[n].base_h;
			if(  h <= 0 || x >= CR.clip_rect.xx || y >= CR.clip_rect.yy || x + w <= CR.clip_rect.x || y + h <= CR.clip_rect.y  ) {
				// not visible => we are done
				// happens quite often ...
				return;
			}

			// colors for 2nd company color
			if(  player_nr >= 0  ) {
				activate_player_color( player_nr, daynight );
			}
			else {
				// no player
				activate_player_color( 0, daynight );
			}
			// color replacement needs the original data => sp points to non-cached data
			const PIXVAL *sp = images[n].base_data;

			GLuint tex = getIndexImgTex( images[n], sp );
			display_img_pc( x, y,
			                images[n].base_w, images[n].base_h,
			                tex, rgbmap_current_tex  CLIP_NUM_PAR );
		}
	} // number ok
}


/**
 * draw unscaled images, replaces base color
 */
void display_base_img(const image_id n, scr_coord_val xp, scr_coord_val yp, const sint8 player_nr, const bool daynight, const bool dirty  CLIP_NUM_DEF)
{
	if(  base_tile_raster_width == tile_raster_width  ) {
		// same size => use standard routine
		display_color_img( n, xp, yp, player_nr, daynight, dirty  CLIP_NUM_PAR );
	}
	else if(  n < images.size()  ) {
		// now test if visible and clipping needed
		const scr_coord_val x = images[n].base_x + xp;
		const scr_coord_val y = images[n].base_y + yp;
		const scr_coord_val w = images[n].base_w;
		const scr_coord_val h = images[n].base_h;

		if(  h <= 0 || x >= CR.clip_rect.xx || y >= CR.clip_rect.yy || x + w <= CR.clip_rect.x || y + h <= CR.clip_rect.y  ) {
			// not visible => we are done
			// happens quite often ...
			return;
		}

		// colors for 2nd company color
		if(  player_nr >= 0  ) {
			activate_player_color( player_nr, daynight );
		}
		else {
			// no player
			activate_player_color( 0, daynight );
		}

		// color replacement needs the original data => sp points to non-cached data
		const PIXVAL *sp = images[n].base_data;

		GLuint tex = getIndexImgTex( images[n], sp );
		display_img_pc( x, y,
		                images[n].base_w, images[n].base_h,
		                tex, rgbmap_current_tex  CLIP_NUM_PAR );
	} // number ok
}

inline PIXVAL colors_blend25(PIXVAL background, PIXVAL foreground) { return rgb_shr1( background ) + rgb_shr2( background ) + rgb_shr2( foreground ); }
inline PIXVAL colors_blend50(PIXVAL background, PIXVAL foreground) { return rgb_shr1( background ) + rgb_shr1( foreground ); }
inline PIXVAL colors_blend75(PIXVAL background, PIXVAL foreground) { return rgb_shr2( background ) + rgb_shr1( foreground ) + rgb_shr2( foreground ); }
inline PIXVAL colors_blend_alpha32(PIXVAL background, PIXVAL foreground, int alpha)
{
	uint32 b = ( ( background << 16 ) | background ) & MASK_32;
	uint32 f = ( ( foreground << 16 ) | foreground ) & MASK_32;
	uint32 r = ( ( f * alpha + ( 32 - alpha ) * b ) >> 5 ) & MASK_32;
	return r | ( r >> 16 );
}

// Blends two colors. Possible values for alpha: 0..32
PIXVAL display_blend_colors_alpha32(PIXVAL background, PIXVAL foreground, int alpha)
{
	// alpha takes values 0 .. 32
	switch( alpha ) {
		case 0: // nothing to do ...
			return background;
		case 8:
			return colors_blend25( background, foreground );
		case 16:
			return colors_blend50( background, foreground );
		case 24:
			return colors_blend75( background, foreground );
		case 32:
			return foreground;
		default:
			return colors_blend_alpha32( background, foreground, alpha );
	}
}


// Blends two colors
PIXVAL display_blend_colors(PIXVAL background, PIXVAL foreground, int percent_blend)
{
	return display_blend_colors_alpha32( background, foreground, ( percent_blend * 32 ) / 100 );
}

/**
 * Blends a rectangular region with a color
 */
void display_blend_wh_rgb(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, PIXVAL colval, int percent_blend)
{
	scr_coord_val rw = w;
	scr_coord_val rh = h;
	const scr_coord_val xoff = clip_wh( &xp, &w, CR0.clip_rect.x, CR0.clip_rect.xx );
	const scr_coord_val yoff = clip_wh( &yp, &h, CR0.clip_rect.y, CR0.clip_rect.yy );

	if(  w > 0 && h > 0  ) {

		const float alpha = percent_blend / 100.0;

		glActiveTextureARB( GL_TEXTURE0_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glUseProgram( combined_program );

		glUniform1i( combined_texColor_Location, 0 );
		glUniform1i( combined_texRGBMap_Location, 1 );
		glUniform1i( combined_texAlpha_Location, 2 );

		glUniform4f( combined_alphaMask_Location,
		             0.0, 0.0, 0.0, alpha );
		glColor4f( ( colval & 0xf800 ) / float( 0xf800 ),
		           ( colval & 0x07e0 ) / float( 0x07e0 ),
		           ( colval & 0x001f ) / float( 0x001f ),
		           1.0 );


		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glBegin( GL_QUADS );
		glTexCoord2f( xoff   / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp,     yp );
		glTexCoord2f( ( xoff + w ) / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp + w,   yp );
		glTexCoord2f( ( xoff + w ) / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp + w,   yp + h );
		glTexCoord2f( xoff   / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp,     yp + h );
		glEnd();

		glUseProgram( 0 );

		glActiveTextureARB( GL_TEXTURE0_ARB );
	}
}


static void display_img_blend_wc(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, GLuint tex, GLuint rgbmap_tex, float alpha  CLIP_NUM_DEF)
{
	scr_coord_val rw = w;
	scr_coord_val rh = h;
	const scr_coord_val xoff = clip_wh( &xp, &w, CR.clip_rect.x, CR.clip_rect.xx );
	const scr_coord_val yoff = clip_wh( &yp, &h, CR.clip_rect.y, CR.clip_rect.yy );

	if(  w > 0 && h > 0  ) {
		glActiveTextureARB( GL_TEXTURE0_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, tex );
		glActiveTextureARB( GL_TEXTURE1_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, rgbmap_tex );

		glUseProgram( combined_program );

		glUniform1i( combined_texColor_Location, 0 );
		glUniform1i( combined_texRGBMap_Location, 1 );
		glUniform1i( combined_texAlpha_Location, 2 );

		glUniform4f( combined_alphaMask_Location,
		             0.0, 0.0, 0.0, alpha );
		glColor4f( 0.0, 0.0, 0.0, 0.0 );

		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glBegin( GL_QUADS );
		glTexCoord2f( xoff   / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp,     yp );
		glTexCoord2f( ( xoff + w ) / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp + w,   yp );
		glTexCoord2f( ( xoff + w ) / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp + w,   yp + h );
		glTexCoord2f( xoff   / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp,     yp + h );
		glEnd();

		glUseProgram( 0 );

		glActiveTextureARB( GL_TEXTURE1_ARB );
		glDisable( GL_TEXTURE_2D );
		glActiveTextureARB( GL_TEXTURE0_ARB );
	}
}

static void display_img_blend_wc_colour(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, GLuint tex, PIXVAL colour, float alpha  CLIP_NUM_DEF)
{
	scr_coord_val rw = w;
	scr_coord_val rh = h;
	const scr_coord_val xoff = clip_wh( &xp, &w, CR.clip_rect.x, CR.clip_rect.xx );
	const scr_coord_val yoff = clip_wh( &yp, &h, CR.clip_rect.y, CR.clip_rect.yy );

	if(  w > 0 && h > 0  ) {
		glActiveTextureARB( GL_TEXTURE0_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, tex );

		glUseProgram( combined_program );

		glUniform1i( combined_texColor_Location, 0 );
		glUniform1i( combined_texRGBMap_Location, 1 );
		glUniform1i( combined_texAlpha_Location, 2 );

		glUniform4f( combined_alphaMask_Location,
		             0.0, 0.0, 0.0, alpha );
		glColor4f( ( colour & 0xf800 ) / float( 0xf800 ),
		           ( colour & 0x07e0 ) / float( 0x07e0 ),
		           ( colour & 0x001f ) / float( 0x001f ),
		           1.0 );

		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glBegin( GL_QUADS );
		glTexCoord2f( xoff   / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp,     yp );
		glTexCoord2f( ( xoff + w ) / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp + w,   yp );
		glTexCoord2f( ( xoff + w ) / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp + w,   yp + h );
		glTexCoord2f( xoff   / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp,     yp + h );
		glEnd();

		glUseProgram( 0 );

		glActiveTextureARB( GL_TEXTURE0_ARB );
	}
}

/* from here code for transparent images */

static void display_img_alpha_wc(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, GLuint tex, GLuint rgbmap_tex, GLuint alphatex, const uint8 alpha_flags, PIXVAL  CLIP_NUM_DEF )
{
	//more exact: r/g/b channel from alphatex is selected by alpha_flags
	//to be the alpha channel for this blt.

	scr_coord_val rw = w;
	scr_coord_val rh = h;
	const scr_coord_val xoff = clip_wh( &xp, &w, CR.clip_rect.x, CR.clip_rect.xx );
	const scr_coord_val yoff = clip_wh( &yp, &h, CR.clip_rect.y, CR.clip_rect.yy );

	if(  w > 0 && h > 0  ) {
		glActiveTextureARB( GL_TEXTURE0_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, tex );
		glActiveTextureARB( GL_TEXTURE1_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, rgbmap_tex );
		glActiveTextureARB( GL_TEXTURE2_ARB );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, alphatex );

		glUseProgram( combined_program );

		glUniform1i( combined_texColor_Location, 0 );
		glUniform1i( combined_texRGBMap_Location, 1 );
		glUniform1i( combined_texAlpha_Location, 2 );

		//todo: someone please explain to me why there is 2.0 needed here
		glUniform4f( combined_alphaMask_Location,
		             ( alpha_flags & ALPHA_RED ) ? 2.0 : 0.0,
		             ( alpha_flags & ALPHA_GREEN ) ? 2.0 : 0.0,
		             ( alpha_flags & ALPHA_BLUE ) ? 1.0 : 0.0,
		             0.0 );
		glColor4f( 0, 0, 0, 0.0 );

		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glBegin( GL_QUADS );
		glTexCoord2f( xoff   / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp,     yp );
		glTexCoord2f( ( xoff + w ) / float( rw ),  yoff   / float( rh ) );
		glVertex2i( xp + w,   yp );
		glTexCoord2f( ( xoff + w ) / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp + w,   yp + h );
		glTexCoord2f( xoff   / float( rw ), ( yoff + h ) / float( rh ) );
		glVertex2i( xp,     yp + h );
		glEnd();

		glUseProgram( 0 );

		glActiveTextureARB( GL_TEXTURE2_ARB );
		glDisable( GL_TEXTURE_2D );
		glActiveTextureARB( GL_TEXTURE1_ARB );
		glDisable( GL_TEXTURE_2D );
		glActiveTextureARB( GL_TEXTURE0_ARB );
	}
}


/**
 * draws the transparent outline of an image
 */
void display_rezoomed_img_blend(const image_id n, scr_coord_val xp, scr_coord_val yp, const signed char /*player_nr*/, const FLAGGED_PIXVAL color_index, const bool /*daynight*/, const bool /*dirty*/  CLIP_NUM_DEF)
{
	if(  n < images.size()  ) {
		// need to go to nightmode and or rezoomed?
		rezoom_img( n );
		PIXVAL *sp = images[n].base_data;

		// now, since zooming may have change this image
		float zoom = get_img_zoom( n );

		xp += floor( images[n].base_x * zoom );
		yp += floor( images[n].base_y * zoom );

		// get the real color
		const PIXVAL color = color_index & 0xFFFF;
		float alpha = ( color_index & TRANSPARENT_FLAGS ) / TRANSPARENT25_FLAG / 4.0;

		if(  color_index & OUTLINE_FLAG  ) {
			GLuint tex = getIndexImgTex( images[n], sp );
			display_img_blend_wc_colour( xp, yp,
			                             ceil( images[n].base_w * zoom ),
			                             ceil( images[n].base_h * zoom ),
			                             tex, color, alpha  CLIP_NUM_PAR );
		}
		else {
			GLuint tex = getIndexImgTex( images[n], sp );
			display_img_blend_wc( xp, yp,
			                      ceil( images[n].base_w * zoom ),
			                      ceil( images[n].base_h * zoom ),
			                      tex, rgbmap_day_night_tex, alpha  CLIP_NUM_PAR );
		}
	}
}


void display_rezoomed_img_alpha(const image_id n, const image_id alpha_n, const unsigned alpha_flags, scr_coord_val xp, scr_coord_val yp, const sint8 /*player_nr*/, const FLAGGED_PIXVAL color_index, const bool /*daynight*/, const bool /*dirty*/  CLIP_NUM_DEF)
{
	if(  n < images.size() && alpha_n < images.size()  ) {
		// need to go to nightmode and or rezoomed?
		rezoom_img( n );
		rezoom_img( alpha_n );
		PIXVAL *sp = images[n].base_data;
		// alphamap image uses base data as we don't want to recode
		PIXVAL *alphamap = images[alpha_n].base_data;

		// now, since zooming may have change this image
		float zoom = get_img_zoom( n );

		xp += floor( images[n].base_x * zoom );
		yp += floor( images[n].base_y * zoom );

		// get the real color
		const PIXVAL color = color_index & 0xFFFF;

		GLuint tex = getIndexImgTex( images[n], sp );
		GLuint alphatex = getBaseImgTex( images[alpha_n], alphamap );
		display_img_alpha_wc( xp, yp,
		                      ceil( images[n].base_w * zoom ),
		                      ceil( images[n].base_h * zoom ),
		                      tex, rgbmap_day_night_tex,
		                      alphatex, alpha_flags,
		                      color  CLIP_NUM_PAR );
	}
}


// For blending or outlining unzoomed image. Adapted from display_base_img() and display_unzoomed_img_blend()
void display_base_img_blend(const image_id n, scr_coord_val xp, scr_coord_val yp, const signed char player_nr, const FLAGGED_PIXVAL color_index, const bool daynight, const bool dirty  CLIP_NUM_DEF)
{
	if(  base_tile_raster_width == tile_raster_width  ) {
		// same size => use standard routine
		display_rezoomed_img_blend( n, xp, yp, player_nr, color_index, daynight, dirty  CLIP_NUM_PAR );
	}
	else if(  n < images.size()  ) {
		// now test if visible and clipping needed
		scr_coord_val x = images[n].base_x + xp;
		scr_coord_val y = images[n].base_y + yp;
		scr_coord_val w = images[n].base_w;
		scr_coord_val h = images[n].base_h;
		if(  h == 0 || x >= CR.clip_rect.xx || y >= CR.clip_rect.yy || x + w <= CR.clip_rect.x || y + h <= CR.clip_rect.y  ) {
			// not visible => we are done
			// happens quite often ...
			return;
		}

		PIXVAL *sp = images[n].base_data;

		// new block for new variables
		{
			const PIXVAL color = color_index & 0xFFFF;

			float alpha = ( color_index & TRANSPARENT_FLAGS ) / TRANSPARENT25_FLAG / 4.0;
			GLuint tex;

			// recode is needed only for blending
			if(  !( color_index & OUTLINE_FLAG )  ) {
				// colors for 2nd company color
				if(  player_nr >= 0  ) {
					activate_player_color( player_nr, daynight );
				}
				else {
					// no player
					activate_player_color( 0, daynight );
				}
				tex = getIndexImgTex( images[n], sp );
				display_img_blend_wc( x, y,
				                      images[n].base_w, images[n].base_h,
				                      tex, rgbmap_current_tex, alpha  CLIP_NUM_PAR );
			}
			else {
				tex = getIndexImgTex( images[n], sp );
				display_img_blend_wc_colour( x, y,
				                             images[n].base_w, images[n].base_h,
				                             tex,
				                             color, alpha  CLIP_NUM_PAR );
			}
		}
	} // number ok
}


void display_base_img_alpha(const image_id n, const image_id alpha_n, const unsigned alpha_flags, scr_coord_val xp, scr_coord_val yp, const sint8 player_nr, const FLAGGED_PIXVAL color_index, const bool daynight, const bool dirty  CLIP_NUM_DEF)
{
	if(  base_tile_raster_width == tile_raster_width  ) {
		// same size => use standard routine
		display_rezoomed_img_alpha( n, alpha_n, alpha_flags, xp, yp, player_nr, color_index, daynight, dirty  CLIP_NUM_PAR );
	}
	else if(  n < images.size()  ) {
		// now test if visible and clipping needed
		scr_coord_val x = images[n].base_x + xp;
		scr_coord_val y = images[n].base_y + yp;
		scr_coord_val w = images[n].base_w;
		scr_coord_val h = images[n].base_h;
		if(  h == 0 || x >= CR.clip_rect.xx || y >= CR.clip_rect.yy || x + w <= CR.clip_rect.x || y + h <= CR.clip_rect.y  ) {
			// not visible => we are done
			// happens quite often ...
			return;
		}

		PIXVAL *sp = images[n].base_data;
		PIXVAL *alphamap = images[alpha_n].base_data;

		// new block for new variables
		{
			const PIXVAL color = color_index & 0xFFFF;

			// colors for 2nd company color
			if(  player_nr >= 0  ) {
				activate_player_color( player_nr, daynight );
			}
			else {
				// no player
				activate_player_color( 0, daynight );
			}
			GLuint tex = getIndexImgTex( images[n], sp );
			GLuint alphatex = getBaseImgTex( images[n], alphamap );

			display_img_alpha_wc( x, y,
			                      images[n].base_w, images[n].base_h,
			                      tex, rgbmap_current_tex,
			                      alphatex, alpha_flags,
			                      color  CLIP_NUM_PAR );
		}
	} // number ok
}


// ----------------- basic painting procedures ----------------


// scrolls horizontally, will ignore clipping etc.
void display_scroll_band(scr_coord_val start_y, scr_coord_val x_offset, scr_coord_val h)
{
	glReadBuffer( GL_BACK );
	glDrawBuffer( GL_BACK );
	glDisable( GL_TEXTURE_2D );
	glDisable( GL_BLEND );
	if(  x_offset > 0  ) {
		glRasterPos2i( 0,        start_y + h );
		glCopyPixels( x_offset, disp_height - start_y - h,
		              disp_width - x_offset, h, GL_COLOR );
	}
	else {
		glRasterPos2i( x_offset, start_y + h );
		glCopyPixels( 0,        disp_height - start_y - h,
		              disp_width + x_offset, h, GL_COLOR );
	}
}


/**
 * Draw one Pixel
 */
static void display_pixel(scr_coord_val x, scr_coord_val y, PIXVAL color)
{
	if(  x >= CR0.clip_rect.x && x < CR0.clip_rect.xx && y >= CR0.clip_rect.y && y < CR0.clip_rect.yy  ) {
		glColor3f( ( color & 0xf800 ) / float( 0x10000 ),
		           ( color & 0x07e0 ) / float( 0x00800 ),
		           ( color & 0x001f ) / float( 0x00020 ) );
		glDisable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glBegin( GL_QUADS );
		glVertex2i( x + 1, y );
		glVertex2i( x, y );
		glVertex2i( x, y + 1 );
		glVertex2i( x + 1, y + 1 );
		glEnd();
	}
}


/**
 * Draw filled rectangle
 */
static void display_fb_internal(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, PIXVAL colval, bool /*dirty*/, scr_coord_val cL, scr_coord_val cR, scr_coord_val cT, scr_coord_val cB)
{
	if(  clip_lr( &xp, &w, cL, cR ) && clip_lr( &yp, &h, cT, cB )  ) {
		glColor3f( ( colval & 0xf800 ) / float( 0x10000 ),
		           ( colval & 0x07e0 ) / float( 0x00800 ),
		           ( colval & 0x001f ) / float( 0x00020 ) );
		glDisable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glBegin( GL_QUADS );
		glVertex2i( xp + w, yp );
		glVertex2i( xp, yp );
		glVertex2i( xp, yp + h );
		glVertex2i( xp + w, yp + h );
		glEnd();
	}
}


void display_fillbox_wh_rgb(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, PIXVAL color, bool dirty)
{
	display_fb_internal( xp, yp, w, h, color, dirty, 0, disp_width, 0, disp_height );
}


void display_fillbox_wh_clip_rgb(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, PIXVAL color, bool dirty  CLIP_NUM_DEF)
{
	display_fb_internal( xp, yp, w, h, color, dirty, CR.clip_rect.x, CR.clip_rect.xx, CR.clip_rect.y, CR.clip_rect.yy );
}


void display_filled_roundbox_clip(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, PIXVAL color, bool dirty)
{
	display_fillbox_wh_clip_rgb(xp+2,   yp, w-4, h, color, dirty);
	display_fillbox_wh_clip_rgb(xp,     yp+2, 1, h-4, color, dirty);
	display_fillbox_wh_clip_rgb(xp+1,   yp+1, 1, h-2, color, dirty);
	display_fillbox_wh_clip_rgb(xp+w-1, yp+2, 1, h-4, color, dirty);
	display_fillbox_wh_clip_rgb(xp+w-2, yp+1, 1, h-2, color, dirty);
}


/**
 * Draw vertical line
 */
static void display_vl_internal(const scr_coord_val xp, scr_coord_val yp, scr_coord_val h, const PIXVAL colval, int /*dirty*/, scr_coord_val cL, scr_coord_val cR, scr_coord_val cT, scr_coord_val cB)
{
	if(  xp >= cL && xp < cR && clip_lr( &yp, &h, cT, cB )  ) {
		glColor3f( ( colval & 0xf800 ) / float( 0x10000 ),
		           ( colval & 0x07e0 ) / float( 0x00800 ),
		           ( colval & 0x001f ) / float( 0x00020 ) );
		glDisable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glBegin( GL_QUADS );
		glVertex2i( xp + 1, yp );
		glVertex2i( xp, yp );
		glVertex2i( xp, yp + h );
		glVertex2i( xp + 1, yp + h );
		glEnd();
	}
}


void display_vline_wh_rgb(const scr_coord_val xp, scr_coord_val yp, scr_coord_val h, const PIXVAL color, bool dirty)
{
	display_vl_internal( xp, yp, h, color, dirty, 0, disp_width, 0, disp_height );
}


void display_vline_wh_clip_rgb(const scr_coord_val xp, scr_coord_val yp, scr_coord_val h, const PIXVAL color, bool dirty  CLIP_NUM_DEF)
{
	display_vl_internal( xp, yp, h, color, dirty, CR.clip_rect.x, CR.clip_rect.xx, CR.clip_rect.y, CR.clip_rect.yy );
}


/**
 * Draw raw Pixel data
 */
void display_array_wh(scr_coord_val xp, scr_coord_val yp, scr_coord_val w, scr_coord_val h, const PIXVAL *arr)
{
	const int arr_w = w;
	const int arr_h = h;

	const scr_coord_val xoff = clip_wh( &xp, &w, CR0.clip_rect.x, CR0.clip_rect.xx );
	const scr_coord_val yoff = clip_wh( &yp, &h, CR0.clip_rect.y, CR0.clip_rect.yy );

	if(  w > 0 && h > 0  ) {
		GLuint texname = getArrayTex( arr, arr_w, arr_h );

		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, texname );
		glColor3f( 1, 1, 1 );
		glBegin( GL_QUADS );
		glTexCoord2f( xoff / float( arr_w ), yoff / float( arr_h ) );
		glVertex2i( xp, yp );
		glTexCoord2f( ( xoff + w ) / float( arr_w ), yoff / float( arr_h ) );
		glVertex2i( xp + w, yp );
		glTexCoord2f( ( xoff + w ) / float( arr_w ), ( yoff + h ) / float( arr_h ) );
		glVertex2i( xp + w, yp + h );
		glTexCoord2f( xoff / float( arr_w ), ( yoff + h ) / float( arr_h ) );
		glVertex2i( xp, yp + h );
		glEnd();
	}
}


// --------------------------------- text rendering stuff ------------------------------

bool display_load_font(const char *fname, bool reload)
{
	font_t loaded_fnt;

	if(  fname == NULL  ) {
		dbg->error( "display_load_font", "NULL filename" );
		return false;
	}

	// skip reloading if already in memory, if bdf font
	if(  !reload && default_font.is_loaded() && strcmp( default_font.get_fname(), fname ) == 0  ) {
		return true;
	}

	if(  loaded_fnt.load_from_file( fname )  ) {
		default_font = loaded_fnt;
		default_font_ascent    = default_font.get_ascent();
		default_font_linespace = default_font.get_linespace();

		// find default number width
		const char* digits = "0123456789";
		default_font_numberwidth = 0;
		while (*digits) {
			int pixel = default_font.get_glyph_advance(*digits++);
			if (pixel > default_font_numberwidth) {
				default_font_numberwidth = pixel;
			}
		}

		env_t::fontname = fname;

		for(  auto it = charpage.begin(); it != charpage.end(); it++  ) {
			glDeleteTextures( 1, &it->texture );
		}
		chartex.clear();
		charpage.clear();
		return default_font.is_loaded();
	}

	return false;
}


scr_coord_val display_get_char_width(utf32 c)
{
	return default_font.get_glyph_advance( c );
}


scr_coord_val display_get_number_width()
{
	return default_font_numberwidth;
}

/**
 * For the next logical character in the text, returns the character code
 * as well as retrieves the char byte count and the screen pixel width
 * CAUTION : The text pointer advances to point to the next logical character
 */
utf32 get_next_char_with_metrics(const char *&text, unsigned char &byte_length, unsigned char &pixel_width)
{
	size_t len = 0;
	utf32 const char_code = utf8_decoder_t::decode( (utf8 const *)text, len );

	if(  char_code == UNICODE_NUL || char_code == '\n'  ) {
		// case : end of text reached -> do not advance text pointer
		// also stop at linebreaks
		byte_length = 0;
		pixel_width = 0;
		return 0;
	}
	else {
		text += len;
		byte_length = (uint8)len;
		pixel_width = default_font.get_glyph_advance( char_code );
	}
	return char_code;
}


/* returns true, if this is a valid character */
bool has_character(utf16 char_code)
{
	return default_font.is_valid_glyph( char_code );
}



/*
 * returns the index of the last character that would fit within the width
 * If an ellipsis len is given, it will only return the last character up to this len if the full length cannot be fitted
 * @returns index of next character. if text[index]==0 the whole string fits
 */
size_t display_fit_proportional(const char *text, scr_coord_val max_width)
{
	size_t max_idx = 0;

	uint8 byte_length = 0;
	uint8 pixel_width = 0;
	scr_coord_val current_offset = 0;

	const char *tmp_text = text;
	while(  get_next_char_with_metrics( tmp_text, byte_length, pixel_width ) && max_width > ( current_offset + pixel_width )  ) {
		current_offset += pixel_width;
		max_idx += byte_length;
	}
	return max_idx;
}


/**
 * For the previous logical character in the text, returns the character code
 * as well as retrieves the char byte count and the screen pixel width
 * CAUTION : The text pointer recedes to point to the previous logical character
 */
utf32 get_prev_char_with_metrics(const char *&text, const char *const text_start, unsigned char &byte_length, unsigned char &pixel_width)
{
	if(  text <= text_start  ) {
		// case : start of text reached or passed -> do not move the pointer backwards
		byte_length = 0;
		pixel_width = 0;
		return 0;
	}

	utf32 char_code;
	// determine the start of the previous logical character
	do {
		--text;
	} while(  text > text_start && ( *text & 0xC0 ) == 0x80  );

	size_t len = 0;
	char_code = utf8_decoder_t::decode( (utf8 const *)text, len );
	byte_length = (uint8)len;
	pixel_width = default_font.get_glyph_advance( char_code );

	return char_code;
}


/* proportional_string_width with a text of a given length
* extended for universal font routines with unicode support
*/
scr_coord_val display_calc_proportional_string_len_width(const char *text, size_t len)
{
	uint8 byte_length = 0;
	uint8 pixel_width = 0;
	size_t idx = 0;
	scr_coord_val width = 0;

	while(  get_next_char_with_metrics( text, byte_length, pixel_width ) && idx < len  ) {
		width += pixel_width;
		idx += byte_length;
	}
	return width;
}


/* display_calc_proportional_multiline_string_len_width
* calculates the width and hieght of a box containing the text inside
*/
void display_calc_proportional_multiline_string_len_width(int &xw, int &yh, const char *text)
{
	const font_t *const fnt = &default_font;
	int width = 0;
	bool last_cr = false;

	xw = yh = 0;

	const utf8 *p = reinterpret_cast<const utf8 *>( text );
	while(  const utf32 iUnicode = utf8_decoder_t::decode( p )  ) {

		if(  iUnicode == '\n'  ) {
			// new line: record max width
			xw = max( xw, width );
			yh += LINESPACE;
			width = 0;
			last_cr = true;
			continue;
		}
		last_cr = false;
		width += fnt->get_glyph_advance( iUnicode );
	}
	xw = max( xw, width );
	if(  !last_cr  ) {
		// extra CR of the last was not already a CR
		yh += LINESPACE;
	}
}


/**
 * len parameter added - use -1 for previous behaviour.
 * completely renovated for unicode and 10 bit width and variable height
 */
scr_coord_val display_text_proportional_len_clip_rgb(scr_coord_val x, scr_coord_val y, const char* txt, control_alignment_t flags, const PIXVAL color, bool /*dirty*/, sint32 len  CLIP_NUM_DEF)
{
	scr_coord_val cL, cR, cT, cB;

	// TAKE CARE: Clipping area may be larger than actual screen size
	if(  ( flags & DT_CLIP )  ) {
		cL = CR.clip_rect.x;
		cR = CR.clip_rect.xx;
		cT = CR.clip_rect.y;
		cB = CR.clip_rect.yy;
	}
	else {
		cL = 0;
		cR = disp_width;
		cT = 0;
		cB = disp_height;
	}

	if(  len < 0  ) {
		// don't know len yet
		len = 0x7FFF;
	}

	// adapt x-coordinate for alignment
	switch( flags & ( ALIGN_LEFT | ALIGN_CENTER_H | ALIGN_RIGHT ) ) {
		case ALIGN_LEFT:
			// nothing to do
			break;

		case ALIGN_CENTER_H:
			x -= display_calc_proportional_string_len_width( txt, len ) / 2;
			break;

		case ALIGN_RIGHT:
			x -= display_calc_proportional_string_len_width( txt, len );
			break;
	}

	// still something to display?
	const font_t *const fnt = &default_font;

	if(  x >= cR || y >= cB || y + fnt->get_linespace() <= cT  ) {
		// nothing to display
		return 0;
	}

	// store the initial x (for dirty marking)
	const scr_coord_val x0 = x;

	// big loop, draw char by char
	utf8_decoder_t decoder( (utf8 const *)txt );
	size_t iTextPos = 0; // pointer on text position

	while(  iTextPos < (size_t)len && decoder.has_next()  ) {
		// decode char
		utf32 c = decoder.next();
		iTextPos = decoder.get_position() - (utf8 const *)txt;

		if(  c == '\n'  ) {
			// stop at linebreak
			break;
		}
		// print unknown character?
		else if(  !fnt->is_valid_glyph( c )  ) {
			c = 0;
		}

		//todo: what are we supposed to do with it? seems to be the upper border of valid glyph data
		//const uint8 glyph_yoffset = std::min(fnt->get_glyph_yoffset(c), (uint8)y_offset);
		// do the display

		{
			const font_t::glyph_t &glyph = fnt->get_glyph( c );
			scr_coord_val tx = 0;
			scr_coord_val sx = x + glyph.left;
			scr_coord_val w = glyph.width;
			scr_coord_val rw = w;
			if(  sx < cL  ) {
				tx += cL - sx;
				w -= cL - sx;
				sx += cL - sx;
			}
			if(  sx + w > cR  ) {
				w = cR - sx;
			}
			scr_coord_val ty = 0;
			scr_coord_val sy = y + glyph.top;
			scr_coord_val h = glyph.height;
			scr_coord_val rh = h;

			if(  sy < cT  ) {
				ty += cT - sy;
				h -= cT - sy;
				sy += cT - sy;
			}
			if(  sy + h > cB  ) {
				h = cB - sy;
			}

			if(  w > 0 && h > 0  ) {
				GLfloat glx = 0, gly = 0, glw = 0, glh = 0;
				GLuint texname = getGlyphTex( c, fnt,
				                              glx, gly,
				                              glw, glh );

				glEnable( GL_TEXTURE_2D );
				glBindTexture( GL_TEXTURE_2D, texname );
				glColor3f( ( color & 0xf800 ) / float( 0x10000 ),
				           ( color & 0x07e0 ) / float( 0x00800 ),
				           ( color & 0x001f ) / float( 0x00020 ) );
				glEnable( GL_BLEND );
				glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
				glBegin( GL_QUADS );
				glTexCoord2f( glx + tx / float( rw ) * glw,
				              gly + ty / float( rh ) * glh );
				glVertex2i( sx,         sy );
				glTexCoord2f( glx + ( tx + w ) / float( rw ) * glw,
				              gly + ty / float( rh ) * glh );
				glVertex2i( sx + w,     sy );
				glTexCoord2f( glx + ( tx + w ) / float( rw ) * glw,
				              gly + ( ty + h ) / float( rh ) * glh );
				glVertex2i( sx + w,     sy + h );
				glTexCoord2f( glx + tx / float( rw ) * glw,
				              gly + ( ty + h ) / float( rh ) * glh );
				glVertex2i( sx,         sy + h );
				glEnd();
			}
		}

		x += fnt->get_glyph_advance( c );

	}

	// warning: actual len might be longer, due to clipping!
	return x - x0;
}


/// Displays a string which is abbreviated by the (language specific) ellipsis character if too wide
/// If enough space is given then it just displays the full string
void display_proportional_ellipsis_rgb(scr_rect r, const char *text, int align, const PIXVAL color, const bool dirty, bool shadowed, PIXVAL shadow_color)
{
	const scr_coord_val ellipsis_width = translator::get_lang()->ellipsis_width;
	const scr_coord_val max_screen_width = r.w;
	size_t max_idx = 0;

	uint8 byte_length = 0;
	uint8 pixel_width = 0;
	scr_coord_val current_offset = 0;

	if(  align & ALIGN_CENTER_V  ) {
		r.y += ( r.h - LINESPACE ) / 2;
		align &= ~ALIGN_CENTER_V;
	}

	const char *tmp_text = text;
	while(  get_next_char_with_metrics( tmp_text, byte_length, pixel_width ) && max_screen_width >= ( current_offset + ellipsis_width + pixel_width )  ) {
		current_offset += pixel_width;
		max_idx += byte_length;
	}
	size_t max_idx_before_ellipsis = max_idx;
	scr_coord_val max_offset_before_ellipsis = current_offset;

	// now check if the text would fit completely
	if(  ellipsis_width && pixel_width > 0  ) {
		// only when while above failed because of exceeding length
		current_offset += pixel_width;
		max_idx += byte_length;
		// check the rest ...
		while(  get_next_char_with_metrics( tmp_text, byte_length, pixel_width ) && max_screen_width >= ( current_offset + pixel_width )  ) {
			current_offset += pixel_width;
			max_idx += byte_length;
		}
		// if it does not fit
		if(  max_screen_width < ( current_offset + pixel_width )  ) {
			scr_coord_val w = 0;
			// since we know the length already, we try to center the text with the remaining pixels of the last character
			if(  align & ALIGN_CENTER_H  ) {
				w = ( max_screen_width - max_offset_before_ellipsis - ellipsis_width ) / 2;
			}
			if(  shadowed  ) {
				display_text_proportional_len_clip_rgb( r.x + w + 1, r.y + 1, text, ALIGN_LEFT | DT_CLIP, shadow_color, dirty, max_idx_before_ellipsis  CLIP_NUM_DEFAULT );
			}
			w += display_text_proportional_len_clip_rgb( r.x + w, r.y, text, ALIGN_LEFT | DT_CLIP, color, dirty, max_idx_before_ellipsis  CLIP_NUM_DEFAULT );

			if(  shadowed  ) {
				display_text_proportional_len_clip_rgb( r.x + w + 1, r.y + 1, translator::translate( "..." ), ALIGN_LEFT | DT_CLIP, shadow_color, dirty, -1  CLIP_NUM_DEFAULT );
			}

			display_text_proportional_len_clip_rgb( r.x + w, r.y, translator::translate( "..." ), ALIGN_LEFT | DT_CLIP, color, dirty, -1  CLIP_NUM_DEFAULT );
			return;
		}
		else {
			// if this fits, end of string
			max_idx += byte_length;
			current_offset += pixel_width;
		}
	}
	switch( align & ALIGN_RIGHT ) {
		case ALIGN_CENTER_H:
			r.x += ( max_screen_width - current_offset ) / 2;
			break;
		case ALIGN_RIGHT:
			r.x += max_screen_width - current_offset;
		default: ;
	}
	if(  shadowed  ) {
		display_text_proportional_len_clip_rgb( r.x + 1, r.y + 1, text, ALIGN_LEFT | DT_CLIP, shadow_color, dirty, -1  CLIP_NUM_DEFAULT );
	}
	display_text_proportional_len_clip_rgb( r.x, r.y, text, ALIGN_LEFT | DT_CLIP, color, dirty, -1  CLIP_NUM_DEFAULT );
}


/**
 * Draw shaded rectangle using direct color values
 */
void display_ddd_box_rgb(scr_coord_val x1, scr_coord_val y1, scr_coord_val w, scr_coord_val h, PIXVAL tl_color, PIXVAL rd_color, bool dirty)
{
	display_fillbox_wh_rgb( x1, y1,         w, 1, tl_color, dirty );
	display_fillbox_wh_rgb( x1, y1 + h - 1, w, 1, rd_color, dirty );

	h -= 2;

	display_vline_wh_rgb( x1,         y1 + 1, h, tl_color, dirty );
	display_vline_wh_rgb( x1 + w - 1, y1 + 1, h, rd_color, dirty );
}


void display_outline_proportional_rgb(scr_coord_val xpos, scr_coord_val ypos, PIXVAL text_color, PIXVAL shadow_color, const char *text, int dirty, sint32 len)
{
	const int flags = ALIGN_LEFT | DT_CLIP;
	display_text_proportional_len_clip_rgb( xpos - 1, ypos, text, flags, shadow_color, dirty, len  CLIP_NUM_DEFAULT );
	display_text_proportional_len_clip_rgb( xpos + 1, ypos + 2, text, flags, shadow_color, dirty, len  CLIP_NUM_DEFAULT );
	display_text_proportional_len_clip_rgb( xpos, ypos + 1, text, flags, text_color, dirty, len  CLIP_NUM_DEFAULT );
}


void display_shadow_proportional_rgb(scr_coord_val xpos, scr_coord_val ypos, PIXVAL text_color, PIXVAL shadow_color, const char *text, int dirty, sint32 len)
{
	const int flags = ALIGN_LEFT | DT_CLIP;
	display_text_proportional_len_clip_rgb( xpos + 1, ypos + 1 + ( 12 - LINESPACE ) / 2, text, flags, shadow_color, dirty, len  CLIP_NUM_DEFAULT );
	display_text_proportional_len_clip_rgb( xpos, ypos + ( 12 - LINESPACE ) / 2, text, flags, text_color, dirty, len  CLIP_NUM_DEFAULT );
}


/**
 * Draw shaded rectangle using direct color values
 */
void display_ddd_box_clip_rgb(scr_coord_val x1, scr_coord_val y1, scr_coord_val w, scr_coord_val h, PIXVAL tl_color, PIXVAL rd_color)
{
	display_fillbox_wh_clip_rgb( x1, y1,         w, 1, tl_color, true );
	display_fillbox_wh_clip_rgb( x1, y1 + h - 1, w, 1, rd_color, true );

	h -= 2;

	display_vline_wh_clip_rgb( x1,         y1 + 1, h, tl_color, true );
	display_vline_wh_clip_rgb( x1 + w - 1, y1 + 1, h, rd_color, true );
}


/**
 * display text in 3d box with clipping
 */
void display_ddd_proportional_clip(scr_coord_val xpos, scr_coord_val ypos, FLAGGED_PIXVAL ddd_color, FLAGGED_PIXVAL text_color, const char *text, int dirty  CLIP_NUM_DEF)
{
	const int vpadding = LINESPACE / 7;
	const int hpadding = LINESPACE / 4;

	scr_coord_val width = proportional_string_width( text );

	PIXVAL lighter = display_blend_colors_alpha32( ddd_color, color_idx_to_rgb( COL_WHITE ), 8 /* 25% */ );
	PIXVAL darker  = display_blend_colors_alpha32( ddd_color, color_idx_to_rgb( COL_BLACK ), 8 /* 25% */ );

	display_fillbox_wh_clip_rgb( xpos + 1, ypos - vpadding + 1, width + 2 * hpadding - 2, LINESPACE + 2 * vpadding - 1, ddd_color, dirty CLIP_NUM_PAR );

	display_fillbox_wh_clip_rgb( xpos, ypos - vpadding, width + 2 * hpadding - 2, 1, lighter, dirty );
	display_fillbox_wh_clip_rgb( xpos, ypos + LINESPACE + vpadding, width + 2 * hpadding - 2, 1, darker,  dirty );

	display_vline_wh_clip_rgb( xpos, ypos - vpadding, LINESPACE + vpadding * 2, lighter, dirty );
	display_vline_wh_clip_rgb( xpos + width + 2 * hpadding - 2, ypos - vpadding, LINESPACE + vpadding * 2, darker,  dirty );

	display_text_proportional_len_clip_rgb( xpos + hpadding, ypos + 1, text, ALIGN_LEFT | DT_CLIP, text_color, dirty, -1 );
}


/**
 * Draw multiline text
 */
scr_coord_val display_multiline_text_rgb(scr_coord_val x, scr_coord_val y, const char *buf, PIXVAL color)
{
	scr_coord_val max_px_len = 0;
	if(  buf != NULL && *buf != '\0'  ) {
		const char *next;

		do {
			next = strchr( buf, '\n' );
			const scr_coord_val px_len = display_text_proportional_len_clip_rgb(
			                x, y, buf,
			                ALIGN_LEFT | DT_CLIP, color, true,
			                next != NULL ? (int)(size_t)( next - buf ) : -1
			                   );
			if(  px_len > max_px_len  ) {
				max_px_len = px_len;
			}
			y += LINESPACE;
		} while(  (void)( buf = ( next ? next + 1 : NULL ) ), buf != NULL  );
	}
	return max_px_len;
}


/**
 * draw line from x,y to xx,yy
 **/
void display_direct_line_rgb(const scr_coord_val x, const scr_coord_val y, const scr_coord_val xx, const scr_coord_val yy, const PIXVAL colval)
{
	//todo: make GL do this
	int i, steps;
	sint64 xp, yp;
	sint64 xs, ys;

	const int dx = xx - x;
	const int dy = yy - y;

	steps = ( abs( dx ) > abs( dy ) ? abs( dx ) : abs( dy ) );
	if(  steps == 0  ) {
		steps = 1;
	}

	xs = ( (sint64)dx << 16 ) / steps;
	ys = ( (sint64)dy << 16 ) / steps;

	xp = (sint64)x << 16;
	yp = (sint64)y << 16;

	for(  i = 0; i <= steps; i++  ) {
		display_pixel( xp >> 16, yp >> 16, colval );
		xp += xs;
		yp += ys;
	}
}


//taken from function display_direct_line() above, to draw a dotted line: draw=pixels drawn, dontDraw=pixels skipped
void display_direct_line_dotted_rgb(const scr_coord_val x, const scr_coord_val y, const scr_coord_val xx, const scr_coord_val yy, const scr_coord_val draw, const scr_coord_val dontDraw, const PIXVAL colval)
{
	//todo: make GL do this
	int i, steps;
	sint64 xp, yp;
	sint64 xs, ys;
	int counter = 0;
	bool mustDraw = true;

	const int dx = xx - x;
	const int dy = yy - y;

	steps = ( abs( dx ) > abs( dy ) ? abs( dx ) : abs( dy ) );
	if(  steps == 0  ) {
		steps = 1;
	}

	xs = ( (sint64)dx << 16 ) / steps;
	ys = ( (sint64)dy << 16 ) / steps;

	xp = (sint64)x << 16;
	yp = (sint64)y << 16;

	for(  i = 0; i <= steps; i++  ) {
		counter ++;
		if(  mustDraw  ) {
			if(  counter == draw  ) {
				mustDraw = !mustDraw;
				counter = 0;
			}
		}
		if(  !mustDraw  ) {
			if(  counter == dontDraw  ) {
				mustDraw = !mustDraw;
				counter = 0;
			}
		}

		if(  mustDraw  ) {
			display_pixel( xp >> 16, yp >> 16, colval );
		}
		xp += xs;
		yp += ys;
	}
}


// bresenham circle (from wikipedia ...)
void display_circle_rgb(scr_coord_val x0, scr_coord_val  y0, int radius, const PIXVAL colval)
{
	int f = 1 - radius;
	int ddF_x = 1;
	int ddF_y = -2 * radius;
	int x = 0;
	int y = radius;

	display_pixel( x0, y0 + radius, colval );
	display_pixel( x0, y0 - radius, colval );
	display_pixel( x0 + radius, y0, colval );
	display_pixel( x0 - radius, y0, colval );

	while(  x < y  ) {
		// ddF_x == 2 * x + 1;
		// ddF_y == -2 * y;
		// f == x*x + y*y - radius*radius + 2*x - y + 1;
		if(  f >= 0  ) {
			y--;
			ddF_y += 2;
			f += ddF_y;
		}

		x++;
		ddF_x += 2;
		f += ddF_x;

		display_pixel( x0 + x, y0 + y, colval );
		display_pixel( x0 - x, y0 + y, colval );
		display_pixel( x0 + x, y0 - y, colval );
		display_pixel( x0 - x, y0 - y, colval );
		display_pixel( x0 + y, y0 + x, colval );
		display_pixel( x0 - y, y0 + x, colval );
		display_pixel( x0 + y, y0 - x, colval );
		display_pixel( x0 - y, y0 - x, colval );
	}
}


// bresenham circle (from wikipedia ...)
void display_filled_circle_rgb(scr_coord_val x0, scr_coord_val  y0, int radius, const PIXVAL colval)
{
	int f = 1 - radius;
	int ddF_x = 1;
	int ddF_y = -2 * radius;
	int x = 0;
	int y = radius;

	display_fb_internal( x0 - radius, y0, radius + radius + 1, 1, colval, false, CR0.clip_rect.x, CR0.clip_rect.xx, CR0.clip_rect.y, CR0.clip_rect.yy );
	display_pixel( x0, y0 + radius, colval );
	display_pixel( x0, y0 - radius, colval );
	display_pixel( x0 + radius, y0, colval );
	display_pixel( x0 - radius, y0, colval );

	while(  x < y  ) {
		// ddF_x == 2 * x + 1;
		// ddF_y == -2 * y;
		// f == x*x + y*y - radius*radius + 2*x - y + 1;
		if(  f >= 0  ) {
			y--;
			ddF_y += 2;
			f += ddF_y;
		}

		x++;
		ddF_x += 2;
		f += ddF_x;
		display_fb_internal( x0 - x, y0 + y, x + x, 1, colval, false, CR0.clip_rect.x, CR0.clip_rect.xx, CR0.clip_rect.y, CR0.clip_rect.yy );
		display_fb_internal( x0 - x, y0 - y, x + x, 1, colval, false, CR0.clip_rect.x, CR0.clip_rect.xx, CR0.clip_rect.y, CR0.clip_rect.yy );

		display_fb_internal( x0 - y, y0 + x, y + y, 1, colval, false, CR0.clip_rect.x, CR0.clip_rect.xx, CR0.clip_rect.y, CR0.clip_rect.yy );
		display_fb_internal( x0 - y, y0 - x, y + y, 1, colval, false, CR0.clip_rect.x, CR0.clip_rect.xx, CR0.clip_rect.y, CR0.clip_rect.yy );
	}
//	mark_rect_dirty_wc( x0-radius, y0-radius, x0+radius+1, y0+radius+1 );
}



void display_signal_direction_rgb(scr_coord_val x, scr_coord_val y, uint8 way_dir, uint8 sig_dir, PIXVAL col1, PIXVAL col1_dark, bool is_diagonal, uint8 slope)
{
	uint8 width  = is_diagonal ? current_tile_raster_width / 6 * 0.353 : current_tile_raster_width / 6;
	const uint8 height = is_diagonal ? current_tile_raster_width / 6 * 0.353 : current_tile_raster_width / 12;
	const uint8 thickness = max( current_tile_raster_width / 36, 2 );

	x += current_tile_raster_width / 2;
	y += ( current_tile_raster_width * 9 ) / 16;

	if(  is_diagonal  ) {

		if(  way_dir == ribi_t::northeast || way_dir == ribi_t::southwest  ) {
			// vertical
			x += ( way_dir == ribi_t::northeast ) ? current_tile_raster_width / 4 : ( -current_tile_raster_width / 4 );
			y += current_tile_raster_width / 16;
			width = width << 2; // 4x

			// upper
			for(  uint8 xoff = 0; xoff < width / 2; xoff++  ) {
				const uint8 yoff = ( uint8 )( ( xoff + 1 ) / 2 );
				// up
				if(  sig_dir & ribi_t::east || sig_dir & ribi_t::south  ) {
					display_vline_wh_clip_rgb( x + xoff, y + yoff, width / 4 - yoff, col1, true );
					display_vline_wh_clip_rgb( x - xoff - 1, y + yoff, width / 4 - yoff, col1, true );
				}
				// down
				if(  sig_dir & ribi_t::west || sig_dir & ribi_t::north  ) {
					display_vline_wh_clip_rgb( x + xoff, y + current_tile_raster_width / 6,              width / 4 - yoff, col1,      true );
					display_vline_wh_clip_rgb( x + xoff, y + current_tile_raster_width / 6 + width / 4 - yoff, thickness,    col1_dark, true );
					display_vline_wh_clip_rgb( x - xoff - 1, y + current_tile_raster_width / 6,              width / 4 - yoff, col1,      true );
					display_vline_wh_clip_rgb( x - xoff - 1, y + current_tile_raster_width / 6 + width / 4 - yoff, thickness,    col1_dark, true );
				}
			}
			// up
			if(  sig_dir & ribi_t::east || sig_dir & ribi_t::south  ) {
				display_fillbox_wh_clip_rgb( x - width / 2, y + width / 4, width, thickness, col1_dark, true );
			}
		}
		else {
			// horizontal
			y -= current_tile_raster_width / 12;
			if(  way_dir == ribi_t::southeast  ) {
				y += current_tile_raster_width / 4;
			}

			for(  uint8 xoff = 0; xoff < width * 2; xoff++  ) {
				const uint8 h = width * 2 - (scr_coord_val)( xoff + 1 );
				// left
				if(  sig_dir & ribi_t::north || sig_dir & ribi_t::east  ) {
					display_vline_wh_clip_rgb( x - xoff - width * 2, y + (scr_coord_val)( ( xoff + 1 ) / 2 ),   h, col1, true );
					display_vline_wh_clip_rgb( x - xoff - width * 2, y + (scr_coord_val)( ( xoff + 1 ) / 2 ) + h, thickness, col1_dark, true );
				}
				// right
				if(  sig_dir & ribi_t::south || sig_dir & ribi_t::west  ) {
					display_vline_wh_clip_rgb( x + xoff + width * 2, y + (scr_coord_val)( ( xoff + 1 ) / 2 ),   h, col1, true );
					display_vline_wh_clip_rgb( x + xoff + width * 2, y + (scr_coord_val)( ( xoff + 1 ) / 2 ) + h, thickness, col1_dark, true );
				}
			}
		}
	}
	else {
		if(  sig_dir & ribi_t::south  ) {
			// upper right
			scr_coord_val slope_offset_y = corner_se( slope ) * TILE_HEIGHT_STEP;
			for(  uint8 xoff = 0; xoff < width; xoff++  ) {
				display_vline_wh_clip_rgb( x + xoff, y - slope_offset_y, (scr_coord_val)( xoff / 2 ) + 1, col1, true );
				display_vline_wh_clip_rgb( x + xoff, y - slope_offset_y + (scr_coord_val)( xoff / 2 ) + 1, thickness, col1_dark, true );
			}
		}
		if(  sig_dir & ribi_t::east  ) {
			scr_coord_val slope_offset_y = corner_se( slope ) * TILE_HEIGHT_STEP;
			for(  uint8 xoff = 0; xoff < width; xoff++  ) {
				display_vline_wh_clip_rgb( x - xoff - 1, y - slope_offset_y, (scr_coord_val)( xoff / 2 ) + 1, col1, true );
				display_vline_wh_clip_rgb( x - xoff - 1, y - slope_offset_y + (scr_coord_val)( xoff / 2 ) + 1, thickness, col1_dark, true );
			}
		}
		if(  sig_dir & ribi_t::west  ) {
			scr_coord_val slope_offset_y = corner_nw( slope ) * TILE_HEIGHT_STEP;
			for(  uint8 xoff = 0; xoff < width; xoff++  ) {
				display_vline_wh_clip_rgb( x + xoff, y - slope_offset_y + height * 2 - (scr_coord_val)( xoff / 2 ) + 1, (scr_coord_val)( xoff / 2 ) + 1, col1, true );
				display_vline_wh_clip_rgb( x + xoff, y - slope_offset_y + height * 2 + 1, thickness, col1_dark, true );
			}
		}
		if(  sig_dir & ribi_t::north  ) {
			scr_coord_val slope_offset_y = corner_nw( slope ) * TILE_HEIGHT_STEP;
			for(  uint8 xoff = 0; xoff < width; xoff++  ) {
				display_vline_wh_clip_rgb( x - xoff - 1, y - slope_offset_y + height * 2 - (scr_coord_val)( xoff / 2 ) + 1, (scr_coord_val)( xoff / 2 ) + 1, col1, true );
				display_vline_wh_clip_rgb( x - xoff - 1, y - slope_offset_y + height * 2 + 1, thickness, col1_dark, true );
			}
		}
	}
}


/**
 * Print a bezier curve between points A and B
 * @param Ax,Ay start coordinate of Bezier curve
 * @param Bx,By end coordinate of Bezier curve
 * @param ADx,ADy vector for start direction of curve
 * @param BDx,BDy vector for end direction of Bezier curve
 * @param colore color for curve to be drawn
 * @param draw for dotted lines, how many pixels to be drawn (leave 0 for solid line)
 * @param dontDraw for dotted lines, how many pixels to not be drawn (leave 0 for solid line)
 */
void draw_bezier_rgb(scr_coord_val Ax, scr_coord_val Ay, scr_coord_val Bx, scr_coord_val By, scr_coord_val ADx, scr_coord_val ADy, scr_coord_val BDx, scr_coord_val BDy, const PIXVAL colore, scr_coord_val draw, scr_coord_val dontDraw)
{
	scr_coord_val Cx, Cy, Dx, Dy;
	Cx = Ax + ADx;
	Cy = Ay + ADy;
	Dx = Bx + BDx;
	Dy = By + BDy;

	/* float a,b,rx,ry,oldx,oldy;
	for (float t=0.0;t<=1;t+=0.05)
	{
		a = t;
		b = 1.0 - t;
		if (t>0.0)
		{
			oldx=rx;
			oldy=ry;
		}
		rx = Ax*b*b*b + 3*Cx*b*b*a + 3*Dx*b*a*a + Bx*a*a*a;
		ry = Ay*b*b*b + 3*Cy*b*b*a + 3*Dy*b*a*a + By*a*a*a;
		if (t>0.0)
			if (!draw && !dontDraw)
				display_direct_line_rgb(rx,ry,oldx,oldy,colore);
			else
				display_direct_line_dotted_rgb(rx,ry,oldx,oldy,draw,dontDraw,colore);
	  }
	*/

	sint32 rx = Ax * 32 * 32 * 32; // init with a=0, b=32
	sint32 ry = Ay * 32 * 32 * 32; // init with a=0, b=32

	// fixed point: we cycle between 0 and 32, rather than 0 and 1
	for(  sint32 a = 1; a <= 32; a++  ) {
		const sint32 b = 32 - a;
		const sint32 oldx = rx;
		const sint32 oldy = ry;
		rx = Ax * b * b * b + 3 * Cx * b * b * a + 3 * Dx * b * a * a + Bx * a * a * a;
		ry = Ay * b * b * b + 3 * Cy * b * b * a + 3 * Dy * b * a * a + By * a * a * a;
		// fixed point: due to cycling between 0 and 32 (1<<5), we divide by 32^3 == 1<<15 because of cubic interpolation
		if(  !draw && !dontDraw  ) {
			display_direct_line_rgb( rx >> 15, ry >> 15, oldx >> 15, oldy >> 15, colore );
		}
		else {
			display_direct_line_dotted_rgb( rx >> 15, ry >> 15, oldx >> 15, oldy >> 15, draw, dontDraw, colore );
		}
	}
}



// Only right facing at the moment
void display_right_triangle_rgb(scr_coord_val x, scr_coord_val y, scr_coord_val height, const PIXVAL colval, const bool dirty)
{
	y += ( height / 2 );
	while(  height > 0  ) {
		display_vline_wh_rgb( x, y - ( height / 2 ), height, colval, dirty );
		x++;
		height -= 2;
	}
}



// ------------------- other support routines that actually interface with the OS -----------------


/**
 * copies only the changed areas to the screen using the "tile dirty buffer"
 * To get large changes, actually the current and the previous one is used.
 */
void display_flush_buffer()
{
	dr_textur( 0, 0, disp_width, disp_height );
}


/**
 * Turn mouse pointer visible/invisible
 */
void display_show_pointer(int yesno)
{
#ifdef USE_SOFTPOINTER
	softpointer = yesno;
#else
	show_pointer( yesno );
#endif
}


/**
 * mouse pointer image
 */
void display_set_pointer(int pointer)
{
	standard_pointer = pointer;
}


/**
 * mouse pointer image
 */
void display_show_load_pointer(int loading)
{
#ifdef USE_SOFTPOINTER
	softpointer = !loading;
#else
	set_pointer( loading );
#endif
}


static int inited = false;

static GLuint compileShader(GLuint type, char const *source, int length, char const *name)
{
	GLuint shader = glCreateShader( type );
	GLint result;

	glShaderSource( shader, 1, &source, &length );
	glCompileShader( shader );
	glGetShaderiv( shader, GL_COMPILE_STATUS, &result );
	if(  result == GL_FALSE  ) {
		char info[65536];
		GLsizei len;
		glGetShaderInfoLog( shader, sizeof(info), &len, info );
		fputs( info, stderr );

		glDeleteShader( shader );
		shader = 0;
		dbg->fatal( "compileShader", "Failed to compile shader %s", name );
	}
	return shader;
}

static GLuint linkProgram(GLuint vertex, GLuint fragment, char const *name)
{
	GLuint program = glCreateProgram();
	GLint result;

	glAttachShader( program, vertex );
	glAttachShader( program, fragment );

	glLinkProgram( program );
	glGetProgramiv( program, GL_LINK_STATUS, &result );
	if(  result == GL_FALSE  ) {
		char info[65536];
		GLsizei len;
		glGetProgramInfoLog( program, sizeof(info), &len, info );
		fputs( info, stderr );

		glDeleteProgram( program );
		program = 0;
		dbg->fatal( "linkProgram", "Failed to link %s", name );
	}

	return program;
}

/**
 * Initialises the graphics module
 */
bool simgraph_init(scr_size window_size, sint16 full_screen)
{
	disp_actual_width = window_size.w;
	disp_height = window_size.h;

	// get real width from os-dependent routines
	disp_width = dr_os_open( window_size, full_screen );
	if(  disp_width <= 0  ) {
		dr_fatal_notify( "Cannot open window!" );
		return false;
	}

	dr_textur_init();
	inited = true;

	// init, load, and check fonts
	if(  !display_load_font( env_t::fontname.c_str() )  ) {
		env_t::fontname = dr_get_system_font();
		if(  !display_load_font( env_t::fontname.c_str() )  ) {
			env_t::fontname = FONT_PATH_X "cyr.bdf";
			if(  !display_load_font( env_t::fontname.c_str() )  ) {
				dr_fatal_notify( "No fonts found!" );
				return false;
			}
		}
	}

	// init player colors
	for(  int i = 0; i < MAX_PLAYER_COUNT; i++  ) {
		player_offsets[i][0] = i * 8;
		player_offsets[i][1] = i * 8 + 24;
	}

	display_set_clip_wh( 0, 0, disp_width, disp_height );

	// Calculate daylight rgbmap and save it for unshaded tile drawing
	player_day = 0;
	display_day_night_shift( 0 );
	memcpy( specialcolormap_all_day, specialcolormap_day_night, 256 * sizeof(PIXVAL) );
	memcpy( rgbmap_all_day, rgbmap_day_night, RGBMAPSIZE * sizeof(PIXVAL) );
	updateRGBMap( rgbmap_all_day_tex, rgbmap_all_day, 0 );

	GLuint combined_fragmentShader;
	GLuint vertexShader;

	combined_fragmentShader = compileShader(
	                GL_FRAGMENT_SHADER,
	                combined_fragmentShaderText,
	                sizeof(combined_fragmentShaderText),
	                "combined fragment shader" );
	vertexShader = compileShader(
	                GL_VERTEX_SHADER,
	                vertexShaderText,
	                sizeof(vertexShaderText),
	                "vertex shader" );


	combined_program = linkProgram( vertexShader,
	                                combined_fragmentShader,
	                                "combined program" );

	glDeleteShader( combined_fragmentShader );
	glDeleteShader( vertexShader );

	combined_texColor_Location = glGetUniformLocation( combined_program, "texColor" );
	combined_texRGBMap_Location = glGetUniformLocation( combined_program, "texRGBMap" );
	combined_texAlpha_Location = glGetUniformLocation( combined_program, "texAlpha" );
	combined_alphaMask_Location = glGetUniformLocation( combined_program, "alphaMask" );


	return true;
}


/**
 * Check if the graphic module already was initialized.
 */
bool is_display_init()
{
	return inited && default_font.is_loaded();
}


/**
 * Close the Graphic module
 */
void simgraph_exit()
{
	display_free_all_images_above( 0 );
	images.clear();

	dr_os_close();
}


/* changes display size
 */
void simgraph_resize(scr_size new_window_size)
{
	disp_actual_width = max( 16, new_window_size.w );
	if(  new_window_size.h <= 0  ) {
		new_window_size.h = 64;
	}
	// only resize, if internal values are different
	if(  disp_width != new_window_size.w || disp_height != new_window_size.h  ) {
		scr_coord_val new_pitch = dr_textur_resize( NULL, new_window_size.w, new_window_size.h );
		if(  new_pitch != disp_width || disp_height != new_window_size.h  ) {
			disp_width = new_pitch;
			disp_height = new_window_size.h;

			display_set_clip_wh( 0, 0, disp_actual_width, disp_height );
		}
	}
}


/**
 * Take Screenshot
 */
bool display_snapshot( const scr_rect &area )
{
	if(  access( SCREENSHOT_PATH_X, W_OK ) == -1  ) {
		return false; // directory not accessible
	}

	static int number = 0;
	char filename[80];

	// find the first not used screenshot image
	do {
		sprintf( filename, SCREENSHOT_PATH_X "simscr%02d.png", number++ );
	} while(  access( filename, W_OK ) != -1  );

	// now save the screenshot
	scr_rect clipped_area = area;
	clipped_area.clip( scr_rect( 0, 0, disp_actual_width, disp_height ) );

	raw_image_t img( clipped_area.w, clipped_area.h, raw_image_t::FMT_RGB888 );

#if 0
	for(  scr_coord_val y = clipped_area.y; y < clipped_area.y + clipped_area.h; ++y  ) {
		uint8 *dst = img.access_pixel( 0, y );
		const PIXVAL *row = textur + clipped_area.x + y * disp_width;

		for(  scr_coord_val x = clipped_area.x; x < clipped_area.x + clipped_area.w; ++x  ) {
			const rgb888_t pixel = pixval_to_rgb888( *row++ );
			*dst++ = pixel.r;
			*dst++ = pixel.g;
			*dst++ = pixel.b;
		}
	}
#endif
	return img.write_png( filename );
}
