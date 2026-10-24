// Launcher portraits: the player's
// profile shows an ANIMATED unit portrait taken from the user's own game files.
// This is the launcher-side decoder -- part of the small SDL-free
// libopenbw_minimap.so, so SetupActivity can use it without starting the engine.
//
//   nativeCatalog(gameDir)        -> one line per portrait id from arr\portdata.dat:
//                                    "id|idleBase|nIdle|talkBase|nTalk|smkChange|voices"
//                                    (voices = ';'-joined sound\... paths of the "what" lines
//                                    of the first unit that uses the portrait; may be empty)
//                                    (base = the tbl string, e.g. "tmarine\TMaFid0";
//                                    the files are portrait\<base><variant>.smk)
//   nativeDecodeSmk(gameDir,path) -> int[]: {w, h, frames, usPerFrame, ARGB...}; the
//                                    pixels are ONE horizontal strip of all frames
//                                    (frames*w x h), ready for Bitmap.createBitmap.
//
// Nothing is cached on disk and no game pixel ever leaves the device: the archive
// is the user's own copy (the app ships none).
#include <jni.h>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include "data_loading.h"
#include "minimap_gen.h"
#include "ui/libsmacker/smacker.h"
#include <atomic>
#include "ui/casc_inflate.h"       // CASC reader + the stb-backed zlib inflate (HD portraits)
#include "ui/webm.h"
#include "ui/vp9_inter_pixels.h"   // the project VP9 decoder

namespace {

using loader_t = bwgame::data_loading::data_files_loader<>;

std::mutex g_mtx;
std::unique_ptr<loader_t> g_loader;   // opening the MPQs parses their tables: keep it
std::string g_loader_dir;

loader_t* get_loader(const char* gamedir) {
	if (g_loader && g_loader_dir == gamedir) return g_loader.get();
	g_loader.reset();
	g_loader_dir.clear();
	try {
		auto l = std::make_unique<loader_t>(bwgame::data_loading::data_files_directory(bwgame::a_string(gamedir)));
		bwgame::minimap_gen::attach_cd_archives(*l, gamedir);
		g_loader = std::move(l);
		g_loader_dir = gamedir;
	} catch (...) {
		return nullptr;
	}
	return g_loader.get();
}

// arr\portdata.tbl: u16 count, u16 offsets[], zero-terminated strings (1-based index).
std::string tbl_string(const bwgame::a_vector<uint8_t>& d, size_t index) {
	if (index == 0) return {};
	size_t p = 2 + (index - 1) * 2;
	if (p + 2 > d.size()) return {};
	size_t off = (size_t)d[p] | ((size_t)d[p + 1] << 8);
	std::string s;
	while (off < d.size() && d[off]) s += (char)d[off++];
	return s;
}

}

extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_PortraitStore_nativeCatalog(JNIEnv* env, jclass, jstring jGameDir) {
	const char* gamedir = env->GetStringUTFChars(jGameDir, nullptr);
	std::string out;
	try {
		std::lock_guard<std::mutex> lk(g_mtx);
		loader_t* l = get_loader(gamedir);
		if (l) {
			bwgame::a_vector<uint8_t> dat, tbl;
			(*l)(dat, "arr\\portdata.dat");
			(*l)(tbl, "arr\\portdata.tbl");
			// {u32 tbl_index} x N, {u8 smk_change} x N, {u8 unknown} x N; the first half of
			// the records are the idle clips, the second half the talking ones.
			size_t n = dat.size() / 6, half = n / 2;
			auto rd32 = [&](size_t off) {
				return (uint32_t)dat[off] | ((uint32_t)dat[off + 1] << 8) | ((uint32_t)dat[off + 2] << 16) | ((uint32_t)dat[off + 3] << 24);
			};
			auto variants = [&](const std::string& base) {
				int c = 0;
				if (base.empty()) return 0;
				for (int v = 0; v != 4; ++v) {
					bwgame::a_string p = bwgame::a_string("portrait\\") + base.c_str() + (char)('0' + v) + ".smk";
					if (!l->file_exists(p)) break;
					++c;
				}
				return c;
			};
			// portrait id -> the "what" (selected) voice lines of the unit that wears it:
			// units.dat gives portrait + first/last what sound, sfxdata.dat/.tbl the file.
			std::vector<std::string> voices(half);
			try {
				bwgame::a_vector<uint8_t> ud, sd, st;
				(*l)(ud, "arr\\units.dat");
				(*l)(sd, "arr\\sfxdata.dat");
				(*l)(st, "arr\\sfxdata.tbl");
				auto units = bwgame::data_loading::load_units_dat(ud);
				auto sounds = bwgame::data_loading::load_sfxdata_dat(sd);
				for (auto& u : units.vec) {
					if (u.portrait < 0 || (size_t)u.portrait >= half || !voices[(size_t)u.portrait].empty()) continue;
					if (u.first_what_sound <= 0 || u.last_what_sound < u.first_what_sound) continue;
					std::string v;
					for (int k = u.first_what_sound; k <= u.last_what_sound && k < (int)sounds.vec.size(); ++k) {
						std::string fn = tbl_string(st, (size_t)sounds.vec[(size_t)k].filename_index);
						if (fn.empty()) continue;
						if (!v.empty()) v += ';';
						v += "sound\\" + fn;
					}
					voices[(size_t)u.portrait] = v;
				}
			} catch (...) {}
			for (size_t id = 0; id != half; ++id) {
				std::string idle = tbl_string(tbl, rd32(id * 4));
				std::string talk = tbl_string(tbl, rd32((id + half) * 4));
				int change = dat[n * 4 + id];
				out += std::to_string(id) + "|" + idle + "|" + std::to_string(variants(idle)) + "|" + talk + "|" + std::to_string(variants(talk)) + "|" + std::to_string(change) + "|" + voices[id] + "\n";
			}
		}
	} catch (...) {
		out.clear();
	}
	env->ReleaseStringUTFChars(jGameDir, gamedir);
	if (out.empty()) return nullptr;
	return env->NewStringUTF(out.c_str());
}

namespace {
jintArray decode_smk(JNIEnv* env, jstring jGameDir, jstring jPath, unsigned long step, unsigned long max_frames);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_org_openbw_android_PortraitStore_nativeDecodeSmk(JNIEnv* env, jclass, jstring jGameDir, jstring jPath) {
	return decode_smk(env, jGameDir, jPath, 1, 64);   // portraits are 6..25 frames
}

// Larger menu art (campaign emblems: ~200x220, 70 frames): every `step`-th frame,
// at most `maxFrames` of them; usPerFrame in the result is already multiplied.
extern "C" JNIEXPORT jintArray JNICALL
Java_org_openbw_android_PortraitStore_nativeDecodeSmkStep(JNIEnv* env, jclass, jstring jGameDir, jstring jPath, jint step, jint maxFrames) {
	return decode_smk(env, jGameDir, jPath, step < 1 ? 1 : (unsigned long)step, maxFrames < 1 ? 1 : (unsigned long)maxFrames);
}

namespace {
jintArray decode_smk(JNIEnv* env, jstring jGameDir, jstring jPath, unsigned long step, unsigned long max_frames) {
	const char* gamedir = env->GetStringUTFChars(jGameDir, nullptr);
	const char* path = env->GetStringUTFChars(jPath, nullptr);
	std::vector<jint> px;
	try {
		bwgame::a_vector<uint8_t> data;
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			loader_t* l = get_loader(gamedir);
			if (l) (*l)(data, path);
		}
		if (!data.empty()) {
			smk s = smk_open_memory(data.data(), (unsigned long)data.size());
			if (s) {
				unsigned long cur = 0, frames = 0, w = 0, h = 0;
				double usf = 0;
				unsigned char yscale = 0;
				smk_info_all(s, &cur, &frames, &usf);
				smk_info_video(s, &w, &h, &yscale);
				frames = (frames + step - 1) / step;               // frames we keep
				if (frames > max_frames) frames = max_frames;
				usf *= (double)step;
				if (w > 0 && h > 0 && w <= 512 && h <= 512 && frames > 0) {
					if (usf < 1000.0) usf = 100000.0;              // measured: every portrait is 100 ms / frame
					px.assign(4 + (size_t)w * frames * h, 0);
					px[0] = (jint)w; px[1] = (jint)h; px[2] = (jint)frames; px[3] = (jint)usf;
					smk_enable_video(s, 1);
					smk_first(s);
					for (unsigned long fr = 0; fr != frames; ++fr) {
						const unsigned char* pal = smk_get_palette(s);
						const unsigned char* vid = smk_get_video(s);
						if (pal && vid) {
							for (unsigned long y = 0; y != h; ++y) {
								jint* dst = px.data() + 4 + (size_t)y * (w * frames) + (size_t)fr * w;
								const unsigned char* src = vid + (size_t)y * w;
								for (unsigned long x = 0; x != w; ++x) {
									const unsigned char* c = pal + (size_t)src[x] * 3;
									dst[x] = (jint)(0xff000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2]);
								}
							}
						}
						if (fr + 1 == frames) break;
						bool ended = false;
						for (unsigned long k = 0; k != step && !ended; ++k) if ((signed char)smk_next(s) < 0) ended = true;
						if (ended) { px[2] = (jint)(fr + 1); break; }
					}
				}
				smk_close(s);
			}
		}
	} catch (...) {
		px.clear();
	}
	env->ReleaseStringUTFChars(jGameDir, gamedir);
	env->ReleaseStringUTFChars(jPath, path);
	if (px.empty()) return nullptr;
	// a clip that ended early keeps its header honest: trim nothing, the Java side
	// only reads px[2] frames out of the strip
	jintArray arr = env->NewIntArray((jsize)px.size());
	if (arr) env->SetIntArrayRegion(arr, 0, (jsize)px.size(), px.data());
	return arr;
}
}

