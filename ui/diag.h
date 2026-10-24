// diag.h -- in-engine bug-catcher / diagnostics harness (DIAG), P1 core.
//
// A READ-ONLY, runtime-gated observability layer. It emits versioned JSON-line
// events on a DEDICATED logcat tag "OBDIAG" (parallel to the free-form "OpenBW"
// tag), so a human `adb logcat -s OBDIAG` or a parser/LLM gets one jq-ready JSON
// object per line with nothing interleaved. It also mirrors to a file so a whole
// session is `adb pull`-able.
//
// Presentation / tooling ONLY: it never mutates sim state (bwgame.h/actions.h/
// sync.h/replay.h) or the action stream, it times itself on wall-clock (never a
// sim frame), and when disabled it is a single branch. It therefore carries NO
// sim-determinism gate.
//
// P1 scope: the sink + schema + the OBDIAG tag + a file sink + two detectors
// (rally_partial, sel_cap). Ring buffer, rate-limit, crash hooks, and the
// OB_DIAG compile-gate for zero-cost release are later phases.
#ifndef BWGAME_UI_DIAG_H
#define BWGAME_UI_DIAG_H

#include <string>
#include <cstdio>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <exception>

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <ctime>
#include <ucontext.h>
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
#endif

namespace bwgame {
namespace diag {

enum class Sev { info, warn, error, fatal };
static inline const char* sev_str(Sev s) {
	switch (s) { case Sev::info: return "info"; case Sev::warn: return "warn";
	             case Sev::error: return "error"; default: return "fatal"; }
}

// --- crash-breadcrumb ring (pre-allocated BSS; read from the signal handler) ---
// A fixed POD ring of the most recent events. Populated by Sink::event() when
// --diag is on; the crash handler flushes it WITHOUT touching the heap (on a
// SIGSEGV the allocator may be the corrupt thing). All scalars + fixed char[]:
// no pointers into heap-owned strings, so it is safe to read at death.
namespace crash_detail {
	struct ring_rec { uint32_t frame; uint32_t t_ms; uint8_t sev; char cat[16]; char code[24]; };
	static const int RING_CAP = 256;
	static ring_rec g_ring[RING_CAP];          // BSS, exists before any allocation
	static volatile uint32_t g_ring_head = 0;  // monotonic write count
	static volatile uint32_t g_last_frame = 0; // last sampled st.current_frame

	static inline void copy_fixed(char* dst, size_t cap, const char* src) {
		size_t i = 0; for (; src && src[i] && i + 1 < cap; ++i) dst[i] = src[i]; dst[i] = 0;
	}
	static inline void ring_push(uint32_t frame, uint32_t t_ms, uint8_t sev,
	                             const char* cat, const char* code) {
		uint32_t h = g_ring_head++;                 // wraps naturally; reader uses modulo
		ring_rec& r = g_ring[h % RING_CAP];
		r.frame = frame; r.t_ms = t_ms; r.sev = sev;
		copy_fixed(r.cat, sizeof r.cat, cat);
		copy_fixed(r.code, sizeof r.code, code);
	}
}

// Single sink, runtime-enabled by the --diag flag (mirrors the trigger-trace
// null-default idiom: off by default, one branch when compiled-in-but-off).
class Sink {
public:
	static Sink& get() { static Sink s; return s; }
	void set_enabled(bool e) { if (e && !enabled_) t0_ = now_ms(); enabled_ = e; }
	bool enabled() const { return enabled_; }
	void set_file(const std::string& path) {
		if (fp_) { fclose(fp_); fp_ = nullptr; }
		fp_ = fopen(path.c_str(), "wb");
	}

