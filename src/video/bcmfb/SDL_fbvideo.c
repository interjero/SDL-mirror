/* bcmfb based SDL video driver implementation.
*  SDL - Simple DirectMedia Layer
*  Copyright (C) 1997-2012 Sam Lantinga
*  
*  SDL bcmfb backend
*  Copyright (C) 2016 Emanuel Strobel
*/
#include "SDL_config.h"

/* Framebuffer console based SDL video driver implementation.
*/

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifndef HAVE_GETPAGESIZE 
#include <asm/page.h>		/* For definition of PAGE_SIZE */
#endif

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "SDL_fbvideo.h"
#include "SDL_fbmouse_c.h"
#include "SDL_fbevents_c.h"

//#define BCMFB_DEBUG
//#define BCMFB_ACCEL_DEBUG

/* A list of video resolutions that we query for (sorted largest to smallest) */
static const SDL_Rect checkres[] = {
	{  0, 0,  1920, 1080 },		// bcmfb
	{  0, 0,  1408, 1056 },		// bcmfb
	{  0, 0,  1280, 1024 },		// bcmfb
	{  0, 0,  1280,  720 },		// bcmfb
	{  0, 0,  1152,  864 },		// bcmfb
	{  0, 0,  1024,  768 },		// bcmfb
	{  0, 0,  960,   720 },		// bcmfb
	{  0, 0,  800,   600 },		// bcmfb
	{  0, 0,  768,   576 },		// bcmfb
	{  0, 0,  720,   576 },		// bcmfb PAL
	{  0, 0,  640,   480 },		// bcmfb 16 bpp: 0x111, or 273
	{  0, 0,  640,   400 },		// bcmfb  8 bpp: 0x100, or 256
	{  0, 0,  512,   384 },		// bcmfb
	{  0, 0,  320,   240 },		// bcmfb
	{  0, 0,  320,   200 }
};
static const struct {
	int xres;
	int yres;
	int pixclock;
	int left;
	int right;
	int upper;
	int lower;
	int hslen;
	int vslen;
	int sync;
	int vmode;
} BCMFB_timings[] = {
	// bcmfb U:320x200i-341 mode "320x200-170"
	{  320,  200, 60440,  32, 32, 20,  4,  48,  1, 0, 2 },	/* 170 Hz */
	// bcmfb U:320x200d-69 mode "320x200-140"
	{  320,  200, 79440,  16, 16, 20,  4,  48,  1, 0, 2 },	/* 140 Hz */
	// bcmfb 320x240d-75 
	{  320,  240, 63492,  16, 16, 16,  4,  48,  2, 0, 0 },	/* 75 Hz */
	// bcmfb U:512x384p-77 mode "512x384-78"
	{  512,  384, 49603,  48, 16, 16,  1,  64,  3, 0, 0 },	/* 78 Hz */
	// bcmfb U:640x400i-185 mode "640x400-92"
	{  640,  400, 28272,  48, 32, 17, 22, 128, 12, 0, 0 },	/* 92 Hz */
	// bcmfb U:640x480p-75 mode "640x480-75"
	{  640,  480, 31747, 120, 16, 16,  1,  64,  3, 0, 0 },	/* 75 Hz */
	// bcmfb U:720x576i-170 mode "720x576-85"
	{  720,  576, 20000,  64, 64, 32, 32,  64,  2, 0, 0 },	/* 85 Hz */
	// bcmfb U:768x576p-59 mode "768x576-60"
	{  768,  576, 26101, 144, 16, 28, 6,  112,  4, 0, 0 },	/* 60 Hz */
	// bcmfb U:800x600p-72 mode "800x600-72"
	{  800,  600, 20000,  64, 56, 23, 37, 120,  6, 0, 0 },	/* 72 Hz */
	// bcmfb U:960x720p-59 mode "960x720-60"
	{  960,  720, 17686, 144, 24, 28, 8,  122,  4, 0, 0 },	/* 60 Hz */
	// bcmfb U:1024x768p-70 mode "1024x768-70"
	{  1024, 768, 13333, 144, 24, 29, 3,  136,  6, 0, 0 },	/* 70 Hz */
	// bcmfb U:1152x864p-59 mode "1152x864-60"
	{  1152, 864, 12286, 192, 32, 30, 4,  128,  4, 0, 0 },	/* 60 Hz */
	// bcmfb U:1280x720i-86 mode "1280x720-43"
	{  1280, 720, 20000, 64,  64, 32, 32,  64,  2, 0, 0 },	/* 43 Hz */
	// bcmfb U:1280x1024p-59 mode "1280x1024-60"
	{  1280, 1024, 9369, 224, 32, 32, 4,  136,  4, 0, 0 },	/* 60 Hz */
	// bcmfb U:1408x1056p-59 mode mode "1408x1056-60"
	{  1408, 1056, 8214, 256, 40, 32, 5,  144,  5, 0, 0 },	/* 60 Hz */
	// bcmfb U:1920x1080i-41 mode "1920x1080-21"
	{  1920, 1080, 20000, 64, 64, 32, 32,  64,  2, 0, 0 }	/* 21 Hz */
};
enum {
	BCMFB_ROTATE_NONE = 0,
	BCMFB_ROTATE_CCW = 90,
	BCMFB_ROTATE_UD = 180,
	BCMFB_ROTATE_CW = 270
};

#define min(a,b) ((a)<(b)?(a):(b))

/* Initialization/Query functions */
static int BCMFB_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **BCMFB_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *BCMFB_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int BCMFB_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void BCMFB_VideoQuit(_THIS);

/* Hardware surface functions */
static int BCMFB_InitHWSurfaces(_THIS, SDL_Surface *screen, char *base, int size);
static void BCMFB_FreeHWSurfaces(_THIS);
static int BCMFB_AllocHWSurface(_THIS, SDL_Surface *surface);
static int BCMFB_LockHWSurface(_THIS, SDL_Surface *surface);
static void BCMFB_UnlockHWSurface(_THIS, SDL_Surface *surface);
static void BCMFB_FreeHWSurface(_THIS, SDL_Surface *surface);
static void BCMFB_WaitVBL(_THIS);
static void BCMFB_WaitIdle(_THIS);
static int BCMFB_FlipHWSurface(_THIS, SDL_Surface *surface);

/* Internal palette functions */
static void BCMFB_SavePalette(_THIS, struct fb_fix_screeninfo *finfo,
                                  struct fb_var_screeninfo *vinfo);
static void BCMFB_RestorePalette(_THIS);

/* Shadow buffer functions */
static BCMFB_bitBlit BCMFB_blit16;
static BCMFB_bitBlit BCMFB_blit16blocked;

static int SDL_getpagesize(void)
{
#ifdef HAVE_GETPAGESIZE
	return getpagesize();
#elif defined(PAGE_SIZE)
	return PAGE_SIZE;
#else
#error Can not determine system page size.
	return 4096;  /* this is what it USED to be in Linux... */
#endif
}

/* Small wrapper for mmap() so we can play nicely with no-mmu hosts
 * (non-mmu hosts disallow the MAP_SHARED flag) */

static void *do_mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
	void *ret;
	ret = mmap(start, length, prot, flags, fd, offset);
	if ( ret == (char *)-1 && (flags & MAP_SHARED) ) {
		ret = mmap(start, length, prot,
		           (flags & ~MAP_SHARED) | MAP_PRIVATE, fd, offset);
	}
	return ret;
}

/* FB driver bootstrap functions */

static int BCMFB_Available(void)
{
	int console = -1;
	/* Added check for /fb/0 (devfs) */
	/* but - use environment variable first... if it fails, still check defaults */
	int idx = 0;
	const char *SDL_fbdevs[4] = { NULL, "/dev/fb0", "/dev/fb/0", NULL };

	SDL_fbdevs[0] = SDL_getenv("SDL_FBDEV");
	if( !SDL_fbdevs[0] )
		idx++;
	for( ; SDL_fbdevs[idx]; idx++ )
	{
		console = open(SDL_fbdevs[idx], O_RDWR, 0);
		if ( console >= 0 ) {
			close(console);
			break;
		}
	}
	return(console >= 0);
}

