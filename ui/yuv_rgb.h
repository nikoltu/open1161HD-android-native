// yuv_rgb.h -- BT.601 studio-range YUV 4:2:0 -> packed RGB (Faze 3.5 of
// open1161HD). The exact integer transform the SC:R portrait pipeline needs
// after VP8/VP9 decode. Byte-for-byte identical to the grp-toolkit oracle
// (vp8_decode.py Frame.rgb): chroma sampled nearest-neighbour, two-tap-free,
// all integer with an arithmetic >>8 (a float path produces different pixels).
// Pure presentation; no determinism impact.
#ifndef BWGAME_UI_YUV_RGB_H
#define BWGAME_UI_YUV_RGB_H

#include <cstdint>

namespace bwgame {
namespace yuv {

static inline uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// VP9 colour-space signal (RFC / libvpx CS_*). Only the two seen in the SC:R
// portrait corpus matter here.
enum { CS_BT_601 = 1, CS_SRGB = 7 };

// One pixel, BT.601 studio range (VP8 carbot portraits + VP9 cs=0/1 4:2:0).
// R,G,B written to out[0..2].
static inline void bt601(int Y, int U, int V, uint8_t* out) {
	int c = Y - 16, d = U - 128, e = V - 128;
	out[0] = clamp255((298 * c + 409 * e + 128) >> 8);
	out[1] = clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
	out[2] = clamp255((298 * c + 516 * d + 128) >> 8);
}

// VP9 profile-1 sRGB (color_space=7, 4:4:4): the "Y/U/V" planes ARE full-range
// sRGB G/B/R -- NO colour matrix, NO subsampling. The RGB is a lossless plane
// reorder (correctness follows from the byte-exact planes; there is no lossy
// transform to validate). Applying BT.601 here is WRONG and would shift colours.
// This is what the SC:R HD portraits use (measured: sd/hd2 portrait = profile-1
// cs=7). Args named by their plane role: planeY=G, planeU=B, planeV=R.
static inline void srgb(int planeY_G, int planeU_B, int planeV_R, uint8_t* out) {
	out[0] = (uint8_t)planeV_R; // R
	out[1] = (uint8_t)planeY_G; // G
	out[2] = (uint8_t)planeU_B; // B
}

// A 4:2:0 (or 4:4:4) plane trio -> packed RGB via accessor lambdas. `getY(x,y)`,
// `getU(cx,cy)`, `getV(cx,cy)` return 8-bit samples; sx/sy are the chroma
// subsampling shifts (1 for 4:2:0). out must hold w*h*3 bytes.
template <class GY, class GU, class GV>
static inline void to_rgb(int w, int h, int sx, int sy, GY getY, GU getU, GV getV, uint8_t* out) {
	size_t k = 0;
	for (int j = 0; j < h; ++j) {
		for (int i = 0; i < w; ++i) {
			bt601(getY(i, j), getU(i >> sx, j >> sy), getV(i >> sx, j >> sy), &out[k]);
			k += 3;
		}
	}
}

} // namespace yuv
} // namespace bwgame

#endif