// Launcher backdrop (LAUNCHER-REDESIGN F6): an 8-bit PCX from the user's MPQs
// (glue\palmm\backgnd.pcx, the main-menu space scene) -> int[]{w, h, ARGB...}.
extern "C" JNIEXPORT jintArray JNICALL
Java_org_openbw_android_PortraitStore_nativeDecodePcx(JNIEnv* env, jclass, jstring jGameDir, jstring jPath) {
	const char* gamedir = env->GetStringUTFChars(jGameDir, nullptr);
	const char* path = env->GetStringUTFChars(jPath, nullptr);
	std::vector<jint> px;
	try {
		bwgame::a_vector<uint8_t> d;
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			loader_t* l = get_loader(gamedir);
			if (l && l->file_exists(path)) (*l)(d, path);
		}
		// header: 0x0A, version, encoding 1 (RLE), 8 bpp, window, ..., planes @65, bytes/line @66
		if (d.size() > 128 + 769 && d[0] == 0x0a && d[2] == 1 && d[3] == 8 && d[65] == 1
			&& d[d.size() - 769] == 0x0c) {
			size_t w = (size_t)(d[8] | (d[9] << 8)) - (size_t)(d[4] | (d[5] << 8)) + 1;
			size_t h = (size_t)(d[10] | (d[11] << 8)) - (size_t)(d[6] | (d[7] << 8)) + 1;
			size_t bpl = (size_t)(d[66] | (d[67] << 8));
			const uint8_t* pal = d.data() + d.size() - 768;
			if (w > 0 && h > 0 && w <= 4096 && h <= 4096 && bpl >= w) {
				px.assign(2 + w * h, 0);
				px[0] = (jint)w; px[1] = (jint)h;
				size_t pos = 128, end = d.size() - 769;
				std::vector<uint8_t> line(bpl);
				for (size_t y = 0; y != h; ++y) {
					size_t x = 0;
					while (x < bpl && pos < end) {
						uint8_t b = d[pos++];
						size_t n = 1;
						if ((b & 0xc0) == 0xc0) { n = b & 0x3f; if (pos >= end) break; b = d[pos++]; }
						while (n-- && x < bpl) line[x++] = b;
					}
					jint* dst = px.data() + 2 + y * w;
					for (size_t i = 0; i != w; ++i) {
						const uint8_t* c = pal + (size_t)line[i] * 3;
						dst[i] = (jint)(0xff000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2]);
					}
				}
			}
		}
	} catch (...) {
		px.clear();
	}
	env->ReleaseStringUTFChars(jGameDir, gamedir);
	env->ReleaseStringUTFChars(jPath, path);
	if (px.empty()) return nullptr;
	jintArray arr = env->NewIntArray((jsize)px.size());
	if (arr) env->SetIntArrayRegion(arr, 0, (jsize)px.size(), px.data());
	return arr;
}