static void BCMFB_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_VideoDevice *BCMFB_CreateDevice(int devindex)
{
	int i;
	SDL_VideoDevice *this;
	/* Initialize all variables that we clean on shutdown */
	this = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if ( this ) {
		SDL_memset(this, 0, (sizeof *this));
		this->hidden = (struct SDL_PrivateVideoData *)
				SDL_malloc((sizeof *this->hidden));
	}
	if ( (this == NULL) || (this->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( this ) {
			SDL_free(this);
		}
		return(0);
	}
	SDL_memset(this->hidden, 0, (sizeof *this->hidden));
	for (i = 0; i < 32; i++) {
		if (kbd_fd[i] != -1) {
			close(kbd_fd[i]);
		}
		kbd_fd[i] = -1;
	}

	for (i = 0; i < 32; i++) {
		mice_fd[i] = -1;
	}
	
	m_index = -1;
	kbd_index = -1;
	panel = -1;
	kbd = -1;
	arc = -1;
	rc = -1;
	
	wait_vbl = BCMFB_WaitVBL;
	wait_idle = BCMFB_WaitIdle;

	/* Set the function pointers */
	this->VideoInit = BCMFB_VideoInit;
	this->ListModes = BCMFB_ListModes;
	this->SetVideoMode = BCMFB_SetVideoMode;
	this->SetColors = BCMFB_SetColors;
	this->UpdateRects = NULL;
	this->VideoQuit = BCMFB_VideoQuit;
	this->AllocHWSurface = BCMFB_AllocHWSurface;
	this->CheckHWBlit = NULL;
	this->FillHWRect = NULL;
	this->SetHWColorKey = NULL;
	this->SetHWAlpha = NULL;
	this->LockHWSurface = BCMFB_LockHWSurface;
	this->UnlockHWSurface = BCMFB_UnlockHWSurface;
	this->FlipHWSurface = BCMFB_FlipHWSurface;
	this->FreeHWSurface = BCMFB_FreeHWSurface;
	this->SetCaption = NULL;
	this->SetIcon = NULL;
	this->IconifyWindow = NULL;
	this->GrabInput = NULL;
	this->GetWMInfo = NULL;
	this->InitOSKeymap = BCMFB_InitOSKeymap;
	this->PumpEvents = BCMFB_PumpEvents;

	this->free = BCMFB_DeleteDevice;

	return this;
}

VideoBootStrap BCMFB_bootstrap = {
	"bcmfb", "BCMFB Framebuffer Console",
	BCMFB_Available, BCMFB_CreateDevice
};

#define BCMFB_MODES_DB	"/etc/bcmfb.modes"

static int read_bcmfb_modes_line(FILE*f, char* line, int length)
{
	int blank;
	char* c;
	int i;
	
	blank=0;
	/* find a relevant line */
	do
	{
		if (!fgets(line,length,f))
			return 0;
		c=line;
		while(((*c=='\t')||(*c==' '))&&(*c!=0))
			c++;
		
		if ((*c=='\n')||(*c=='#')||(*c==0))
			blank=1;
		else
			blank=0;
	}
	while(blank);
	/* remove whitespace at the begining of the string */
	i=0;
	do
	{
		line[i]=c[i];
		i++;
	}
	while(c[i]!=0);
	return 1;
}

static int read_bcmfb_conf_mode(FILE *f, struct fb_var_screeninfo *vinfo)
{
	char line[1024];
	char option[256];

	/* Find a "geometry" */
	do {
		if (read_bcmfb_modes_line(f, line, sizeof(line))==0)
			return 0;
		if (SDL_strncmp(line,"geometry",8)==0)
			break;
	}
	while(1);

	SDL_sscanf(line, "geometry %d %d %d %d %d", &vinfo->xres, &vinfo->yres, 
			&vinfo->xres_virtual, &vinfo->yres_virtual, &vinfo->bits_per_pixel);
	if (read_bcmfb_modes_line(f, line, sizeof(line))==0)
		return 0;
			
	SDL_sscanf(line, "timings %d %d %d %d %d %d %d", &vinfo->pixclock, 
			&vinfo->left_margin, &vinfo->right_margin, &vinfo->upper_margin, 
			&vinfo->lower_margin, &vinfo->hsync_len, &vinfo->vsync_len);
		
	vinfo->sync=0;
	vinfo->vmode=FB_VMODE_NONINTERLACED;
				
	/* Parse misc options */
	do {
		if (read_bcmfb_modes_line(f, line, sizeof(line))==0)
			return 0;

		if (SDL_strncmp(line,"hsync",5)==0) {
			SDL_sscanf(line,"hsync %s",option);
			if (SDL_strncmp(option,"high",4)==0)
				vinfo->sync |= FB_SYNC_HOR_HIGH_ACT;
		}
		else if (SDL_strncmp(line,"vsync",5)==0) {
			SDL_sscanf(line,"vsync %s",option);
			if (SDL_strncmp(option,"high",4)==0)
				vinfo->sync |= FB_SYNC_VERT_HIGH_ACT;
		}
		else if (SDL_strncmp(line,"csync",5)==0) {
			SDL_sscanf(line,"csync %s",option);
			if (SDL_strncmp(option,"high",4)==0)
				vinfo->sync |= FB_SYNC_COMP_HIGH_ACT;
		}
		else if (SDL_strncmp(line,"extsync",5)==0) {
			SDL_sscanf(line,"extsync %s",option);
			if (SDL_strncmp(option,"true",4)==0)
				vinfo->sync |= FB_SYNC_EXT;
		}
		else if (SDL_strncmp(line,"laced",5)==0) {
			SDL_sscanf(line,"laced %s",option);
			if (SDL_strncmp(option,"true",4)==0)
				vinfo->vmode |= FB_VMODE_INTERLACED;
		}
		else if (SDL_strncmp(line,"double",6)==0) {
			SDL_sscanf(line,"double %s",option);
			if (SDL_strncmp(option,"true",4)==0)
				vinfo->vmode |= FB_VMODE_DOUBLE;
		}
	}
	while(SDL_strncmp(line,"endmode",7)!=0);

	return 1;
}

static int BCMFB_CheckMode(_THIS, struct fb_var_screeninfo *vinfo,
                        int index, unsigned int *w, unsigned int *h)
{
	int mode_okay;

	mode_okay = 0;
	vinfo->bits_per_pixel = (index+1)*8;
	vinfo->height = 0;
	vinfo->width = 0;
	vinfo->xres = vinfo->xres_virtual = *w;
	vinfo->yres = vinfo->yres_virtual = *h;
	vinfo->activate = FB_ACTIVATE_TEST;
	vinfo->xoffset = vinfo->yoffset = 0;
	switch(vinfo->bits_per_pixel) {
		case 32:
			vinfo->transp.offset = 0;
			vinfo->transp.length = 8;
			vinfo->red.offset = 8;
			vinfo->red.length = 8;
			vinfo->green.offset = 16;
			vinfo->green.length = 8;
			vinfo->blue.offset = 24;
			vinfo->blue.length = 8;
			break;
		case 24:
			vinfo->transp.offset = 0;
			vinfo->transp.length = 0;
			vinfo->red.offset = 0;
			vinfo->red.length = 8;
			vinfo->green.offset = 8;
			vinfo->green.length = 8;
			vinfo->blue.offset = 16;
			vinfo->blue.length = 8;
			break;
		case 16:
			vinfo->red.offset = 0;
			vinfo->red.length = 5;
			vinfo->green.offset = 5;
			vinfo->green.length = 6;
			vinfo->blue.offset = 11;
			vinfo->blue.length = 5;
			vinfo->transp.offset = 16;
			vinfo->transp.length = 0;
			break;
		case 8:
			vinfo->red.offset = 0;
			vinfo->red.length = 3;
			vinfo->green.offset = 3;
			vinfo->green.length = 3;
			vinfo->blue.offset = 4;
			vinfo->blue.length = 2;
			vinfo->transp.offset = 8;
			vinfo->transp.length = 0;
			break;
	}
	if ( ioctl(console_fd, FBIOPUT_VSCREENINFO, vinfo) == 0 ) {
#ifdef BCMFB_DEBUG
		fprintf(stderr, "Checked mode %dx%d at %d bpp, got mode %dx%d at %d bpp\n", *w, *h, (index+1)*8, vinfo->xres, vinfo->yres, vinfo->bits_per_pixel);
#endif
		if ( (((vinfo->bits_per_pixel+7)/8)-1) == index ) {
			*w = vinfo->xres;
			*h = vinfo->yres;
			mode_okay = 1;
		}
	}
	return mode_okay;
}

static int BCMFB_AddMode(_THIS, int index, unsigned int w, unsigned int h, int check_timings)
{
	SDL_Rect *mode;
	int i;
	int next_mode;

	/* Check to see if we already have this mode */
	if ( SDL_nummodes[index] > 0 ) {
		mode = SDL_modelist[index][SDL_nummodes[index]-1];
		if ( (mode->w == w) && (mode->h == h) ) {
#ifdef BCMFB_DEBUG
			fprintf(stderr, "We already have mode %dx%d at %d bytes per pixel\n", w, h, index+1);
#endif
			return(0);
		}
	}

	/* Only allow a mode if we have a valid timing for it */
	if ( check_timings ) {
		int found_timing = 0;
		for ( i=0; i<(sizeof(BCMFB_timings)/sizeof(BCMFB_timings[0])); ++i ) {
			if ( (w == BCMFB_timings[i].xres) &&
			     (h == BCMFB_timings[i].yres) && BCMFB_timings[i].pixclock ) {
				found_timing = 1;
				break;
			}
		}
		if ( !found_timing ) {
#ifdef BCMFB_DEBUG
			fprintf(stderr, "No valid timing line for mode %dx%d\n", w, h);
#endif
			return(0);
		}
	}

	/* Set up the new video mode rectangle */
	mode = (SDL_Rect *)SDL_malloc(sizeof *mode);
	if ( mode == NULL ) {
		SDL_OutOfMemory();
		return(-1);
	}
	mode->x = 0;
	mode->y = 0;
	mode->w = w;
	mode->h = h;
#ifdef BCMFB_DEBUG
	fprintf(stderr, "Adding mode %dx%d at %d bytes per pixel\n", w, h, index+1);
#endif

	/* Allocate the new list of modes, and fill in the new mode */
	next_mode = SDL_nummodes[index];
	SDL_modelist[index] = (SDL_Rect **)
	       SDL_realloc(SDL_modelist[index], (1+next_mode+1)*sizeof(SDL_Rect *));
	if ( SDL_modelist[index] == NULL ) {
		SDL_OutOfMemory();
		SDL_nummodes[index] = 0;
		SDL_free(mode);
		return(-1);
	}
	SDL_modelist[index][next_mode] = mode;
	SDL_modelist[index][next_mode+1] = NULL;
	SDL_nummodes[index]++;

	return(0);
}

static int cmpmodes(const void *va, const void *vb)
{
    const SDL_Rect *a = *(const SDL_Rect**)va;
    const SDL_Rect *b = *(const SDL_Rect**)vb;
    if ( a->h == b->h )
        return b->w - a->w;
    else
        return b->h - a->h;
}

static void BCMFB_SortModes(_THIS)
{
	int i;
	for ( i=0; i<NUM_MODELISTS; ++i ) {
		if ( SDL_nummodes[i] > 0 ) {
			SDL_qsort(SDL_modelist[i], SDL_nummodes[i], sizeof *SDL_modelist[i], cmpmodes);
		}
	}
}

static int BCMFB_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
	const int pagesize = SDL_getpagesize();
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	int i, j, current_index;
	unsigned int current_w, current_h;
	const char *SDL_fbdev;
	const char *rotation;
	FILE *modesdb;

	/* Initialize the library */
	SDL_fbdev = SDL_getenv("SDL_FBDEV");
	if ( SDL_fbdev == NULL ) {
		SDL_fbdev = "/dev/fb0";
	}
	console_fd = open(SDL_fbdev, O_RDWR, 0);
	if ( console_fd < 0 ) {
		SDL_SetError("Unable to open %s", SDL_fbdev);
		return(-1);
	}

#if !SDL_THREADS_DISABLED
	/* Create the hardware surface lock mutex */
	hw_lock = SDL_CreateMutex();
	if ( hw_lock == NULL ) {
		SDL_SetError("Unable to create lock mutex");
		BCMFB_VideoQuit(this);
		return(-1);
	}
#endif

	/* Get the type of video hardware */
	if ( ioctl(console_fd, FBIOGET_FSCREENINFO, &finfo) < 0 ) {
		SDL_SetError("Couldn't get console hardware info");
		BCMFB_VideoQuit(this);
		return(-1);
	}
	switch (finfo.type) {
		case FB_TYPE_PACKED_PIXELS:
			/* Supported, no worries.. */
			break;
		default:
			SDL_SetError("Unsupported console hardware");
			BCMFB_VideoQuit(this);
			return(-1);
	}
	switch (finfo.visual) {
		case FB_VISUAL_TRUECOLOR:
		case FB_VISUAL_PSEUDOCOLOR:
		case FB_VISUAL_STATIC_PSEUDOCOLOR:
		case FB_VISUAL_DIRECTCOLOR:
			break;
		default:
			SDL_SetError("Unsupported console hardware");
			BCMFB_VideoQuit(this);
			return(-1);
	}

	mapped_memlen = finfo.smem_len;

	/* Determine the current screen depth */
	if ( ioctl(console_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ) {
		SDL_SetError("Couldn't get console pixel format");
		BCMFB_VideoQuit(this);
		return(-1);
	}
	vformat->BitsPerPixel = vinfo.bits_per_pixel;
	if ( vformat->BitsPerPixel < 8 ) {
		/* Assuming VGA16, we handle this via a shadow framebuffer */
		vformat->BitsPerPixel = 8;
	}
	for ( i=0; i<vinfo.transp.length; ++i ) {
		vformat->Amask <<= 1;
		vformat->Amask |= (0x00000001<<vinfo.transp.offset);
	}
	for ( i=0; i<vinfo.red.length; ++i ) {
		vformat->Rmask <<= 1;
		vformat->Rmask |= (0x00000001<<vinfo.red.offset);
	}
	for ( i=0; i<vinfo.green.length; ++i ) {
		vformat->Gmask <<= 1;
		vformat->Gmask |= (0x00000001<<vinfo.green.offset);
	}
	for ( i=0; i<vinfo.blue.length; ++i ) {
		vformat->Bmask <<= 1;
		vformat->Bmask |= (0x00000001<<vinfo.blue.offset);
	}
	saved_vinfo = vinfo;

	/* Save hardware palette, if needed */
	BCMFB_SavePalette(this, &finfo, &vinfo);

	/* If the I/O registers are available, memory map them so we
	   can take advantage of any supported hardware acceleration.
	 */
	vinfo.accel_flags = 0;	/* Temporarily reserve registers */
	ioctl(console_fd, FBIOPUT_VSCREENINFO, &vinfo);
	
	rotate = BCMFB_ROTATE_NONE;
	rotation = SDL_getenv("SDL_VIDEO_BCMFB_ROTATION");
	if (rotation != NULL) {
		if (SDL_strlen(rotation) == 0) {
			shadow_fb = 0;
			rotate = BCMFB_ROTATE_NONE;
#ifdef BCMFB_DEBUG
			printf("Not rotating, no shadow\n");
#endif
		} else if (!SDL_strcmp(rotation, "NONE")) {
			shadow_fb = 1;
			rotate = BCMFB_ROTATE_NONE;
#ifdef BCMFB_DEBUG
			printf("Not rotating, but still using shadow\n");
#endif
		} else if (!SDL_strcmp(rotation, "CW")) {
			shadow_fb = 1;
			rotate = BCMFB_ROTATE_CW;
#ifdef BCMFB_DEBUG
			printf("Rotating screen clockwise\n");
#endif
		} else if (!SDL_strcmp(rotation, "CCW")) {
			shadow_fb = 1;
			rotate = BCMFB_ROTATE_CCW;
#ifdef BCMFB_DEBUG
			printf("Rotating screen counter clockwise\n");
#endif
		} else if (!SDL_strcmp(rotation, "UD")) {
			shadow_fb = 1;
			rotate = BCMFB_ROTATE_UD;
#ifdef BCMFB_DEBUG
			printf("Rotating screen upside down\n");
#endif
		} else {
			SDL_SetError("\"%s\" is not a valid value for "
				 "SDL_VIDEO_BCMFB_ROTATION", rotation);
			return(-1);
		}
	}

	if (rotate == BCMFB_ROTATE_CW || rotate == BCMFB_ROTATE_CCW) {
		current_w = vinfo.yres;
		current_h = vinfo.xres;
	} else {
		current_w = vinfo.xres;
		current_h = vinfo.yres;
	}

	/* Query for the list of available video modes */
	current_index = ((vinfo.bits_per_pixel+7)/8)-1;
	modesdb = fopen(BCMFB_MODES_DB, "r");
	for ( i=0; i<NUM_MODELISTS; ++i ) {
		SDL_nummodes[i] = 0;
		SDL_modelist[i] = NULL;
	}
	if ( SDL_getenv("SDL_BCMFB_BROKEN_MODES") != NULL ) {
		BCMFB_AddMode(this, current_index, current_w, current_h, 0);
	} else if(modesdb) {
		while ( read_bcmfb_conf_mode(modesdb, &vinfo) ) {
			for ( i=0; i<NUM_MODELISTS; ++i ) {
				unsigned int w, h;

				if (rotate == BCMFB_ROTATE_CW || rotate == BCMFB_ROTATE_CCW) {
					w = vinfo.yres;
					h = vinfo.xres;
				} else {
					w = vinfo.xres;
					h = vinfo.yres;
				}
				/* See if we are querying for the current mode */
				if ( i == current_index ) {
					if ( (current_w > w) || (current_h > h) ) {
						/* Only check once */
						BCMFB_AddMode(this, i, current_w, current_h, 0);
						current_index = -1;
					}
				}
				if ( BCMFB_CheckMode(this, &vinfo, i, &w, &h) ) {
					BCMFB_AddMode(this, i, w, h, 0);
				}
			}
		}
		fclose(modesdb);
		BCMFB_SortModes(this);
	} else {
		for ( i=0; i<NUM_MODELISTS; ++i ) {
			for ( j=0; j<(sizeof(checkres)/sizeof(checkres[0])); ++j ) {
				unsigned int w, h;

				if (rotate == BCMFB_ROTATE_CW || rotate == BCMFB_ROTATE_CCW) {
					w = checkres[j].h;
					h = checkres[j].w;
				} else {
					w = checkres[j].w;
					h = checkres[j].h;
				}
				/* See if we are querying for the current mode */
				if ( i == current_index ) {
					if ( (current_w > w) || (current_h > h) ) {
						/* Only check once */
						BCMFB_AddMode(this, i, current_w, current_h, 0);
						current_index = -1;
					}
				}
				if ( BCMFB_CheckMode(this, &vinfo, i, &w, &h) ) {
					BCMFB_AddMode(this, i, w, h, 1);
				}
			}
		}
	}

	this->info.current_w = current_w;
	this->info.current_h = current_h;
	this->info.wm_available = 0;
	this->info.hw_available = !shadow_fb;
	this->info.video_mem = shadow_fb ? 0 : finfo.smem_len/1024;
	
	if (shadow_fb) {
		shadow_mem = (char *)SDL_malloc(mapped_memlen);
		if (shadow_mem == NULL) {
			SDL_SetError("BCMFB no memory for shadow");
			return (-1);
		} 
	}

	/* Enable mouse and keyboard support */
	if ( BCMFB_OpenKeyboard(this) < 0 ) {
		BCMFB_VideoQuit(this);
		return(-1);
	}
	if ( BCMFB_OpenMouse(this) < 0 ) {
		const char *sdl_nomouse;
		sdl_nomouse = SDL_getenv("SDL_NOMOUSE");
		if ( ! sdl_nomouse ) {
			SDL_SetError("Unable to open mouse");
			BCMFB_VideoQuit(this);
			return(-1);
		}
	}

	/* We're done! */
	return(0);
}

static SDL_Rect **BCMFB_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	return(SDL_modelist[((format->BitsPerPixel+7)/8)-1]);
}

