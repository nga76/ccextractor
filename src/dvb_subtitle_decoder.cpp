/*
 * DVB subtitle decoding
 * Copyright (c) 2005 Ian Caulfield
 * Copyright (c) 2004 Michael Niedermayer <michaelni@gmx.at>
 * Copyright (c) 2014 Anshul Maheshwari <er.anshul.maheshwari@gmail.com>
 * License: GPL 2.0
 *
 * You should have received a copy of the GNU General Public
 * License along with CCExtractor; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */
/**
 * @file dvbsub.c
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
/* convert values between host and network byte order(big endian) */
#ifdef _WIN32
#include <winsock2.h> 
#else
#include <arpa/inet.h> 
#endif

#ifdef _MSC_VER
#define snprintf(str,size,format,...) _snprintf(str,size-1,format,__VA_ARGS__)
#endif


#include "dvb_subtitle_decoder.h"
#define DEBUG

#ifdef DEBUG
#define PNG_DEBUG 3
#include "png.h"
#endif

#define DVBSUB_PAGE_SEGMENT     0x10
#define DVBSUB_REGION_SEGMENT   0x11
#define DVBSUB_CLUT_SEGMENT     0x12
#define DVBSUB_OBJECT_SEGMENT   0x13
#define DVBSUB_DISPLAYDEFINITION_SEGMENT 0x14
#define DVBSUB_DISPLAY_SEGMENT  0x80


#define RL32(x) (*(unsigned int *)(x))
#define RB32(x) (ntohl(*(unsigned int *)(x)))
#define RL16(x) (*(unsigned short int*)(x))
#define RB16(x) (ntohs(*(unsigned short int*)(x)))


#define SCALEBITS 10
#define ONE_HALF  (1 << (SCALEBITS - 1))
#define FIX(x)    ((int) ((x) * (1<<SCALEBITS) + 0.5))

#define YUV_TO_RGB1_CCIR(cb1, cr1)\
{\
    cb = (cb1) - 128;\
    cr = (cr1) - 128;\
    r_add = FIX(1.40200*255.0/224.0) * cr + ONE_HALF;\
    g_add = - FIX(0.34414*255.0/224.0) * cb - FIX(0.71414*255.0/224.0) * cr + \
            ONE_HALF;\
    b_add = FIX(1.77200*255.0/224.0) * cb + ONE_HALF;\
}

#define YUV_TO_RGB2_CCIR(r, g, b, y1)\
{\
    y = ((y1) - 16) * FIX(255.0/219.0);\
    r = cm[(y + r_add) >> SCALEBITS];\
    g = cm[(y + g_add) >> SCALEBITS];\
    b = cm[(y + b_add) >> SCALEBITS];\
}

#define times4(x) x, x, x, x
#define times256(x) times4(times4(times4(times4(times4(x)))))

#define MAX_NEG_CROP 1024
const uint8_t crop_tab[256 + 2 * MAX_NEG_CROP] = {
times256(0x00),
0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,
0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F,
0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F,
0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,
0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF,
times256(0xFF)
};

#define cm (crop_tab + MAX_NEG_CROP)

const char *dvb_language[]=
{
    "und",
    "eng",
    "fin",
    NULL
};

static __inline unsigned int bytestream_get_byte(const uint8_t **b)
{
    (*b) += 1;
    return ((const uint8_t*)(*b -1))[0];
}

static __inline unsigned int bytestream_get_be16(const uint8_t **b)
{
    (*b) += 2;
    return RB16(*b -2);
}
typedef struct GetBitContext {
    const uint8_t *buffer, *buffer_end;
    int index;
    int size_in_bits;
    int size_in_bits_plus8;
} GetBitContext;


/**
 * Initialize GetBitContext.
 * @param buffer bitstream buffer, must be FF_INPUT_BUFFER_PADDING_SIZE bytes
 *        larger than the actual read bits because some optimized bitstream
 *        readers read 32 or 64 bit at once and could read over the end
 * @param bit_size the size of the buffer in bits
 * @return 0 on success, AVERROR_INVALIDDATA if the buffer_size would overflow.
 */
static __inline int init_get_bits(GetBitContext *s, const uint8_t *buffer,
                                int bit_size)
{
    int buffer_size;
    int ret = 0;

    if (bit_size >= INT_MAX - 7 || bit_size < 0 || !buffer) {
        buffer_size = bit_size = 0;
        buffer      = NULL;
        ret         = -1;
        errno = EINVAL;
    }

    buffer_size = (bit_size + 7) >> 3;

    s->buffer             = buffer;
    s->size_in_bits       = bit_size;
    s->size_in_bits_plus8 = bit_size + 8;
    s->buffer_end         = buffer + buffer_size;
    s->index              = 0;

    return ret;
}

static __inline int get_bits_count(const GetBitContext *s)
{
    return s->index;
}

static __inline unsigned int get_bits(GetBitContext *s, int n)
{
    register int tmp;
    unsigned int re_index = s->index;
    unsigned int re_cache = 0;
    unsigned int re_size_plus8 = s->size_in_bits_plus8;

    if(n <=0 && n>25)
        return -1;
    re_cache = RB32( s->buffer + (re_index >> 3 )) << (re_index & 7);

    tmp = ((uint32_t) re_cache) >> (32 -n);

    re_index = ( (re_size_plus8 < re_index + (n) )? ( re_size_plus8 ) : ( re_index + (n)) );

    s->index = re_index;
    return tmp;
}
static __inline unsigned int get_bits1(GetBitContext *s)
{
    unsigned int index = s->index;
    uint8_t result     = s->buffer[index >> 3];

    result <<= index & 7;
    result >>= 8 - 1;

    if (s->index < s->size_in_bits_plus8)
        index++;
    s->index = index;

    return result;
}

