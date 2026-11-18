// arc_import.h -- RAR (RARLab UnRAR, deps/unrar) and 7z (LZMA SDK, deps/lzma)
// listing and extraction for the launcher's first-run import. Decompression
// only. Shared by arc_import_jni.cpp and the host harness tools/arccheck.cpp.
//
// Errors are short tokens the Java side maps to messages: password, volume,
// dictionary, corrupt, cancel, open, write, unsupported, memory.
#ifndef OPENBW_ARC_IMPORT_H
#define OPENBW_ARC_IMPORT_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "../../../../../deps/lzma/C/7z.h"
#include "../../../../../deps/lzma/C/7zAlloc.h"
#include "../../../../../deps/lzma/C/7zCrc.h"
#include "../../../../../deps/lzma/C/7zFile.h"
#include "../../../../../deps/lzma/C/Lzma2Dec.h"
#include "../../../../../deps/lzma/C/LzmaDec.h"
}

// after the LZMA headers: dll.hpp #defines LONG / UINT / HANDLE on Unix
#ifdef _WIN32
#include <windows.h>
#elif !defined(_UNIX)
#define _UNIX // dll.hpp's own typedefs (unrar's os.hpp sets this for its sources)
#endif
#include "../../../../../deps/unrar/dll.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace arcimp {

struct entry {
	std::string name; // archive path, '/' separated, UTF-8
	uint64_t size = 0;
	bool dir = false;
	bool encrypted = false;
	bool link = false;
};

// bytes written so far for the whole call; return false to cancel
using progress_fn = std::function<bool(uint64_t)>;

// Larger dictionaries / single folders are refused (phone memory).
constexpr uint64_t max_dictionary = 256ull << 20;
constexpr uint64_t max_memory_folder = 192ull << 20;

inline void utf8_append(std::string& s, uint32_t c) {
	if (c < 0x80) s += (char)c;
	else if (c < 0x800) { s += (char)(0xc0 | c >> 6); s += (char)(0x80 | (c & 0x3f)); }
	else if (c < 0x10000) { s += (char)(0xe0 | c >> 12); s += (char)(0x80 | ((c >> 6) & 0x3f)); s += (char)(0x80 | (c & 0x3f)); }
	else { s += (char)(0xf0 | c >> 18); s += (char)(0x80 | ((c >> 12) & 0x3f)); s += (char)(0x80 | ((c >> 6) & 0x3f)); s += (char)(0x80 | (c & 0x3f)); }
}

inline std::string normalize_name(std::string s) {
	for (auto& c : s) if (c == '\\') c = '/';
	return s;
}

// ---------------------------------------------------------------- RAR

struct rar_ctx {
	progress_fn progress;
	uint64_t done = 0;
	bool cancelled = false;
	bool volume_missing = false;
	bool password = false;
	bool large_dict = false;
};

inline int CALLBACK rar_callback(UINT msg, LPARAM user, LPARAM p1, LPARAM p2) {
	auto* c = (rar_ctx*)user;
	switch (msg) {
	case UCM_CHANGEVOLUME:
	case UCM_CHANGEVOLUMEW:
		if (p2 == RAR_VOL_ASK) { c->volume_missing = true; return -1; }
		return 1;
	case UCM_NEEDPASSWORD:
	case UCM_NEEDPASSWORDW:
		c->password = true;
		return -1;
	case UCM_LARGEDICT:
		c->large_dict = true;
		return 0;
	case UCM_PROCESSDATA:
		c->done += (uint64_t)p2;
		if (c->progress && !c->progress(c->done)) { c->cancelled = true; return -1; }
		return 1;
	}
	(void)p1;
	return 0;
}

inline std::string rar_error(int code, const rar_ctx& c) {
	if (c.cancelled) return "cancel";
	if (c.password) return "password";
	if (c.volume_missing) return "volume";
	if (c.large_dict) return "dictionary";
	switch (code) {
	case ERAR_MISSING_PASSWORD: case ERAR_BAD_PASSWORD: return "password";
	case ERAR_NO_MEMORY: return "memory";
	case ERAR_EOPEN: return "open";
	case ERAR_ECREATE: case ERAR_EWRITE: return "write";
	case ERAR_LARGE_DICT: case ERAR_USER_LIMIT: return "dictionary";
	case ERAR_UNKNOWN_FORMAT: return "unsupported";
	}
	return "corrupt";
}

