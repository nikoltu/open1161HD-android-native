# Third-party code in deps/

| Directory | What | Version / source | Licence | Shipped in the APK |
|---|---|---|---|---|
| `asio/` | Asio networking (header-only) | from upstream OpenBW | Boost Software License 1.0 (`asio/LICENSE_1_0.txt`) | yes (engine) |
| `unrar/` | RARLab UnRAR source, **decompression only** | 7.3.2, `https://www.rarlab.com/rar/unrarsrc-7.3.2.tar.gz`, unmodified | UnRAR licence (`unrar/license.txt`): free use to handle RAR archives; must not be used to build a RAR-compatible archiver or re-create the RAR compression algorithm | yes, `libarcimport.so` (launcher import) |
| `lzma/C/` | LZMA SDK 7z decoder subset (7z container, LZMA, LZMA2, PPMd, BCJ/BCJ2/ARM filters, CRC) | 26.06, `https://github.com/ip7z/7zip/releases/download/26.06/lzma2606.7z`, unmodified files | public domain (`lzma/lzma-sdk.txt`) | yes, `libarcimport.so` |

Notes:
- The app shows the licence texts in About (`android/app/src/main/assets/licenses/UnRAR.txt`, `LZMA-SDK.txt`).
- `unrar/` is the complete unmodified source tree. Android builds it with `RARDLL` and a forced include
  (`android/app/src/main/cpp/arc_compat.h`) that only declares `lutimes` for API < 26; no vendored file is edited.
- `lzma/C/` holds only the files the 7z decoder needs; `7zDec.c` is compiled with `Z7_PPMD_SUPPORT`.
- Host check of both: `bash tools/arccheck.sh <work_dir> [rar_test_vectors_dir]` (7-Zip as the oracle).