static void freep(void *arg)
{
    void **ptr = (void **)arg;
    free(*ptr);
    *ptr = NULL;

}
#ifdef DEBUG
static void png_save(const char *filename, uint32_t *bitmap, int w, int h)
{
	FILE *f;
	char fname[40];
	png_structp png_ptr;
	png_infop info_ptr;
	png_bytep* row_pointer;
	static png_color palette[10] =
	{
		{ 0xff, 0xff, 0xff }, // COL_WHITE = 0,
		{ 0x00, 0xff, 0x00 }, // COL_GREEN = 1,
		{ 0x00, 0x00, 0xff }, // COL_BLUE = 2,
		{ 0x00, 0xff, 0xff }, // COL_CYAN = 3,
		{ 0xff, 0x00, 0x00 }, // COL_RED = 4,
		{ 0xff, 0xff, 0x00 }, // COL_YELLOW = 5,
		{ 0xff, 0x00, 0xff }, // COL_MAGENTA = 6,
		{ 0xff, 0xff, 0xff }, // COL_USERDEFINED = 7,
		{ 0x00, 0x00, 0x00 }, // COL_BLACK = 8
		{ 0x00, 0x00, 0x00 } // COL_TRANSPARENT = 9
	};
	static png_byte alpha[10] =
	{255,255,255,255,255,255,255,255,255,0};
	int i =0;
	int j =0;
	int k =0;
	unsigned char array[1024] = "";


	snprintf(fname, sizeof(fname), "%s.png", filename);
	f = fopen(fname, "wb");
	if (!f)
	{
		perror(fname);
		return;
	}

	if (!(png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL, NULL, NULL)))
	{
		printf("unable to create png write struct\n");
	}

	if (!(info_ptr = png_create_info_struct(png_ptr)))
	{
		printf("unable to create png info struct\n");
		png_destroy_write_struct(&png_ptr, (png_infopp) NULL);
	}

	row_pointer = (png_bytep*)malloc(sizeof(png_bytep) * h);
	if(!row_pointer)
	{
		printf("unable to allocate row_pointer\n");
		png_destroy_write_struct(&png_ptr, (png_infopp) NULL);
	}

	png_init_io(png_ptr,f);

	png_set_IHDR (png_ptr,info_ptr,w,h,
				  /* bit_depth */ 8,PNG_COLOR_TYPE_RGB_ALPHA,
				  PNG_INTERLACE_NONE,
				  PNG_COMPRESSION_TYPE_DEFAULT,
				  PNG_FILTER_TYPE_DEFAULT);

	for (i = 0; i < h; i++)
		row_pointer[i] = (png_byte*)malloc(png_get_rowbytes(png_ptr,info_ptr));

	png_set_tRNS (png_ptr, info_ptr, alpha, sizeof(alpha) / sizeof(alpha[0]), NULL);
	png_write_info (png_ptr, info_ptr);

	for (i = 0; i < h; i++)
	{
		for(j = 0 ; j < png_get_rowbytes(png_ptr,info_ptr) ; j+=4)
		{
			k = bitmap[i * w + (j/4)];
			row_pointer[i][j] = ((k >> 16) & 0xff);
			row_pointer[i][j+1] = ((k >> 8) & 0xff);
			row_pointer[i][j+2] = ((k) & 0xff);
			row_pointer[i][j+3] = (k >> 24) & 0xff;
		}
	}

	png_write_image (png_ptr, row_pointer);

	png_write_end (png_ptr, info_ptr);

	for (i = 0; i < h; i++)
		free(row_pointer[i]);
	free(row_pointer);
	png_destroy_write_struct(&png_ptr, (png_infopp) NULL);
	fclose(f);
}

#endif

#define RGBA(r,g,b,a) (((unsigned)(a) << 24) | ((r) << 16) | ((g) << 8) | (b))

typedef struct DVBSubCLUT {
    int id;
    int version;

    uint32_t clut4[4];
    uint32_t clut16[16];
    uint32_t clut256[256];

    struct DVBSubCLUT *next;
} DVBSubCLUT;

static DVBSubCLUT default_clut;

typedef struct DVBSubObjectDisplay {
    int object_id;
    int region_id;

    int x_pos;
    int y_pos;

    int fgcolor;
    int bgcolor;

    struct DVBSubObjectDisplay *region_list_next;
    struct DVBSubObjectDisplay *object_list_next;
} DVBSubObjectDisplay;

typedef struct DVBSubObject {
    int id;
    int version;

    int type;

    DVBSubObjectDisplay *display_list;

    struct DVBSubObject *next;
} DVBSubObject;

typedef struct DVBSubRegionDisplay {
    int region_id;

    int x_pos;
    int y_pos;

    struct DVBSubRegionDisplay *next;
} DVBSubRegionDisplay;

typedef struct DVBSubRegion {
    int id;
    int version;

    int width;
    int height;
    int depth;

    int clut;
    int bgcolor;

    uint8_t *pbuf;
    int buf_size;
    int dirty;

    DVBSubObjectDisplay *display_list;

    struct DVBSubRegion *next;
} DVBSubRegion;

typedef struct DVBSubDisplayDefinition {
    int version;

    int x;
    int y;
    int width;
    int height;
} DVBSubDisplayDefinition;

typedef struct DVBSubContext {
    int composition_id;
    int ancillary_id;

    int version;
    int time_out;
    DVBSubRegion *region_list;
    DVBSubCLUT   *clut_list;
    DVBSubObject *object_list;

    DVBSubRegionDisplay *display_list;
    DVBSubDisplayDefinition *display_definition;
} DVBSubContext;


static DVBSubObject* get_object(DVBSubContext *ctx, int object_id)
{
    DVBSubObject *ptr = ctx->object_list;

    while (ptr && ptr->id != object_id) {
        ptr = ptr->next;
    }

    return ptr;
}

static DVBSubCLUT* get_clut(DVBSubContext *ctx, int clut_id)
{
    DVBSubCLUT *ptr = ctx->clut_list;

    while (ptr && ptr->id != clut_id) {
        ptr = ptr->next;
    }

    return ptr;
}

static DVBSubRegion* get_region(DVBSubContext *ctx, int region_id)
{
    DVBSubRegion *ptr = ctx->region_list;

    while (ptr && ptr->id != region_id) {
        ptr = ptr->next;
    }

    return ptr;
}