inline std::string rar_name(const RARHeaderDataEx& h) {
	std::string s;
	for (size_t i = 0; i != sizeof(h.FileNameW) / sizeof(wchar_t) && h.FileNameW[i]; ++i) {
		uint32_t c = (uint32_t)h.FileNameW[i];
		if (sizeof(wchar_t) == 2 && c >= 0xd800 && c < 0xdc00 && h.FileNameW[i + 1]) {
			uint32_t lo = (uint32_t)h.FileNameW[++i];
			c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
		}
		utf8_append(s, c);
	}
	return normalize_name(s);
}

inline HANDLE rar_open(const std::string& first_volume, unsigned mode, rar_ctx& ctx, std::string& err) {
	RAROpenArchiveDataEx d;
	memset(&d, 0, sizeof d);
	d.ArcName = (char*)first_volume.c_str();
	d.OpenMode = mode;
	d.Callback = rar_callback;
	d.UserData = (LPARAM)&ctx;
	d.MaxAllocate = (unsigned)(max_dictionary >> 20) + 64;
	HANDLE h = RAROpenArchiveEx(&d);
	if (!h || d.OpenResult != ERAR_SUCCESS) {
		if (h) RARCloseArchive(h);
		err = rar_error((int)d.OpenResult, ctx);
		return nullptr;
	}
	if (d.Flags & ROADF_ENCHEADERS) { RARCloseArchive(h); err = "password"; return nullptr; }
	return h;
}

inline std::string rar_list(const std::string& first_volume, std::vector<entry>& out) {
	rar_ctx ctx;
	std::string err;
	HANDLE h = rar_open(first_volume, RAR_OM_LIST, ctx, err);
	if (!h) return err;
	RARHeaderDataEx hd;
	memset(&hd, 0, sizeof hd);
	int r;
	while ((r = RARReadHeaderEx(h, &hd)) == ERAR_SUCCESS) {
		entry e;
		e.name = rar_name(hd);
		e.size = (uint64_t)hd.UnpSizeHigh << 32 | hd.UnpSize;
		e.dir = (hd.Flags & RHDF_DIRECTORY) != 0;
		e.encrypted = (hd.Flags & RHDF_ENCRYPTED) != 0;
		e.link = hd.RedirType != 0;
		if (!(hd.Flags & RHDF_SPLITBEFORE)) out.push_back(e);
		if ((r = RARProcessFile(h, RAR_SKIP, nullptr, nullptr)) != ERAR_SUCCESS) break;
	}
	RARCloseArchive(h);
	if (r != ERAR_END_ARCHIVE) return rar_error(r, ctx);
	return "";
}

// want: archive name -> destination path. Every wanted name must be extracted.
inline std::string rar_extract(const std::string& first_volume, const std::map<std::string, std::string>& want, progress_fn progress) {
	rar_ctx ctx;
	ctx.progress = std::move(progress);
	std::string err;
	HANDLE h = rar_open(first_volume, RAR_OM_EXTRACT, ctx, err);
	if (!h) return err;
	RARHeaderDataEx hd;
	memset(&hd, 0, sizeof hd);
	size_t got = 0;
	int r;
	while ((r = RARReadHeaderEx(h, &hd)) == ERAR_SUCCESS) {
		auto it = want.find(rar_name(hd));
		bool take = it != want.end() && !(hd.Flags & RHDF_DIRECTORY) && hd.RedirType == 0;
		if (take && (hd.Flags & RHDF_ENCRYPTED)) { RARCloseArchive(h); return "password"; }
		if (take && (uint64_t)hd.DictSize * 1024 > max_dictionary) { RARCloseArchive(h); return "dictionary"; }
		r = RARProcessFile(h, take ? RAR_EXTRACT : RAR_SKIP, nullptr, take ? (char*)it->second.c_str() : nullptr);
		if (r != ERAR_SUCCESS) break;
		if (take) ++got;
	}
	RARCloseArchive(h);
	if (r != ERAR_END_ARCHIVE) return rar_error(r, ctx);
	if (got < want.size()) return "corrupt";
	return "";
}

// ---------------------------------------------------------------- 7z

