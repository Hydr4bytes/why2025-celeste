#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define SDL_MAIN_USE_CALLBACKS 1 /* use the callbacks instead of main() */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "thirdparty/celeste.h"
#include "bitmap.h"
#include "font.h"
#include "tilemap.h"

#define SCALE 3

#define FB_WIDTH  (128 * SCALE)
#define FB_HEIGHT (128 * SCALE)
#define W_HEIGHT  (128 * SCALE)
#define W_WIDTH   (128 * SCALE)

#define STEP_RATE_IN_MILLISECONDS 33

#if true
__always_inline int
__issignalingf (float x)
{
  uint32_t ix = *((uint32_t*)&x);
  if (!_IEEE_754_2008_SNAN)
    return (ix & 0x7fc00000u) == 0x7fc00000u;
  return 2 * (ix ^ 0x00400000u) > 0xFF800000u;
}
#endif

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static Uint64 last_step;

static SDL_Texture *gfx = NULL;
static SDL_Texture *font = NULL;

static Uint16 buttons_state = 0;

static const SDL_Color base_palette[16] = {
	{0x00, 0x00, 0x00},
	{0x1d, 0x2b, 0x53},
	{0x7e, 0x25, 0x53},
	{0x00, 0x87, 0x51},
	{0xab, 0x52, 0x36},
	{0x5f, 0x57, 0x4f},
	{0xc2, 0xc3, 0xc7},
	{0xff, 0xf1, 0xe8},
	{0xff, 0x00, 0x4d},
	{0xff, 0xa3, 0x00},
	{0xff, 0xec, 0x27},
	{0x00, 0xe4, 0x36},
	{0x29, 0xad, 0xff},
	{0x83, 0x76, 0x9c},
	{0xff, 0x77, 0xa8},
	{0xff, 0xcc, 0xaa}
};
static SDL_Color palette[16];

static inline int get_color(char idx) {
	const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_XRGB8888);

	SDL_Color c = palette[idx%16];
	return SDL_MapRGB(format, NULL, c.r,c.g,c.b);
}

static void reset_palette(void) {
	memcpy(palette, base_palette, sizeof palette);
}

static int get_tile_flag(int tile, int flag) {
	return tile < sizeof(tile_flags)/sizeof(*tile_flags) && (tile_flags[tile] & (1 << flag)) != 0;
}

void draw_tilemap(int16_t x, int16_t y, SDL_Texture* bitmap, int16_t start_x, int16_t start_y,  int16_t w, int16_t h) {
	if (x < 0) {
		start_x -= x;
		w += x;
		x = 0;
	}
	if (y < 0) {
		start_y -= y;
		h += y;
		y = 0;
	}
	if (x + w > W_WIDTH) {
		w = W_WIDTH - x;
	}
	if (y + h > W_HEIGHT) {
		h = W_HEIGHT - y;
	}

	SDL_FRect src_rect = {start_x, start_y, w, h};
	SDL_FRect dst_rect = {x*SCALE, y*SCALE, w*SCALE, h*SCALE};

	SDL_RenderTexture(renderer, bitmap, &src_rect, &dst_rect);
}

static void p8_line(int x0, int y0, int x1, int y1, unsigned char col) {
	SDL_SetRenderDrawColor(renderer, col >> 16, (col >> 8) & 0xFF, col & 0xFF, 255);

	#define CLAMP(v,min,max) v = v < min ? min : v >= max ? max-1 : v;
	CLAMP(x0,0,W_WIDTH);
	CLAMP(y0,0,W_HEIGHT);
	CLAMP(x1,0,W_WIDTH);
	CLAMP(y1,0,W_HEIGHT);

	#undef CLAMP
  #define PLOT(x,y) do {                                                        \
     SDL_RenderFillRect(renderer, &(SDL_FRect){x*SCALE,y*SCALE,SCALE,SCALE}); \
	} while (0)
	int sx, sy, dx, dy, err, e2;
	dx = abs(x1 - x0);
	dy = abs(y1 - y0);
	if (!dx && !dy) return;

	if (x0 < x1) sx = 1; else sx = -1;
	if (y0 < y1) sy = 1; else sy = -1;
	err = dx - dy;
	if (!dy && !dx) return;
	else if (!dx) { //vertical line
		for (int y = y0; y != y1; y += sy) PLOT(x0,y);
	} else if (!dy) { //horizontal line
		for (int x = x0; x != x1; x += sx) PLOT(x,y0);
	} while (x0 != x1 || y0 != y1) {
		PLOT(x0, y0);
		e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x0 += sx;
		}
		if (e2 < dx) {
			err += dx;
			y0 += sy;
		}
	}
	#undef PLOT
}

