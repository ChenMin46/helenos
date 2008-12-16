/*
 * Copyright (c) 2008 Martin Decky
 * Copyright (c) 2006 Jakub Vana
 * Copyright (c) 2006 Ondrej Palkovsky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @defgroup fb Graphical framebuffer
 * @brief	HelenOS graphical framebuffer.
 * @ingroup fbs
 * @{
 */

/** @file
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ddi.h>
#include <sysinfo.h>
#include <align.h>
#include <as.h>
#include <ipc/fb.h>
#include <ipc/ipc.h>
#include <ipc/ns.h>
#include <ipc/services.h>
#include <kernel/errno.h>
#include <kernel/genarch/fb/visuals.h>
#include <async.h>
#include <bool.h>

#include "font-8x16.h"
#include "fb.h"
#include "main.h"
#include "../console/screenbuffer.h"
#include "ppm.h"

#include "pointer.xbm"
#include "pointer_mask.xbm"

#define DEFAULT_BGCOLOR  0xf0f0f0
#define DEFAULT_FGCOLOR  0x000000

#define MAX_ANIM_LEN     8
#define MAX_ANIMATIONS   4
#define MAX_PIXMAPS      256  /**< Maximum number of saved pixmaps */
#define MAX_VIEWPORTS    128  /**< Viewport is a rectangular area on the screen */

typedef void (*rgb_conv_t)(void *, uint32_t);

struct {
	uint8_t *fb_addr;
	
	unsigned int xres;
	unsigned int yres;
	
	unsigned int scanline;
	unsigned int glyphscanline;
	
	unsigned int pixelbytes;
	unsigned int glyphbytes;
	
	rgb_conv_t rgb_conv;
} screen;

typedef struct {
	bool initialized;
	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;
	
	/* Text support in window */
	unsigned int cols;
	unsigned int rows;
	
	/* Style and glyphs for text printing */
	style_t style;
	uint8_t *glyphs;
	uint8_t *bgpixel;
	
	/* Auto-cursor position */
	bool cursor_active;
	unsigned int cur_col;
	unsigned int cur_row;
	bool cursor_shown;
	
	/* Back buffer */
	unsigned int bbsize;
	uint8_t *backbuf;
} viewport_t;

typedef struct {
	bool initialized;
	bool enabled;
	unsigned int vp;
	
	unsigned int pos;
	unsigned int animlen;
	unsigned int pixmaps[MAX_ANIM_LEN];
} animation_t;

static animation_t animations[MAX_ANIMATIONS];
static bool anims_enabled;

typedef struct {
	unsigned int width;
	unsigned int height;
	uint8_t *data;
} pixmap_t;

static pixmap_t pixmaps[MAX_PIXMAPS];
static viewport_t viewports[128];

static bool client_connected = false;  /**< Allow only 1 connection */

#define RED(x, bits)                 ((x >> (8 + 8 + 8 - bits)) & ((1 << bits) - 1))
#define GREEN(x, bits)               ((x >> (8 + 8 - bits)) & ((1 << bits) - 1))
#define BLUE(x, bits)                ((x >> (8 - bits)) & ((1 << bits) - 1))

#define COL2X(col)                   ((col) * FONT_WIDTH)
#define ROW2Y(row)                   ((row) * FONT_SCANLINES)

#define X2COL(x)                     ((x) / FONT_WIDTH)
#define Y2ROW(y)                     ((y) / FONT_SCANLINES)

#define FB_POS(x, y)                 ((y) * screen.scanline + (x) * screen.pixelbytes)
#define BB_POS(vport, col, row)      ((row) * vport->cols + (col))
#define GLYPH_POS(glyph, y, cursor)  (((glyph) + (cursor) * FONT_GLYPHS) * screen.glyphbytes + (y) * screen.glyphscanline)


/** ARGB 8:8:8:8 conversion
 *
 */
static void rgb_0888(void *dst, uint32_t rgb)
{
	*((uint32_t *) dst) = rgb & 0xffffff;
}


/** ABGR 8:8:8:8 conversion
 *
 */
static void bgr_0888(void *dst, uint32_t rgb)
{
	*((uint32_t *) dst)
	    = (BLUE(rgb, 8) << 16) | (GREEN(rgb, 8) << 8) | RED(rgb, 8);
}


/** BGR 8:8:8 conversion
 *
 */
static void rgb_888(void *dst, uint32_t rgb)
{
#if defined(FB_INVERT_ENDIAN)
	*((uint32_t *) dst)
	    = (BLUE(rgb, 8) << 16) | (GREEN(rgb, 8) << 8) | RED(rgb, 8)
	    | (*((uint32_t *) dst) & 0xff0000);
#else
	*((uint32_t *) dst)
	    = (rgb & 0xffffff) | (*((uint32_t *) dst) & 0xff0000);
#endif
}


/** RGB 5:5:5 conversion
 *
 */