/* Various screen update functions available */
static void BCMFB_DirectUpdate(_THIS, int numrects, SDL_Rect *rects);

#ifdef BCMFB_DEBUG
static void print_vinfo(struct fb_var_screeninfo *vinfo)
{
	fprintf(stderr, "Printing vinfo:\n");
	fprintf(stderr, "\txres: %d\n", vinfo->xres);
	fprintf(stderr, "\tyres: %d\n", vinfo->yres);
	fprintf(stderr, "\txres_virtual: %d\n", vinfo->xres_virtual);
	fprintf(stderr, "\tyres_virtual: %d\n", vinfo->yres_virtual);
	fprintf(stderr, "\txoffset: %d\n", vinfo->xoffset);
	fprintf(stderr, "\tyoffset: %d\n", vinfo->yoffset);
	fprintf(stderr, "\tbits_per_pixel: %d\n", vinfo->bits_per_pixel);
	fprintf(stderr, "\tgrayscale: %d\n", vinfo->grayscale);
	fprintf(stderr, "\tnonstd: %d\n", vinfo->nonstd);
	fprintf(stderr, "\tactivate: %d\n", vinfo->activate);
	fprintf(stderr, "\theight: %d\n", vinfo->height);
	fprintf(stderr, "\twidth: %d\n", vinfo->width);
	fprintf(stderr, "\taccel_flags: %d\n", vinfo->accel_flags);
	fprintf(stderr, "\tpixclock: %d\n", vinfo->pixclock);
	fprintf(stderr, "\tleft_margin: %d\n", vinfo->left_margin);
	fprintf(stderr, "\tright_margin: %d\n", vinfo->right_margin);
	fprintf(stderr, "\tupper_margin: %d\n", vinfo->upper_margin);
	fprintf(stderr, "\tlower_margin: %d\n", vinfo->lower_margin);
	fprintf(stderr, "\thsync_len: %d\n", vinfo->hsync_len);
	fprintf(stderr, "\tvsync_len: %d\n", vinfo->vsync_len);
	fprintf(stderr, "\tsync: %d\n", vinfo->sync);
	fprintf(stderr, "\tvmode: %d\n", vinfo->vmode);
	fprintf(stderr, "\tred: %d/%d\n", vinfo->red.length, vinfo->red.offset);
	fprintf(stderr, "\tgreen: %d/%d\n", vinfo->green.length, vinfo->green.offset);
	fprintf(stderr, "\tblue: %d/%d\n", vinfo->blue.length, vinfo->blue.offset);
	fprintf(stderr, "\talpha: %d/%d\n", vinfo->transp.length, vinfo->transp.offset);
}
static void print_finfo(struct fb_fix_screeninfo *finfo)
{
	fprintf(stderr, "Printing finfo:\n");
	fprintf(stderr, "\tsmem_start = %p\n", (char *)finfo->smem_start);
	fprintf(stderr, "\tsmem_len = %d\n", finfo->smem_len);
	fprintf(stderr, "\ttype = %d\n", finfo->type);
	fprintf(stderr, "\ttype_aux = %d\n", finfo->type_aux);
	fprintf(stderr, "\tvisual = %d\n", finfo->visual);
	fprintf(stderr, "\txpanstep = %d\n", finfo->xpanstep);
	fprintf(stderr, "\typanstep = %d\n", finfo->ypanstep);
	fprintf(stderr, "\tywrapstep = %d\n", finfo->ywrapstep);
	fprintf(stderr, "\tline_length = %d\n", finfo->line_length);
	fprintf(stderr, "\tmmio_start = %p\n", (char *)finfo->mmio_start);
	fprintf(stderr, "\tmmio_len = %d\n", finfo->mmio_len);
	fprintf(stderr, "\taccel = %d\n", finfo->accel);
}
#endif