static void p8_rectfill(int x0, int y0, int x1, int y1, int col) {
	SDL_SetRenderDrawColor(renderer, col >> 16, (col >> 8) & 0xFF, col & 0xFF, 255);
	SDL_RenderFillRect(renderer, &(SDL_FRect){SCALE*x0, SCALE*y0, SCALE*(x1-x0+1), SCALE*(y1-y0+1)});
}

static void p8_circfill(int cx, int cy, int r, int col) {
	SDL_SetRenderDrawColor(renderer, col >> 16, (col >> 8) & 0xFF, col & 0xFF, 255);

	if (r <= 1) {
		SDL_RenderFillRect(renderer, &(SDL_FRect){SCALE*(cx-1), SCALE*cy, SCALE*3, SCALE});
		SDL_RenderFillRect(renderer, &(SDL_FRect){SCALE*cx, SCALE*(cy-1), SCALE, SCALE*3});
	} else if (r <= 2) {
		SDL_RenderFillRect(renderer, &(SDL_FRect){SCALE*(cx-2), SCALE*(cy-1), SCALE*5, SCALE*3});
		SDL_RenderFillRect(renderer, &(SDL_FRect){SCALE*(cx-1), SCALE*(cy-2), SCALE*3, SCALE*5});
	} else if (r <= 3) {
		SDL_RenderFillRect(renderer, &(SDL_FRect){SCALE*(cx-3), SCALE*(cy-1), SCALE*7, SCALE*3});
		SDL_RenderFillRect(renderer, &(SDL_FRect){SCALE*(cx-1), SCALE*(cy-3), SCALE*3, SCALE*7});
		SDL_RenderFillRect(renderer, &(SDL_FRect){SCALE*(cx-2), SCALE*(cy-2), SCALE*5, SCALE*5});
	} else { //i dont think the game uses this
		int f = 1 - r; //used to track the progress of the drawn circle (since its semi-recursive)
		int ddFx = 1; //step x
		int ddFy = -2 * r; //step y
		int x = 0;
		int y = r;

		//this algorithm doesn't account for the diameters
		//so we have to set them manually
		p8_line(cx,cy-y, cx,cy+r, col);
		p8_line(cx+r,cy, cx-r,cy, col);

		while (x < y) {
			if (f >= 0) {
				y--;
				ddFy += 2;
				f += ddFy;
			}
			x++;
			ddFx += 2;
			f += ddFx;

			//build our current arc
			p8_line(cx+x,cy+y, cx-x,cy+y, col);
			p8_line(cx+x,cy-y, cx-x,cy-y, col);
			p8_line(cx+y,cy+x, cx-y,cy+x, col);
			p8_line(cx+y,cy-x, cx-y,cy-x, col);
		}
	}
}

static void p8_print(const char* str, int x, int y, int col) {
	for (char c = *str; c; c = *(++str)) {
		c &= 0x7F;
		SDL_FRect srcrc = {8*(c%16), 8*(c/16)};
		SDL_FRect dstrc = {x*SCALE, y*SCALE, SCALE, SCALE};
		SDL_RenderTexture(renderer, font, &srcrc, &dstrc);

		x += 4;
	}
}