static void rgb_555(void *dst, uint32_t rgb)
{
	*((uint16_t *) dst)
	    = (RED(rgb, 5) << 10) | (GREEN(rgb, 5) << 5) | BLUE(rgb, 5);
}


/** RGB 5:6:5 conversion
 *
 */
static void rgb_565(void *dst, uint32_t rgb)
{
	*((uint16_t *) dst)
	    = (RED(rgb, 5) << 11) | (GREEN(rgb, 6) << 5) | BLUE(rgb, 5);
}


/** RGB 3:2:3
 *
 */
static void rgb_323(void *dst, uint32_t rgb)
{
	*((uint8_t *) dst)
	    = ~((RED(rgb, 3) << 5) | (GREEN(rgb, 2) << 3) | BLUE(rgb, 3));
}


/** Redraw viewport
 *
 * @param vport Viewport to redraw
 *
 */
static void vport_redraw(viewport_t *vport)
{
	unsigned int row;
	
	for (row = 0; row < vport->rows; row++) {
		unsigned int y = vport->y + ROW2Y(row);
		unsigned int yd;
		
		for (yd = 0; yd < FONT_SCANLINES; yd++) {
			unsigned int x;
			unsigned int col;
			
			for (col = 0, x = vport->x; col < vport->cols; col++, x += FONT_WIDTH)
				memcpy(&screen.fb_addr[FB_POS(x, y + yd)],
			    &vport->glyphs[GLYPH_POS(vport->backbuf[BB_POS(vport, col, row)], yd, false)],
			    screen.glyphscanline);
		}
	}
	
	if (COL2X(vport->cols) < vport->width) {
		unsigned int y;
		
		for (y = 0; y < vport->height; y++) {
			unsigned int x;
			
			for (x = COL2X(vport->cols); x < vport->width; x++)
				memcpy(&screen.fb_addr[FB_POS(x, y)], vport->bgpixel, screen.pixelbytes);
		}
	}
	
	if (ROW2Y(vport->rows) < vport->height) {
		unsigned int y;
		
		for (y = ROW2Y(vport->rows); y < vport->height; y++) {
			unsigned int x;
			
			for (x = 0; x < vport->width; x++)
				memcpy(&screen.fb_addr[FB_POS(x, y)], vport->bgpixel, screen.pixelbytes);
		}
	}
}


/** Clear viewport
 *
 * @param vport Viewport to clear
 *
 */
static void vport_clear(viewport_t *vport)
{
	memset(vport->backbuf, 0, vport->bbsize);
	vport_redraw(vport);
}


/** Scroll viewport by given number of lines
 *
 * @param vport Viewport to scroll
 * @param lines Number of lines to scroll
 *
 */
static void vport_scroll(viewport_t *vport, int lines)
{
	unsigned int row;
	
	for (row = 0; row < vport->rows; row++) {
		unsigned int y = vport->y + ROW2Y(row);
		unsigned int yd;
		
		for (yd = 0; yd < FONT_SCANLINES; yd++) {
			unsigned int x;
			unsigned int col;
			
			for (col = 0, x = vport->x; col < vport->cols; col++, x += FONT_WIDTH) {
				uint8_t glyph;
				
				if ((row + lines >= 0) && (row + lines < vport->rows)) {
					if (vport->backbuf[BB_POS(vport, col, row)] == vport->backbuf[BB_POS(vport, col, row + lines)])
						continue;
					
					glyph = vport->backbuf[BB_POS(vport, col, row + lines)];
				} else
					glyph = 0;
				
				memcpy(&screen.fb_addr[FB_POS(x, y + yd)],
				    &vport->glyphs[GLYPH_POS(glyph, yd, false)], screen.glyphscanline);
			}
		}
	}
	
	if (lines > 0) {
		memcpy(vport->backbuf, vport->backbuf + vport->cols * lines, vport->cols * (vport->rows - lines));
		memset(&vport->backbuf[BB_POS(vport, 0, vport->rows - lines)], 0, vport->cols * lines);
	} else {
		memcpy(vport->backbuf - vport->cols * lines, vport->backbuf, vport->cols * (vport->rows + lines));
		memset(vport->backbuf, 0, - vport->cols * lines);
	}
}


/** Render glyphs
 *
 * Convert glyphs from device independent font
 * description to current visual representation.
 *
 * @param vport Viewport
 *
 */
