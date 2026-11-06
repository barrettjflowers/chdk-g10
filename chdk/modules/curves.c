#include "camera_info.h"
#include "conf.h"
#include "shooting.h"
#include "raw.h"
#include "gui_lang.h"

#include "curves.h"
#include "module_def.h"

/*
	Expands curves features
	
	RGB curves:  	ouput = curve[input]
	L curves: 		output = curve[L] *(input-black) + black
	RGB curves and L curves are 4 colors curves
	L curves contain multiplicator factor (value = multiplicator * 10^12)
	
	DR curves:		output = curve[L] *(input-black) + black
	DR curves are gray scale only
	There are 2 DR curves +1EV and +2EV gain
	
	Auto DR automatically calculate the +1EV or +2EV base on 
	EV under exposure selected by user to avoid highlight burnout 
	(using Zebra)
	
	External function added:  shooting_get_ev_correction1 (added in core/shooting)
	Modified gui.c for curve enable
	
	3/28/09: Fix black holes, and column skipping, use Green as luminance in Luminance curve
	
*/
unsigned short *curve_data = NULL;

#define CURVE_SIZE 1024
#define CURVE_BYTES (CURVE_SIZE*sizeof(unsigned short))

#define BLACK 32
#define LIM( a ) (((a)>1023) ? 1023: a )
#define RBLACK( a ) (((a)>BLACK) ? ((a)-BLACK) : 0)

#define CURVE0( a, b ) (((a) * curve0[b]) >> 12)
#define CURVE1( a, b ) (((a) * curve1[b]) >> 12)
#define CURVE2( a, b ) (((a) * curve2[b]) >> 12)
#define CURVE3( a, b ) (((a) * curve3[b]) >> 12)

typedef enum {
	CURVE_NONE,
	CURVE_CUSTOM,
	CURVE_SYSTEM,
}CURVE_TYPE;

CURVE_TYPE current_curve_type = CURVE_NONE;

static void curve_free_data() {
	free(curve_data);
	curve_data = NULL;
	current_curve_type = CURVE_NONE;
}

// TODO we should avoid re-loading the same file
static int curve_load_data(const char *name,CURVE_TYPE curve_type) {
	unsigned size;
	FILE *fd;
	if (current_curve_type != curve_type) {
		curve_free_data();
	}

	if (curve_type == CURVE_NONE) {
		return 1;
	}

	if(!name || !*name) {
		curve_free_data();
		return 0;
	}

 	if( curve_type == CURVE_CUSTOM ) {
 		size = CURVE_BYTES*4;
	}
	else {
 		size = CURVE_BYTES*2;
	}
	
	if (!curve_data)
		curve_data=malloc(size);

	if(!curve_data) {
		curve_free_data();
		return 0;
	}
	fd = fopen(name, "r+b");
	if (!fd) {
		curve_free_data();
		return 0;
	}
	fread(curve_data, 1, size, fd);
	fclose(fd);
	current_curve_type = curve_type;
	return 1;
}

void curve_init_mode() {
	switch(conf.curve_enable) {
		case 1: // custom - ensure alloc and load conf.curve_file
			curve_load_data(conf.curve_file,CURVE_CUSTOM);
		break;
		case 2: // system - ensure alloc and load syscurve
		case 3:
		case 4:
			curve_load_data("A/CHDK/SYSCURVES.CVF",CURVE_SYSTEM);
		break;
		default:
			conf.curve_enable = 0;
		case 0: // disabled - free
			curve_free_data();
	}
}

void curve_set_mode(int value) {
	if((value>=0) && (value<=4)) conf.curve_enable=value;
	curve_init_mode();
}

void curve_set_file(const char *s) {
    int l;
	if(s) {
        if(strncmp(s,"A/",2)==0) strncpy(conf.curve_file,s,99);
        else {
            l=strlen(CURVE_DIR);
            strcpy(conf.curve_file,CURVE_DIR);
            conf.curve_file[l]='/';
            strncpy(&conf.curve_file[l+1],s,99-l-1);
        }
        conf.curve_file[99]=0x0;
        curve_init_mode();
   	}
}