struct sz_archive {
	CFileInStream file;
	CLookToRead2 look;
	CSzArEx db;
	ISzAlloc alloc = {SzAlloc, SzFree};
	ISzAlloc alloc_temp = {SzAllocTemp, SzFreeTemp};
	bool opened = false;
	bool db_open = false;
	static const size_t look_size = 1 << 16;

	~sz_archive() {
		if (db_open) SzArEx_Free(&db, &alloc);
		if (look.buf) ISzAlloc_Free(&alloc, look.buf);
		if (opened) File_Close(&file.file);
	}

	// fd >= 0 (POSIX): the descriptor is duplicated and owned here; else path is opened
	std::string open(const char* path, int fd) {
		static bool crc_ready = false;
		if (!crc_ready) { CrcGenerateTable(); crc_ready = true; }
		FileInStream_CreateVTable(&file);
		File_Construct(&file.file);
		memset(&look, 0, sizeof look);
#ifndef _WIN32
		if (fd >= 0) {
			file.file.fd = dup(fd);
			if (file.file.fd < 0) return "open";
			if (lseek(file.file.fd, 0, SEEK_SET) != 0) { close(file.file.fd); return "open"; }
		} else
#endif
		{
			(void)fd;
			if (InFile_Open(&file.file, path) != 0) return "open";
		}
		opened = true;
		LookToRead2_CreateVTable(&look, False);
		look.buf = (Byte*)ISzAlloc_Alloc(&alloc, look_size);
		if (!look.buf) return "memory";
		look.bufSize = look_size;
		look.realStream = &file.vt;
		LookToRead2_INIT(&look);
		SzArEx_Init(&db);
		SRes res = SzArEx_Open(&db, &look.vt, &alloc, &alloc_temp);
		if (res != SZ_OK) return res == SZ_ERROR_MEM ? "memory" : res == SZ_ERROR_UNSUPPORTED ? "unsupported" : "corrupt";
		db_open = true;
		return "";
	}

	std::string name(UInt32 i) const {
		size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
		std::vector<UInt16> buf(len + 1);
		SzArEx_GetFileNameUtf16(&db, i, buf.data());
		std::string s;
		for (size_t k = 0; k < len && buf[k]; ++k) {
			uint32_t c = buf[k];
			if (c >= 0xd800 && c < 0xdc00 && k + 1 < len) c = 0x10000 + ((c - 0xd800) << 10) + (buf[++k] - 0xdc00);
			utf8_append(s, c);
		}
		return normalize_name(s);
	}
};

inline std::string sz_list(const char* path, int fd, std::vector<entry>& out) {
	sz_archive a;
	std::string err = a.open(path, fd);
	if (!err.empty()) return err;
	for (UInt32 i = 0; i != a.db.NumFiles; ++i) {
		entry e;
		e.name = a.name(i);
		e.dir = SzArEx_IsDir(&a.db, i);
		e.size = SzArEx_GetFileSize(&a.db, i);
		out.push_back(e);
	}
	return "";
}