static void render_glyphs(viewport_t* vport)
{
	unsigned int glyph;
	
	for (glyph = 0; glyph < FONT_GLYPHS; glyph++) {
		unsigned int y;
		
		for (y = 0; y < FONT_SCANLINES; y++) {
			unsigned int x;
			
			for (x = 0; x < FONT_WIDTH; x++) {
				screen.rgb_conv(&vport->glyphs[GLYPH_POS(glyph, y, false) + x * screen.pixelbytes],
				    (fb_font[glyph * FONT_SCANLINES + y] & (1 << (7 - x)))
				    ? vport->style.fg_color : vport->style.bg_color);
				
				uint32_t curcolor;
				
				if (y < FONT_SCANLINES - 2)
					curcolor =
					    (fb_font[glyph * FONT_SCANLINES + y] & (1 << (7 - x)))
					    ? vport->style.fg_color : vport->style.bg_color;
				else
					curcolor = vport->style.fg_color;
				
				screen.rgb_conv(&vport->glyphs[GLYPH_POS(glyph, y, true) + x * screen.pixelbytes], curcolor);
			}
		}
	}
	
	screen.rgb_conv(vport->bgpixel, vport->style.bg_color);
}


/** Create new viewport
 *
 * @param x      Origin of the viewport (x).
 * @param y      Origin of the viewport (y).
 * @param width  Width of the viewport.
 * @param height Height of the viewport.
 *
 * @return New viewport number.
 *
 */
static int vport_create(unsigned int x, unsigned int y,
    unsigned int width, unsigned int height)
{
	unsigned int i;
	
	for (i = 0; i < MAX_VIEWPORTS; i++) {
		if (!viewports[i].initialized)
			break;
	}
	if (i == MAX_VIEWPORTS)
		return ELIMIT;
	
	unsigned int cols = width / FONT_WIDTH;
	unsigned int rows = height / FONT_SCANLINES;
	unsigned int bbsize = cols * rows;
	unsigned int glyphsize = 2 * FONT_GLYPHS * screen.glyphbytes;
	
	uint8_t *backbuf = (uint8_t *) malloc(bbsize);
	if (!backbuf)
		return ENOMEM;
	
	uint8_t *glyphs = (uint8_t *) malloc(glyphsize);
	if (!glyphs) {
		free(backbuf);
		return ENOMEM;
	}
	
	uint8_t *bgpixel = (uint8_t *) malloc(screen.pixelbytes);
	if (!bgpixel) {
		free(glyphs);
		free(backbuf);
		return ENOMEM;
	}
	
	memset(backbuf, 0, bbsize);
	memset(glyphs, 0, glyphsize);
	memset(bgpixel, 0, screen.pixelbytes);
	
	viewports[i].x = x;
	viewports[i].y = y;
	viewports[i].width = width;
	viewports[i].height = height;
	
	viewports[i].cols = cols;
	viewports[i].rows = rows;
	
	viewports[i].style.bg_color = DEFAULT_BGCOLOR;
	viewports[i].style.fg_color = DEFAULT_FGCOLOR;
	
	viewports[i].glyphs = glyphs;
	viewports[i].bgpixel = bgpixel;
	
	viewports[i].cur_col = 0;
	viewports[i].cur_row = 0;
	viewports[i].cursor_active = false;
	viewports[i].cursor_shown = false;
	
	viewports[i].bbsize = bbsize;
	viewports[i].backbuf = backbuf;
	
	viewports[i].initialized = true;
	
	render_glyphs(&viewports[i]);
	
	return i;
}


/** Initialize framebuffer as a chardev output device
 *
 * @param addr   Address of the framebuffer
 * @param xres   Screen width in pixels
 * @param yres   Screen height in pixels
 * @param visual Bits per pixel (8, 16, 24, 32)
 * @param scan   Bytes per one scanline
 *
 */
static bool screen_init(void *addr, unsigned int xres, unsigned int yres,
    unsigned int scan, unsigned int visual)
{
	switch (visual) {
	case VISUAL_INDIRECT_8:
		screen.rgb_conv = rgb_323;
		screen.pixelbytes = 1;
		break;
	case VISUAL_RGB_5_5_5:
		screen.rgb_conv = rgb_555;
		screen.pixelbytes = 2;
		break;
	case VISUAL_RGB_5_6_5:
		screen.rgb_conv = rgb_565;
		screen.pixelbytes = 2;
		break;
	case VISUAL_RGB_8_8_8:
		screen.rgb_conv = rgb_888;
		screen.pixelbytes = 3;
		break;
	case VISUAL_RGB_8_8_8_0:
		screen.rgb_conv = rgb_888;
		screen.pixelbytes = 4;
		break;
	case VISUAL_RGB_0_8_8_8:
		screen.rgb_conv = rgb_0888;
		screen.pixelbytes = 4;
		break;
	case VISUAL_BGR_0_8_8_8:
		screen.rgb_conv = bgr_0888;
		screen.pixelbytes = 4;
		break;
	default:
		return false;
	}

	screen.fb_addr = (unsigned char *) addr;
	screen.xres = xres;
	screen.yres = yres;
	screen.scanline = scan;
	
	screen.glyphscanline = FONT_WIDTH * screen.pixelbytes;
	screen.glyphbytes = screen.glyphscanline * FONT_SCANLINES;
	
	/* Create first viewport */
	vport_create(0, 0, xres, yres);
	
	return true;
}