static int choose_bcmfb_conf_mode(struct fb_var_screeninfo *vinfo)
{
	int matched;
	FILE *modesdb;
	struct fb_var_screeninfo cinfo;

	matched = 0;
	modesdb = fopen(BCMFB_MODES_DB, "r");
	if ( modesdb ) {
		/* Parse the mode definition file */
		while ( read_bcmfb_conf_mode(modesdb, &cinfo) ) {
			if ( (vinfo->xres == cinfo.xres && vinfo->yres == cinfo.yres) &&
			     (!matched || (vinfo->bits_per_pixel == cinfo.bits_per_pixel)) ) {
				if ( matched ) {
					break;
				}
				matched = 1;
			}
		}
		fclose(modesdb);
	}
	return(matched);
}

static int choose_bcmfb_mode(struct fb_var_screeninfo *vinfo)
{
	int matched;
	int i;

	/* Check for BCMFB timings */
	matched = 0;
	for ( i=0; i<(sizeof(BCMFB_timings)/sizeof(BCMFB_timings[0])); ++i ) {
		if ( (vinfo->xres == BCMFB_timings[i].xres) &&
		     (vinfo->yres == BCMFB_timings[i].yres) ) {
#ifdef BCMFB_DEBUG
			fprintf(stderr, "BCMFB using %dx%d\n",
						vinfo->xres, vinfo->yres);
#endif
			matched = 1;
			break;
		}
	}
	return(matched);
}