namespace detail {

struct sz_writer {
	FILE* f = nullptr;
	std::string path;
	uint64_t left = 0;
	UInt32 crc = CRC_INIT_VAL;
	bool check_crc = false;
	UInt32 want_crc = 0;
};

// Single-coder LZMA / LZMA2 / Copy folder: decoded in chunks, never whole.
inline std::string sz_stream_folder(sz_archive& a, UInt32 folder, const CSzFolder& f, const Byte* props,
		const std::function<std::string(const Byte*, size_t)>& sink) {
	const CSzAr& ar = a.db.db;
	UInt32 pack_index = ar.FoStartPackStreamIndex[folder];
	UInt64 pack_pos = a.db.dataPos + ar.PackPositions[pack_index];
	UInt64 pack_left = ar.PackPositions[pack_index + 1] - ar.PackPositions[pack_index];
	UInt64 unpack_left = SzAr_GetFolderUnpackSize(&ar, folder);
	if (LookInStream_SeekTo(&a.look.vt, pack_pos) != SZ_OK) return "corrupt";
	const CSzCoderInfo& c = f.Coders[0];
	std::vector<Byte> out(1 << 18);

	auto read_in = [&](const void*& buf, size_t& size) -> bool {
		size = (size_t)(pack_left < (1 << 16) ? pack_left : (1 << 16));
		if (size == 0) { buf = nullptr; return true; }
		return ILookInStream_Look(&a.look.vt, &buf, &size) == SZ_OK;
	};

	if (c.MethodID == 0) {
		while (unpack_left) {
			const void* buf; size_t size;
			if (!read_in(buf, size) || size == 0) return "corrupt";
			if (size > unpack_left) size = (size_t)unpack_left;
			std::string e = sink((const Byte*)buf, size);
			if (e == "done") return "";
			if (!e.empty()) return e;
			ILookInStream_Skip(&a.look.vt, size);
			pack_left -= size;
			unpack_left -= size;
		}
		return "";
	}
	const bool lzma2 = c.MethodID == 0x21;
	CLzmaDec lz;
	CLzma2Dec lz2;
	if (lzma2) {
		if (c.PropsSize != 1) return "unsupported";
		Byte p = props[0];
		uint64_t dict = p == 40 ? 0xffffffffull : ((uint64_t)(2 | (p & 1)) << (p / 2 + 11));
		if (dict > max_dictionary) return "dictionary";
		Lzma2Dec_CONSTRUCT(&lz2);
		if (Lzma2Dec_Allocate(&lz2, p, &a.alloc) != SZ_OK) return "memory";
		Lzma2Dec_Init(&lz2);
	} else {
		if (c.PropsSize < 5) return "unsupported";
		uint64_t dict = (uint64_t)props[1] | (uint64_t)props[2] << 8 | (uint64_t)props[3] << 16 | (uint64_t)props[4] << 24;
		if (dict > max_dictionary) return "dictionary";
		LzmaDec_CONSTRUCT(&lz);
		if (LzmaDec_Allocate(&lz, props, c.PropsSize, &a.alloc) != SZ_OK) return "memory";
		LzmaDec_Init(&lz);
	}
	std::string err;
	while (unpack_left) {
		const void* buf; size_t in_size;
		if (!read_in(buf, in_size)) { err = "corrupt"; break; }
		size_t out_size = out.size();
		if (out_size > unpack_left) out_size = (size_t)unpack_left;
		SizeT in_used = in_size, out_used = out_size;
		ELzmaStatus status;
		SRes res = lzma2 ? Lzma2Dec_DecodeToBuf(&lz2, out.data(), &out_used, (const Byte*)buf, &in_used, LZMA_FINISH_ANY, &status)
		                 : LzmaDec_DecodeToBuf(&lz, out.data(), &out_used, (const Byte*)buf, &in_used, LZMA_FINISH_ANY, &status);
		if (res != SZ_OK) { err = "corrupt"; break; }
		ILookInStream_Skip(&a.look.vt, in_used);
		pack_left -= in_used;
		unpack_left -= out_used;
		if (out_used) {
			err = sink(out.data(), out_used);
			if (err == "done") { err.clear(); break; } // every wanted file of this folder is out
			if (!err.empty()) break;
		}
		if (in_used == 0 && out_used == 0) { err = "corrupt"; break; }
	}
	if (lzma2) Lzma2Dec_Free(&lz2, &a.alloc);
	else LzmaDec_Free(&lz, &a.alloc);
	return err;
}

} // namespace detail