/** Draw glyph at given position relative to viewport 
 *
 * @param vport  Viewport identification
 * @param cursor Draw glyph with cursor
 * @param col    Screen position relative to viewport
 * @param row    Screen position relative to viewport
 *
 */
static void draw_glyph(viewport_t *vport, bool cursor, unsigned int col, unsigned int row)
{
	unsigned int x = vport->x + COL2X(col);
	unsigned int y = vport->y + ROW2Y(row);
	unsigned int yd;
	
	uint8_t glyph = vport->backbuf[BB_POS(vport, col, row)];
	
	for (yd = 0; yd < FONT_SCANLINES; yd++)
		memcpy(&screen.fb_addr[FB_POS(x, y + yd)],
		    &vport->glyphs[GLYPH_POS(glyph, yd, cursor)], screen.glyphscanline);
}


/** Hide cursor if it is shown
 *
 */
static void cursor_hide(viewport_t *vport)
{
	if ((vport->cursor_active) && (vport->cursor_shown)) {
		draw_glyph(vport, false, vport->cur_col, vport->cur_row);
		vport->cursor_shown = false;
	}
}


/** Show cursor if cursor showing is enabled
 *
 */
static void cursor_show(viewport_t *vport)
{
	/* Do not check for cursor_shown */
	if (vport->cursor_active) {
		draw_glyph(vport, true, vport->cur_col, vport->cur_row);
		vport->cursor_shown = true;
	}
}


/** Invert cursor, if it is enabled
 *
 */
static void cursor_blink(viewport_t *vport)
{
	if (vport->cursor_shown)
		cursor_hide(vport);
	else
		cursor_show(vport);
}


/** Draw character at given position relative to viewport
 *
 * @param vport  Viewport identification
 * @param c      Character to draw
 * @param col    Screen position relative to viewport
 * @param row    Screen position relative to viewport
 *
 */
static void draw_char(viewport_t *vport, uint8_t c, unsigned int col, unsigned int row)
{
	/* Do not hide cursor if we are going to overwrite it */
	if ((vport->cursor_active) && (vport->cursor_shown) &&
	    ((vport->cur_col != col) || (vport->cur_row != row)))
		cursor_hide(vport);
	
	uint8_t glyph = vport->backbuf[BB_POS(vport, col, row)];
	
	if (glyph != c) {
		vport->backbuf[BB_POS(vport, col, row)] = c;
		draw_glyph(vport, false, col, row);
	}
	
	vport->cur_col = col;
	vport->cur_row = row;
	
	vport->cur_col++;
	if (vport->cur_col >= vport->cols) {
		vport->cur_col = 0;
		vport->cur_row++;
		if (vport->cur_row >= vport->rows)
			vport->cur_row--;
	}
	
	cursor_show(vport);
}


/** Draw text data to viewport
 *
 * @param vport Viewport id
 * @param data  Text data fitting exactly into viewport
 *
 */
static void draw_text_data(viewport_t *vport, keyfield_t *data)
{
	unsigned int i;
	
	for (i = 0; i < vport->cols * vport->rows; i++) {
		unsigned int col = i % vport->cols;
		unsigned int row = i / vport->cols;
		
		uint8_t glyph = vport->backbuf[BB_POS(vport, col, row)];
		
		// TODO: use data[i].style
		
		if (glyph != data[i].character) {
			vport->backbuf[BB_POS(vport, col, row)] = data[i].character;
			draw_glyph(vport, false, col, row);
		}
	}
	cursor_show(vport);
}


static void putpixel_pixmap(void *data, unsigned int x, unsigned int y, uint32_t color)
{
	int pm = *((int *) data);
	pixmap_t *pmap = &pixmaps[pm];
	unsigned int pos = (y * pmap->width + x) * screen.pixelbytes;
	
	screen.rgb_conv(&pmap->data[pos], color);
}


static void putpixel(void *data, unsigned int x, unsigned int y, uint32_t color)
{
	viewport_t *vport = (viewport_t *) data;
	unsigned int dx = vport->x + x;
	unsigned int dy = vport->y + y;
	
	screen.rgb_conv(&screen.fb_addr[FB_POS(dx, dy)], color);
}


/** Return first free pixmap
 *
 */
static int find_free_pixmap(void)
{
	unsigned int i;
	
	for (i = 0; i < MAX_PIXMAPS; i++)
		if (!pixmaps[i].data)
			return i;
	
	return -1;
}


/** Create a new pixmap and return appropriate ID
 *
 */
static int shm2pixmap(unsigned char *shm, size_t size)
{
	int pm;
	pixmap_t *pmap;
	
	pm = find_free_pixmap();
	if (pm == -1)
		return ELIMIT;
	
	pmap = &pixmaps[pm];
	
	if (ppm_get_data(shm, size, &pmap->width, &pmap->height))
		return EINVAL;
	
	pmap->data = malloc(pmap->width * pmap->height * screen.pixelbytes);
	if (!pmap->data)
		return ENOMEM;
	
	ppm_draw(shm, size, 0, 0, pmap->width, pmap->height, putpixel_pixmap, (void *) &pm);
	
	return pm;
}