static SDL_Surface *BCMFB_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	int i;
	Uint32 Rmask;
	Uint32 Gmask;
	Uint32 Bmask;
	Uint32 Amask;
	char *surfaces_mem;
	int surfaces_len;

	/* Set the terminal into graphics mode */
	if ( BCMFB_EnterGraphicsMode(this) < 0 ) {
		return(NULL);
	}

	/* Restore the original palette */
	BCMFB_RestorePalette(this);

	/* Set the video mode and get the final screen format */
	if ( ioctl(console_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ) {
		SDL_SetError("BCMFB couldn't get console screen info");
		return(NULL);
	}
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB original vinfo:\n");
	print_vinfo(&vinfo);
#endif
	/* Do not use double buffering with shadow buffer */
	if (shadow_fb) {
		flags &= ~SDL_DOUBLEBUF;
	}

	if ( (vinfo.xres != width) || (vinfo.yres != height) ||
	     (vinfo.bits_per_pixel != bpp) || (flags & SDL_DOUBLEBUF) ) {
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB changing vinfo...\n");
#endif
		vinfo.activate = FB_ACTIVATE_NOW;
		vinfo.accel_flags = 0;
		vinfo.bits_per_pixel = bpp;
		vinfo.xres = vinfo.xres_virtual = width;
		vinfo.yres = vinfo.yres_virtual = height;
		vinfo.width = 0;
		vinfo.height = 0;
		vinfo.xoffset = 0;
		vinfo.yoffset = 0;
		vinfo.grayscale = 0;
		switch(vinfo.bits_per_pixel) {
			case 32:
				vinfo.transp.offset = 0;
				vinfo.transp.length = 8;
				vinfo.red.offset = 8;
				vinfo.red.length = 8;
				vinfo.green.offset = 16;
				vinfo.green.length = 8;
				vinfo.blue.offset = 24;
				vinfo.blue.length = 8;
				break;
			case 24:
				vinfo.transp.offset = 0;
				vinfo.transp.length = 0;
				vinfo.red.offset = 0;
				vinfo.red.length = 8;
				vinfo.green.offset = 8;
				vinfo.green.length = 8;
				vinfo.blue.offset = 16;
				vinfo.blue.length = 8;
				break;
			case 16:
				vinfo.red.offset = 11;
				vinfo.red.length = 5;
				vinfo.green.offset = 5;
				vinfo.green.length = 6;
				vinfo.blue.offset = 0;
				vinfo.blue.length = 5;
				vinfo.transp.offset = 16;
				vinfo.transp.length = 0;
				break;
			case 8:
				vinfo.red.offset = 0;
				vinfo.red.length = 3;
				vinfo.green.offset = 3;
				vinfo.green.length = 3;
				vinfo.blue.offset = 4;
				vinfo.blue.length = 2;
				vinfo.transp.offset = 8;
				vinfo.transp.length = 0;
				break;
		}
		if ( ! choose_bcmfb_conf_mode(&vinfo) ) {
			choose_bcmfb_mode(&vinfo);
		}
#ifdef BCMFB_DEBUG
		fprintf(stderr, "BCMFB wanted vinfo:\n");
		print_vinfo(&vinfo);
#endif
		if ( !shadow_fb &&
				ioctl(console_fd, FBIOPUT_VSCREENINFO, &vinfo) < 0 ) {
			vinfo.yres_virtual = height;
			if ( ioctl(console_fd, FBIOPUT_VSCREENINFO, &vinfo) < 0 ) {
				SDL_SetError("BCMFB couldn't set console screen info");
				return(NULL);
			}
		}
	} else {
		int maxheight;
		if ( flags & SDL_DOUBLEBUF ) {
			maxheight = height*2;
		} else {
			maxheight = height;
		}
		if ( vinfo.yres_virtual > maxheight ) {
			vinfo.yres_virtual = maxheight;
		}
	}
	cache_vinfo = vinfo;
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB actual vinfo:\n");
	print_vinfo(&vinfo);
#endif
	Amask = 0;
	for ( i=0; i<vinfo.transp.length; ++i ) {
		Amask <<= 1;
		Amask |= (0x00000001<<vinfo.transp.offset);
	}
	Rmask = 0;
	for ( i=0; i<vinfo.red.length; ++i ) {
		Rmask <<= 1;
		Rmask |= (0x00000001<<vinfo.red.offset);
	}
	Gmask = 0;
	for ( i=0; i<vinfo.green.length; ++i ) {
		Gmask <<= 1;
		Gmask |= (0x00000001<<vinfo.green.offset);
	}
	Bmask = 0;
	for ( i=0; i<vinfo.blue.length; ++i ) {
		Bmask <<= 1;
		Bmask |= (0x00000001<<vinfo.blue.offset);
	}
	if ( ! SDL_ReallocFormat(current, vinfo.bits_per_pixel,
	                                  Rmask, Gmask, Bmask, Amask) ) {
		return(NULL);
	}

	/* Get the fixed information about the console hardware.
	   This is necessary since finfo.line_length changes.
	 */
	if ( ioctl(console_fd, FBIOGET_FSCREENINFO, &finfo) < 0 ) {
		SDL_SetError("BCMFB couldn't get console hardware info");
		return(NULL);
	}
	
	mapped_memlen = finfo.smem_len;
	mapped_mem = do_mmap(NULL, mapped_memlen,
	                  PROT_READ|PROT_WRITE, MAP_SHARED, console_fd, 0);
	if ( mapped_mem == (char *)-1 ) {
		SDL_SetError("Unable to memory map the video hardware");
		mapped_mem = NULL;
		BCMFB_VideoQuit(this);
		return(-1);
	}
	
	mapped_mem += vinfo.yoffset * finfo.line_length;

#ifdef BCMFB_DEBUG
	printf("VIDEO MEM          : 0x%8x (%dMB)\n", finfo.smem_len, (finfo.smem_len/1024/1024));
	printf("BCMFB I/O          : %p\n", mapped_mem);
#endif

	/* Save hardware palette, if needed */
	BCMFB_SavePalette(this, &finfo, &vinfo);

	if (shadow_fb) {
		if (vinfo.bits_per_pixel == 16) {
			blitFunc = (rotate == BCMFB_ROTATE_NONE ||
					rotate == BCMFB_ROTATE_UD) ?
				BCMFB_blit16 : BCMFB_blit16blocked;
		} else {
#ifdef BCMFB_DEBUG
			fprintf(stderr, "BCMFB Init vinfo:\n");
			print_vinfo(&vinfo);
#endif
			SDL_SetError("BCMFB using software buffer, but no blitter "
					"function is available for %d bpp.",
					vinfo.bits_per_pixel);
			return(NULL);
		}
	}

	/* Set up the new mode framebuffer */
	current->flags &= SDL_FULLSCREEN;
	if (shadow_fb) {
		current->flags |= SDL_SWSURFACE;
	} else {
		current->flags |= SDL_HWSURFACE;
	}
	/* We have got smp */
	current->flags |= SDL_ASYNCBLIT;
	
	current->w = vinfo.xres;
	current->h = vinfo.yres;
	if (shadow_fb) {
		current->pitch = current->w * ((vinfo.bits_per_pixel + 7) / 8);
		current->pixels = shadow_mem;
		physlinebytes = finfo.line_length;
	} else {
		current->pitch = finfo.line_length;
		current->pixels = mapped_mem;
	}
	
	/* Set up the information for hardware surfaces */
	surfaces_mem = (char *)current->pixels +
		vinfo.yres_virtual*current->pitch;
	surfaces_len = (shadow_fb) ?
		0 : (mapped_memlen-(surfaces_mem-mapped_mem));

	BCMFB_FreeHWSurfaces(this);
	BCMFB_InitHWSurfaces(this, current, surfaces_mem, surfaces_len);

	/* Let the application know we have a hardware palette */
	switch (finfo.visual) {
		case FB_VISUAL_PSEUDOCOLOR:
		current->flags |= SDL_HWPALETTE;
		break;
		default:
		break;
	}

	/* Update for double-buffering, if we can */
	if ( flags & SDL_DOUBLEBUF ) {
		if ( vinfo.yres_virtual == (height*2) ) {
			current->flags |= SDL_DOUBLEBUF;
			flip_page = 0;
			flip_address[0] = (char *)current->pixels;
			flip_address[1] = (char *)current->pixels+
				current->h*current->pitch;
			this->screen = current;
			BCMFB_FlipHWSurface(this, current);
			this->screen = NULL;
		}
	}
	
	/* Set the update rectangle function */
	this->UpdateRects = BCMFB_DirectUpdate;

	/* We're done */
	return(current);
}

