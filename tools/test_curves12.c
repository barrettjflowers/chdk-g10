// Host test for the 12 bit curve code in modules/curves.c.
// Runs the fast (word based) code from curves.c against a slow per-pixel
// reference on a random raw buffer, for both RGB and L curves.
//
// Build: tools/test_curves12.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- minimal stand-ins for the CHDK environment ---
#define CURVE_SIZE 1024
typedef enum { CURVE_NONE, CURVE_CUSTOM, CURVE_SYSTEM } CURVE_TYPE;
static struct { unsigned raw_rows, raw_rowpix, raw_rowlen; unsigned black_level; int cfa_pattern; } camera_sensor;
static unsigned short *curve_data;
static CURVE_TYPE current_curve_type;
static unsigned char *rawbuf;
static char *get_raw_image_addr(void) { return (char *)rawbuf; }

#include "curves12_block.inc"   // extracted from modules/curves.c by the .sh

// --- slow reference, one pixel at a time (same formulas as core/raw.c) ---
static unsigned get_px(unsigned char *buf, unsigned x, unsigned y) {
	unsigned char *a = buf + y*camera_sensor.raw_rowlen + (x/4)*6;
	switch (x%4) {
		case 0: return ((unsigned)a[1] << 4) | (a[0] >> 4);
		case 1: return ((unsigned)(a[0] & 0x0F) << 8) | a[3];
		case 2: return ((unsigned)a[2] << 4) | (a[5] >> 4);
		default: return ((unsigned)(a[5] & 0x0F) << 8) | a[4];
	}
}
static void set_px(unsigned char *buf, unsigned x, unsigned y, unsigned v) {
	unsigned char *a = buf + y*camera_sensor.raw_rowlen + (x/4)*6;
	switch (x%4) {
		case 0: a[0] = (a[0]&0x0F) | (unsigned char)(v << 4); a[1] = (unsigned char)(v >> 4); break;
		case 1: a[0] = (a[0]&0xF0) | (unsigned char)(v >> 8); a[3] = (unsigned char)v; break;
		case 2: a[2] = (unsigned char)(v >> 4); a[5] = (a[5]&0x0F) | (unsigned char)(v << 4); break;
		default: a[4] = (unsigned char)v; a[5] = (a[5]&0xF0) | (unsigned char)(v >> 8); break;
	}
}
static unsigned ref_lookup(const unsigned short *c, unsigned v) {
	unsigned i = v >> 2, f = v & 3;
	unsigned out = (i < CURVE_SIZE-1) ? c[i]*(4-f) + c[i+1]*f : c[i]*4 + f;
	return out > 4095 ? 4095 : out;
}
static void ref_rgb(unsigned char *buf) {
	for (unsigned y = 0; y < camera_sensor.raw_rows; y++)
		for (unsigned x = 0; x < camera_sensor.raw_rowpix; x++) {
			const unsigned short *c = curve_data + ((y&1)*2 + (x&1)) * CURVE_SIZE;
			set_px(buf, x, y, ref_lookup(c, get_px(buf, x, y)));
		}
}
static void ref_L(unsigned char *buf) {
	unsigned black = camera_sensor.black_level;
	unsigned gcol0 = ((camera_sensor.cfa_pattern & 0xFF) == 1) ? 0 : 1;
	for (unsigned y = 0; y < camera_sensor.raw_rows; y++) {
		unsigned gcol = (y&1) ? !gcol0 : gcol0;
		for (unsigned x = 0; x < camera_sensor.raw_rowpix; x += 2) {
			unsigned v[2] = { get_px(buf, x, y), get_px(buf, x+1, y) };
			unsigned g = v[gcol];
			if (!g) continue;
			unsigned li = g > black ? (g - black) >> 2 : 0;
			if (li > CURVE_SIZE-1) li = CURVE_SIZE-1;
			for (int q = 0; q < 2; q++) {
				if (!v[q]) continue;
				unsigned m = curve_data[((y&1)*2 + q) * CURVE_SIZE + li];
				unsigned out = (((v[q] > black ? v[q] - black : 0) * m) >> 12) + black;
				set_px(buf, x+q, y, out > 4095 ? 4095 : out);
			}
		}
	}
}

int main(void) {
	int fails = 0;
	camera_sensor.raw_rowpix = 64; camera_sensor.raw_rows = 6;
	camera_sensor.raw_rowlen = camera_sensor.raw_rowpix * 12 / 8;
	camera_sensor.black_level = 127; camera_sensor.cfa_pattern = 0x01000201;  // G10
	size_t size = camera_sensor.raw_rowlen * camera_sensor.raw_rows;
	unsigned char *orig = malloc(size), *ref = malloc(size);
	rawbuf = malloc(size);
	curve_data = malloc(4 * CURVE_SIZE * sizeof(unsigned short));
	srand(1);
	for (size_t i = 0; i < size; i++) orig[i] = rand();
	orig[0] = 0; orig[1] = 0;   // a dead pixel (value 0) to exercise the skip path

	// 1. unpack/pack round trip and agreement with get_px
	memcpy(rawbuf, orig, size);
	for (unsigned y = 0; y < camera_sensor.raw_rows; y++) {
		unsigned *w = (unsigned *)(rawbuf + y*camera_sensor.raw_rowlen);
		for (unsigned x = 0; x < camera_sensor.raw_rowpix; x += 8, w += 3) {
			unsigned p[8]; UNPACK8(w, p);
			for (int k = 0; k < 8; k++) if (p[k] != get_px(orig, x+k, y)) fails++;
			PACK8(w, p);
		}
	}
	if (memcmp(rawbuf, orig, size)) fails++;
	printf("pack/unpack: %s\n", fails ? "FAIL" : "ok");

	// 2. RGB curve, different random curve per channel
	for (int i = 0; i < 4*CURVE_SIZE; i++) curve_data[i] = rand() % 1024;
	current_curve_type = CURVE_CUSTOM;
	memcpy(rawbuf, orig, size); memcpy(ref, orig, size);
	curveRGB_apply12(); ref_rgb(ref);
	int f2 = memcmp(rawbuf, ref, size) != 0; fails += f2;
	printf("RGB curve:   %s\n", f2 ? "FAIL" : "ok");

	// 3. L curve (multiplier tables, values around 4096 = x1)
	for (int i = 0; i < 4*CURVE_SIZE; i++) curve_data[i] = 2048 + rand() % 8192;
	memcpy(rawbuf, orig, size); memcpy(ref, orig, size);
	curveL_apply12(0); ref_L(ref);
	int f3 = memcmp(rawbuf, ref, size) != 0; fails += f3;
	printf("L curve:     %s\n", f3 ? "FAIL" : "ok");

	printf(fails ? "FAIL\n" : "PASS\n");
	return fails != 0;
}
