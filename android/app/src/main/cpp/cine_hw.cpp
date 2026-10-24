// open1161HD Faze 4.7 Variant B: hardware WebM cinematic decode.
//
// SC:R cinematics are full-screen VP9 (+Vorbis) WebM up to 1920x1080 -- far too
// heavy for our software VP9. This uses
// the platform's hardware decoder via the NDK media API: AMediaExtractor demuxes
// the WebM, AMediaCodec hardware-decodes the video onto an AImageReader surface,
// and we read the normalized YUV_420_888 image back and convert to RGB for the
// engine's existing letterbox present loop. Video only in this milestone.
//
// AImageReader is API 26; the whole path is a no-op below that (the caller falls
// back to the SMK cinematic). The engine's SDL surface stays the presentation
// path -- we only produce RGB frames here.
#include "cine_hw.h"

#include <android/api-level.h>
#include <android/log.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <jni.h>
#include "SDL.h" // SDL_AndroidGetJNIEnv / SDL_AndroidGetActivity for the Variant A bridge

#define CLOG(...) __android_log_print(ANDROID_LOG_INFO, "OpenBW", __VA_ARGS__)

namespace cine_hw {

// API 26/29 symbols loaded dynamically (app minSdk is 24; these are only reached
// on API >= 26, guarded in open()).
typedef media_status_t (*AImage_getHardwareBuffer_t)(const AImage*, AHardwareBuffer**);
typedef int (*AHardwareBuffer_lockPlanes_t)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, AHardwareBuffer_Planes*);
typedef int (*AHardwareBuffer_unlock_t)(AHardwareBuffer*, int32_t*);
typedef void (*AHardwareBuffer_describe_t)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
static AImage_getHardwareBuffer_t p_getHB = nullptr;
static AHardwareBuffer_lockPlanes_t p_lockPlanes = nullptr;
static AHardwareBuffer_unlock_t p_unlock = nullptr;
static AHardwareBuffer_describe_t p_describe = nullptr;
static void load_syms() {
	static bool done = false;
	if (done) return; done = true;
	void* lib = dlopen("libmediandk.so", RTLD_NOW);
	if (lib) p_getHB = (AImage_getHardwareBuffer_t)dlsym(lib, "AImage_getHardwareBuffer");
	void* ah = dlopen("libandroid.so", RTLD_NOW);
	if (ah) {
		p_lockPlanes = (AHardwareBuffer_lockPlanes_t)dlsym(ah, "AHardwareBuffer_lockPlanes");
		p_unlock = (AHardwareBuffer_unlock_t)dlsym(ah, "AHardwareBuffer_unlock");
		p_describe = (AHardwareBuffer_describe_t)dlsym(ah, "AHardwareBuffer_describe");
	}
	CLOG("cine_hw syms: mediandk=%p android=%p getHB=%p lock=%p unlock=%p", lib, ah, (void*)p_getHB, (void*)p_lockPlanes, (void*)p_unlock);
}

struct decoder {
	AMediaExtractor* extractor = nullptr;
	AMediaCodec* codec = nullptr;
	AImageReader* reader = nullptr;
	ANativeWindow* window = nullptr;
	FILE* src_fp = nullptr; // kept open for the extractor's lifetime
	std::string tmp_path;
	int w = 0, h = 0;
	bool input_eos = false;
	bool output_eos = false;
	int rgb_fails = 0; // consecutive image_to_rgb failures (opaque output -> bail fast)
};