int pico8emu(CELESTE_P8_CALLBACK_TYPE call, ...) { 
	static int camera_x = 0, camera_y = 0;

	va_list args;
	int ret = 0;
	va_start(args, call);

	#define INT_ARG() va_arg(args, int)
	#define BOOL_ARG() (Celeste_P8_bool_t)va_arg(args, int)
	#define RET_INT(_i) do {ret = (_i); goto end;} while (0)
	#define RET_BOOL(_b) RET_INT(!!(_b))

	switch (call) {
		case CELESTE_P8_MUSIC: 
			break;
		case CELESTE_P8_SFX:
			break;
		case CELESTE_P8_SPR: { //spr(sprite,x,y,cols,rows,flipx,flipy)
			int sprite = INT_ARG();
			int x = INT_ARG();
			int y = INT_ARG();
			int cols = INT_ARG();
			int rows = INT_ARG();
			int flipx = BOOL_ARG();
			int flipy = BOOL_ARG();

			(void)cols;
			(void)rows;

			if (sprite >= 0) {
				draw_tilemap(x-camera_x, y-camera_y, gfx, 8*(sprite % 16), 8*(sprite / 16), 8, 8);
			}
		} break;
		case CELESTE_P8_BTN: { //btn(b)
			int b = INT_ARG();
			//assert(b >= 0 && b <= 5);
			RET_BOOL(buttons_state & (1 << b));
		} break;
		case CELESTE_P8_PAL: { //pal(a,b)
			int a = INT_ARG();
			int b = INT_ARG();
			if (a >= 0 && a < 16 && b >= 0 && b < 16) {
				//swap palette colors
				palette[a] = base_palette[b];
			}
		} break;
		case CELESTE_P8_PAL_RESET: { //pal()
			reset_palette();
		} break;
		case CELESTE_P8_PRINT: { //print(str,x,y,col)
 			const char* str = va_arg(args, const char*);
			int x = INT_ARG() - camera_x;
			int y = INT_ARG() - camera_y;
			int col = INT_ARG() % 16;

			p8_print(str,x,y,col);
		} break;
		case CELESTE_P8_RECTFILL: { //rectfill(x0,y0,x1,y1,col)
			int x0 = INT_ARG() - camera_x;
			int y0 = INT_ARG() - camera_y;
			int x1 = INT_ARG() - camera_x;
			int y1 = INT_ARG() - camera_y;
			int col = INT_ARG();
			int realcolor = get_color(col);

			int w = (x1 - x0 + 1);
			int h = (y1 - y0 + 1);
			if (w > 0 && h > 0) {
				p8_rectfill(x0, y0, x1, y1, realcolor);
			}
		} break;
		case CELESTE_P8_CIRCFILL: { //circfill(x,y,r,col)
			int cx = INT_ARG() - camera_x;
			int cy = INT_ARG() - camera_y;
			int r = INT_ARG();
			int col = INT_ARG();

			int realcolor = get_color(col);
			if (r >= 0) {
				p8_circfill(cx, cy, r, realcolor);
			}
		} break;
		case CELESTE_P8_LINE: { //line(x0,y0,x1,y1,col)
			int x0 = INT_ARG() - camera_x;
			int y0 = INT_ARG() - camera_y;
			int x1 = INT_ARG() - camera_x;
			int y1 = INT_ARG() - camera_y;
			int col = INT_ARG();
			int realcolor = get_color(col);

			p8_line(x0, y0, x1, y1, realcolor);
		} break;
		case CELESTE_P8_CAMERA: { //camera(x,y)
			camera_x = INT_ARG();
			camera_y = INT_ARG();
		} break;
		case CELESTE_P8_FGET: { //fget(tile,flag)
 			int tile = INT_ARG();
			int flag = INT_ARG();

			RET_INT(get_tile_flag(tile, flag));
		} break;
		case CELESTE_P8_MGET: { //mget(tx,ty)
 			int tx = INT_ARG();
			int ty = INT_ARG();

			RET_INT(tilemap_data[tx+ty*128]);
		} break;
		case CELESTE_P8_MAP: { //map(mx,my,tx,ty,mw,mh,mask)
			int mx = INT_ARG(), my = INT_ARG();
			int tx = INT_ARG(), ty = INT_ARG();
			int mw = INT_ARG(), mh = INT_ARG();
			int mask = INT_ARG();
			
			for (int x = 0; x < mw; x++) {
				for (int y = 0; y < mh; y++) {
					int tile = tilemap_data[x + mx + (y + my)*128];
					//hack
					if (mask == 0 || (mask == 4 && tile_flags[tile] == 4) || get_tile_flag(tile, mask != 4 ? mask-1 : mask)) {
						draw_tilemap((tx+x*8 - camera_x), (ty+y*8 - camera_y), gfx, 8*(tile % 16), 8*(tile / 16), 8, 8);
					}
				}
			}
		} break;
	}

	end:
	va_end(args);
	return ret;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    Uint64 const  now = SDL_GetTicks();
    SDL_FRect     r;
    unsigned      i;
    unsigned      j;
    int           ct;

    while ((now - last_step) >= STEP_RATE_IN_MILLISECONDS) {
        Celeste_P8_update();

		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer);

		Celeste_P8_draw();

		SDL_RenderPresent(renderer);

        last_step += STEP_RATE_IN_MILLISECONDS;
    }

    return SDL_APP_CONTINUE;
}


