// casc_inflate.h -- zero-dependency zlib inflate provider for ui/casc.h.
//
// ui/casc.h leaves the zlib inflate (BLTE 'Z' chunks) injectable so the header
// carries no compression dependency. The engine's MPQ path uses PKWARE, not
// zlib, so there is no zlib inflate to borrow -- EXCEPT the one already vendored
// inside ui/stb_image.h (its PNG decoder). stbi_zlib_decode_malloc decodes a
// standard zlib stream, which is exactly what a BLTE 'Z' chunk body is. This
// adapter exposes it as a casc::inflate_fn, so wiring CASC into the engine needs
// no new third-party code and no build changes.
//
// Include this in the (single) TU that also compiles the stb implementation
// (ui/ddsgrp.h does, via STB_IMAGE_IMPLEMENTATION). Proven byte-identical to a
// libz-backed inflate across the whole CASC corpus (tools/casc_check.sh).
#ifndef BWGAME_UI_CASC_INFLATE_H
#define BWGAME_UI_CASC_INFLATE_H

#include "casc.h"
#include "ddsgrp.h"   // pulls the stb_image implementation (STBI_ONLY_PNG -> zlib)
#include <cstdlib>

namespace bwgame {
namespace casc {

// A casc::inflate_fn backed by stb_image's zlib decoder.
static inline inflate_fn stb_inflate() {
	return [](const uint8_t* src, size_t srclen, size_t /*hint*/, std::vector<uint8_t>& out) -> bool {
		int outlen = 0;
		char* p = stbi_zlib_decode_malloc((const char*)src, (int)srclen, &outlen);
		if (!p) return false;
		out.assign((const uint8_t*)p, (const uint8_t*)p + outlen);
		free(p); // STBI_FREE defaults to free
		return true;
	};
}

} // namespace casc
} // namespace bwgame

#endif