inline std::string sz_extract(const char* path, int fd, const std::map<std::string, std::string>& want, progress_fn progress) {
	sz_archive a;
	std::string err = a.open(path, fd);
	if (!err.empty()) return err;
	const CSzAr& ar = a.db.db;
	uint64_t done = 0;
	size_t got = 0;
	std::map<UInt32, std::vector<UInt32>> folders;
	for (UInt32 i = 0; i != a.db.NumFiles; ++i) {
		if (SzArEx_IsDir(&a.db, i)) continue;
		auto it = want.find(a.name(i));
		if (it == want.end()) continue;
		UInt32 folder = a.db.FileToFolder[i];
		if (folder == (UInt32)-1) {
			FILE* f = fopen(it->second.c_str(), "wb");
			if (!f) return "write";
			fclose(f);
			++got;
			continue;
		}
		folders[folder].push_back(i);
	}
	for (auto& fe : folders) {
		UInt32 folder = fe.first;
		CSzFolder f;
		CSzData sd;
		sd.Data = ar.CodersData + ar.FoCodersOffsets[folder];
		sd.Size = ar.FoCodersOffsets[folder + 1] - ar.FoCodersOffsets[folder];
		if (SzGetNextFolderItem(&f, &sd) != SZ_OK) return "corrupt";
		const Byte* props = ar.CodersData + ar.FoCodersOffsets[folder] + f.Coders[0].PropsOffset;
		const UInt32 first = a.db.FolderToFile[folder];
		const UInt64 folder_base = a.db.UnpackPositions[first];
		for (UInt32 k = 0; k != f.NumCoders; ++k) if (f.Coders[k].MethodID == 0x06F10701) return "password"; // AES
		// file index -> destination, in folder order
		std::vector<detail::sz_writer> files;
		for (UInt32 i : fe.second) {
			detail::sz_writer w;
			w.path = want.at(a.name(i));
			w.left = SzArEx_GetFileSize(&a.db, i);
			w.check_crc = SzBitWithVals_Check(&a.db.CRCs, i);
			w.want_crc = w.check_crc ? a.db.CRCs.Vals[i] : 0;
			files.push_back(w);
		}
		const bool streamable = f.NumCoders == 1 && f.NumPackStreams == 1 &&
			(f.Coders[0].MethodID == 0 || f.Coders[0].MethodID == 0x30101 || f.Coders[0].MethodID == 0x21);
		std::string ferr;
		uint64_t pos = 0; // offset inside the folder output
		size_t next = 0;  // next wanted file (fe.second is ascending = folder order)
		auto close_file = [&](detail::sz_writer& w) -> std::string {
			if (!w.f) return "";
			bool ok = fclose(w.f) == 0;
			w.f = nullptr;
			if (!ok) return "write";
			if (w.check_crc && CRC_GET_DIGEST(w.crc) != w.want_crc) return "corrupt";
			++got;
			return "";
		};
		auto sink = [&](const Byte* data, size_t n) -> std::string {
			while (n) {
				if (next == fe.second.size()) { pos += n; return "done"; }
				UInt32 idx = fe.second[next];
				uint64_t start = a.db.UnpackPositions[idx] - folder_base;
				auto& w = files[next];
				if (pos < start) {
					uint64_t skip = start - pos;
					if (skip >= n) { pos += n; return ""; }
					data += skip; n -= (size_t)skip; pos += skip;
				}
				if (!w.f) {
					w.f = fopen(w.path.c_str(), "wb");
					if (!w.f) return "write";
				}
				size_t k = (size_t)(w.left < n ? w.left : n);
				if (k && fwrite(data, 1, k, w.f) != k) return "write";
				w.crc = CrcUpdate(w.crc, data, k);
				w.left -= k; data += k; n -= k; pos += k; done += k;
				if (progress && !progress(done)) return "cancel";
				if (w.left == 0) {
					std::string e = close_file(w);
					if (!e.empty()) return e;
					++next;
				}
			}
			return "";
		};
		if (streamable) {
			ferr = detail::sz_stream_folder(a, folder, f, props, sink);
		} else {
			UInt64 size = SzAr_GetFolderUnpackSize(&ar, folder);
			if (size > max_memory_folder) return "unsupported";
			std::vector<Byte> buf((size_t)size);
			SRes res = SzAr_DecodeFolder(&ar, folder, &a.look.vt, a.db.dataPos, buf.data(), (size_t)size, &a.alloc_temp);
			if (res != SZ_OK) return res == SZ_ERROR_UNSUPPORTED ? "unsupported" : res == SZ_ERROR_MEM ? "memory" : "corrupt";
			ferr = sink(buf.data(), buf.size());
			if (ferr == "done") ferr.clear();
		}
		// zero-length files at the end of a folder
		while (ferr.empty() && next < fe.second.size() && files[next].left == 0) {
			auto& w = files[next];
			if (!w.f && !(w.f = fopen(w.path.c_str(), "wb"))) { ferr = "write"; break; }
			ferr = close_file(w);
			++next;
		}
		for (auto& w : files) if (w.f) { fclose(w.f); w.f = nullptr; }
		if (!ferr.empty()) return ferr;
		if (next != fe.second.size()) return "corrupt";
	}
	if (got < want.size()) return "corrupt";
	return "";
}

} // namespace arcimp

#endif