/** Handle shared memory communication calls
 *
 * Protocol for drawing pixmaps:
 * - FB_PREPARE_SHM(client shm identification)
 * - IPC_M_AS_AREA_SEND
 * - FB_DRAW_PPM(startx, starty)
 * - FB_DROP_SHM
 *
 * Protocol for text drawing
 * - IPC_M_AS_AREA_SEND
 * - FB_DRAW_TEXT_DATA
 *
 * @param callid Callid of the current call
 * @param call   Current call data
 * @param vp     Active viewport
 *
 * @return false if the call was not handled byt this function, true otherwise
 *
 * Note: this function is not threads safe, you would have
 * to redefine static variables with __thread
 *
 */
static bool shm_handle(ipc_callid_t callid, ipc_call_t *call, int vp)
{
	static keyfield_t *interbuffer = NULL;
	static size_t intersize = 0;
	
	static unsigned char *shm = NULL;
	static ipcarg_t shm_id = 0;
	static size_t shm_size;
	
	bool handled = true;
	int retval = EOK;
	viewport_t *vport = &viewports[vp];
	unsigned int x;
	unsigned int y;
	
	switch (IPC_GET_METHOD(*call)) {
	case IPC_M_SHARE_OUT:
		/* We accept one area for data interchange */
		if (IPC_GET_ARG1(*call) == shm_id) {
			void *dest = as_get_mappable_page(IPC_GET_ARG2(*call));
			shm_size = IPC_GET_ARG2(*call);
			if (!ipc_answer_1(callid, EOK, (sysarg_t) dest))
				shm = dest;
			else
				shm_id = 0;
			
			if (shm[0] != 'P')
				return false;
			
			return true;
		} else {
			intersize = IPC_GET_ARG2(*call);
			receive_comm_area(callid, call, (void *) &interbuffer);
		}
		return true;
	case FB_PREPARE_SHM:
		if (shm_id)
			retval = EBUSY;
		else 
			shm_id = IPC_GET_ARG1(*call);
		break;
		
	case FB_DROP_SHM:
		if (shm) {
			as_area_destroy(shm);
			shm = NULL;
		}
		shm_id = 0;
		break;
		
	case FB_SHM2PIXMAP:
		if (!shm) {
			retval = EINVAL;
			break;
		}
		retval = shm2pixmap(shm, shm_size);
		break;
	case FB_DRAW_PPM:
		if (!shm) {
			retval = EINVAL;
			break;
		}
		x = IPC_GET_ARG1(*call);
		y = IPC_GET_ARG2(*call);
		
		if ((x > vport->width) || (y > vport->height)) {
			retval = EINVAL;
			break;
		}
		
		ppm_draw(shm, shm_size, IPC_GET_ARG1(*call),
		    IPC_GET_ARG2(*call), vport->width - x, vport->height - y, putpixel, (void *) vport);
		break;
	case FB_DRAW_TEXT_DATA:
		if (!interbuffer) {
			retval = EINVAL;
			break;
		}
		if (intersize < vport->cols * vport->rows * sizeof(*interbuffer)) {
			retval = EINVAL;
			break;
		}
		draw_text_data(vport, interbuffer);
		break;
	default:
		handled = false;
	}
	
	if (handled)
		ipc_answer_0(callid, retval);
	return handled;
}


static void copy_vp_to_pixmap(viewport_t *vport, pixmap_t *pmap)
{
	unsigned int width = vport->width;
	unsigned int height = vport->height;
	
	if (width + vport->x > screen.xres)
		width = screen.xres - vport->x;
	if (height + vport->y > screen.yres)
		height = screen.yres - vport->y;
	
	unsigned int realwidth = pmap->width <= width ? pmap->width : width;
	unsigned int realheight = pmap->height <= height ? pmap->height : height;
	
	unsigned int srcrowsize = vport->width * screen.pixelbytes;
	unsigned int realrowsize = realwidth * screen.pixelbytes;
	
	unsigned int y;
	for (y = 0; y < realheight; y++) {
		unsigned int tmp = (vport->y + y) * screen.scanline + vport->x * screen.pixelbytes;
		memcpy(pmap->data + srcrowsize * y, screen.fb_addr + tmp, realrowsize);
	}
}


/** Save viewport to pixmap
 *
 */
static int save_vp_to_pixmap(viewport_t *vport)
{
	int pm;
	pixmap_t *pmap;
	
	pm = find_free_pixmap();
	if (pm == -1)
		return ELIMIT;
	
	pmap = &pixmaps[pm];
	pmap->data = malloc(screen.pixelbytes * vport->width * vport->height);
	if (!pmap->data)
		return ENOMEM;
	
	pmap->width = vport->width;
	pmap->height = vport->height;
	
	copy_vp_to_pixmap(vport, pmap);
	
	return pm;
}