static void delete_region_display_list(DVBSubContext *ctx, DVBSubRegion *region)
{
    DVBSubObject *object, *obj2, **obj2_ptr;
    DVBSubObjectDisplay *display, *obj_disp, **obj_disp_ptr;

    while (region->display_list) {
        display = region->display_list;

        object = get_object(ctx, display->object_id);

        if (object) {
            obj_disp_ptr = &object->display_list;
            obj_disp = *obj_disp_ptr;

            while (obj_disp && obj_disp != display) {
                obj_disp_ptr = &obj_disp->object_list_next;
                obj_disp = *obj_disp_ptr;
            }

            if (obj_disp) {
                *obj_disp_ptr = obj_disp->object_list_next;

                if (!object->display_list) {
                    obj2_ptr = &ctx->object_list;
                    obj2 = *obj2_ptr;

                    while (obj2 != object) {
                        assert(obj2);
                        obj2_ptr = &obj2->next;
                        obj2 = *obj2_ptr;
                    }

                    *obj2_ptr = obj2->next;

                    free(obj2);
                }
            }
        }

        region->display_list = display->region_list_next;

        free(display);
    }

}

static void delete_cluts(DVBSubContext *ctx)
{
    DVBSubCLUT *clut;

    while (ctx->clut_list) {
        clut = ctx->clut_list;

        ctx->clut_list = clut->next;

        free(clut);
    }
}

static void delete_objects(DVBSubContext *ctx)
{
    DVBSubObject *object;

    while (ctx->object_list) {
        object = ctx->object_list;

        ctx->object_list = object->next;

        free(object);
    }
}

static void delete_regions(DVBSubContext *ctx)
{
    DVBSubRegion *region;

    while (ctx->region_list) {
        region = ctx->region_list;

        ctx->region_list = region->next;

        delete_region_display_list(ctx, region);

        free(region->pbuf);
        free(region);
    }
}
/**
 * @param composition_id composition-page_id found in Subtitle descriptors
 *                       associated with     subtitle stream in the    PMT
 *                       it could be -1 if not found in PMT.
 * @param ancillary_id ancillary-page_id found in Subtitle descriptors
 *                     associated with     subtitle stream in the    PMT.
 *                       it could be -1 if not found in PMT.
 *
 * @return DVB context kept as void* for abstraction
 *
 */
void* dvbsub_init_decoder(int composition_id,int ancillary_id)
{
    int i, r, g, b, a = 0;
    DVBSubContext *ctx = (DVBSubContext*)malloc(sizeof(DVBSubContext));
    memset(ctx,0,sizeof(DVBSubContext));

    ctx->composition_id = composition_id;
    ctx->ancillary_id   = ancillary_id;

    ctx->version = -1;

    default_clut.id = -1;
    default_clut.next = NULL;

    default_clut.clut4[0] = RGBA(  0,   0,   0,   0);
    default_clut.clut4[1] = RGBA(255, 255, 255, 255);
    default_clut.clut4[2] = RGBA(  0,   0,   0, 255);
    default_clut.clut4[3] = RGBA(127, 127, 127, 255);

    default_clut.clut16[0] = RGBA(  0,   0,   0,   0);
    for (i = 1; i < 16; i++) {
        if (i < 8) {
            r = (i & 1) ? 255 : 0;
            g = (i & 2) ? 255 : 0;
            b = (i & 4) ? 255 : 0;
        } else {
            r = (i & 1) ? 127 : 0;
            g = (i & 2) ? 127 : 0;
            b = (i & 4) ? 127 : 0;
        }
        default_clut.clut16[i] = RGBA(r, g, b, 255);
    }

    default_clut.clut256[0] = RGBA(  0,   0,   0,   0);
    for (i = 1; i < 256; i++) {
        if (i < 8) {
            r = (i & 1) ? 255 : 0;
            g = (i & 2) ? 255 : 0;
            b = (i & 4) ? 255 : 0;
            a = 63;
        } else {
            switch (i & 0x88) {
            case 0x00:
                r = ((i & 1) ? 85 : 0) + ((i & 0x10) ? 170 : 0);
                g = ((i & 2) ? 85 : 0) + ((i & 0x20) ? 170 : 0);
                b = ((i & 4) ? 85 : 0) + ((i & 0x40) ? 170 : 0);
                a = 255;
                break;
            case 0x08:
                r = ((i & 1) ? 85 : 0) + ((i & 0x10) ? 170 : 0);
                g = ((i & 2) ? 85 : 0) + ((i & 0x20) ? 170 : 0);
                b = ((i & 4) ? 85 : 0) + ((i & 0x40) ? 170 : 0);
                a = 127;
                break;
            case 0x80:
                r = 127 + ((i & 1) ? 43 : 0) + ((i & 0x10) ? 85 : 0);
                g = 127 + ((i & 2) ? 43 : 0) + ((i & 0x20) ? 85 : 0);
                b = 127 + ((i & 4) ? 43 : 0) + ((i & 0x40) ? 85 : 0);
                a = 255;
                break;
            case 0x88:
                r = ((i & 1) ? 43 : 0) + ((i & 0x10) ? 85 : 0);
                g = ((i & 2) ? 43 : 0) + ((i & 0x20) ? 85 : 0);
                b = ((i & 4) ? 43 : 0) + ((i & 0x40) ? 85 : 0);
                a = 255;
                break;
            }
        }
        default_clut.clut256[i] = RGBA(r, g, b, a);
    }

    return (void*)ctx;
}
int dvbsub_close_decoder(void *dvb_ctx)
{
    DVBSubContext *ctx = (DVBSubContext *)dvb_ctx;
    DVBSubRegionDisplay *display;

    delete_regions(ctx);

    delete_objects(ctx);

    delete_cluts(ctx);

    freep(&ctx->display_definition);

    while (ctx->display_list) {
        display = ctx->display_list;
        ctx->display_list = display->next;
        free(display);
    }

    return 0;
}