// Menu audio (LAUNCHER-REDESIGN F9): writes one file of the user's archives --
// already decompressed by the MPQ reader (Huffman / ADPCM -> plain PCM WAV) -- to
// the app's cache so MediaPlayer / SoundPool can play it. Returns false when the
// file is not in the archives (e.g. the title music without the CD archives).
extern "C" JNIEXPORT jboolean JNICALL
Java_org_openbw_android_MenuAudio_nativeExtractFile(JNIEnv* env, jclass, jstring jGameDir, jstring jPath, jstring jOut) {
	const char* gamedir = env->GetStringUTFChars(jGameDir, nullptr);
	const char* path = env->GetStringUTFChars(jPath, nullptr);
	const char* out = env->GetStringUTFChars(jOut, nullptr);
	bool ok = false;
	try {
		bwgame::a_vector<uint8_t> d;
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			loader_t* l = get_loader(gamedir);
			if (l && l->file_exists(path)) (*l)(d, path);
		}
		if (!d.empty()) {
			FILE* f = fopen(out, "wb");
			if (f) {
				ok = fwrite(d.data(), 1, d.size(), f) == d.size();
				fclose(f);
				if (!ok) remove(out);
			}
		}
	} catch (...) {
		ok = false;
	}
	env->ReleaseStringUTFChars(jGameDir, gamedir);
	env->ReleaseStringUTFChars(jPath, path);
	env->ReleaseStringUTFChars(jOut, out);
	return ok ? JNI_TRUE : JNI_FALSE;
}

// HD portraits (LAUNCHER-REDESIGN F5): a VP9 clip from the user's SC:R install
// (read in place through the CASC reader the engine uses), decoded by the
// project's own VP9 decoder -> int[]{w, h, frames, usPerFrame, ARGB strip}. The
// 224x240 picture is centre-cropped to the SD portrait's 15:14. Slow (hundreds of
// ms): call off the UI thread; the Java side caches the result on disk.
namespace {
std::mutex g_casc_mtx;
std::unique_ptr<bwgame::casc::storage> g_casc;
std::string g_casc_dir;
bool g_casc_ok = false;

bwgame::casc::storage* get_casc(const char* dir) {
	if (g_casc && g_casc_dir == dir) return g_casc_ok ? g_casc.get() : nullptr;
	g_casc.reset(new bwgame::casc::storage());
	g_casc_dir = dir;
	g_casc_ok = false;
	try {
		g_casc_ok = g_casc->open((std::string(dir) + "/data/data").c_str(), dir, bwgame::casc::stb_inflate());
	} catch (...) {
		g_casc_ok = false;
	}
	return g_casc_ok ? g_casc.get() : nullptr;
}
}