/** Draw pixmap on screen
 *
 * @param vp Viewport to draw on
 * @param pm Pixmap identifier
 *
 */
static int draw_pixmap(int vp, int pm)
{
	pixmap_t *pmap = &pixmaps[pm];
	viewport_t *vport = &viewports[vp];
	
	unsigned int width = vport->width;
	unsigned int height = vport->height;
	
	if (width + vport->x > screen.xres)
		width = screen.xres - vport->x;
	if (height + vport->y > screen.yres)
		height = screen.yres - vport->y;
	
	if (!pmap->data)
		return EINVAL;
	
	unsigned int realwidth = pmap->width <= width ? pmap->width : width;
	unsigned int realheight = pmap->height <= height ? pmap->height : height;
	
	unsigned int srcrowsize = vport->width * screen.pixelbytes;
	unsigned int realrowsize = realwidth * screen.pixelbytes;
	
	unsigned int y;
	for (y = 0; y < realheight; y++) {
		unsigned int tmp = (vport->y + y) * screen.scanline + vport->x * screen.pixelbytes;
		memcpy(screen.fb_addr + tmp, pmap->data + y * srcrowsize, realrowsize);
	}
	
	return EOK;
}


/** Tick animation one step forward
 *
 */
static void anims_tick(void)
{
	unsigned int i;
	static int counts = 0;
	
	/* Limit redrawing */
	counts = (counts + 1) % 8;
	if (counts)
		return;

	for (i = 0; i < MAX_ANIMATIONS; i++) {
		if ((!animations[i].animlen) || (!animations[i].initialized) ||
		    (!animations[i].enabled))
			continue;
		
		draw_pixmap(animations[i].vp, animations[i].pixmaps[animations[i].pos]);
		animations[i].pos = (animations[i].pos + 1) % animations[i].animlen;
	}
}


static unsigned int pointer_x;
static unsigned int pointer_y;
static bool pointer_shown, pointer_enabled;
static int pointer_vport = -1;
static int pointer_pixmap = -1;


static void mouse_show(void)
{
	int i, j;
	int visibility;
	int color;
	int bytepos;
	
	if ((pointer_shown) || (!pointer_enabled))
		return;
	
	/* Save image under the cursor */
	if (pointer_vport == -1) {
		pointer_vport = vport_create(pointer_x, pointer_y, pointer_width, pointer_height);
		if (pointer_vport < 0)
			return;
	} else {
		viewports[pointer_vport].x = pointer_x;
		viewports[pointer_vport].y = pointer_y;
	}
	
	if (pointer_pixmap == -1)
		pointer_pixmap = save_vp_to_pixmap(&viewports[pointer_vport]);
	else
		copy_vp_to_pixmap(&viewports[pointer_vport], &pixmaps[pointer_pixmap]);
	
	/* Draw cursor */
	for (i = 0; i < pointer_height; i++)
		for (j = 0; j < pointer_width; j++) {
			bytepos = i * ((pointer_width - 1) / 8 + 1) + j / 8;
			visibility = pointer_mask_bits[bytepos] &
			    (1 << (j % 8));
			if (visibility) {
				color = pointer_bits[bytepos] &
				    (1 << (j % 8)) ? 0 : 0xffffff;
				if (pointer_x + j < screen.xres && pointer_y +
				    i < screen.yres)
					putpixel(&viewports[0], pointer_x + j,
					    pointer_y + i, color);
			}
		}
	pointer_shown = 1;
}


static void mouse_hide(void)
{
	/* Restore image under the cursor */
	if (pointer_shown) {
		draw_pixmap(pointer_vport, pointer_pixmap);
		pointer_shown = 0;
	}
}


static void mouse_move(unsigned int x, unsigned int y)
{
	mouse_hide();
	pointer_x = x;
	pointer_y = y;
	mouse_show();
}