#ifdef BCMFB_DEBUG
void BCMFB_DumpHWSurfaces(_THIS)
{
	vidmem_bucket *bucket;

	printf("BCMFB memory left: %d (%d total)\n", surfaces_memleft, surfaces_memtotal);
	printf("\n");
	printf("         Base  Size\n");
	for ( bucket=&surfaces; bucket; bucket=bucket->next ) {
		printf("Bucket:  %p, %d (%s)\n", bucket->base, bucket->size, bucket->used ? "used" : "free");
		if ( bucket->prev ) {
			if ( bucket->base != bucket->prev->base+bucket->prev->size ) {
				printf("Warning, corrupt bucket list! (prev)\n");
			}
		} else {
			if ( bucket != &surfaces ) {
				printf("Warning, corrupt bucket list! (!prev)\n");
			}
		}
		if ( bucket->next ) {
			if ( bucket->next->base != bucket->base+bucket->size ) {
				printf("Warning, corrupt bucket list! (next)\n");
			}
		}
	}
	printf("\n");
}
#endif

static int BCMFB_InitHWSurfaces(_THIS, SDL_Surface *screen, char *base, int size)
{
	vidmem_bucket *bucket;

	surfaces_memtotal = size;
	surfaces_memleft = size;

	if ( surfaces_memleft > 0 ) {
		bucket = (vidmem_bucket *)SDL_malloc(sizeof(*bucket));
		if ( bucket == NULL ) {
			SDL_OutOfMemory();
			return(-1);
		}
		bucket->prev = &surfaces;
		bucket->used = 0;
		bucket->dirty = 0;
		bucket->base = base;
		bucket->size = size;
		bucket->next = NULL;
	} else {
		bucket = NULL;
	}

	surfaces.prev = NULL;
	surfaces.used = 1;
	surfaces.dirty = 0;
	surfaces.base = screen->pixels;
	surfaces.size = (unsigned int)((long)base - (long)surfaces.base);
	surfaces.next = bucket;
	screen->hwdata = (struct private_hwdata *)&surfaces;
	return(0);
}
static void BCMFB_FreeHWSurfaces(_THIS)
{
	vidmem_bucket *bucket, *freeable;

	bucket = surfaces.next;
	while ( bucket ) {
		freeable = bucket;
		bucket = bucket->next;
		SDL_free(freeable);
	}
	surfaces.next = NULL;
}

static int BCMFB_AllocHWSurface(_THIS, SDL_Surface *surface)
{
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB_AllocHWSurface\n");
#endif
	vidmem_bucket *bucket;
	int size;
	int extra;

/* Temporarily, we only allow surfaces the same width as display.
   Some blitters require the pitch between two hardware surfaces
   to be the same.  Others have interesting alignment restrictions.
   Until someone who knows these details looks at the code...
*/
if ( surface->pitch > SDL_VideoSurface->pitch ) {
	SDL_SetError("Surface requested wider than screen");
	return(-1);
}
surface->pitch = SDL_VideoSurface->pitch;
	size = surface->h * surface->pitch;
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB allocating bucket of %d bytes\n", size);
#endif

	/* Quick check for available mem */
	if ( size > surfaces_memleft ) {
		SDL_SetError("Not enough video memory");
		return(-1);
	}

	/* Search for an empty bucket big enough */
	for ( bucket=&surfaces; bucket; bucket=bucket->next ) {
		if ( ! bucket->used && (size <= bucket->size) ) {
			break;
		}
	}
	if ( bucket == NULL ) {
		SDL_SetError("Video memory too fragmented");
		return(-1);
	}

	/* Create a new bucket for left-over memory */
	extra = (bucket->size - size);
	if ( extra ) {
		vidmem_bucket *newbucket;

#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB adding new free bucket of %d bytes\n", extra);
#endif
		newbucket = (vidmem_bucket *)SDL_malloc(sizeof(*newbucket));
		if ( newbucket == NULL ) {
			SDL_OutOfMemory();
			return(-1);
		}
		newbucket->prev = bucket;
		newbucket->used = 0;
		newbucket->base = bucket->base+size;
		newbucket->size = extra;
		newbucket->next = bucket->next;
		if ( bucket->next ) {
			bucket->next->prev = newbucket;
		}
		bucket->next = newbucket;
	}

	/* Set the current bucket values and return it! */
	bucket->used = 1;
	bucket->size = size;
	bucket->dirty = 0;
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB allocated %d bytes at %p\n", bucket->size, bucket->base);
#endif
	surfaces_memleft -= size;
	surface->flags |= SDL_HWSURFACE;
	surface->pixels = bucket->base;
	surface->hwdata = (struct private_hwdata *)bucket;
	return(0);
}
static void BCMFB_FreeHWSurface(_THIS, SDL_Surface *surface)
{
	vidmem_bucket *bucket, *freeable;

	/* Look for the bucket in the current list */
	for ( bucket=&surfaces; bucket; bucket=bucket->next ) {
		if ( bucket == (vidmem_bucket *)surface->hwdata ) {
			break;
		}
	}
	if ( bucket && bucket->used ) {
		/* Add the memory back to the total */
#ifdef DGA_DEBUG
	printf("BCMFB freeing bucket of %d bytes\n", bucket->size);
#endif
		surfaces_memleft += bucket->size;

		/* Can we merge the space with surrounding buckets? */
		bucket->used = 0;
		if ( bucket->next && ! bucket->next->used ) {
#ifdef DGA_DEBUG
	printf("BCMFB merging with next bucket, for %d total bytes\n", bucket->size+bucket->next->size);
#endif
			freeable = bucket->next;
			bucket->size += bucket->next->size;
			bucket->next = bucket->next->next;
			if ( bucket->next ) {
				bucket->next->prev = bucket;
			}
			SDL_free(freeable);
		}
		if ( bucket->prev && ! bucket->prev->used ) {
#ifdef DGA_DEBUG
	printf("BCMFB merging with previous bucket, for %d total bytes\n", bucket->prev->size+bucket->size);
#endif
			freeable = bucket;
			bucket->prev->size += bucket->size;
			bucket->prev->next = bucket->next;
			if ( bucket->next ) {
				bucket->next->prev = bucket->prev;
			}
			SDL_free(freeable);
		}
	}
	surface->pixels = NULL;
	surface->hwdata = NULL;
}