// BT.601 limited-range YUV -> RGB (the standard for SD/HD video). Fills rgb with
// w*h*3 bytes (R,G,B). Handles per-plane row/pixel strides (NV12/NV21/I420 all
// arrive as YUV_420_888 through the ImageReader).
static bool image_to_rgb(AImage* img, std::vector<uint8_t>& rgb, int w, int h) {
	// AImage_getPlaneData exposes only the Y plane for this decoder's output; the
	// chroma planes come back null. Reading the underlying AHardwareBuffer with
	// lockPlanes gives all three (Y/U/V) with correct strides. (getHardwareBuffer
	// is API 26, lockPlanes API 29; both dlsym'd -- we only reach here on 26+, and
	// on 24-25 the whole HW path is skipped anyway.)
	load_syms();
	if (!p_getHB || !p_lockPlanes || !p_unlock) return false;
	AHardwareBuffer* hb = nullptr;
	media_status_t gr = p_getHB(img, &hb);
	AHardwareBuffer_Planes pl{};
	int lr = (gr == AMEDIA_OK && hb) ? p_lockPlanes(hb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &pl) : -999;
	{ static bool lg = false; if (!lg) { lg = true; CLOG("cine_hw hbstep: getHB=%d hb=%p lock=%d planeCount=%d", (int)gr, (void*)hb, lr, lr == 0 ? (int)pl.planeCount : -1); } }
	if (gr != AMEDIA_OK || !hb) return false;
	if (lr != 0) return false;
	const uint8_t* yb = (const uint8_t*)pl.planes[0].data;
	int yRow = pl.planes[0].rowStride, yPix = pl.planes[0].pixelStride ? pl.planes[0].pixelStride : 1;
	// A hardware decoder may hand back an opaque/tiled buffer (e.g. Qualcomm UBWC)
	// whose plane strides are unknown (-1) and NOT linearly CPU-readable. Bail so
	// the caller falls back to the SMK cinematic instead of reading garbage.
	if (yRow <= 0 || yPix <= 0) {
		static bool lg = false; if (!lg) { lg = true; CLOG("cine_hw: opaque/tiled output (yRow=%d yPix=%d pc=%d) -> unsupported, fallback", yRow, yPix, pl.planeCount); }
		p_unlock(hb, nullptr); return false;
	}
	const uint8_t *ub, *vb; int uRow, uPix, vRow, vPix;
	if (pl.planeCount >= 3) {
		ub = (const uint8_t*)pl.planes[1].data; uRow = pl.planes[1].rowStride; uPix = pl.planes[1].pixelStride;
		vb = (const uint8_t*)pl.planes[2].data; vRow = pl.planes[2].rowStride; vPix = pl.planes[2].pixelStride;
	} else {
		// Single-plane semiplanar (NV12): the interleaved UV plane follows the Y
		// plane at rowStride * (buffer height). U at even byte, V at odd.
		int bh = h;
		if (p_describe) { AHardwareBuffer_Desc dd{}; p_describe(hb, &dd); if ((int)dd.height >= h) bh = (int)dd.height; }
		ub = yb + (size_t)yRow * bh; uRow = yRow; uPix = 2;
		vb = ub + 1; vRow = yRow; vPix = 2;
	}
	{ static bool lg = false; if (!lg) { lg = true; CLOG("cine_hw hb: pc=%d y=%p(row=%d pix=%d) u=%p(row=%d pix=%d) v=%p(pix=%d)", pl.planeCount, yb, yRow, yPix, ub, uRow, uPix, vb, vPix); } }
	if (!yb || !ub || !vb) { p_unlock(hb, nullptr); return false; }
	rgb.resize((size_t)w * h * 3);
	uint8_t* out = rgb.data();
	for (int y = 0; y < h; ++y) {
		const uint8_t* yr = yb + (size_t)y * yRow;
		const uint8_t* ur = ub + (size_t)(y / 2) * uRow;
		const uint8_t* vr = vb + (size_t)(y / 2) * vRow;
		uint8_t* orow = out + (size_t)y * w * 3;
		for (int x = 0; x < w; ++x) {
			int Y = yr[(size_t)x * yPix];
			int U = ur[(size_t)(x / 2) * uPix];
			int V = vr[(size_t)(x / 2) * vPix];
			int c = Y - 16, d = U - 128, e = V - 128;
			int r = (298 * c + 409 * e + 128) >> 8;
			int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
			int b = (298 * c + 516 * d + 128) >> 8;
			orow[x * 3 + 0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
			orow[x * 3 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
			orow[x * 3 + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
		}
	}
	p_unlock(hb, nullptr);
	return true;
}

decoder* open(const uint8_t* webm, size_t size, const char* tmp_dir,
              int& out_w, int& out_h, double& out_usf) {
	if (android_get_device_api_level() < 26) return nullptr; // AImageReader is API 26+
	if (!webm || !size || !tmp_dir) return nullptr;

	// AMediaExtractor reads from an fd/path; stage the CASC bytes to a temp file.
	std::string path = std::string(tmp_dir) + "/cine_tmp.webm";
	{
		FILE* f = fopen(path.c_str(), "wb");
		if (!f) { CLOG("cine_hw: cannot write %s", path.c_str()); return nullptr; }
		size_t wrote = fwrite(webm, 1, size, f);
		fclose(f);
		if (wrote != size) { remove(path.c_str()); return nullptr; }
	}

	decoder* d = new decoder();
	d->tmp_path = path;
	d->extractor = AMediaExtractor_new();
	// Keep the fd open for the extractor's whole lifetime -- it reads samples
	// lazily, so closing it here would starve the decoder after a few frames.
	d->src_fp = fopen(path.c_str(), "rb");
	if (!d->src_fp) { close(d); return nullptr; }
	media_status_t st = AMediaExtractor_setDataSourceFd(d->extractor, fileno(d->src_fp), 0, (int64_t)size);
	if (st != AMEDIA_OK) { CLOG("cine_hw: setDataSourceFd failed %d", (int)st); close(d); return nullptr; }

	// Find the video track.
	size_t ntracks = AMediaExtractor_getTrackCount(d->extractor);
	int vtrack = -1; const char* mime = nullptr; AMediaFormat* vfmt = nullptr;
	for (size_t i = 0; i < ntracks; ++i) {
		AMediaFormat* fmt = AMediaExtractor_getTrackFormat(d->extractor, i);
		const char* m = nullptr;
		if (AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &m) && m && strncmp(m, "video/", 6) == 0) {
			vtrack = (int)i; mime = m; vfmt = fmt; break;
		}
		AMediaFormat_delete(fmt);
	}
	if (vtrack < 0 || !vfmt) { CLOG("cine_hw: no video track"); close(d); return nullptr; }
	AMediaExtractor_selectTrack(d->extractor, vtrack);

	int32_t w = 0, h = 0, fr = 0;
	AMediaFormat_getInt32(vfmt, AMEDIAFORMAT_KEY_WIDTH, &w);
	AMediaFormat_getInt32(vfmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
	AMediaFormat_getInt32(vfmt, AMEDIAFORMAT_KEY_FRAME_RATE, &fr);
	if (w <= 0 || h <= 0) { AMediaFormat_delete(vfmt); close(d); return nullptr; }
	d->w = w; d->h = h;
	out_w = w; out_h = h;
	out_usf = (fr > 0) ? (1000000.0 / fr) : (1000000.0 / 30.0);

	// ImageReader -> normalized YUV_420_888 we can read back. The chroma planes
	// are only CPU-mapped when the reader is created with CPU_READ_OFTEN, which
	// needs AImageReader_newWithUsage (API 26). We only get here on API >= 26
	// (guarded above), but the app's minSdk is 24, so load the symbol dynamically
	// rather than referencing it at link time. Fall back to plain _new if absent.
	media_status_t rs = AMEDIA_ERROR_UNKNOWN;
	{
		typedef media_status_t (*fn_t)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**);
		static fn_t fn = []() -> fn_t {
			void* lib = dlopen("libmediandk.so", RTLD_NOW);
			return lib ? (fn_t)dlsym(lib, "AImageReader_newWithUsage") : nullptr;
		}();
		if (fn) rs = fn(w, h, AIMAGE_FORMAT_YUV_420_888, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 6, &d->reader);
		else rs = AImageReader_new(w, h, AIMAGE_FORMAT_YUV_420_888, 6, &d->reader);
	}
	if (rs != AMEDIA_OK || !d->reader) { CLOG("cine_hw: AImageReader_new %d", (int)rs); AMediaFormat_delete(vfmt); close(d); return nullptr; }
	AImageReader_getWindow(d->reader, &d->window);

	d->codec = AMediaCodec_createDecoderByType(mime);
	if (!d->codec) { CLOG("cine_hw: createDecoder %s failed", mime); AMediaFormat_delete(vfmt); close(d); return nullptr; }
	media_status_t cs = AMediaCodec_configure(d->codec, vfmt, d->window, nullptr, 0);
	AMediaFormat_delete(vfmt);
	if (cs != AMEDIA_OK) { CLOG("cine_hw: configure %d", (int)cs); close(d); return nullptr; }
	if (AMediaCodec_start(d->codec) != AMEDIA_OK) { CLOG("cine_hw: start failed"); close(d); return nullptr; }
	CLOG("cine_hw: opened %dx%d fr=%d (usf=%.0f)", w, h, fr, out_usf);
	return d;
}

bool next_video(decoder* d, std::vector<uint8_t>& rgb) {
	if (!d || d->output_eos) return false;
	int guard = 0;
	int dbg_in = 0, dbg_out = 0, dbg_acq = 0, dbg_tryagain = 0; media_status_t dbg_last_acq = AMEDIA_OK;
	for (;;) {
		if (++guard > 4000) { CLOG("cine_hw: next spin guard hit in=%d out=%d acq=%d try=%d", dbg_in, dbg_out, dbg_acq, dbg_tryagain); return false; }
		// Feed input.
		if (!d->input_eos) {
			ssize_t inIdx = AMediaCodec_dequeueInputBuffer(d->codec, 2000);
			if (inIdx >= 0) {
				size_t cap = 0;
				uint8_t* buf = AMediaCodec_getInputBuffer(d->codec, inIdx, &cap);
				ssize_t ss = buf ? AMediaExtractor_readSampleData(d->extractor, buf, cap) : -1;
				if (ss < 0) {
					AMediaCodec_queueInputBuffer(d->codec, inIdx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
					d->input_eos = true;
				} else {
					int64_t pts = AMediaExtractor_getSampleTime(d->extractor);
					AMediaCodec_queueInputBuffer(d->codec, inIdx, 0, (size_t)ss, pts, 0);
					++dbg_in;
					AMediaExtractor_advance(d->extractor);
				}
			}
		}
		// Drain output.
		AMediaCodecBufferInfo info;
		ssize_t outIdx = AMediaCodec_dequeueOutputBuffer(d->codec, &info, 5000);
		if (outIdx >= 0) {
			++dbg_out;
			bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
			// Configured with a Surface -> info.size is 0 (the frame lives on the
			// surface), so gate render on the frame NOT being codec-config, never on
			// size, or every frame is discarded and the ImageReader stays empty.
			bool render = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0;
			AMediaCodec_releaseOutputBuffer(d->codec, outIdx, render);
			if (render) {
				// The render is asynchronous; poll the ImageReader briefly for it.
				for (int tries = 0; tries < 60; ++tries) {
					AImage* img = nullptr;
					media_status_t r = AImageReader_acquireNextImage(d->reader, &img);
					dbg_last_acq = r;
					if (r == AMEDIA_OK && img) {
						++dbg_acq;
						bool ok = image_to_rgb(img, rgb, d->w, d->h);
						AImage_delete(img);
						if (ok) { d->rgb_fails = 0; if (eos) d->output_eos = true; return true; }
						// Unreadable (opaque/tiled) output: give up quickly instead of
						// spinning the whole clip so the caller falls back at once.
						if (++d->rgb_fails >= 3) { d->output_eos = true; return false; }
						break;
					}
					if (r != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
			if (eos) { d->output_eos = true; CLOG("cine_hw: EOS in=%d out=%d acq=%d lastacq=%d", dbg_in, dbg_out, dbg_acq, (int)dbg_last_acq); return false; }
		} else if (outIdx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
			++dbg_tryagain;
			if (d->input_eos) { d->output_eos = true; CLOG("cine_hw: drain-done in=%d out=%d acq=%d lastacq=%d try=%d", dbg_in, dbg_out, dbg_acq, (int)dbg_last_acq, dbg_tryagain); return false; }
		} else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			CLOG("cine_hw: output format changed");
		}
		// otherwise (buffers changed): loop again.
	}
}

void close(decoder* d) {
	if (!d) return;
	if (d->codec) { AMediaCodec_stop(d->codec); AMediaCodec_delete(d->codec); }
	if (d->reader) AImageReader_delete(d->reader); // also releases the window
	if (d->extractor) AMediaExtractor_delete(d->extractor);
	if (d->src_fp) fclose(d->src_fp);
	if (!d->tmp_path.empty()) remove(d->tmp_path.c_str());
	delete d;
}

} // namespace cine_hw

// open1161HD Faze 4.7 Variant A: play a WebM cinematic through the OS video player
// (OpenBWActivity.playCinematic -> VideoView). The OS handles VP9 decode, Vorbis
// audio and any device-specific (UBWC/tiled) output, so this works on every SoC --
// unlike the native CPU-readback path above. Blocks the calling (game) thread until
// the clip finishes or is skipped.
namespace cine_java {
bool play(const uint8_t* webm, size_t size, const char* tmp_dir) {
	if (!webm || !size || !tmp_dir) return false;
	std::string path = std::string(tmp_dir) + "/cine_play.webm";
	{
		FILE* f = fopen(path.c_str(), "wb");
		if (!f) return false;
		size_t wr = fwrite(webm, 1, size, f);
		fclose(f);
		if (wr != size) { remove(path.c_str()); return false; }
	}
	JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
	jobject act = (jobject)SDL_AndroidGetActivity();
	bool ok = false;
	if (env && act) {
		jclass cls = env->GetObjectClass(act);
		jmethodID mid = cls ? env->GetMethodID(cls, "playCinematic", "(Ljava/lang/String;)Z") : nullptr;
		if (mid) {
			jstring jp = env->NewStringUTF(path.c_str());
			ok = env->CallBooleanMethod(act, mid, jp) == JNI_TRUE;
			env->DeleteLocalRef(jp);
		} else {
			CLOG("cine_java: playCinematic method not found");
		}
		if (cls) env->DeleteLocalRef(cls);
		env->DeleteLocalRef(act);
	}
	remove(path.c_str());
	return ok;
}
} // namespace cine_java