// Start-up title picture (v1.0.9): an uncompressed 32-bit RGBA DDS from the user's
// SC:R install (glue/title/title.dds, 3412x1920; hd2/... is half that) ->
// int[]{w, h, ARGB...}. Only that plain layout is read; anything else returns null.
extern "C" JNIEXPORT jintArray JNICALL
Java_org_openbw_android_PortraitStore_nativeDecodeDds(JNIEnv* env, jclass, jstring jCascDir, jstring jName) {
	const char* dir = env->GetStringUTFChars(jCascDir, nullptr);
	const char* name = env->GetStringUTFChars(jName, nullptr);
	jintArray arr = nullptr;
	try {
		std::vector<uint8_t> d;
		{
			std::lock_guard<std::mutex> lk(g_casc_mtx);
			bwgame::casc::storage* c = get_casc(dir);
			if (c && c->has_name(name)) c->read_name(name, d);
		}
		auto u32 = [&](size_t o) { return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24); };
		if (d.size() > 128 && d[0] == 'D' && d[1] == 'D' && d[2] == 'S' && d[3] == ' ') {
			const size_t h = u32(12), w = u32(16);
			const uint32_t pf_flags = u32(80), fourcc = u32(84), bits = u32(88);
			const bool rgba = (pf_flags & 0x40) && fourcc == 0 && bits == 32 && u32(92) == 0xffu && u32(96) == 0xff00u && u32(100) == 0xff0000u;
			if (rgba && w > 0 && h > 0 && w <= 4096 && h <= 4096 && d.size() >= 128 + w * h * 4) {
				std::vector<jint> px(2 + w * h);
				px[0] = (jint)w; px[1] = (jint)h;
				const uint8_t* s = d.data() + 128;
				jint* dst = px.data() + 2;
				for (size_t i = 0, n = w * h; i != n; ++i, s += 4)
					dst[i] = (jint)(0xff000000u | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | (uint32_t)s[2]);
				d.clear(); d.shrink_to_fit();
				arr = env->NewIntArray((jsize)px.size());
				if (arr) env->SetIntArrayRegion(arr, 0, (jsize)px.size(), px.data());
			}
		}
	} catch (...) {
		arr = nullptr;
	}
	env->ReleaseStringUTFChars(jCascDir, dir);
	env->ReleaseStringUTFChars(jName, name);
	return arr;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_org_openbw_android_PortraitStore_nativeDecodeWebm(JNIEnv* env, jclass, jstring jCascDir, jstring jName, jint maxFrames) {
	const char* dir = env->GetStringUTFChars(jCascDir, nullptr);
	const char* name = env->GetStringUTFChars(jName, nullptr);
	std::vector<jint> px;
	try {
		std::vector<uint8_t> bytes;
		{
			std::lock_guard<std::mutex> lk(g_casc_mtx);
			bwgame::casc::storage* c = get_casc(dir);
			if (c && c->has_name(name)) c->read_name(name, bytes);
		}
		if (!bytes.empty()) {
			bwgame::webm::file_t doc = bwgame::webm::parse(bytes.data(), bytes.size());
			if (doc.ok && !doc.frames.empty()) {
				std::vector<std::vector<uint8_t>> frames;
				int shown = 0, w = 0, h = 0;
				bwgame::vp9::decode_sequence_hash(bytes.data(), bytes.size(), doc.frames, &shown, /*as_rgb=*/true, &frames, &w, &h, nullptr);
				size_t n = frames.size();
				if (maxFrames > 0 && n > (size_t)maxFrames) n = (size_t)maxFrames;
				if (n > 0 && w > 0 && h > 0) {
					int ch = w * 14 / 15;                 // 15:14, like the 60x56 SD clips
					if (ch > h) ch = h;
					int y0 = (h - ch) / 2;
					px.assign(4 + (size_t)w * n * ch, 0);
					px[0] = w; px[1] = ch; px[2] = (jint)n; px[3] = 100000;   // the game plays them at 10 fps
					for (size_t f = 0; f != n; ++f) {
						const std::vector<uint8_t>& fr = frames[f];
						if (fr.size() < (size_t)w * h * 3) continue;
						for (int y = 0; y != ch; ++y) {
							const uint8_t* src = fr.data() + (size_t)(y0 + y) * w * 3;
							jint* dst = px.data() + 4 + (size_t)y * (w * n) + f * w;
							for (int x = 0; x != w; ++x)
								dst[x] = (jint)(0xff000000u | ((uint32_t)src[x * 3] << 16) | ((uint32_t)src[x * 3 + 1] << 8) | (uint32_t)src[x * 3 + 2]);
						}
					}
				}
			}
		}
	} catch (...) {
		px.clear();
	}
	env->ReleaseStringUTFChars(jCascDir, dir);
	env->ReleaseStringUTFChars(jName, name);
	if (px.empty()) return nullptr;
	jintArray arr = env->NewIntArray((jsize)px.size());
	if (arr) env->SetIntArrayRegion(arr, 0, (jsize)px.size(), px.data());
	return arr;
}
