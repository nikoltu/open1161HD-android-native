// v0.53: JNI wrapper around minimap_gen.h so the launcher (SetupActivity) can
// render a map's minimap to a PNG for the long-press preview. This lives in its
// own small shared library (libopenbw_minimap.so) that pulls in only the
// header-only data-loading layer - no SDL, no engine state - so the launcher
// can load it cheaply without starting the game.
#include <jni.h>
#include "minimap_gen.h"
#include "oe_profile.h"

extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_SetupActivity_nativeGenMinimap(
		JNIEnv* env, jclass, jstring jGameDir, jstring jMapFile, jstring jOutPng) {
	const char* gamedir = env->GetStringUTFChars(jGameDir, nullptr);
	const char* mapfile = env->GetStringUTFChars(jMapFile, nullptr);
	const char* outpng = env->GetStringUTFChars(jOutPng, nullptr);
	bwgame::a_string err;
	bool ok = false;
	try {
		ok = bwgame::minimap_gen::generate_minimap_png(gamedir, mapfile, outpng, err);
	} catch (std::exception& e) {
		err = e.what();
	} catch (...) {
		err = "unknown error";
	}
	env->ReleaseStringUTFChars(jGameDir, gamedir);
	env->ReleaseStringUTFChars(jMapFile, mapfile);
	env->ReleaseStringUTFChars(jOutPng, outpng);
	// Return null on success, or the error message so Java can log it.
	if (ok) return nullptr;
	return env->NewStringUTF(err.c_str());
}

// v0.56: scenario name + description for the campaign briefing preview.
// Returns "name\ndescription" on success, or null on failure.
extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_SetupActivity_nativeMapInfo(
		JNIEnv* env, jclass, jstring jMapFile) {
	const char* mapfile = env->GetStringUTFChars(jMapFile, nullptr);
	bwgame::a_string name, desc, err;
	bool ok = false;
	try { ok = bwgame::minimap_gen::get_map_info(mapfile, name, desc, err); }
	catch (...) { ok = false; }
	env->ReleaseStringUTFChars(jMapFile, mapfile);
	if (!ok) return nullptr;
	bwgame::a_string out = name + "\n" + desc;
	return env->NewStringUTF(out.c_str());
}