static const struct {
    char const *key;
    char const *value;
} extended_metadata[] = {
    {SDL_PROP_APP_METADATA_URL_STRING, ""},
    {SDL_PROP_APP_METADATA_CREATOR_STRING, "Lvna"},
    {SDL_PROP_APP_METADATA_COPYRIGHT_STRING, "Placed in the public domain"},
    {SDL_PROP_APP_METADATA_TYPE_STRING, "game"}
};

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    size_t i;

    if (!SDL_SetAppMetadata("Celeste", "1.0", "com.example.Celeste")) {
        return SDL_APP_FAILURE;
    }

    for (i = 0; i < SDL_arraysize(extended_metadata); i++) {
        if (!SDL_SetAppMetadataProperty(extended_metadata[i].key, extended_metadata[i].value)) {
            return SDL_APP_FAILURE;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Create window first
    window = SDL_CreateWindow("celeste", W_WIDTH, W_HEIGHT, 0);
    if (!window) {
        printf("Failed to create window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Check display capabilities
    SDL_DisplayID          display      = SDL_GetDisplayForWindow(window);
    SDL_DisplayMode const *current_mode = SDL_GetCurrentDisplayMode(display);
    if (current_mode) {
        printf(
            "Current display mode: %dx%d @%.2fHz, format: %s",
            current_mode->w,
            current_mode->h,
            current_mode->refresh_rate,
            SDL_GetPixelFormatName(current_mode->format)
        );
    }

    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        printf("Failed to create renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Check renderer properties
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (props) {
        char const *name = SDL_GetStringProperty(props, SDL_PROP_RENDERER_NAME_STRING, "Unknown");
        printf("Renderer: %s", name);

        SDL_PixelFormat const *formats =
            (SDL_PixelFormat const *)SDL_GetPointerProperty(props, SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, NULL);
        if (formats) {
            printf("Supported texture formats:");
            for (int j = 0; formats[j] != SDL_PIXELFORMAT_UNKNOWN; j++) {
                printf("  Format %d: %s", j, SDL_GetPixelFormatName(formats[j]));
            }
        }
    }

	gfx = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, 128, 64);
	if (!gfx) {
		printf("Failed to create texture: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	SDL_SetTextureScaleMode(gfx, SDL_SCALEMODE_NEAREST);
	if(!SDL_UpdateTexture(gfx, NULL, celeste_gfx, 128 * sizeof(uint16_t))) {
		printf("Failed to update texture: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	gfx = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, 128, 64);
	if (!gfx) {
		printf("Failed to create texture: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	SDL_SetTextureScaleMode(gfx, SDL_SCALEMODE_NEAREST);
	if(!SDL_UpdateTexture(gfx, NULL, celeste_gfx, 128 * sizeof(uint16_t))) {
		printf("Failed to update texture: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	font = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, 128, 85);
	if (!font) {
		printf("Failed to create texture: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	SDL_SetTextureScaleMode(font, SDL_SCALEMODE_NEAREST);
	if(!SDL_UpdateTexture(font, NULL, celeste_font, 128 * sizeof(uint16_t))) {
		printf("Failed to update texture: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	printf("Celeste starting...\n");

	Celeste_P8_set_call_func(pico8emu);
	Celeste_P8_set_rndseed(8);
	Celeste_P8_init();

	printf("Celeste initialized.\n");

    last_step = SDL_GetTicks();

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    switch (event->type) {
        case SDL_EVENT_QUIT: return SDL_APP_SUCCESS;
		case SDL_EVENT_KEY_DOWN: {
			SDL_Scancode scancode = event->key.scancode;

			if (scancode == SDL_SCANCODE_ESCAPE || scancode == SDL_SCANCODE_Q) {
				return SDL_APP_SUCCESS;
			} else if (scancode == SDL_SCANCODE_R) {
				Celeste_P8__DEBUG();
			} else if (scancode == SDL_SCANCODE_LEFT) {
				buttons_state |= (1<<0);
			} else if (scancode == SDL_SCANCODE_RIGHT) {
				buttons_state |= (1<<1);
			} else if (scancode == SDL_SCANCODE_UP) {
				buttons_state |= (1<<2);
			} else if (scancode == SDL_SCANCODE_DOWN) {
				buttons_state |= (1<<3);
			} else if (scancode == SDL_SCANCODE_Z) {
				buttons_state |= (1<<4);
			} else if (scancode == SDL_SCANCODE_X) {
				buttons_state |= (1<<5);
			}
		} break;
		case SDL_EVENT_KEY_UP: {
			SDL_Scancode scancode = event->key.scancode;

			if (scancode == SDL_SCANCODE_LEFT) {
				buttons_state &= ~(1<<0);
			} else if (scancode == SDL_SCANCODE_RIGHT) {
				buttons_state &= ~(1<<1); // up
			} else if (scancode == SDL_SCANCODE_UP) {
				buttons_state &= ~(1<<2); // left
			} else if (scancode == SDL_SCANCODE_DOWN) {
				buttons_state &= ~(1<<3); // down
			} else if (scancode == SDL_SCANCODE_Z) {
				buttons_state &= ~(1<<4); // Z
			} else if (scancode == SDL_SCANCODE_X) {
				buttons_state &= ~(1<<5); // X
			}
		} break;
        default: break;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
}
