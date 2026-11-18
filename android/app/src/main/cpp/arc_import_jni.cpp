// JNI side of ArchiveImporter.java: RAR / 7z listing and extraction (arc_import.h).
#include <jni.h>
#include "arc_compat.h"
#include "arc_import.h"
#if defined(__ANDROID__) && __ANDROID_API__ < 26
#include <fcntl.h>
#include <sys/stat.h>
extern "C" int lutimes(const char* path, const struct timeval tv[2]) {
	struct timespec ts[2];
	for (int i = 0; i != 2; ++i) { ts[i].tv_sec = tv[i].tv_sec; ts[i].tv_nsec = tv[i].tv_usec * 1000; }
	return utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
}
#endif

namespace {

std::string jstr(JNIEnv* env, jstring s) {
	if (!s) return {};
	const char* c = env->GetStringUTFChars(s, nullptr);
	std::string r = c ? c : "";
	if (c) env->ReleaseStringUTFChars(s, c);
	return r;
}

// Strings from native UTF-8 may hold 4-byte sequences, which NewStringUTF
// (modified UTF-8) rejects; build them from UTF-16 instead.
jstring to_jstring(JNIEnv* env, const std::string& s) {
	std::vector<jchar> u;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = (unsigned char)s[i];
		uint32_t cp; size_t n;
		if (c < 0x80) { cp = c; n = 1; }
		else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = (c & 0x1f) << 6 | (s[i + 1] & 0x3f); n = 2; }
		else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = (c & 0x0f) << 12 | (s[i + 1] & 0x3f) << 6 | (s[i + 2] & 0x3f); n = 3; }
		else if ((c >> 3) == 30 && i + 3 < s.size()) { cp = (c & 0x07) << 18 | (s[i + 1] & 0x3f) << 12 | (s[i + 2] & 0x3f) << 6 | (s[i + 3] & 0x3f); n = 4; }
		else { cp = 0xfffd; n = 1; }
		if (cp >= 0x10000) { cp -= 0x10000; u.push_back((jchar)(0xd800 + (cp >> 10))); u.push_back((jchar)(0xdc00 + (cp & 0x3ff))); }
		else u.push_back((jchar)cp);
		i += n;
	}
	return env->NewString(u.data(), (jsize)u.size());
}

} // namespace

// kind: 1 = RAR (path = first volume), 2 = 7z (fd, or path when fd < 0).
// Returns {"" or error token, then "flags\tsize\tname" per entry}; flags d/e/l/-.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_openbw_android_ArchiveImporter_nList(JNIEnv* env, jclass, jint kind, jstring jpath, jint fd) {
	std::string path = jstr(env, jpath);
	std::vector<arcimp::entry> list;
	std::string err = kind == 1 ? arcimp::rar_list(path, list) : arcimp::sz_list(path.c_str(), fd, list);
	if (!err.empty()) list.clear();
	jclass str = env->FindClass("java/lang/String");
	jobjectArray out = env->NewObjectArray((jsize)(list.size() + 1), str, nullptr);
	env->SetObjectArrayElement(out, 0, to_jstring(env, err));
	for (size_t i = 0; i != list.size(); ++i) {
		auto& e = list[i];
		std::string flags;
		if (e.dir) flags += 'd';
		if (e.encrypted) flags += 'e';
		if (e.link) flags += 'l';
		if (flags.empty()) flags = "-";
		jstring js = to_jstring(env, flags + "\t" + std::to_string(e.size) + "\t" + e.name);
		env->SetObjectArrayElement(out, (jsize)(i + 1), js);
		env->DeleteLocalRef(js);
	}
	return out;
}

// names[i] -> dests[i]; progress.onBytes(long total) returns false to cancel.
// Returns null on success, else an error token.
extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_ArchiveImporter_nExtract(JNIEnv* env, jclass, jint kind, jstring jpath, jint fd,
		jobjectArray names, jobjectArray dests, jobject progress) {
	std::string path = jstr(env, jpath);
	std::map<std::string, std::string> want;
	jsize n = env->GetArrayLength(names);
	for (jsize i = 0; i != n; ++i) {
		auto jn = (jstring)env->GetObjectArrayElement(names, i);
		auto jd = (jstring)env->GetObjectArrayElement(dests, i);
		want[jstr(env, jn)] = jstr(env, jd);
		env->DeleteLocalRef(jn);
		env->DeleteLocalRef(jd);
	}
	jmethodID on_bytes = nullptr;
	if (progress) on_bytes = env->GetMethodID(env->GetObjectClass(progress), "onBytes", "(J)Z");
	uint64_t reported = 0;
	auto cb = [&](uint64_t done) -> bool {
		if (!on_bytes || done - reported < (1u << 20)) return true;
		reported = done;
		bool go = env->CallBooleanMethod(progress, on_bytes, (jlong)done);
		if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
		return go;
	};
	std::string err;
	try {
		err = kind == 1 ? arcimp::rar_extract(path, want, cb) : arcimp::sz_extract(path.c_str(), fd, want, cb);
	} catch (...) {
		err = "corrupt";
	}
	return err.empty() ? nullptr : to_jstring(env, err);
}