// TODO border pixels should not be hard coded
void curveRGB_apply() {
	int i,j;
	unsigned short pixVal0, pixVal1, pixVal2;
	unsigned char *src;

	unsigned short *curve0 = curve_data;
	unsigned short *curve1 = curve0 + CURVE_SIZE;
	unsigned short *curve2 = curve1 + CURVE_SIZE;
	unsigned short *curve3 = curve2 + CURVE_SIZE;
	
	if (current_curve_type != CURVE_CUSTOM) 
		return;
	
	// Set pointer to picture raw data in memory
	src = (unsigned char *) get_raw_image_addr();	
	
	
	// Loop through picture rows
	for (i=camera_sensor.raw_rows; i;i-=2){
		// Loop through picture columns 
		for (j=camera_sensor.raw_rowpix; j; j-=8, src+=10){
			pixVal0=((0x3fc&(((unsigned short)(src[1]))<<2)) | (src[0] >> 6));
			pixVal1=((0x3f0&(((unsigned short)(src[0]))<<4)) | (src[3] >> 4));
				pixVal0 = curve0[pixVal0];
				pixVal1 = curve1[pixVal1];
            *(src+1) = (unsigned char) ((pixVal0>>2)); // 0
            *src = (unsigned char) ((pixVal0<<6)|(pixVal1>>4)); //0, 1

			pixVal2=((0x3c0&(((unsigned short)(src[3]))<<6)) | (src[2] >> 2));
			pixVal0=((0x300&(((unsigned short)(src[2]))<<8)) | (src[5])); 
				pixVal2 = curve0[pixVal2];
				pixVal0 = curve1[pixVal0];
            *(src+3) = (unsigned char) ((pixVal1<<4)|(pixVal2>>6)); //1,2
            *(src+2) = (unsigned char) ((pixVal2<<2)|(pixVal0>>8)); //2,3 =>(2,0)
            *(src+5) = (unsigned char) ((pixVal0)); //3 (=>0)

			pixVal0=((0x3fc&(((unsigned short)(src[4]))<<2)) | (src[7] >> 6)); 
			pixVal1=((0x3f0&(((unsigned short)(src[7]))<<4)) | (src[6] >> 4)); 
				pixVal0 = curve0[pixVal0];
				pixVal1 = curve1[pixVal1];
            *(src+4) = (unsigned char) ((pixVal0>>2)); // 4 => 0
            *(src+7) = (unsigned char) ((pixVal0<<6)|(pixVal1>>4)); // 4,5 => (0,1)

			pixVal2=((0x3c0&(((unsigned short)(src[6]))<<6)) | (src[9] >> 2)); 
			pixVal0=((0x300&(((unsigned short)(src[9]))<<8)) | (src[8]));
				pixVal2 = curve0[pixVal2];
				pixVal0 = curve1[pixVal0];
            *(src+6) = (unsigned char) ((pixVal1<<4)|(pixVal2>>6)); // 5,6 => (1,2)
            *(src+9) = (unsigned char) ((pixVal2<<2)|(pixVal0>>8)); // 6,7 =>(2,0)
            *(src+8) = (unsigned char) ((pixVal0)); //7 (=>0)		}
		}
		for (j=camera_sensor.raw_rowpix;j; j-=8, src+=10){
			pixVal0=((0x3fc&(((unsigned short)(src[1]))<<2)) | (src[0] >> 6));
			pixVal1=((0x3f0&(((unsigned short)(src[0]))<<4)) | (src[3] >> 4));
				pixVal0 = curve2[pixVal0];
				pixVal1 = curve3[pixVal1];
            *(src+1) = (unsigned char) ((pixVal0>>2)); // 0
            *src = (unsigned char) ((pixVal0<<6)|(pixVal1>>4)); //0, 1

			pixVal2=((0x3c0&(((unsigned short)(src[3]))<<6)) | (src[2] >> 2));
			pixVal0=((0x300&(((unsigned short)(src[2]))<<8)) | (src[5])); 
				pixVal2 = curve2[pixVal2];
				pixVal0 = curve3[pixVal0];
            *(src+3) = (unsigned char) ((pixVal1<<4)|(pixVal2>>6)); //1,2
            *(src+2) = (unsigned char) ((pixVal2<<2)|(pixVal0>>8)); //2,3 =>(2,0)
            *(src+5) = (unsigned char) ((pixVal0)); //3 (=>0)

			pixVal0=((0x3fc&(((unsigned short)(src[4]))<<2)) | (src[7] >> 6)); 
			pixVal1=((0x3f0&(((unsigned short)(src[7]))<<4)) | (src[6] >> 4)); 
				pixVal0 = curve2[pixVal0];
				pixVal1 = curve3[pixVal1];
            *(src+4) = (unsigned char) ((pixVal0>>2)); // 4 => 0
            *(src+7) = (unsigned char) ((pixVal0<<6)|(pixVal1>>4)); // 4,5 => (0,1)

			pixVal2=((0x3c0&(((unsigned short)(src[6]))<<6)) | (src[9] >> 2)); 
			pixVal0=((0x300&(((unsigned short)(src[9]))<<8)) | (src[8]));
				pixVal2 = curve2[pixVal2];
				pixVal0 = curve3[pixVal0];
            *(src+6) = (unsigned char) ((pixVal1<<4)|(pixVal2>>6)); // 5,6 => (1,2)
            *(src+9) = (unsigned char) ((pixVal2<<2)|(pixVal0>>8)); // 6,7 =>(2,0)
            *(src+8) = (unsigned char) ((pixVal0)); //7 (=>0)
		}
	}
}