static int dvbsub_read_2bit_string(uint8_t *destbuf, int dbuf_len,
                                   const uint8_t **srcbuf, int buf_size,
                                   int non_mod, uint8_t *map_table, int x_pos)
{
    GetBitContext gb;

    int bits;
    int run_length;
    int pixels_read = x_pos;

    init_get_bits(&gb, *srcbuf, buf_size << 3);

    destbuf += x_pos;

    while (get_bits_count(&gb) < buf_size << 3 && pixels_read < dbuf_len) {
        bits = get_bits(&gb, 2);

        if (bits) {
            if (non_mod != 1 || bits != 1) {
                if (map_table)
                    *destbuf++ = map_table[bits];
                else
                    *destbuf++ = bits;
            }
            pixels_read++;
        } else {
            bits = get_bits1(&gb);
            if (bits == 1) {
                run_length = get_bits(&gb, 3) + 3;
                bits = get_bits(&gb, 2);

                if (non_mod == 1 && bits == 1)
                    pixels_read += run_length;
                else {
                    if (map_table)
                        bits = map_table[bits];
                    while (run_length-- > 0 && pixels_read < dbuf_len) {
                        *destbuf++ = bits;
                        pixels_read++;
                    }
                }
            } else {
                bits = get_bits1(&gb);
                if (bits == 0) {
                    bits = get_bits(&gb, 2);
                    if (bits == 2) {
                        run_length = get_bits(&gb, 4) + 12;
                        bits = get_bits(&gb, 2);

                        if (non_mod == 1 && bits == 1)
                            pixels_read += run_length;
                        else {
                            if (map_table)
                                bits = map_table[bits];
                            while (run_length-- > 0 && pixels_read < dbuf_len) {
                                *destbuf++ = bits;
                                pixels_read++;
                            }
                        }
                    } else if (bits == 3) {
                        run_length = get_bits(&gb, 8) + 29;
                        bits = get_bits(&gb, 2);

                        if (non_mod == 1 && bits == 1)
                            pixels_read += run_length;
                        else {
                            if (map_table)
                                bits = map_table[bits];
                            while (run_length-- > 0 && pixels_read < dbuf_len) {
                                *destbuf++ = bits;
                                pixels_read++;
                            }
                        }
                    } else if (bits == 1) {
                        if (map_table)
                            bits = map_table[0];
                        else
                            bits = 0;
                        run_length = 2;
                        while (run_length-- > 0 && pixels_read < dbuf_len) {
                            *destbuf++ = bits;
                            pixels_read++;
                        }
                    } else {
                        (*srcbuf) += (get_bits_count(&gb) + 7) >> 3;
                        return pixels_read;
                    }
                } else {
                    if (map_table)
                        bits = map_table[0];
                    else
                        bits = 0;
                    *destbuf++ = bits;
                    pixels_read++;
                }
            }
        }
    }

    if (get_bits(&gb, 6))
        printf("DVBSub error: line overflow\n");

    (*srcbuf) += (get_bits_count(&gb) + 7) >> 3;

    return pixels_read;
}

static int dvbsub_read_4bit_string(uint8_t *destbuf, int dbuf_len,
                                   const uint8_t **srcbuf, int buf_size,
                                   int non_mod, uint8_t *map_table, int x_pos)
{
    GetBitContext gb;

    int bits;
    int run_length;
    int pixels_read = x_pos;

    init_get_bits(&gb, *srcbuf, buf_size << 3);

    destbuf += x_pos;

    while (get_bits_count(&gb) < buf_size << 3 && pixels_read < dbuf_len) {
        bits = get_bits(&gb, 4);

        if (bits) {
            if (non_mod != 1 || bits != 1) {
                if (map_table)
                    *destbuf++ = map_table[bits];
                else
                    *destbuf++ = bits;
            }
            pixels_read++;
        } else {
            bits = get_bits1(&gb);
            if (bits == 0) {
                run_length = get_bits(&gb, 3);

                if (run_length == 0) {
                    (*srcbuf) += (get_bits_count(&gb) + 7) >> 3;
                    return pixels_read;
                }

                run_length += 2;

                if (map_table)
                    bits = map_table[0];
                else
                    bits = 0;

                while (run_length-- > 0 && pixels_read < dbuf_len) {
                    *destbuf++ = bits;
                    pixels_read++;
                }
            } else {
                bits = get_bits1(&gb);
                if (bits == 0) {
                    run_length = get_bits(&gb, 2) + 4;
                    bits = get_bits(&gb, 4);

                    if (non_mod == 1 && bits == 1)
                        pixels_read += run_length;
                    else {
                        if (map_table)
                            bits = map_table[bits];
                        while (run_length-- > 0 && pixels_read < dbuf_len) {
                            *destbuf++ = bits;
                            pixels_read++;
                        }
                    }
                } else {
                    bits = get_bits(&gb, 2);
                    if (bits == 2) {
                        run_length = get_bits(&gb, 4) + 9;
                        bits = get_bits(&gb, 4);

                        if (non_mod == 1 && bits == 1)
                            pixels_read += run_length;
                        else {
                            if (map_table)
                                bits = map_table[bits];
                            while (run_length-- > 0 && pixels_read < dbuf_len) {
                                *destbuf++ = bits;
                                pixels_read++;
                            }
                        }
                    } else if (bits == 3) {
                        run_length = get_bits(&gb, 8) + 25;
                        bits = get_bits(&gb, 4);

                        if (non_mod == 1 && bits == 1)
                            pixels_read += run_length;
                        else {
                            if (map_table)
                                bits = map_table[bits];
                            while (run_length-- > 0 && pixels_read < dbuf_len) {
                                *destbuf++ = bits;
                                pixels_read++;
                            }
                        }
                    } else if (bits == 1) {
                        if (map_table)
                            bits = map_table[0];
                        else
                            bits = 0;
                        run_length = 2;
                        while (run_length-- > 0 && pixels_read < dbuf_len) {
                            *destbuf++ = bits;
                            pixels_read++;
                        }
                    } else {
                        if (map_table)
                            bits = map_table[0];
                        else
                            bits = 0;
                        *destbuf++ = bits;
                        pixels_read ++;
                    }
                }
            }
        }
    }

    if (get_bits(&gb, 8))
        printf("DVBSub error: line overflow\n");

    (*srcbuf) += (get_bits_count(&gb) + 7) >> 3;

    return pixels_read;
}