static int anim_handle(ipc_callid_t callid, ipc_call_t *call, int vp)
{
	bool handled = true;
	int retval = EOK;
	int i, nvp;
	int newval;
	
	switch (IPC_GET_METHOD(*call)) {
	case FB_ANIM_CREATE:
		nvp = IPC_GET_ARG1(*call);
		if (nvp == -1)
			nvp = vp;
		if (nvp >= MAX_VIEWPORTS || nvp < 0 ||
			!viewports[nvp].initialized) {
			retval = EINVAL;
			break;
		}
		for (i = 0; i < MAX_ANIMATIONS; i++) {
			if (!animations[i].initialized)
				break;
		}
		if (i == MAX_ANIMATIONS) {
			retval = ELIMIT;
			break;
		}
		animations[i].initialized = 1;
		animations[i].animlen = 0;
		animations[i].pos = 0;
		animations[i].enabled = 0;
		animations[i].vp = nvp;
		retval = i;
		break;
	case FB_ANIM_DROP:
		i = IPC_GET_ARG1(*call);
		if (i >= MAX_ANIMATIONS || i < 0) {
			retval = EINVAL;
			break;
		}
		animations[i].initialized = 0;
		break;
	case FB_ANIM_ADDPIXMAP:
		i = IPC_GET_ARG1(*call);
		if (i >= MAX_ANIMATIONS || i < 0 ||
			!animations[i].initialized) {
			retval = EINVAL;
			break;
		}
		if (animations[i].animlen == MAX_ANIM_LEN) {
			retval = ELIMIT;
			break;
		}
		newval = IPC_GET_ARG2(*call);
		if (newval < 0 || newval > MAX_PIXMAPS ||
			!pixmaps[newval].data) {
			retval = EINVAL;
			break;
		}
		animations[i].pixmaps[animations[i].animlen++] = newval;
		break;
	case FB_ANIM_CHGVP:
		i = IPC_GET_ARG1(*call);
		if (i >= MAX_ANIMATIONS || i < 0) {
			retval = EINVAL;
			break;
		}
		nvp = IPC_GET_ARG2(*call);
		if (nvp == -1)
			nvp = vp;
		if (nvp >= MAX_VIEWPORTS || nvp < 0 ||
			!viewports[nvp].initialized) {
			retval = EINVAL;
			break;
		}
		animations[i].vp = nvp;
		break;
	case FB_ANIM_START:
	case FB_ANIM_STOP:
		i = IPC_GET_ARG1(*call);
		if (i >= MAX_ANIMATIONS || i < 0) {
			retval = EINVAL;
			break;
		}
		newval = (IPC_GET_METHOD(*call) == FB_ANIM_START);
		if (newval ^ animations[i].enabled) {
			animations[i].enabled = newval;
			anims_enabled += newval ? 1 : -1;
		}
		break;
	default:
		handled = 0;
	}
	if (handled)
		ipc_answer_0(callid, retval);
	return handled;
}


/** Handler for messages concerning pixmap handling
 *
 */
static int pixmap_handle(ipc_callid_t callid, ipc_call_t *call, int vp)
{
	bool handled = true;
	int retval = EOK;
	int i, nvp;
	
	switch (IPC_GET_METHOD(*call)) {
	case FB_VP_DRAW_PIXMAP:
		nvp = IPC_GET_ARG1(*call);
		if (nvp == -1)
			nvp = vp;
		if (nvp < 0 || nvp >= MAX_VIEWPORTS ||
			!viewports[nvp].initialized) {
			retval = EINVAL;
			break;
		}
		i = IPC_GET_ARG2(*call);
		retval = draw_pixmap(nvp, i);
		break;
	case FB_VP2PIXMAP:
		nvp = IPC_GET_ARG1(*call);
		if (nvp == -1)
			nvp = vp;
		if (nvp < 0 || nvp >= MAX_VIEWPORTS ||
			!viewports[nvp].initialized)
			retval = EINVAL;
		else
			retval = save_vp_to_pixmap(&viewports[nvp]);
		break;
	case FB_DROP_PIXMAP:
		i = IPC_GET_ARG1(*call);
		if (i >= MAX_PIXMAPS) {
			retval = EINVAL;
			break;
		}
		if (pixmaps[i].data) {
			free(pixmaps[i].data);
			pixmaps[i].data = NULL;
		}
		break;
	default:
		handled = 0;
	}
	
	if (handled)
		ipc_answer_0(callid, retval);
	return handled;
	
}

/** Function for handling connections to FB
 *
 */