void curveL_apply(unsigned sys_index) {
	int i,j;
	unsigned short pixVal0, pixVal1, pixVal2;
	unsigned char *src;
	
	unsigned short *curve0;
	unsigned short *curve1;
	unsigned short *curve2;
	unsigned short *curve3;
	if (current_curve_type == CURVE_SYSTEM) {
		if (sys_index > 1) 
			return;
		curve0 = curve1 = curve2 = curve3 = curve_data + sys_index*CURVE_SIZE;
	}
	else if(current_curve_type == CURVE_CUSTOM) {
		curve0 = curve_data;
		curve1 = curve0 + CURVE_SIZE;
		curve2 = curve1 + CURVE_SIZE;
		curve3 = curve2 + CURVE_SIZE;
	} else {
		return;
	}


	// Set pointer to picture raw data in memory
	src = (unsigned char *) get_raw_image_addr();	
	
	// Loop through picture rows
	for (i=camera_sensor.raw_rows; i;i-=2){
		// Loop through picture columns 
		for (j=camera_sensor.raw_rowpix; j; j-=8, src+=10){
			pixVal0=((0x3fc&(((unsigned short)(src[1]))<<2)) | (src[0] >> 6));
			pixVal1=((0x3f0&(((unsigned short)(src[0]))<<4)) | (src[3] >> 4));
			if (pixVal1) {
				pixVal1 = RBLACK( pixVal1 );
				if (pixVal0) {
					pixVal0 = RBLACK( pixVal0 );
					pixVal0 = CURVE0( pixVal0, pixVal1 ) + BLACK;
					pixVal0 = LIM( pixVal0 );
				}
				pixVal1 = CURVE1( pixVal1, pixVal1 ) + BLACK;
				pixVal1 = LIM( pixVal1 );
			}
            *(src+1) = (unsigned char) ((pixVal0>>2)); // 0
            *src = (unsigned char) ((pixVal0<<6)|(pixVal1>>4)); //0, 1

			pixVal2=((0x3c0&(((unsigned short)(src[3]))<<6)) | (src[2] >> 2));
			pixVal0=((0x300&(((unsigned short)(src[2]))<<8)) | (src[5])); 
			if (pixVal0) {
				pixVal0 = RBLACK( pixVal0 );
				if (pixVal2) {
					pixVal2 = RBLACK( pixVal2 );
					pixVal2 = CURVE0( pixVal2, pixVal0 ) + BLACK;
					pixVal2 = LIM( pixVal2 );
				}
				pixVal0 = CURVE1( pixVal0, pixVal0 ) + BLACK;
				pixVal0 = LIM( pixVal0 );
			}
            *(src+3) = (unsigned char) ((pixVal1<<4)|(pixVal2>>6)); //1,2
            *(src+2) = (unsigned char) ((pixVal2<<2)|(pixVal0>>8)); //2,3 =>(2,0)
            *(src+5) = (unsigned char) ((pixVal0)); //3 (=>0)

			pixVal0=((0x3fc&(((unsigned short)(src[4]))<<2)) | (src[7] >> 6)); 
			pixVal1=((0x3f0&(((unsigned short)(src[7]))<<4)) | (src[6] >> 4)); 
			if (pixVal1) {
				pixVal1 = RBLACK( pixVal1 );
				if (pixVal0) {
					pixVal0 = RBLACK( pixVal0 );
					pixVal0 = CURVE0( pixVal0, pixVal1 ) + BLACK;
					pixVal0 = LIM( pixVal0 );
				}
				pixVal1 = CURVE1( pixVal1, pixVal1 ) + BLACK;
				pixVal1 = LIM( pixVal1 );
			}
            *(src+4) = (unsigned char) ((pixVal0>>2)); // 4 => 0
            *(src+7) = (unsigned char) ((pixVal0<<6)|(pixVal1>>4)); // 4,5 => (0,1)

			pixVal2=((0x3c0&(((unsigned short)(src[6]))<<6)) | (src[9] >> 2)); 
			pixVal0=((0x300&(((unsigned short)(src[9]))<<8)) | (src[8]));
			if (pixVal0) {
				pixVal0 = RBLACK( pixVal0 );
				if (pixVal2) {
					pixVal2 = RBLACK( pixVal2 );
					pixVal2 = CURVE0( pixVal2, pixVal0 ) + BLACK;
					pixVal2 = LIM( pixVal2 );
				}
				pixVal0 = CURVE1( pixVal0, pixVal0 ) + BLACK;
				pixVal0 = LIM( pixVal0 );
			}
            *(src+6) = (unsigned char) ((pixVal1<<4)|(pixVal2>>6)); // 5,6 => (1,2)
            *(src+9) = (unsigned char) ((pixVal2<<2)|(pixVal0>>8)); // 6,7 =>(2,0)
            *(src+8) = (unsigned char) ((pixVal0)); //7 (=>0)
		}
		for (j=camera_sensor.raw_rowpix; j; j-=8, src+=10){
			pixVal0=((0x3fc&(((unsigned short)(src[1]))<<2)) | (src[0] >> 6));
			pixVal1=((0x3f0&(((unsigned short)(src[0]))<<4)) | (src[3] >> 4));
			if (pixVal0) {
				pixVal0 = RBLACK( pixVal0 );
				if (pixVal1) {
					pixVal1 = RBLACK( pixVal1 );
					pixVal1 = CURVE3( pixVal1, pixVal0 ) + BLACK;
					pixVal1 = LIM( pixVal1 );
				}
				pixVal0 = CURVE2( pixVal0, pixVal0 ) + BLACK;
				pixVal0 = LIM( pixVal0 );
			}
            *(src+1) = (unsigned char) ((pixVal0>>2)); // 0
            *src = (unsigned char) ((pixVal0<<6)|(pixVal1>>4)); //0, 1

			pixVal2=((0x3c0&(((unsigned short)(src[3]))<<6)) | (src[2] >> 2));
			pixVal0=((0x300&(((unsigned short)(src[2]))<<8)) | (src[5])); 
			if (pixVal2) {
				pixVal2 = RBLACK( pixVal2 );
				if (pixVal0) {
					pixVal0 = RBLACK( pixVal0 );
					pixVal0 = CURVE3( pixVal0, pixVal2 ) + BLACK;
					pixVal0 = LIM( pixVal0 );
				}
				pixVal2 = CURVE2( pixVal2, pixVal2 ) + BLACK;
				pixVal2 = LIM( pixVal2 );
			}
            *(src+3) = (unsigned char) ((pixVal1<<4)|(pixVal2>>6)); //1,2
            *(src+2) = (unsigned char) ((pixVal2<<2)|(pixVal0>>8)); //2,3 =>(2,0)
            *(src+5) = (unsigned char) ((pixVal0)); //3 (=>0)

			pixVal0=((0x3fc&(((unsigned short)(src[4]))<<2)) | (src[7] >> 6)); 
			pixVal1=((0x3f0&(((unsigned short)(src[7]))<<4)) | (src[6] >> 4)); 
			if (pixVal0) {
				pixVal0 = RBLACK( pixVal0 );
				if (pixVal1) {
					pixVal1 = RBLACK( pixVal1 );
					pixVal1 = CURVE3( pixVal1, pixVal0 ) + BLACK;
					pixVal1 = LIM( pixVal1 );
				}
				pixVal0 = CURVE2( pixVal0, pixVal0 ) + BLACK;
				pixVal0 = LIM( pixVal0 );
			}
            *(src+4) = (unsigned char) ((pixVal0>>2)); // 4 => 0
            *(src+7) = (unsigned char) ((pixVal0<<6)|(pixVal1>>4)); // 4,5 => (0,1)

			pixVal2=((0x3c0&(((unsigned short)(src[6]))<<6)) | (src[9] >> 2)); 
			pixVal0=((0x300&(((unsigned short)(src[9]))<<8)) | (src[8]));
			if (pixVal2) {
				pixVal2 = RBLACK( pixVal2 );
				if (pixVal0) {
					pixVal0 = RBLACK( pixVal0 );
					pixVal0 = CURVE3( pixVal0, pixVal2 ) + BLACK;
					pixVal0 = LIM( pixVal0 );
				}
				pixVal2 = CURVE2( pixVal2, pixVal2 ) + BLACK;
				pixVal2 = LIM( pixVal2 );
			}
            *(src+6) = (unsigned char) ((pixVal1<<4)|(pixVal2>>6)); // 5,6 => (1,2)
            *(src+9) = (unsigned char) ((pixVal2<<2)|(pixVal0>>8)); // 6,7 =>(2,0)
            *(src+8) = (unsigned char) ((pixVal0)); //7 (=>0)
		}
	}
}