static int dvbsub_read_8bit_string(uint8_t *destbuf, int dbuf_len,
                                    const uint8_t **srcbuf, int buf_size,
                                    int non_mod, uint8_t *map_table, int x_pos)
{
    const uint8_t *sbuf_end = (*srcbuf) + buf_size;
    int bits;
    int run_length;
    int pixels_read = x_pos;

    destbuf += x_pos;

    while (*srcbuf < sbuf_end && pixels_read < dbuf_len) {
        bits = *(*srcbuf)++;

        if (bits) {
            if (non_mod != 1 || bits != 1) {
                if (map_table)
                    *destbuf++ = map_table[bits];
                else
                    *destbuf++ = bits;
            }
            pixels_read++;
        } else {
            bits = *(*srcbuf)++;
            run_length = bits & 0x7f;
            if ((bits & 0x80) == 0) {
                if (run_length == 0) {
                    return pixels_read;
                }

                if (map_table)
                    bits = map_table[0];
                else
                    bits = 0;
                while (run_length-- > 0 && pixels_read < dbuf_len) {
                    *destbuf++ = bits;
                    pixels_read++;
                }
            } else {
                bits = *(*srcbuf)++;

                if (non_mod == 1 && bits == 1)
                    pixels_read += run_length;
                if (map_table)
                    bits = map_table[bits];
                else while (run_length-- > 0 && pixels_read < dbuf_len) {
                    *destbuf++ = bits;
                    pixels_read++;
                }
            }
        }
    }

    if (*(*srcbuf)++)
        printf("DVBSub error: line overflow\n");

    return pixels_read;
}



