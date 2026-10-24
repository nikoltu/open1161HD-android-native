// open1161HD Faze 4.7 Variant B: hardware WebM cinematic decode (AMediaCodec).
// Android-only. The engine (ui.h) calls these through std::function callbacks set
// by android_main, so the cross-platform header stays free of NDK media headers.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace cine_hw {

struct decoder; // opaque

// Open a WebM cinematic from in-memory bytes (staged to tmp_dir for the
// extractor). Returns null if unsupported (API < 26) or on any failure. Fills the
// video dimensions and microseconds-per-frame.
decoder* open(const uint8_t* webm, size_t size, const char* tmp_dir,
              int& out_w, int& out_h, double& out_usf);

// Decode the next video frame into rgb (out_w*out_h*3, R,G,B). false at EOF.
bool next_video(decoder* d, std::vector<uint8_t>& rgb);

void close(decoder* d);

} // namespace cine_hw

// Faze 4.7 Variant A: play a WebM cinematic full-screen through the OS video
// player (OpenBWActivity.playCinematic -> VideoView). The OS handles VP9 + Vorbis
// and device-specific (UBWC/tiled) output, so this is robust on every SoC. Blocks
// the calling thread until the clip finishes or is skipped. Returns true if played.
namespace cine_java {
bool play(const uint8_t* webm, size_t size, const char* tmp_dir);
}