/*
	12 bit sensor support.
	Curve files keep the 10 bit format (1024 entries per channel), so the same
	.CVF files and SYSCURVES.CVF work on both sensor types.

	Speed: the raw buffer is uncached, so memory accesses are expensive.
	Pixels are handled 8 at a time as 3 aligned 32 bit words (12 bytes).
	Macros instead of functions, because CHDK builds with -fno-inline.

	Packing (see get_raw_pixel in core/raw.c): each 6 byte group holds 4 pixels
	as a 48 bit big-endian stream split into 3 little-endian halfwords h0 h1 h2:
	p0 = h0>>4, p1 = (h0&F)<<8 | h1>>8, p2 = (h1&FF)<<4 | h2>>12, p3 = h2&FFF
*/

#define MAX12  4095
#define LUT12  4096

#define UNPACK8(w, p) do { \
	unsigned h0_=(w)[0]&0xFFFF, h1_=(w)[0]>>16, h2_=(w)[1]&0xFFFF; \
	unsigned h3_=(w)[1]>>16, h4_=(w)[2]&0xFFFF, h5_=(w)[2]>>16; \
	(p)[0]=h0_>>4; (p)[1]=((h0_&0xF)<<8)|(h1_>>8); (p)[2]=((h1_&0xFF)<<4)|(h2_>>12); (p)[3]=h2_&0xFFF; \
	(p)[4]=h3_>>4; (p)[5]=((h3_&0xF)<<8)|(h4_>>8); (p)[6]=((h4_&0xFF)<<4)|(h5_>>12); (p)[7]=h5_&0xFFF; \
} while (0)