static int BCMFB_LockHWSurface(_THIS, SDL_Surface *surface)
{
	if ( switched_away ) {
		return -2; /* no hardware access */
	}
	if ( surface == this->screen ) {
		SDL_mutexP(hw_lock);
		if ( BCMFB_IsSurfaceBusy(surface) ) {
			BCMFB_WaitBusySurfaces(this);
		}
	} else {
		if ( BCMFB_IsSurfaceBusy(surface) ) {
			BCMFB_WaitBusySurfaces(this);
		}
	}
	return(0);
}
static void BCMFB_UnlockHWSurface(_THIS, SDL_Surface *surface)
{
	if ( surface == this->screen ) {
		SDL_mutexV(hw_lock);
	}
}

static void BCMFB_WaitVBL(_THIS)
{
	fprintf(stderr, "BCMFB_WaitVBL\n");
#ifdef FBIOWAITRETRACE /* Heheh, this didn't make it into the main kernel */
	ioctl(console_fd, FBIOWAITRETRACE, 0);
#endif
	return;
}

static void BCMFB_WaitIdle(_THIS)
{
	return;
}

static int BCMFB_FlipHWSurface(_THIS, SDL_Surface *surface)
{
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB_FlipHWSurface\n");
#endif
	if ( switched_away ) {
		return -2; /* no hardware access */
	}

	/* Wait for vertical retrace and then flip display */
	cache_vinfo.yoffset = flip_page*surface->h;
	if ( BCMFB_IsSurfaceBusy(this->screen) ) {
		BCMFB_WaitBusySurfaces(this);
	}
	wait_vbl(this);
	if ( ioctl(console_fd, FBIOPAN_DISPLAY, &cache_vinfo) < 0 ) {
		SDL_SetError("ioctl(FBIOPAN_DISPLAY) failed");
		return(-1);
	}
	flip_page = !flip_page;

	surface->pixels = flip_address[flip_page];
	return(0);
}

static void BCMFB_blit16(Uint8 *byte_src_pos, int src_right_delta, int src_down_delta,
		Uint8 *byte_dst_pos, int dst_linebytes, int width, int height)
{
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB_blit16\n");
#endif
	int w;
	Uint16 *src_pos = (Uint16 *)byte_src_pos;
	Uint16 *dst_pos = (Uint16 *)byte_dst_pos;

	while (height) {
		Uint16 *src = src_pos;
		Uint16 *dst = dst_pos;
		for (w = width; w != 0; w--) {
			*dst = *src;
			src += src_right_delta;
			dst++;
		}
		dst_pos = (Uint16 *)((Uint8 *)dst_pos + dst_linebytes);
		src_pos += src_down_delta;
		height--;
	}
}

#define BLOCKSIZE_W 32
#define BLOCKSIZE_H 32

static void BCMFB_blit16blocked(Uint8 *byte_src_pos, int src_right_delta, int src_down_delta, 
		Uint8 *byte_dst_pos, int dst_linebytes, int width, int height)
{
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB_blit16blocked\n");
#endif
	int w;
	Uint16 *src_pos = (Uint16 *)byte_src_pos;
	Uint16 *dst_pos = (Uint16 *)byte_dst_pos;

	while (height > 0) {
		Uint16 *src = src_pos;
		Uint16 *dst = dst_pos;
		for (w = width; w > 0; w -= BLOCKSIZE_W) {
			BCMFB_blit16((Uint8 *)src,
					src_right_delta,
					src_down_delta,
					(Uint8 *)dst,
					dst_linebytes,
					min(w, BLOCKSIZE_W),
					min(height, BLOCKSIZE_H));
			src += src_right_delta * BLOCKSIZE_W;
			dst += BLOCKSIZE_W;
		}
		dst_pos = (Uint16 *)((Uint8 *)dst_pos + dst_linebytes * BLOCKSIZE_H);
		src_pos += src_down_delta * BLOCKSIZE_H;
		height -= BLOCKSIZE_H;
	}
}

static void BCMFB_DirectUpdate(_THIS, int numrects, SDL_Rect *rects)
{
	//fprintf(stderr, "BCMFB_DirectUpdate\n");
	if (!shadow_fb) {
		/* The application is already updating the visible video memory */
		return;
	}
	int width = cache_vinfo.xres;
	int height = cache_vinfo.yres;
	int bytes_per_pixel = (cache_vinfo.bits_per_pixel + 7) / 8;
	int i;

	if (cache_vinfo.bits_per_pixel != 16) {
		SDL_SetError("Shadow copy only implemented for 16 bpp");
		return;
	}

	for (i = 0; i < numrects; i++) {
		int x1, y1, x2, y2;
		int scr_x1, scr_y1, scr_x2, scr_y2;
		int sha_x1, sha_y1;
		int shadow_right_delta;  /* Address change when moving right in dest */
		int shadow_down_delta;   /* Address change when moving down in dest */
		char *src_start;
		char *dst_start;

		x1 = rects[i].x; 
		y1 = rects[i].y;
		x2 = x1 + rects[i].w; 
		y2 = y1 + rects[i].h;

		if (x1 < 0) {
			x1 = 0;
		} else if (x1 > width) {
			x1 = width;
		}
		if (x2 < 0) {
			x2 = 0;
		} else if (x2 > width) {
			x2 = width;
		}
		if (y1 < 0) {
			y1 = 0;
		} else if (y1 > height) {
			y1 = height;
		}
		if (y2 < 0) {
			y2 = 0;
		} else if (y2 > height) {
			y2 = height;
		}
		if (x2 <= x1 || y2 <= y1) {
			continue;
		}

		switch (rotate) {
			case BCMFB_ROTATE_NONE:
				sha_x1 = scr_x1 = x1;
				sha_y1 = scr_y1 = y1;
				scr_x2 = x2;
				scr_y2 = y2;
				shadow_right_delta = 1;
				shadow_down_delta = width;
				break;
			case BCMFB_ROTATE_CCW:
				scr_x1 = y1;
				scr_y1 = width - x2;
				scr_x2 = y2;
				scr_y2 = width - x1;
				sha_x1 = x2 - 1;
				sha_y1 = y1;
				shadow_right_delta = width;
				shadow_down_delta = -1;
				break;
			case BCMFB_ROTATE_UD:
				scr_x1 = width - x2;
				scr_y1 = height - y2;
				scr_x2 = width - x1;
				scr_y2 = height - y1;
				sha_x1 = x2 - 1;
				sha_y1 = y2 - 1;
				shadow_right_delta = -1;
				shadow_down_delta = -width;
				break;
			case BCMFB_ROTATE_CW:
				scr_x1 = height - y2;
				scr_y1 = x1;
				scr_x2 = height - y1;
				scr_y2 = x2;
				sha_x1 = x1;
				sha_y1 = y2 - 1;
				shadow_right_delta = -width;
				shadow_down_delta = 1;
				break;
			default:
				SDL_SetError("Unknown rotation");
				return;
		}

		src_start = shadow_mem +
			(sha_y1 * width + sha_x1) * bytes_per_pixel;
		dst_start = mapped_mem + scr_y1 * physlinebytes + 
			scr_x1 * bytes_per_pixel;
			
		blitFunc((Uint8 *) src_start,
				shadow_right_delta, 
				shadow_down_delta, 
				(Uint8 *) dst_start,
				physlinebytes,
				scr_x2 - scr_x1,
				scr_y2 - scr_y1);
	}
}