static void fb_client_connection(ipc_callid_t iid, ipc_call_t *icall)
{
	unsigned int vp = 0;
	viewport_t *vport = &viewports[vp];
	
	if (client_connected) {
		ipc_answer_0(iid, ELIMIT);
		return;
	}
	
	/* Accept connection */
	client_connected = true;
	ipc_answer_0(iid, EOK);
	
	while (true) {
		ipc_callid_t callid;
		ipc_call_t call;
		int retval;
		unsigned int i;
		int scroll;
		uint8_t glyph;
		unsigned int row, col;
		
		if ((vport->cursor_active) || (anims_enabled))
			callid = async_get_call_timeout(&call, 250000);
		else
			callid = async_get_call(&call);
		
		mouse_hide();
		if (!callid) {
			cursor_blink(vport);
			anims_tick();
			mouse_show();
			continue;
		}
		
		if (shm_handle(callid, &call, vp))
			continue;
		
		if (pixmap_handle(callid, &call, vp))
			continue;
		
		if (anim_handle(callid, &call, vp))
			continue;
		
		switch (IPC_GET_METHOD(call)) {
		case IPC_M_PHONE_HUNGUP:
			client_connected = false;
			
			/* Cleanup other viewports */
			for (i = 1; i < MAX_VIEWPORTS; i++)
				vport->initialized = false;
			
			/* Exit thread */
			return;
		
		case FB_PUTCHAR:
			glyph = IPC_GET_ARG1(call);
			row = IPC_GET_ARG2(call);
			col = IPC_GET_ARG3(call);
			
			if ((col >= vport->cols) || (row >= vport->rows)) {
				retval = EINVAL;
				break;
			}
			ipc_answer_0(callid, EOK);
			
			draw_char(vport, glyph, col, row);
			
			/* Message already answered */
			continue;
		case FB_CLEAR:
			vport_clear(vport);
			cursor_show(vport);
			retval = EOK;
			break;
		case FB_CURSOR_GOTO:
			row = IPC_GET_ARG1(call);
			col = IPC_GET_ARG2(call);
			
			if ((col >= vport->cols) || (row >= vport->rows)) {
				retval = EINVAL;
				break;
			}
			retval = EOK;
			
			cursor_hide(vport);
			vport->cur_col = col;
			vport->cur_row = row;
			cursor_show(vport);
			break;
		case FB_CURSOR_VISIBILITY:
			cursor_hide(vport);
			vport->cursor_active = IPC_GET_ARG1(call);
			cursor_show(vport);
			retval = EOK;
			break;
		case FB_GET_CSIZE:
			ipc_answer_2(callid, EOK, vport->rows, vport->cols);
			continue;
		case FB_SCROLL:
			scroll = IPC_GET_ARG1(call);
			if ((scroll > (int) vport->rows) || (scroll < (-(int) vport->rows))) {
				retval = EINVAL;
				break;
			}
			cursor_hide(vport);
			vport_scroll(vport, scroll);
			cursor_show(vport);
			retval = EOK;
			break;
		case FB_VIEWPORT_SWITCH:
			i = IPC_GET_ARG1(call);
			if (i >= MAX_VIEWPORTS) {
				retval = EINVAL;
				break;
			}
			if (!viewports[i].initialized) {
				retval = EADDRNOTAVAIL;
				break;
			}
			cursor_hide(vport);
			vp = i;
			vport = &viewports[vp];
			cursor_show(vport);
			retval = EOK;
			break;
		case FB_VIEWPORT_CREATE:
			retval = vport_create(IPC_GET_ARG1(call) >> 16,
			    IPC_GET_ARG1(call) & 0xffff,
			    IPC_GET_ARG2(call) >> 16,
			    IPC_GET_ARG2(call) & 0xffff);
			break;
		case FB_VIEWPORT_DELETE:
			i = IPC_GET_ARG1(call);
			if (i >= MAX_VIEWPORTS) {
				retval = EINVAL;
				break;
			}
			if (!viewports[i].initialized) {
				retval = EADDRNOTAVAIL;
				break;
			}
			viewports[i].initialized = false;
			if (viewports[i].glyphs)
				free(viewports[i].glyphs);
			if (viewports[i].bgpixel)
				free(viewports[i].bgpixel);
			if (viewports[i].backbuf)
				free(viewports[i].backbuf);
			retval = EOK;
			break;
		case FB_SET_STYLE:
			vport->style.fg_color = IPC_GET_ARG1(call);
			vport->style.bg_color = IPC_GET_ARG2(call);
			render_glyphs(vport);
			retval = EOK;
			break;
		case FB_GET_RESOLUTION:
			ipc_answer_2(callid, EOK, screen.xres, screen.yres);
			continue;
		case FB_POINTER_MOVE:
			pointer_enabled = true;
			mouse_move(IPC_GET_ARG1(call), IPC_GET_ARG2(call));
			retval = EOK;
			break;
		default:
			retval = ENOENT;
		}
		ipc_answer_0(callid, retval);
	}
}

/** Initialization of framebuffer
 *
 */
int fb_init(void)
{
	async_set_client_connection(fb_client_connection);
	
	void *fb_ph_addr = (void *) sysinfo_value("fb.address.physical");
	unsigned int fb_offset = sysinfo_value("fb.offset");
	unsigned int fb_width = sysinfo_value("fb.width");
	unsigned int fb_height = sysinfo_value("fb.height");
	unsigned int fb_scanline = sysinfo_value("fb.scanline");
	unsigned int fb_visual = sysinfo_value("fb.visual");
	
	unsigned int fbsize = fb_scanline * fb_height;
	void *fb_addr = as_get_mappable_page(fbsize);
	
	physmem_map(fb_ph_addr + fb_offset, fb_addr,
	    ALIGN_UP(fbsize, PAGE_SIZE) >> PAGE_WIDTH, AS_AREA_READ | AS_AREA_WRITE);
	
	if (screen_init(fb_addr, fb_width, fb_height, fb_scanline, fb_visual))
		return 0;
	
	return -1;
}

/**
 * @}
 */