#define PACK8(w, p) do { \
	unsigned h0_=((p)[0]<<4)|((p)[1]>>8), h1_=(((p)[1]&0xFF)<<8)|((p)[2]>>4), h2_=(((p)[2]&0xF)<<12)|(p)[3]; \
	unsigned h3_=((p)[4]<<4)|((p)[5]>>8), h4_=(((p)[5]&0xFF)<<8)|((p)[6]>>4), h5_=(((p)[6]&0xF)<<12)|(p)[7]; \
	(w)[0]=h0_|(h1_<<16); (w)[1]=h2_|(h3_<<16); (w)[2]=h4_|(h5_<<16); \
} while (0)

// column parity (0/1) of the green pixel in even rows, from the CFA pattern.
// cfa_pattern byte 0 = row 0 col 0 color (0=R 1=G 2=B).
static int green_col_even_row() {
	return ((camera_sensor.cfa_pattern & 0xFF) == 1) ? 0 : 1;
}

// Word access needs 4 byte aligned rows and whole 8 pixel groups (true on the G10).
static int raw_word_ok(const unsigned char *row) {
	return !((unsigned)row & 3) && !(camera_sensor.raw_rowlen & 3) && !(camera_sensor.raw_rowpix & 7);
}

// Expand the 4 x 1024 entry (10 bit) curves to 4 x 4096 entry (12 bit) tables,
// interpolating between entries. Caller frees.
static unsigned short *build_lut12() {
	unsigned k, v;
	unsigned short *lut = malloc(4 * LUT12 * sizeof(unsigned short));
	if (!lut)
		return NULL;
	for (k = 0; k < 4; k++) {
		const unsigned short *c = curve_data + k*CURVE_SIZE;
		unsigned short *t = lut + k*LUT12;
		for (v = 0; v < LUT12; v++) {
			unsigned i = v >> 2, f = v & 3;
			unsigned out = (i < CURVE_SIZE-1) ? c[i]*(4-f) + c[i+1]*f : c[i]*4 + f;
			t[v] = (out > MAX12) ? MAX12 : out;
		}
	}
	return lut;
}