void BCMFB_SavePaletteTo(_THIS, int palette_len, __u16 *area)
{
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB_SavePaletteTo\n");
#endif
	struct fb_cmap cmap;
		
	cmap.start = 0;
	cmap.len = palette_len;
	cmap.red = &area[0*palette_len];
	cmap.green = &area[1*palette_len];
	cmap.blue = &area[2*palette_len];
	cmap.transp = NULL;
	ioctl(console_fd, FBIOGETCMAP, &cmap);
	
}

void BCMFB_RestorePaletteFrom(_THIS, int palette_len, __u16 *area)
{
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB_RestorePaletteFrom\n");
#endif
	struct fb_cmap cmap;

	cmap.start = 0;
	cmap.len = palette_len;
	cmap.red = &area[0*palette_len];
	cmap.green = &area[1*palette_len];
	cmap.blue = &area[2*palette_len];
	cmap.transp = NULL;
	ioctl(console_fd, FBIOPUTCMAP, &cmap);
}

static void BCMFB_SavePalette(_THIS, struct fb_fix_screeninfo *finfo,
                                  struct fb_var_screeninfo *vinfo)
{
	int i;

	/* Save hardware palette, if needed */
	if ( finfo->visual == FB_VISUAL_PSEUDOCOLOR ) {
		saved_cmaplen = 1<<vinfo->bits_per_pixel;
		saved_cmap=(__u16 *)SDL_malloc(3*saved_cmaplen*sizeof(*saved_cmap));
		if ( saved_cmap != NULL ) {
			BCMFB_SavePaletteTo(this, saved_cmaplen, saved_cmap);
		}
	}

	/* Added support for FB_VISUAL_DIRECTCOLOR.
	   With this mode pixel information is passed through the palette...
	   Neat fading and gamma correction effects can be had by simply
	   fooling around with the palette instead of changing the pixel
	   values themselves... Very neat!

	   Adam Meyerowitz 1/19/2000
	   ameyerow@optonline.com
	*/
	if ( finfo->visual == FB_VISUAL_DIRECTCOLOR ) {
		__u16 new_entries[3*256];

		/* Save the colormap */
		saved_cmaplen = 256;
		saved_cmap=(__u16 *)SDL_malloc(3*saved_cmaplen*sizeof(*saved_cmap));
		if ( saved_cmap != NULL ) {
			BCMFB_SavePaletteTo(this, saved_cmaplen, saved_cmap);
		}

		/* Allocate new identity colormap */
		for ( i=0; i<256; ++i ) {
	      		new_entries[(0*256)+i] =
			new_entries[(1*256)+i] =
			new_entries[(2*256)+i] = (i<<8)|i;
		}
		BCMFB_RestorePaletteFrom(this, 256, new_entries);
	}
}

static void BCMFB_RestorePalette(_THIS)
{
	/* Restore the original palette */
	if ( saved_cmap ) {
		BCMFB_RestorePaletteFrom(this, saved_cmaplen, saved_cmap);
		SDL_free(saved_cmap);
		saved_cmap = NULL;
	}
}

static int BCMFB_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
#ifdef BCMFB_DEBUG
	fprintf(stderr, "BCMFB_SetColors\n");
#endif
	int i;
	__u16 r[256];
	__u16 g[256];
	__u16 b[256];
	struct fb_cmap cmap;

	/* Set up the colormap */
	for (i = 0; i < ncolors; i++) {
		r[i] = colors[i].r << 8;
		g[i] = colors[i].g << 8;
		b[i] = colors[i].b << 8;
	}

	cmap.start = firstcolor;
	cmap.len = ncolors;
	cmap.red = r;
	cmap.green = g;
	cmap.blue = b;
	cmap.transp = NULL;

	if( (ioctl(console_fd, FBIOPUTCMAP, &cmap) < 0) ||
	    !(this->screen->flags & SDL_HWPALETTE) ) {
	        colors = this->screen->format->palette->colors;
		ncolors = this->screen->format->palette->ncolors;
		cmap.start = 0;
		cmap.len = ncolors;
		SDL_memset(r, 0, sizeof(r));
		SDL_memset(g, 0, sizeof(g));
		SDL_memset(b, 0, sizeof(b));
		if ( ioctl(console_fd, FBIOGETCMAP, &cmap) == 0 ) {
			for ( i=ncolors-1; i>=0; --i ) {
				colors[i].r = (r[i]>>8);
				colors[i].g = (g[i]>>8);
				colors[i].b = (b[i]>>8);
			}
		}
		return(0);
	}
	return(1);
}

/* Note:  If we are terminated, this could be called in the middle of
   another SDL video routine -- notably UpdateRects.
*/
static void BCMFB_VideoQuit(_THIS)
{
	int i, j;

	if ( this->screen ) {
		/* Clear screen and tell SDL not to free the pixels */

		const char *dontClearPixels = SDL_getenv("SDL_BCMFB_DONT_CLEAR");

		/* If the framebuffer is not to be cleared, make sure that we won't
		 * display the previous frame when disabling double buffering. */
		if ( dontClearPixels && flip_page == 0 ) {
			SDL_memcpy(flip_address[0], flip_address[1], this->screen->pitch * this->screen->h);
		}

		if ( !dontClearPixels && this->screen->pixels && BCMFB_InGraphicsMode(this) ) {
			SDL_memset(this->screen->pixels,0,this->screen->h*this->screen->pitch);
		}
		/* This test fails when using the VGA16 shadow memory */
		if ( ((char *)this->screen->pixels >= mapped_mem) &&
		     ((char *)this->screen->pixels < (mapped_mem+mapped_memlen)) ) {
			this->screen->pixels = NULL;
		}
	}

	/* Clear the lock mutex */
	if ( hw_lock ) {
		SDL_DestroyMutex(hw_lock);
		hw_lock = NULL;
	}

	/* Clean up defined video modes */
	for ( i=0; i<NUM_MODELISTS; ++i ) {
		if ( SDL_modelist[i] != NULL ) {
			for ( j=0; SDL_modelist[i][j]; ++j ) {
				SDL_free(SDL_modelist[i][j]);
			}
			SDL_free(SDL_modelist[i]);
			SDL_modelist[i] = NULL;
		}
	}

	/* Clean up the memory bucket list */
	BCMFB_FreeHWSurfaces(this);

	/* Close console and input file descriptors */
	if ( console_fd > 0 ) {
		/* Unmap the video framebuffer and I/O registers */
		if ( mapped_mem ) {
			munmap(mapped_mem, mapped_memlen);
			mapped_mem = NULL;
		}
		/* Restore the original video mode and palette */
		if ( BCMFB_InGraphicsMode(this) ) {
			BCMFB_RestorePalette(this);
			ioctl(console_fd, FBIOPUT_VSCREENINFO, &saved_vinfo);
		}

		/* We're all done with the framebuffer */
		close(console_fd);
		console_fd = -1;
	}
	BCMFB_CloseMouse(this);
	BCMFB_CloseKeyboard(this);
}