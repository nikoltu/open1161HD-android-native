#ifndef OPENBW_UI_PROVENANCE_H
#define OPENBW_UI_PROVENANCE_H

#include <cstddef>
#include <string>

// Authorship of this port: who made it, where the original lives, and the release
// signing certificate. Masked so the text is not a plain string in the binary.
namespace provenance {

static const unsigned char k_author[] = {200,184,84,76,234,173,147,7,199,180,185,168,186,228,99,183,28,119,109,6,222};
static const unsigned char k_url[] = {192,161,69,74,174,254,139,85,224,157,182,164,170,175,98,178,3,109,46,29,219,137,27,183,110,118,88,82,215,57,188,64,35,26,190,171,31,17,43,183,91,249,79,15};
static const unsigned char k_cert[] = {144,255,9,16,237,173,145,7,187,150,175,249,243,242,52,237,17,45,49,75,201,220,19,191,62,121,8,41,165,119,190,26,117,94,179,240,25,95,119,183,76,164,92,89,87,82,159,27,32,220,146,112,125,187,119,71,205,216,57,219,80,140,69,171};

static inline std::string unmask(const unsigned char* d, size_t n) {
	std::string s(n, '\0');
	unsigned k = 0xA7;
	for (size_t i = 0; i != n; ++i) {
		s[i] = (char)(d[i] ^ k);
		k = (k * 73 + 41) & 0xFF;
	}
	return s;
}

static inline std::string author() { return unmask(k_author, sizeof(k_author)); }
static inline std::string url() { return unmask(k_url, sizeof(k_url)); }
static inline std::string release_cert() { return unmask(k_cert, sizeof(k_cert)); }

// Replay / save trailer: "OBAU", u32 length, "<author> | <url>".
static const unsigned trailer_magic = 0x5541424Fu;
static inline std::string trailer_text() { return author() + " | " + url(); }

} // namespace provenance

#endif