// UMS lobby: "<scenario 0/1>;<OWNR hex x8>;<SIDE hex x8>", or null on failure.
extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_SetupActivity_nativeMapSlots(
		JNIEnv* env, jclass, jstring jMapFile) {
	const char* mapfile = env->GetStringUTFChars(jMapFile, nullptr);
	bwgame::a_string name, desc, err, slots;
	bool ok = false;
	try { ok = bwgame::minimap_gen::get_map_info(mapfile, name, desc, err, &slots); }
	catch (...) { ok = false; }
	env->ReleaseStringUTFChars(jMapFile, mapfile);
	if (!ok) return nullptr;
	return env->NewStringUTF(slots.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_SetupActivity_nativeMapSlotsInternal(
		JNIEnv* env, jclass, jstring jGameDir, jstring jMapPath, jstring jScratch) {
	const char* gamedir = env->GetStringUTFChars(jGameDir, nullptr);
	const char* mappath = env->GetStringUTFChars(jMapPath, nullptr);
	const char* scratch = env->GetStringUTFChars(jScratch, nullptr);
	bwgame::a_string name, desc, err, slots;
	bool ok = false;
	try { ok = bwgame::minimap_gen::get_map_info_internal(gamedir, mappath, scratch, name, desc, err, &slots); }
	catch (...) { ok = false; }
	env->ReleaseStringUTFChars(jGameDir, gamedir);
	env->ReleaseStringUTFChars(jMapPath, mappath);
	env->ReleaseStringUTFChars(jScratch, scratch);
	if (!ok) return nullptr;
	return env->NewStringUTF(slots.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_SetupActivity_nativeMapInfoInternal(
		JNIEnv* env, jclass, jstring jGameDir, jstring jMapPath, jstring jScratch) {
	const char* gamedir = env->GetStringUTFChars(jGameDir, nullptr);
	const char* mappath = env->GetStringUTFChars(jMapPath, nullptr);
	const char* scratch = env->GetStringUTFChars(jScratch, nullptr);
	bwgame::a_string name, desc, err;
	bool ok = false;
	try { ok = bwgame::minimap_gen::get_map_info_internal(gamedir, mappath, scratch, name, desc, err); }
	catch (...) { ok = false; }
	env->ReleaseStringUTFChars(jGameDir, gamedir);
	env->ReleaseStringUTFChars(jMapPath, mappath);
	env->ReleaseStringUTFChars(jScratch, scratch);
	if (!ok) return nullptr;
	bwgame::a_string out = name + "\n" + desc;
	return env->NewStringUTF(out.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_SetupActivity_nativeGenMinimapInternal(
		JNIEnv* env, jclass, jstring jGameDir, jstring jMapPath, jstring jOutPng) {
	const char* gamedir = env->GetStringUTFChars(jGameDir, nullptr);
	const char* mappath = env->GetStringUTFChars(jMapPath, nullptr);
	const char* outpng = env->GetStringUTFChars(jOutPng, nullptr);
	bwgame::a_string err;
	bool ok = false;
	try {
		ok = bwgame::minimap_gen::generate_minimap_png_internal(gamedir, mappath, outpng, err);
	} catch (std::exception& e) {
		err = e.what();
	} catch (...) {
		err = "unknown error";
	}
	env->ReleaseStringUTFChars(jGameDir, gamedir);
	env->ReleaseStringUTFChars(jMapPath, mappath);
	env->ReleaseStringUTFChars(jOutPng, outpng);
	if (ok) return nullptr;
	return env->NewStringUTF(err.c_str());
}

// G13: Ganymede Extended profile of a map file. null = classic map (no oe\manifest.json);
// "ok;<runtime>" = playable here; "refuse;<runtime>;<kind>;<arg>;<reason>" = refused (kind: damaged / runtime / feature).
extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_SetupActivity_nativeMapProfile(
		JNIEnv* env, jclass, jstring jMapFile) {
	const char* mapfile = env->GetStringUTFChars(jMapFile, nullptr);
	oeprofile::manifest m;
	try { m = oeprofile::check_map_file(mapfile); } catch (...) {}
	env->ReleaseStringUTFChars(jMapFile, mapfile);
	if (!m.present) return nullptr;
	std::string out = (m.playable ? "ok;" : "refuse;") + std::to_string(m.runtime);
	if (!m.playable) {
		std::string arg = m.arg;
		for (char& c : arg) if (c == ';') c = ',';
		out += ";" + m.kind + ";" + arg + ";" + m.reason;
	}
	return env->NewStringUTF(out.c_str());
}

// Authorship for the launcher's About page: "<author>\n<url>\n<1 = signed with the release key>".
#include "provenance.h"
extern "C" JNIEXPORT jstring JNICALL
Java_org_openbw_android_SetupActivity_nativeOrigin(JNIEnv* env, jclass, jobjectArray jDigests) {
	const std::string cert = provenance::release_cert();
	bool official = false;
	jsize n = jDigests ? env->GetArrayLength(jDigests) : 0;
	for (jsize i = 0; i < n && !official; ++i) {
		jstring s = (jstring)env->GetObjectArrayElement(jDigests, i);
		if (!s) continue;
		const char* c = env->GetStringUTFChars(s, nullptr);
		official = c && cert == c;
		if (c) env->ReleaseStringUTFChars(s, c);
		env->DeleteLocalRef(s);
	}
	std::string out = provenance::author() + "\n" + provenance::url() + "\n" + (official ? "1" : "0");
	return env->NewStringUTF(out.c_str());
}
