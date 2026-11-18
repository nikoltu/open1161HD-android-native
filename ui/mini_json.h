#pragma once
// Small strict JSON reader for editor-written side files (oe/manifest.json,
// data/oe/extensions.json). Not used by the sim; a parse failure is a value.
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mini_json {

struct value {
	enum kind_t { null_v, bool_v, number_v, string_v, array_v, object_v } kind = null_v;
	bool b = false;
	double num = 0;
	std::string str;
	std::vector<value> arr;
	std::vector<std::pair<std::string, value>> obj; // file order kept

	const value* get(const std::string& key) const {
		if (kind != object_v) return nullptr;
		for (auto& kv : obj) if (kv.first == key) return &kv.second;
		return nullptr;
	}
	bool is_int() const { return kind == number_v && num == (double)(long long)num; }
	long long as_int(long long def = 0) const { return kind == number_v ? (long long)num : def; }
	std::string as_str(const std::string& def = std::string()) const { return kind == string_v ? str : def; }
	long long int_at(const std::string& key, long long def = 0) const { auto* v = get(key); return v ? v->as_int(def) : def; }
	std::string str_at(const std::string& key, const std::string& def = std::string()) const { auto* v = get(key); return v ? v->as_str(def) : def; }
	bool bool_at(const std::string& key, bool def = false) const { auto* v = get(key); return v && v->kind == bool_v ? v->b : def; }
};

struct parser {
	const char* p;
	const char* e;
	std::string err;
	int depth = 0;

	void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
	bool fail(const char* m) { if (err.empty()) err = m; return false; }

	bool parse_string(std::string& out) {
		if (p >= e || *p != '"') return fail("expected string");
		++p;
		while (p < e && *p != '"') {
			unsigned char c = (unsigned char)*p++;
			if (c < 0x20) return fail("control character in string");
			if (c != '\\') { out += (char)c; continue; }
			if (p >= e) return fail("bad escape");
			char x = *p++;
			switch (x) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				if (e - p < 4) return fail("bad \\u escape");
				unsigned cp = 0;
				for (int i = 0; i != 4; ++i) {
					char h = *p++;
					cp <<= 4;
					if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
					else return fail("bad \\u escape");
				}
				if (cp < 0x80) out += (char)cp;
				else if (cp < 0x800) { out += (char)(0xc0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3f)); }
				else { out += (char)(0xe0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
				break;
			}
			default: return fail("bad escape");
			}
		}
		if (p >= e) return fail("unterminated string");
		++p;
		return true;
	}

	bool parse_value(value& v) {
		ws();
		if (p >= e) return fail("unexpected end");
		if (++depth > 64) return fail("nesting too deep");
		bool ok = parse_inner(v);
		--depth;
		return ok;
	}

	bool parse_inner(value& v) {
		char c = *p;
		if (c == '{') {
			++p;
			v.kind = value::object_v;
			ws();
			if (p < e && *p == '}') { ++p; return true; }
			while (true) {
				ws();
				std::string k;
				if (!parse_string(k)) return false;
				ws();
				if (p >= e || *p != ':') return fail("expected ':'");
				++p;
				value x;
				if (!parse_value(x)) return false;
				v.obj.emplace_back(std::move(k), std::move(x));
				ws();
				if (p < e && *p == ',') { ++p; continue; }
				if (p < e && *p == '}') { ++p; return true; }
				return fail("expected ',' or '}'");
			}
		}
		if (c == '[') {
			++p;
			v.kind = value::array_v;
			ws();
			if (p < e && *p == ']') { ++p; return true; }
			while (true) {
				value x;
				if (!parse_value(x)) return false;
				v.arr.push_back(std::move(x));
				ws();
				if (p < e && *p == ',') { ++p; continue; }
				if (p < e && *p == ']') { ++p; return true; }
				return fail("expected ',' or ']'");
			}
		}
		if (c == '"') { v.kind = value::string_v; return parse_string(v.str); }
		auto word = [&](const char* w) {
			size_t n = 0;
			while (w[n]) ++n;
			if ((size_t)(e - p) < n) return false;
			for (size_t i = 0; i != n; ++i) if (p[i] != w[i]) return false;
			p += n;
			return true;
		};
		if (word("true")) { v.kind = value::bool_v; v.b = true; return true; }
		if (word("false")) { v.kind = value::bool_v; v.b = false; return true; }
		if (word("null")) { v.kind = value::null_v; return true; }
		if (c == '-' || (c >= '0' && c <= '9')) {
			const char* s = p;
			if (*p == '-') ++p;
			while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) ++p;
			std::string t(s, p);
			char* endp = nullptr;
			v.kind = value::number_v;
			v.num = strtod(t.c_str(), &endp);
			if (!endp || *endp) return fail("bad number");
			return true;
		}
		return fail("unexpected character");
	}
};

// False (and err) on malformed input or trailing garbage. A UTF-8 BOM is skipped.
inline bool parse(const char* data, size_t size, value& out, std::string* err = nullptr) {
	parser ps{data, data + size, std::string()};
	if (size >= 3 && (unsigned char)data[0] == 0xef && (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf) ps.p += 3;
	out = value();
	bool ok = ps.parse_value(out);
	if (ok) { ps.ws(); if (ps.p != ps.e) ok = ps.fail("trailing data"); }
	if (!ok && err) *err = ps.err;
	return ok;
}

} // namespace mini_json