	// One event = one JSON line. `data_body` is a pre-built JSON object body
	// WITHOUT the surrounding braces (e.g. "\"sel\":5,\"applied\":3"), or empty.
	void event(long frame, const char* cat, Sev sev, const char* code,
	           const std::string& msg, const std::string& data_body) {
		if (!enabled_) return;
		// Breadcrumb ring first (cheap, POD) so the crash handler has it even if the
		// JSONL formatting below is what faults.
		crash_detail::ring_push((uint32_t)frame, (uint32_t)(now_ms() - t0_), (uint8_t)sev, cat, code);
		std::string line;
		line.reserve(160 + msg.size() + data_body.size());
		line += "{\"ver\":1,\"f\":";  line += std::to_string(frame);
		line += ",\"t\":";            line += std::to_string(now_ms() - t0_);
		line += ",\"cat\":\"";        line += cat;
		line += "\",\"sev\":\"";      line += sev_str(sev);
		line += "\",\"code\":\"";     line += code;
		line += "\",\"msg\":\"";      escape_into(line, msg);
		line += "\",\"data\":{";      line += data_body;
		line += "}}";
#ifdef __ANDROID__
		int prio = sev == Sev::info ? ANDROID_LOG_INFO : sev == Sev::warn ? ANDROID_LOG_WARN
		         : sev == Sev::error ? ANDROID_LOG_ERROR : ANDROID_LOG_FATAL;
		__android_log_write(prio, "OBDIAG", line.c_str());
#else
		fprintf(stderr, "OBDIAG %s\n", line.c_str());
#endif
		if (fp_) {
			fwrite(line.data(), 1, line.size(), fp_);
			fputc('\n', fp_);
			if (sev >= Sev::warn) fflush(fp_);   // faults survive a hard exit
		}
	}

private:
	Sink() = default;
	bool enabled_ = false;
	FILE* fp_ = nullptr;
	long long t0_ = 0;
	static long long now_ms() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	static void escape_into(std::string& o, const std::string& s) {
		for (char c : s) {
			if (c == '"' || c == '\\') { o += '\\'; o += c; }
			else if (c == '\n') o += "\\n";
			else if ((unsigned char)c < 0x20) { /* drop control chars */ }
			else o += c;
		}
	}
};

static inline Sink& sink() { return Sink::get(); }
static inline bool enabled() { return Sink::get().enabled(); }

// Escape an arbitrary string into a JSON string VALUE *including* the surrounding
// quotes, for safely embedding dynamic/untrusted text into an event()'s data_body
// (event() only auto-escapes `msg`; data_body is passed through verbatim by design
// so callers can build "sel":5 fragments, so any string value there must go via
// this).
static inline std::string jval(const std::string& s) {
	std::string o = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\') { o += '\\'; o += c; }
		else if (c == '\n') o += "\\n";
		else if ((unsigned char)c < 0x20) { /* drop control chars */ }
		else o += c;
	}
	o += '"';
	return o;
}

static inline void emit(long frame, const char* cat, Sev sev, const char* code,
                        const std::string& msg, const std::string& data = "") {
	Sink::get().event(frame, cat, sev, code, msg, data);
}

// Update the "last frame the sim reached" scalar the crash header reports. Called
// once per frame from the UI loop; a plain store to a volatile, safe to read from
// the signal handler (never read live st.current_frame there -- the sim object may
// be the corrupt thing).
static inline void set_frame(uint32_t f) { crash_detail::g_last_frame = f; }

// ===========================================================================
// Crash capture (DIAG-CRASH). Installed UNCONDITIONALLY (not --diag-gated): its
// only cost when idle is a sigaction at startup + ~128 KiB BSS, and a native
// crash is exactly when you wish you had context. It NEVER mutates sim and only
// reads pre-copied scalars, so it carries no determinism gate. The handler is
// strictly async-signal-safe: raw write(2) to a pre-opened fd, hand-rolled
// allocation-free number emitters, no malloc/stdio/log/SDL, a re-entrancy guard,
// and it chains to the previously-installed (SDL/ART/OS) handler so the normal
// tombstone still fires -- our JSONL is strictly additive.
// ===========================================================================
#ifdef __ANDROID__
namespace crash_detail {
	static const int NSIG_TRAP = 5;
	static const int g_sigs[NSIG_TRAP] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };
	static struct sigaction g_prev[NSIG_TRAP];
	static char g_sigstack[SIGSTKSZ < 65536 ? 65536 : SIGSTKSZ];   // BSS alt stack
	static int g_crash_fd = -1;
	static volatile sig_atomic_t g_in_handler = 0;
	static std::terminate_handler g_prev_terminate = nullptr;