static void dvbsub_parse_pixel_data_block(void *dvb_ctx, DVBSubObjectDisplay *display,
                                          const uint8_t *buf, int buf_size, int top_bottom, int non_mod)
{
    DVBSubContext *ctx = (DVBSubContext *)dvb_ctx;

    DVBSubRegion *region = get_region(ctx, display->region_id);
    const uint8_t *buf_end = buf + buf_size;
    uint8_t *pbuf;
    int x_pos, y_pos;
    int i;

    uint8_t map2to4[] = { 0x0,  0x7,  0x8,  0xf};
    uint8_t map2to8[] = {0x00, 0x77, 0x88, 0xff};
    uint8_t map4to8[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                         0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t *map_table;

    if (region == 0)
        return;

    pbuf = region->pbuf;
    region->dirty = 1;

    x_pos = display->x_pos;
    y_pos = display->y_pos;

    y_pos += top_bottom;

    while (buf < buf_end) {
        if ((*buf!=0xf0 && x_pos >= region->width) || y_pos >= region->height) {
            printf("Invalid object location! %d-%d %d-%d %02x\n", x_pos, region->width, y_pos, region->height, *buf);
            return;
        }

        switch (*buf++) {
        case 0x10:
            if (region->depth == 8)
                map_table = map2to8;
            else if (region->depth == 4)
                map_table = map2to4;
            else
                map_table = NULL;

            x_pos = dvbsub_read_2bit_string(pbuf + (y_pos * region->width),
                                            region->width, &buf, buf_end - buf,
                                            non_mod, map_table, x_pos);
            break;
        case 0x11:
            if (region->depth < 4) {
                printf("4-bit pixel string in %d-bit region!\n", region->depth);
                return;
            }

            if (region->depth == 8)
                map_table = map4to8;
            else
                map_table = NULL;

            x_pos = dvbsub_read_4bit_string(pbuf + (y_pos * region->width),
                                            region->width, &buf, buf_end - buf,
                                            non_mod, map_table, x_pos);
            break;
        case 0x12:
            if (region->depth < 8) {
                printf("8-bit pixel string in %d-bit region!\n", region->depth);
                return;
            }

            x_pos = dvbsub_read_8bit_string(pbuf + (y_pos * region->width),
                                            region->width, &buf, buf_end - buf,
                                            non_mod, NULL, x_pos);
            break;

        case 0x20:
            map2to4[0] = (*buf) >> 4;
            map2to4[1] = (*buf++) & 0xf;
            map2to4[2] = (*buf) >> 4;
            map2to4[3] = (*buf++) & 0xf;
            break;
        case 0x21:
            for (i = 0; i < 4; i++)
                map2to8[i] = *buf++;
            break;
        case 0x22:
            for (i = 0; i < 16; i++)
                map4to8[i] = *buf++;
            break;

        case 0xf0:
            x_pos = display->x_pos;
            y_pos += 2;
            break;
        default:
            printf("Unknown/unsupported pixel block 0x%x\n", *(buf-1));
        }
    }

}

static void dvbsub_parse_object_segment(void *dvb_ctx,
                                        const uint8_t *buf, int buf_size)
{
    DVBSubContext *ctx = (DVBSubContext*)dvb_ctx;

    const uint8_t *buf_end = buf + buf_size;
    int object_id;
    DVBSubObject *object;
    DVBSubObjectDisplay *display;
    int top_field_len, bottom_field_len;

    int coding_method, non_modifying_color;

    object_id = RB16(buf);
    buf += 2;

    object = get_object(ctx, object_id);

    if (!object)
        return;

    coding_method = ((*buf) >> 2) & 3;
    non_modifying_color = ((*buf++) >> 1) & 1;

    if (coding_method == 0) {
        top_field_len = RB16(buf);
        buf += 2;
        bottom_field_len = RB16(buf);
        buf += 2;

        if (buf + top_field_len + bottom_field_len > buf_end) {
            printf( "Field data size too large\n");
            return;
        }

        for (display = object->display_list; display; display = display->object_list_next) {
            const uint8_t *block = buf;
            int bfl = bottom_field_len;

            dvbsub_parse_pixel_data_block(dvb_ctx, display, block, top_field_len, 0,
                                            non_modifying_color);

            if (bottom_field_len > 0)
                block = buf + top_field_len;
            else
                bfl = top_field_len;

            dvbsub_parse_pixel_data_block(dvb_ctx, display, block, bfl, 1,
                                            non_modifying_color);
        }

    } else if (coding_method == 1) {
        printf( "FIXME support for sring coding standard\n");
    } else {
        printf( "Unknown object coding %d\n", coding_method);
    }

}

static int dvbsub_parse_clut_segment(void *dvb_ctx,
                                        const uint8_t *buf, int buf_size)
{
    DVBSubContext *ctx = (DVBSubContext*)dvb_ctx;

    const uint8_t *buf_end = buf + buf_size;
    int i, clut_id;
    int version;
    DVBSubCLUT *clut;
    int entry_id, depth , full_range;
    int y, cr, cb, alpha;
    int r, g, b, r_add, g_add, b_add;

    printf("DVB clut packet:\n");

    for (i=0; i < buf_size; i++) {
        printf("%02x ", buf[i]);
        if (i % 16 == 15)
            printf("\n");
    }

    if (i % 16)
        printf("\n");

    clut_id = *buf++;
    version = ((*buf)>>4)&15;
    buf += 1;

    clut = get_clut(ctx, clut_id);

    if (!clut) {
        clut = (DVBSubCLUT*)malloc(sizeof(DVBSubCLUT));

        memcpy(clut, &default_clut, sizeof(DVBSubCLUT));

        clut->id = clut_id;
        clut->version = -1;

        clut->next = ctx->clut_list;
        ctx->clut_list = clut;
    }

    if (clut->version != version) {

    clut->version = version;

    while (buf + 4 < buf_end) {
        entry_id = *buf++;

        depth = (*buf) & 0xe0;

        if (depth == 0) {
            printf( "Invalid clut depth 0x%x!\n", *buf);
            return 0;
        }

        full_range = (*buf++) & 1;

        if (full_range) {
            y = *buf++;
            cr = *buf++;
            cb = *buf++;
            alpha = *buf++;
        } else {
            y = buf[0] & 0xfc;
            cr = (((buf[0] & 3) << 2) | ((buf[1] >> 6) & 3)) << 4;
            cb = (buf[1] << 2) & 0xf0;
            alpha = (buf[1] << 6) & 0xc0;

            buf += 2;
        }

        if (y == 0)
            alpha = 0xff;

        YUV_TO_RGB1_CCIR(cb, cr);
        YUV_TO_RGB2_CCIR(r, g, b, y);

        printf("clut %d := (%d,%d,%d,%d)\n", entry_id, r, g, b, alpha);
        if (!!(depth & 0x80) + !!(depth & 0x40) + !!(depth & 0x20) > 1) {
            printf("More than one bit level marked: %x\n", depth);
        }

        if (depth & 0x80)
            clut->clut4[entry_id] = RGBA(r,g,b,255 - alpha);
        else if (depth & 0x40)
            clut->clut16[entry_id] = RGBA(r,g,b,255 - alpha);
        else if (depth & 0x20)
            clut->clut256[entry_id] = RGBA(r,g,b,255 - alpha);
    }
    }
    return 0;
}


static void dvbsub_parse_region_segment(void*dvb_ctx,
                                        const uint8_t *buf, int buf_size)
{
    DVBSubContext *ctx = (DVBSubContext *)dvb_ctx;

    const uint8_t *buf_end = buf + buf_size;
    int region_id, object_id;
    int version;
    DVBSubRegion *region;
    DVBSubObject *object;
    DVBSubObjectDisplay *display;
    int fill;

    if (buf_size < 10)
        return;

    region_id = *buf++;
    version = ((*buf)>>4) & 15;

    region = get_region(ctx, region_id);

    if (!region) {
        region = (struct DVBSubRegion*) malloc(sizeof(struct DVBSubRegion));
        memset(region,0,sizeof(struct DVBSubRegion));

        region->id = region_id;
        region->version = version;

        region->next = ctx->region_list;
        ctx->region_list = region;
    } else if(version == region->version)
        return;
    fill = ((*buf++) >> 3) & 1;

    region->width = RB16(buf);
    buf += 2;
    region->height = RB16(buf);
    buf += 2;

    if (region->width * region->height != region->buf_size) {
        free(region->pbuf);

        region->buf_size = region->width * region->height;

        region->pbuf = (uint8_t*)malloc(region->buf_size);

        fill = 1;
        region->dirty = 0;
    }

    region->depth = 1 << (((*buf++) >> 2) & 7);
    if(region->depth<2 || region->depth>8){
        printf( "region depth %d is invalid\n", region->depth);
        region->depth= 4;
    }
    region->clut = *buf++;

    if (region->depth == 8) {
        region->bgcolor = *buf++;
        buf += 1;
    } else {
        buf += 1;

        if (region->depth == 4)
            region->bgcolor = (((*buf++) >> 4) & 15);
        else
            region->bgcolor = (((*buf++) >> 2) & 3);
    }

    printf("Region %d, (%dx%d)\n", region_id, region->width, region->height);

    if (fill) {
        memset(region->pbuf, region->bgcolor, region->buf_size);
        printf( "Fill region (%d)\n", region->bgcolor);
    }

    delete_region_display_list(ctx, region);

    while (buf + 5 < buf_end) {
        object_id = RB16(buf);
        buf += 2;

        object = get_object(ctx, object_id);

        if (!object) {
            object = (struct DVBSubObject*)malloc(sizeof(struct DVBSubObject));
            memset(object,0,sizeof(struct DVBSubObject));

            object->id = object_id;
            object->next = ctx->object_list;
            ctx->object_list = object;
        }

        object->type = (*buf) >> 6;

        display = (struct DVBSubObjectDisplay*)malloc(sizeof(struct DVBSubObjectDisplay));
        memset(display,0,sizeof(struct DVBSubObjectDisplay));

        display->object_id = object_id;
        display->region_id = region_id;

        display->x_pos = RB16(buf) & 0xfff;
        buf += 2;
        display->y_pos = RB16(buf) & 0xfff;
        buf += 2;

        if ((object->type == 1 || object->type == 2) && buf+1 < buf_end) {
            display->fgcolor = *buf++;
            display->bgcolor = *buf++;
        }

        display->region_list_next = region->display_list;
        region->display_list = display;

        display->object_list_next = object->display_list;
        object->display_list = display;
    }
}

static void dvbsub_parse_page_segment(void *dvb_ctx,
                                        const uint8_t *buf, int buf_size)
{
    DVBSubContext *ctx = (DVBSubContext*)dvb_ctx;
    DVBSubRegionDisplay *display;
    DVBSubRegionDisplay *tmp_display_list, **tmp_ptr;

    const uint8_t *buf_end = buf + buf_size;
    int region_id;
    int page_state;
    int timeout;
    int version;

    if (buf_size < 1)
        return;

    timeout = *buf++;
    version = ((*buf)>>4) & 15;
    page_state = ((*buf++) >> 2) & 3;

    //if version same mean we are already updated
    if (ctx->version == version)
    {
        return;
    }

    ctx->time_out = timeout;
    ctx->version = version;

    printf( "Page time out %ds, state %d\n", ctx->time_out, page_state);

    if (page_state == 1 || page_state == 2) {
        delete_regions(ctx);
        delete_objects(ctx);
        delete_cluts(ctx);
    }

    tmp_display_list = ctx->display_list;
    ctx->display_list = NULL;

    while (buf + 5 < buf_end) {
        region_id = *buf++;
        buf += 1;

        display = tmp_display_list;
        tmp_ptr = &tmp_display_list;

        while (display && display->region_id != region_id) {
            tmp_ptr = &display->next;
            display = display->next;
        }

        if (!display)
        {
            display = (struct DVBSubRegionDisplay*)malloc(sizeof(struct DVBSubRegionDisplay));
            memset(display,0,sizeof(struct DVBSubRegionDisplay));
        }


        display->region_id = region_id;

        display->x_pos = RB16(buf);
        buf += 2;
        display->y_pos = RB16(buf);
        buf += 2;

        *tmp_ptr = display->next;

        display->next = ctx->display_list;
        ctx->display_list = display;

        printf( "Region %d, (%d,%d)\n", region_id, display->x_pos, display->y_pos);
    }

    while (tmp_display_list) {
        display = tmp_display_list;

        tmp_display_list = display->next;

        free(display);
    }

}


#ifdef DEBUG
static void save_display_set(DVBSubContext *ctx)
{
    DVBSubRegion *region;
    DVBSubRegionDisplay *display;
    DVBSubCLUT *clut;
    uint32_t *clut_table;
    int x_pos, y_pos, width, height;
    int x, y, y_off, x_off;
    uint32_t *pbuf;
    char filename[32];
    static int fileno_index = 0;

    x_pos = -1;
    y_pos = -1;
    width = 0;
    height = 0;

    for (display = ctx->display_list; display; display = display->next) {
        region = get_region(ctx, display->region_id);

        if (x_pos == -1) {
            x_pos = display->x_pos;
            y_pos = display->y_pos;
            width = region->width;
            height = region->height;
        } else {
            if (display->x_pos < x_pos) {
                width += (x_pos - display->x_pos);
                x_pos = display->x_pos;
            }

            if (display->y_pos < y_pos) {
                height += (y_pos - display->y_pos);
                y_pos = display->y_pos;
            }

            if (display->x_pos + region->width > x_pos + width) {
                width = display->x_pos + region->width - x_pos;
            }

            if (display->y_pos + region->height > y_pos + height) {
                height = display->y_pos + region->height - y_pos;
            }
        }
    }

    if (x_pos >= 0) {

        pbuf = (uint32_t*)malloc(width * height * 4);
        memset(pbuf,0x0,width * height * 4);


        for (display = ctx->display_list; display; display = display->next) {
            region = get_region(ctx, display->region_id);

            x_off = display->x_pos - x_pos;
            y_off = display->y_pos - y_pos;

            clut = get_clut(ctx, region->clut);

            if (clut == 0)
                clut = &default_clut;

            switch (region->depth) {
            case 2:
                clut_table = clut->clut4;
                break;
            case 8:
                clut_table = clut->clut256;
                break;
            case 4:
            default:
                clut_table = clut->clut16;
                break;
            }

            for (y = 0; y < region->height; y++) {
                for (x = 0; x < region->width; x++) {
                    pbuf[((y + y_off) * width) + x_off + x] =
                        clut_table[region->pbuf[y * region->width + x]];
                }
            }

        }

        snprintf(filename, sizeof(filename), "dvbs.%d", fileno_index);

        png_save(filename, pbuf, width, height);

        free(pbuf);
    }

    fileno_index++;
}
#endif

static void dvbsub_parse_display_definition_segment(void *dvb_ctx,
                                                    const uint8_t *buf,
                                                    int buf_size)
{
    DVBSubContext *ctx = (DVBSubContext *)dvb_ctx;
    DVBSubDisplayDefinition *display_def = ctx->display_definition;
    int dds_version, info_byte;

    if (buf_size < 5)
        return;

    info_byte   = bytestream_get_byte(&buf);
    dds_version = info_byte >> 4;
    if (display_def && display_def->version == dds_version)
        return; // already have this display definition version

    if (!display_def) {
        display_def             = (struct DVBSubDisplayDefinition*)malloc(sizeof(*display_def));
        memset(display_def,0,sizeof(*display_def));
        ctx->display_definition = display_def;
    }
    if (!display_def)
        return;

    display_def->version = dds_version;
    display_def->x       = 0;
    display_def->y       = 0;
    display_def->width   = bytestream_get_be16(&buf) + 1;
    display_def->height  = bytestream_get_be16(&buf) + 1;

    if (buf_size < 13)
        return;

    if (info_byte & 1<<3) { // display_window_flag
        display_def->x = bytestream_get_be16(&buf);
        display_def->width  = bytestream_get_be16(&buf) - display_def->x + 1;
        display_def->y = bytestream_get_be16(&buf);
        display_def->height = bytestream_get_be16(&buf) - display_def->y + 1;
    }
}

static int dvbsub_display_end_segment(void *dvb_ctx, const uint8_t *buf,
                                        int buf_size)
{
    DVBSubContext *ctx = (DVBSubContext *)dvb_ctx;

    DVBSubRegion *region;
    DVBSubRegionDisplay *display;
    DVBSubCLUT *clut;
    int i;



    for (display = ctx->display_list; display; display = display->next)
    {
        region = get_region(ctx, display->region_id);
    }

    i = 0;

    for (display = ctx->display_list; display; display = display->next) {
        region = get_region(ctx, display->region_id);

        if (!region)
            continue;

        if (!region->dirty)
            continue;
        clut = get_clut(ctx, region->clut);

        if (!clut)
            clut = &default_clut;

        i++;
    }
#ifdef DEBUG
    save_display_set(ctx);
#endif

    return 1;
}
/**
 * @param dvb_ctx    PreInitialized DVB context using DVB
 * @param data       output subtitle data, to be implemented
 * @param data_size  Output subtitle data  size. pass the pointer to an intiger, NOT to be NULL.
 * @param buf        buffer containg segment data, first sync byte needto 0x0f.
 *                   does not include data_identifier and subtitle_stream_id.
 * @param buf_size   size of buf buffer
 *
 * @return           -1 on error
 */
int dvbsub_decode(void *dvb_ctx,
                         void *data, int *data_size,
                         const unsigned char *buf, int buf_size)
{
    DVBSubContext *ctx = (DVBSubContext *)dvb_ctx;
//    AVSubtitle *sub = data;
    const uint8_t *p, *p_end;
    int segment_type;
    int page_id;
    int segment_length;
    int ret;
    int got_segment = 0;


    if (buf_size <= 6 || *buf != 0x0f) {
        printf( "incomplete or broken packet");
        return -1;
    }

    p = buf;
    p_end = buf + buf_size;

    while (p_end - p >= 6 && *p == 0x0f) {
        p += 1;
        segment_type = *p++;
        page_id = RB16(p);
        p += 2;
        segment_length = RB16(p);
        p += 2;

        if (p_end - p < segment_length) {
            printf( "incomplete or broken packet");
            return -1;
        }

        if (page_id == ctx->composition_id || page_id == ctx->ancillary_id ||
            ctx->composition_id == -1 || ctx->ancillary_id == -1) {
            switch (segment_type) {
            case DVBSUB_PAGE_SEGMENT:
                dvbsub_parse_page_segment(dvb_ctx, p, segment_length);
                got_segment |= 1;
                break;
            case DVBSUB_REGION_SEGMENT:
                dvbsub_parse_region_segment(dvb_ctx, p, segment_length);
                got_segment |= 2;
                break;
            case DVBSUB_CLUT_SEGMENT:
                ret = dvbsub_parse_clut_segment(dvb_ctx, p, segment_length);
                if (ret < 0) return ret;
                got_segment |= 4;
                break;
            case DVBSUB_OBJECT_SEGMENT:
                dvbsub_parse_object_segment(dvb_ctx, p, segment_length);
                got_segment |= 8;
                break;
            case DVBSUB_DISPLAYDEFINITION_SEGMENT:
                dvbsub_parse_display_definition_segment(dvb_ctx, p, segment_length);
                break;
            case DVBSUB_DISPLAY_SEGMENT:
                *data_size = dvbsub_display_end_segment(dvb_ctx, p, segment_length);
                got_segment |= 16;
                break;
            default:
                printf( "Subtitling segment type 0x%x, page id %d, length %d\n",
                        segment_type, page_id, segment_length);
                break;
            }
        }

        p += segment_length;
    }
    // Some streams do not send a display segment but if we have all the other
    // segments then we need no further data.
    if (got_segment == 15)
        *data_size = dvbsub_display_end_segment(dvb_ctx, p, 0);

    return p - buf;
}
/**
 * @func parse_dvb_description
 *
 * data pointer to this function should be after description length and description tag is parsed
 *
 * @param cfg preallocated dvb_config structure where parsed description will be stored,Not to be NULL
 *
 * @return return -1 if invalid data found other wise 0 if everything goes well
 * errno is set is to EINVAL if invalid data is found
 */
int  parse_dvb_description (struct dvb_config* cfg,unsigned char*data,unsigned int len)
{
    int i = 0;
    int j = 0;
    /* 8 bytes per DVB subtitle substream d2:
     * ISO_639_language_code (3 bytes),
     * subtitling_type (1 byte),
     * composition_page_id (2 bytes),
     * ancillary_page_id (2 bytes)
     */

    cfg->n_language = len/8;

    if (len > 0 && len % 8 != 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (cfg->n_language > 1)
    {
        printf("DVB subtitles with multiple languages");
    }

    if (cfg->n_language > MAX_LANGUAGE_PER_DESC)
    {
        printf("not supported more then %d language",MAX_LANGUAGE_PER_DESC);
    }

    for (i = 0; i < cfg->n_language; i++,data += i*8)
    {
        /* setting language to undefined if not found in language lkup table */
        for(j = 0,cfg->lang_index[i] = 0;dvb_language[j] != NULL;j++)
        {
            if(!strncmp((const char*)(data),dvb_language[j],3))
                cfg->lang_index[i] = j;
        }
        cfg->sub_type[i] = data[3];
        cfg->composition_id[i] = RB16(data + 4);
        cfg->ancillary_id[i] = RB16(data + 6);

    }

    printf("lang %d %s\n",cfg->lang_index[0],dvb_language[cfg->lang_index[0]]);
    printf("comp %hd anci %hd stype %hhd\n",cfg->composition_id[0],cfg->ancillary_id[0],cfg->sub_type[0]);
    return 0;
}