static void curveRGB_apply12() {
	unsigned y, x;
	unsigned p[8];
	unsigned short *lut;
	unsigned char *row = (unsigned char *) get_raw_image_addr();

	if (current_curve_type != CURVE_CUSTOM || !raw_word_ok(row))
		return;
	lut = build_lut12();
	if (!lut)
		return;

	// tables 0/1: even rows, even/odd columns. tables 2/3: odd rows.
	for (y = 0; y < camera_sensor.raw_rows; y++, row += camera_sensor.raw_rowlen) {
		const unsigned short *te = lut + (y & 1) * 2 * LUT12;
		const unsigned short *to = te + LUT12;
		unsigned *w = (unsigned *) row;
		for (x = 0; x < camera_sensor.raw_rowpix; x += 8, w += 3) {
			UNPACK8(w, p);
			p[0]=te[p[0]]; p[1]=to[p[1]]; p[2]=te[p[2]]; p[3]=to[p[3]];
			p[4]=te[p[4]]; p[5]=to[p[5]]; p[6]=te[p[6]]; p[7]=to[p[7]];
			PACK8(w, p);
		}
	}
	free(lut);
}

// L curve: out = ((in-black) * curve[L]) >> 12 + black, L = luminance (green) of the pixel pair
static void curveL_apply12(unsigned sys_index) {
	unsigned y, x, j;
	unsigned p[8];
	unsigned short *curves[4];
	unsigned black = camera_sensor.black_level;
	unsigned char *row = (unsigned char *) get_raw_image_addr();
	unsigned gcol0 = green_col_even_row();

	if (current_curve_type == CURVE_SYSTEM) {
		if (sys_index > 1)
			return;
		curves[0] = curves[1] = curves[2] = curves[3] = curve_data + sys_index*CURVE_SIZE;
	} else if (current_curve_type == CURVE_CUSTOM) {
		curves[0] = curve_data;
		curves[1] = curves[0] + CURVE_SIZE;
		curves[2] = curves[1] + CURVE_SIZE;
		curves[3] = curves[2] + CURVE_SIZE;
	} else {
		return;
	}
	if (!raw_word_ok(row))
		return;

	for (y = 0; y < camera_sensor.raw_rows; y++, row += camera_sensor.raw_rowlen) {
		unsigned gcol = (y & 1) ? !gcol0 : gcol0;     // Bayer: green alternates columns per row
		const unsigned short *ce = curves[(y & 1) * 2];
		const unsigned short *co = curves[(y & 1) * 2 + 1];
		unsigned *w = (unsigned *) row;
		for (x = 0; x < camera_sensor.raw_rowpix; x += 8, w += 3) {
			UNPACK8(w, p);
			for (j = 0; j < 8; j += 2) {             // pixel pairs (j even col, j+1 odd col)
				unsigned g = p[j + gcol], li, out;
				if (!g)
					continue;                         // leave dead pixels alone
				li = (g > black) ? (g - black) >> 2 : 0;
				if (li > CURVE_SIZE-1) li = CURVE_SIZE-1;
				if (p[j]) {
					out = (((p[j] > black ? p[j] - black : 0) * ce[li]) >> 12) + black;
					p[j] = (out > MAX12) ? MAX12 : out;
				}
				if (p[j+1]) {
					out = (((p[j+1] > black ? p[j+1] - black : 0) * co[li]) >> 12) + black;
					p[j+1] = (out > MAX12) ? MAX12 : out;
				}
			}
			PACK8(w, p);
		}
	}
}