	// Module base cache, snapshotted at init / after bot dlopen (normal context).
	struct mod_ent { char name[40]; uintptr_t base; uintptr_t end; };
	// We record ONLY our own libs (allowlist below), not the 300+ system objects a
	// loaded Android process carries: enumerating-all-and-capping is fragile (our
	// libs sort last, so any cap drops them), and attributing a system-lib PC to the
	// nearest of our libs yields a garbage offset. Filtering + a per-module span check
	// (base<=pc<end) means our frames resolve exactly and system frames honestly show
	// no module (use the OS tombstone for those).
	static const int MODS_CAP = 24;
	// substring tokens that identify a module as ours (engine, SDL, STL, bots)
	static bool name_wanted(const char* nm) {
		static const char* toks[] = { "libmain", "libSDL2", "libc++_shared", "libopenbw",
			"steamhammer", "ualbertabot", "aiurlite", "zerglite", "steamlite", "bwapidemo", "libbot" };
		for (const char* t : toks) {
			for (const char* h = nm; *h; ++h) {
				const char* a = h; const char* b = t;
				while (*a && *b && *a == *b) { ++a; ++b; }
				if (!*b) return true;
			}
		}
		return false;
	}
	static mod_ent g_mods[MODS_CAP];
	static int g_mods_n = 0;
	// Set while snapshot_modules() rebuilds g_mods (after a bot dlopen). A crash on
	// another thread during that narrow window skips module attribution (raw pc only)
	// rather than reading a half-written table.
	static volatile sig_atomic_t g_mods_updating = 0;

	static const char* signame(int s) {
		switch (s) { case SIGSEGV: return "SIGSEGV"; case SIGBUS: return "SIGBUS";
		             case SIGABRT: return "SIGABRT"; case SIGILL: return "SIGILL";
		             case SIGFPE: return "SIGFPE"; default: return "SIG?"; }
	}
	static int sig_idx(int s) { for (int i = 0; i < NSIG_TRAP; ++i) if (g_sigs[i] == s) return i; return -1; }