void curve_apply() {
	short EVbias = shooting_get_ev_correction1();

	if (camera_sensor.bits_per_pixel == 12) {
		switch(conf.curve_enable) {
			case 1:
				if (current_curve_type == CURVE_CUSTOM) {
					if (curve_data[0] > 1023) curveL_apply12(0);
					else curveRGB_apply12();
				}
				break;
			case 2:
			case 3:
				if (current_curve_type == CURVE_SYSTEM) curveL_apply12(conf.curve_enable & 1);
				break;
			case 4:
				if (current_curve_type == CURVE_SYSTEM) {
					if (EVbias < -128) curveL_apply12(1);
					else if (EVbias < -32) curveL_apply12(0);
				}
				break;
		}
		return;
	}
	if (camera_sensor.bits_per_pixel != 10)
		return;

	switch(conf.curve_enable) {
		case 0:
			break;
		case 1:		// Custom
			if (current_curve_type == CURVE_CUSTOM){
				if(curve_data[0] > 1023){ // L curve contains multiplicator factors (0 Luminance has factor 1)
					curveL_apply(0);
				} else {
					curveRGB_apply();
				}
			};
			break;
		case 2:
		case 3: // +1EV,  +2EV
			if (current_curve_type == CURVE_SYSTEM) curveL_apply( conf.curve_enable & 1 );
			break;
		case 4:		// Auto DR

			if (current_curve_type == CURVE_SYSTEM) {
				 if (EVbias <-128)  {  // x4 for Bias from "-1 2/3" and below 
					curveL_apply(1);
				} else if(EVbias<-32){ // x2 for Bias from "-1 1/3" to "-2/3"
					curveL_apply(0);
				}
			}
			break;
	}
	
}


// =========  MODULE INIT =================

/***************** BEGIN OF AUXILARY PART *********************
ATTENTION: DO NOT REMOVE OR CHANGE SIGNATURES IN THIS SECTION
**************************************************************/

int _module_can_unload()
{
    return conf.curve_enable == 0;
}

/******************** Module Information structure ******************/

libcurves_sym _libcurves = 
{
    {
         0, 0, _module_can_unload, 0, 0
    },

    curve_init_mode,
    curve_apply,
    curve_set_mode,
    curve_set_file
};

ModuleInfo _module_info =
{
    MODULEINFO_V1_MAGICNUM,
    sizeof(ModuleInfo),
    CURVES_VERSION,				// Module version

    ANY_CHDK_BRANCH, 0, OPT_ARCHITECTURE,			// Requirements of CHDK version
    ANY_PLATFORM_ALLOWED,		// Specify platform dependency

    (int32_t)"Curves (dll)",
    MTYPE_EXTENSION,            //Apply curves to shots. 10 and 12 bit sensors

    &_libcurves.base,

    CONF_VERSION,               // CONF version
    ANY_VERSION,                // CAM SCREEN version
    CAM_SENSOR_VERSION,         // CAM SENSOR version
    ANY_VERSION,                // CAM INFO version

    0,
};

/*************** END OF AUXILARY PART *******************/