	// --- allocation-free emitters (async-signal-safe) ---
	static void aw(int fd, const char* s, size_t n) {
		while (n) { ssize_t w = write(fd, s, n); if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; } s += w; n -= (size_t)w; }
	}
	static void aws(int fd, const char* s) { size_t n = 0; while (s[n]) ++n; aw(fd, s, n); }
	static int u64dec(char* p, uint64_t v) {
		if (!v) { p[0] = '0'; return 1; }
		char t[24]; int n = 0; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
		for (int i = 0; i < n; ++i) p[i] = t[n - 1 - i]; return n;
	}
	static int u64hex(char* p, uint64_t v) {
		static const char H[] = "0123456789abcdef";
		p[0] = '0'; p[1] = 'x'; if (!v) { p[2] = '0'; return 3; }
		char t[16]; int n = 0; while (v) { t[n++] = H[v & 0xf]; v >>= 4; }
		for (int i = 0; i < n; ++i) p[2 + i] = t[n - 1 - i]; return 2 + n;
	}
	static void awdec(int fd, uint64_t v) { char b[24]; aw(fd, b, u64dec(b, v)); }
	static void awhex(int fd, uint64_t v) { char b[18]; aw(fd, b, u64hex(b, v)); }
	// JSON string body: emit already-sanitized ASCII label bytes, escaping " and \.
	static void awjson(int fd, const char* s) {
		for (size_t i = 0; s[i]; ++i) {
			char c = s[i];
			if (c == '"' || c == '\\') { char e[2] = { '\\', c }; aw(fd, e, 2); }
			else if ((unsigned char)c >= 0x20) aw(fd, &c, 1);
		}
	}

	// Snapshot the load bias of every loaded ELF via dl_iterate_phdr. This reports
	// the real soname + load base even for libs mmap'd directly out of base.apk
	// (extractNativeLibs=false) -- which /proc/self/maps hides behind an "...base.apk"
	// path, so a maps scan misses our own libmain.so entirely. NORMAL CONTEXT ONLY
	// (dl_iterate_phdr takes the loader lock): call at init and after a bot dlopen.
	static int phdr_cb(struct dl_phdr_info* info, size_t, void*) {
		if (g_mods_n >= MODS_CAP) return 1;   // stop iterating (safety; allowlist is small)
		const char* path = info->dlpi_name ? info->dlpi_name : "";
		const char* nm = path;                // basename of the soname/path
		for (const char* p = path; *p; ++p) if (*p == '/') nm = p + 1;
		if (!name_wanted(nm)) return 0;       // only our own libs
		// span = highest (p_vaddr + p_memsz) over PT_LOAD segments, so mod_for_pc can
		// range-check and not mis-claim a system-lib PC.
		uintptr_t base = (uintptr_t)info->dlpi_addr, span = 0;
		for (int j = 0; j < info->dlpi_phnum; ++j) {
			const ElfW(Phdr)& ph = info->dlpi_phdr[j];
			if (ph.p_type == PT_LOAD) { uintptr_t e = (uintptr_t)ph.p_vaddr + (uintptr_t)ph.p_memsz; if (e > span) span = e; }
		}
		mod_ent& m = g_mods[g_mods_n++];
		int cp = 0; for (; nm[cp] && cp < (int)sizeof(m.name) - 1; ++cp) m.name[cp] = nm[cp]; m.name[cp] = 0;
		m.base = base; m.end = base + span;   // off = pc - base
		return 0;
	}
	static void snapshot_modules() {
		g_mods_updating = 1; g_mods_n = 0;
		dl_iterate_phdr(phdr_cb, nullptr);
		g_mods_updating = 0;
	}

	static const mod_ent* mod_for_pc(uintptr_t pc) {
		// exact containment: pc must fall inside the module's mapped span. A system-lib
		// PC (not in our allowlist) returns null -> the frame emits pc only, no bogus
		// module/offset.
		for (int i = 0; i < g_mods_n; ++i) if (g_mods[i].base <= pc && pc < g_mods[i].end) return &g_mods[i];
		return nullptr;
	}

	// Emit the shared body: module map, backtrace, event-ring, footer. Used by both
	// the signal handler and the terminate hook.
	static void write_body(int fd, uintptr_t fault_pc, uintptr_t fault_fp, uintptr_t fault_sp) {
		// modules (skipped if a refresh_modules() rebuild is mid-flight on another
		// thread -- a torn g_mods would only mean a null module, but skip to be tidy).
		bool mods_ok = !g_mods_updating;
		if (mods_ok) for (int i = 0; i < g_mods_n; ++i) {
			aws(fd, "{\"v\":1,\"rec\":\"module\",\"name\":\""); awjson(fd, g_mods[i].name);
			aws(fd, "\",\"base\":\""); awhex(fd, (uint64_t)g_mods[i].base); aws(fd, "\"}\n");
		}
		// backtrace: FP-walk (arm64), guarded on every deref. First frame is the exact
		// fault PC from the ucontext (more precise than the FP chain).
		int nframes = 0;
		if (fault_pc) {
			const mod_ent* m = mods_ok ? mod_for_pc(fault_pc) : nullptr;
			aws(fd, "{\"v\":1,\"rec\":\"frame\",\"i\":0,\"pc\":\""); awhex(fd, fault_pc);
			if (m) { aws(fd, "\",\"module\":\""); awjson(fd, m->name); aws(fd, "\",\"off\":\""); awhex(fd, (uint64_t)(fault_pc - m->base)); }
			aws(fd, "\"}\n"); ++nframes;
		}
		uintptr_t fp = fault_fp, prev = 0;
		for (int i = 1; i < 64 && fp; ++i) {
			if ((fp & 0xf) || fp <= prev) break;                 // aligned + monotonic
			if (fault_sp && fp < fault_sp) break;                // FP must be at/above the faulting SP
			if (prev && fp - prev > (16u << 20)) break;          // implausible jump (>16 MiB)
			uintptr_t nextfp = ((volatile uintptr_t*)fp)[0];     // saved x29
			uintptr_t lr     = ((volatile uintptr_t*)fp)[1];     // saved LR (return addr)
			if (!lr) break;
			const mod_ent* m = mods_ok ? mod_for_pc(lr) : nullptr;
			aws(fd, "{\"v\":1,\"rec\":\"frame\",\"i\":"); awdec(fd, (uint64_t)i);
			aws(fd, ",\"pc\":\""); awhex(fd, lr);
			if (m) { aws(fd, "\",\"module\":\""); awjson(fd, m->name); aws(fd, "\",\"off\":\""); awhex(fd, (uint64_t)(lr - m->base)); }
			aws(fd, "\"}\n"); ++nframes;
			prev = fp; fp = nextfp;
		}
		// event breadcrumb ring: oldest preserved -> newest
		uint32_t head = g_ring_head;
		uint32_t count = head < (uint32_t)RING_CAP ? head : (uint32_t)RING_CAP;
		for (uint32_t k = 0; k < count; ++k) {
			const ring_rec& r = g_ring[(head - count + k) % RING_CAP];
			aws(fd, "{\"v\":1,\"rec\":\"event\",\"f\":"); awdec(fd, r.frame);
			aws(fd, ",\"t\":"); awdec(fd, r.t_ms);
			aws(fd, ",\"sev\":"); awdec(fd, r.sev);
			aws(fd, ",\"cat\":\""); awjson(fd, r.cat);
			aws(fd, "\",\"code\":\""); awjson(fd, r.code); aws(fd, "\"}\n");
		}
		aws(fd, "{\"v\":1,\"rec\":\"crash_end\",\"frames\":"); awdec(fd, (uint64_t)nframes);
		aws(fd, ",\"events\":"); awdec(fd, (uint64_t)count); aws(fd, "}\n");
	}

	static void chain_to_prev(int sig, siginfo_t* info, void* uctx) {
		int i = sig_idx(sig);
		if (i >= 0 && (g_prev[i].sa_flags & SA_SIGINFO) && g_prev[i].sa_sigaction) {
			g_prev[i].sa_sigaction(sig, info, uctx);
		} else if (i >= 0 && g_prev[i].sa_handler != SIG_DFL && g_prev[i].sa_handler != SIG_IGN) {
			g_prev[i].sa_handler(sig);
		} else {
			signal(sig, SIG_DFL); raise(sig);   // OS tombstone + correct exit signal
		}
	}

	static void crash_handler(int sig, siginfo_t* info, void* uctx) {
		int saved_errno = errno;   // don't clobber the interrupted context's errno
		if (__atomic_test_and_set(&g_in_handler, __ATOMIC_SEQ_CST)) { errno = saved_errno; chain_to_prev(sig, info, uctx); return; }
		int fd = g_crash_fd;
		if (fd >= 0) {
			uintptr_t pc = 0, fp = 0, sp = 0;
			ucontext_t* uc = (ucontext_t*)uctx;
#if defined(__aarch64__)
			if (uc) { pc = (uintptr_t)uc->uc_mcontext.pc; fp = (uintptr_t)uc->uc_mcontext.regs[29]; sp = (uintptr_t)uc->uc_mcontext.sp; }
#elif defined(__arm__)
			if (uc) { pc = (uintptr_t)uc->uc_mcontext.arm_pc; fp = (uintptr_t)uc->uc_mcontext.arm_fp; sp = (uintptr_t)uc->uc_mcontext.arm_sp; }
#endif
			aws(fd, "{\"v\":1,\"rec\":\"crash_header\",\"sig\":"); awdec(fd, (uint64_t)sig);
			aws(fd, ",\"signame\":\""); aws(fd, signame(sig));
			aws(fd, "\",\"code\":"); awdec(fd, (uint64_t)(info ? info->si_code : 0));
			aws(fd, ",\"fault_addr\":\""); awhex(fd, (uint64_t)(uintptr_t)(info ? info->si_addr : 0));
			aws(fd, "\",\"pid\":"); awdec(fd, (uint64_t)getpid());
			aws(fd, ",\"tid\":"); awdec(fd, (uint64_t)gettid());
			aws(fd, ",\"frame\":"); awdec(fd, (uint64_t)g_last_frame);
			aws(fd, ",\"abi\":\"arm64-v8a\"}\n");
			write_body(fd, pc, fp, sp);
			fsync(fd);
		}
		errno = saved_errno;
		chain_to_prev(sig, info, uctx);
	}

	static void terminate_hook() {
		// Normal (non-signal) context: std::terminate from fatal_error_str/uncaught
		// throw. Safe to format; write a terminate record + the same body, then chain
		// to the previous terminate (which raises SIGABRT -> our handler sees the guard
		// already set and just chains to the OS, avoiding a double body).
		if (!__atomic_test_and_set(&g_in_handler, __ATOMIC_SEQ_CST)) {
			int fd = g_crash_fd;
			if (fd >= 0) {
				aws(fd, "{\"v\":1,\"rec\":\"crash_header\",\"sig\":6,\"signame\":\"terminate\",\"frame\":");
				awdec(fd, (uint64_t)g_last_frame); aws(fd, ",\"abi\":\"arm64-v8a\"}\n");
				write_body(fd, 0, 0, 0);
				fsync(fd);
			}
		}
		if (g_prev_terminate) g_prev_terminate(); else abort();
	}
} // namespace crash_detail

// Install the crash handler + alt stack + terminate hook, and open the crash fd.
// `crash_path` is a pre-built absolute path (heap done here, in normal context, so
// the handler never touches a path string). Safe to call once at startup.
// NOTE (main-thread alt stack): sigaltstack is per-thread, so the static BSS alt
// stack only backs faults on the thread that called this (the SDL/main thread,
// where the sim + bots run). A stack-overflow SIGSEGV on a *worker* thread (audio
// callback, portrait decoder) can't run the handler and goes straight to the OS
// tombstone; a non-overflow off-thread fault still dumps fine (handler runs on the
// live stack). Register the alt stack from a worker's entry point if that ever
// needs covering.
static inline void install_crash_handler(const char* crash_path) {
	using namespace crash_detail;
	if (crash_path && *crash_path)
		g_crash_fd = open(crash_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	snapshot_modules();
	stack_t ss; memset(&ss, 0, sizeof ss);
	ss.ss_sp = g_sigstack; ss.ss_size = sizeof g_sigstack; ss.ss_flags = 0;
	sigaltstack(&ss, nullptr);
	struct sigaction sa; memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = crash_handler;
	sigemptyset(&sa.sa_mask);
	for (int i = 0; i < NSIG_TRAP; ++i) sigaddset(&sa.sa_mask, g_sigs[i]);
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
	for (int i = 0; i < NSIG_TRAP; ++i) sigaction(g_sigs[i], &sa, &g_prev[i]);
	g_prev_terminate = std::set_terminate(terminate_hook);
}
// Re-snapshot module bases (call after a bot .so is dlopen'd so its frames resolve).
static inline void refresh_modules() { crash_detail::snapshot_modules(); }
#else
static inline void install_crash_handler(const char*) {}
static inline void refresh_modules() {}
#endif

} // namespace diag
} // namespace bwgame

// Runtime-gated helper. P1 is always compiled (cost when off = one branch, the
// idiom the sim already trusts for trigger_trace_sink). A later phase wraps this
// in `#ifdef OB_DIAG` for a byte-zero release build (DIAG-ARCH s5.1).
#define DIAG_ON() (::bwgame::diag::enabled())

#endif
