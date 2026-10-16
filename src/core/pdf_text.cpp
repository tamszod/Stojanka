#include "core/pdf_text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "core/archive.h"
#include "core/pdf_crypto.h"

namespace strojanka {

namespace {

bool is_ws(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == '\0'; }
bool is_delim(char c) { return is_ws(c) || c == '/' || c == '[' || c == ']' || c == '<' || c == '>' || c == '(' || c == ')' || c == '%' || c == '{' || c == '}'; }
bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

struct Object {
    std::string dict;    // everything between "obj" and "stream"/"endobj"
    std::string stream;  // raw (still compressed, maybe encrypted) stream bytes
    bool has_stream = false;
    int generation = 0;
};

// "/Length 123" as a direct number, or -1 (missing, or an indirect "N 0 R" reference).
long direct_length(const std::string& dict) {
    for (size_t p = dict.find("/Length"); p != std::string::npos; p = dict.find("/Length", p + 1)) {
        size_t i = p + 7;
        if (i >= dict.size() || !is_ws(dict[i])) continue;  // /Length1 etc. are font program sizes
        while (i < dict.size() && is_ws(dict[i])) ++i;
        size_t j = i;
        while (j < dict.size() && is_digit(dict[j])) ++j;
        if (j == i) return -1;
        size_t k = j;
        while (k < dict.size() && is_ws(dict[k])) ++k;
        if (k < dict.size() && is_digit(dict[k])) return -1;  // "12 0 R"
        return std::atol(dict.substr(i, j - i).c_str());
    }
    return -1;
}

// Every "N G obj ... endobj" in the file, found by scanning (no cross-reference table needed).
std::map<int, Object> read_objects(const std::string& pdf) {
    std::map<int, Object> objs;
    size_t search = 0;
    while (search < pdf.size()) {
        const size_t k = pdf.find("obj", search);
        if (k == std::string::npos) break;
        search = k + 3;
        if (k >= 3 && pdf.compare(k - 3, 3, "end") == 0) continue;
        if (k + 3 < pdf.size() && !is_delim(pdf[k + 3])) continue;
        size_t j = k;
        if (j == 0 || !is_ws(pdf[j - 1])) continue;
        while (j > 0 && is_ws(pdf[j - 1])) --j;
        const size_t gen_end = j;
        while (j > 0 && is_digit(pdf[j - 1])) --j;
        if (j == gen_end || j == 0 || !is_ws(pdf[j - 1])) continue;
        const int generation = std::atoi(pdf.substr(j, gen_end - j).c_str());
        while (j > 0 && is_ws(pdf[j - 1])) --j;
        const size_t num_end = j;
        while (j > 0 && is_digit(pdf[j - 1])) --j;
        if (j == num_end) continue;
        const int num = std::atoi(pdf.substr(j, num_end - j).c_str());

        const size_t body = k + 3;
        size_t endobj = pdf.find("endobj", body);
        size_t st = pdf.find("stream", body);
        Object o;
        o.generation = generation;
        if (st != std::string::npos && (endobj == std::string::npos || st < endobj)) {
            o.dict = pdf.substr(body, st - body);
            size_t data = st + 6;
            if (data < pdf.size() && pdf[data] == '\r') ++data;
            if (data < pdf.size() && pdf[data] == '\n') ++data;
            const long len = direct_length(o.dict);
            size_t data_end;
            if (len >= 0 && data + static_cast<size_t>(len) <= pdf.size()) {
                data_end = data + static_cast<size_t>(len);
            } else {
                data_end = pdf.find("endstream", data);
                if (data_end == std::string::npos) data_end = pdf.size();
                while (data_end > data && (pdf[data_end - 1] == '\n' || pdf[data_end - 1] == '\r')) --data_end;
            }
            o.stream = pdf.substr(data, data_end - data);
            o.has_stream = true;
            endobj = pdf.find("endobj", data_end);
        } else {
            o.dict = pdf.substr(body, endobj == std::string::npos ? std::string::npos : endobj - body);
        }
        objs[num] = std::move(o);
        search = endobj == std::string::npos ? pdf.size() : endobj + 6;
    }
    return objs;
}

std::string decoded(const Object& o) {
    if (!o.has_stream) return {};
    if (o.dict.find("/FlateDecode") == std::string::npos) return o.stream;
    auto out = inflate_data(o.stream);
    return out ? *out : std::string();
}

// The "<< ... >>" that follows `key` in `text`, with nested dictionaries kept intact.
std::string balanced_dict(const std::string& text, const std::string& key) {
    size_t i = text.find(key);
    if (i == std::string::npos) return {};
    i = text.find("<<", i);
    if (i == std::string::npos) return {};
    int depth = 0;
    for (size_t j = i; j + 1 < text.size(); ++j) {
        if (text[j] == '<' && text[j + 1] == '<') {
            ++depth;
            ++j;
        } else if (text[j] == '>' && text[j + 1] == '>') {
            if (--depth == 0) return text.substr(i, j + 2 - i);
            ++j;
        }
    }
    return {};
}

// "/Key 12 0 R" -> 12, or -1.
int ref_after(const std::string& text, const std::string& key) {
    for (size_t p = text.find(key); p != std::string::npos; p = text.find(key, p + 1)) {
        size_t i = p + key.size();
        if (i < text.size() && !is_ws(text[i])) continue;
        while (i < text.size() && is_ws(text[i])) ++i;
        size_t j = i;
        while (j < text.size() && is_digit(text[j])) ++j;
        if (j == i) return -1;
        return std::atoi(text.substr(i, j - i).c_str());
    }
    return -1;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

uint32_t hex_value(const std::string& hex) { return static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16)); }

// A ToUnicode destination (UTF-16BE hex) as UTF-8; "" for U+0000, which some writers use for "unknown".
std::string utf16_hex_to_utf8(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 4 <= hex.size(); i += 4) {
        uint32_t u = hex_value(hex.substr(i, 4));
        if (u >= 0xD800 && u < 0xDC00 && i + 8 <= hex.size()) {
            const uint32_t lo = hex_value(hex.substr(i + 4, 4));
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 4;
        }
        if (u != 0) append_utf8(out, u);
    }
    return out;
}

struct Font {
    std::map<uint32_t, std::string> map;
    bool two_byte = false;  // Type0 (CID) fonts use 2-byte codes
};

std::vector<std::string> hex_tokens(const std::string& s, size_t from, size_t to) {
    std::vector<std::string> out;
    for (size_t p = s.find('<', from); p != std::string::npos && p < to; p = s.find('<', p + 1)) {
        const size_t q = s.find('>', p);
        if (q == std::string::npos || q > to) break;
        out.push_back(s.substr(p + 1, q - p - 1));
        p = q;
    }
    return out;
}

void parse_cmap(const std::string& cmap, Font& font) {
    for (size_t b = cmap.find("beginbfchar"); b != std::string::npos; b = cmap.find("beginbfchar", b + 1)) {
        const size_t e = cmap.find("endbfchar", b);
        const auto t = hex_tokens(cmap, b, e == std::string::npos ? cmap.size() : e);
        for (size_t i = 0; i + 1 < t.size(); i += 2) font.map[hex_value(t[i])] = utf16_hex_to_utf8(t[i + 1]);
    }
    for (size_t b = cmap.find("beginbfrange"); b != std::string::npos; b = cmap.find("beginbfrange", b + 1)) {
        const size_t e = cmap.find("endbfrange", b);
        const size_t end = e == std::string::npos ? cmap.size() : e;
        size_t p = b + 12;
        // Each entry: <lo> <hi> <dst>  or  <lo> <hi> [<d1> <d2> ...]
        while (true) {
            const size_t a = cmap.find('<', p);
            if (a == std::string::npos || a >= end) break;
            const size_t a2 = cmap.find('>', a);
            const size_t c = cmap.find('<', a2);
            const size_t c2 = c == std::string::npos ? c : cmap.find('>', c);
            if (a2 == std::string::npos || c2 == std::string::npos || c2 >= end) break;
            const uint32_t lo = hex_value(cmap.substr(a + 1, a2 - a - 1)), hi = hex_value(cmap.substr(c + 1, c2 - c - 1));
            size_t q = c2 + 1;
            while (q < end && is_ws(cmap[q])) ++q;
            if (q < end && cmap[q] == '[') {
                const size_t close = cmap.find(']', q);
                const auto dsts = hex_tokens(cmap, q, close == std::string::npos ? end : close);
                for (uint32_t code = lo; code <= hi && code - lo < dsts.size(); ++code) font.map[code] = utf16_hex_to_utf8(dsts[code - lo]);
                p = close == std::string::npos ? end : close + 1;
            } else {
                const size_t d = cmap.find('<', q), d2 = d == std::string::npos ? d : cmap.find('>', d);
                if (d2 == std::string::npos || d2 > end) break;
                const std::string dst = cmap.substr(d + 1, d2 - d - 1);
                uint32_t start = hex_value(dst.size() >= 4 ? dst.substr(dst.size() - 4) : dst);
                const std::string prefix = dst.size() > 4 ? dst.substr(0, dst.size() - 4) : "";
                for (uint32_t code = lo; code <= hi && code - lo < 0x10000; ++code) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "%04X", start + (code - lo));
                    font.map[code] = utf16_hex_to_utf8(prefix + buf);
                }
                p = d2 + 1;
            }
        }
    }
}

// Font resource name -> font, from every font dictionary in the file. EO.Pdf keeps names unique
// per file (F5, F6, F25...), and later pages hold their fonts on the page while the first page
// draws through a form XObject, so collecting them all avoids walking the page tree.
std::map<std::string, Font> collect_fonts(const std::map<int, Object>& objs) {
    std::map<std::string, Font> fonts;
    for (const auto& [num, o] : objs) {
        std::string fdict = balanced_dict(o.dict, "/Font");
        if (fdict.empty()) {
            const int indirect = ref_after(o.dict, "/Font");
            auto it = indirect >= 0 ? objs.find(indirect) : objs.end();
            if (it != objs.end()) fdict = it->second.dict;
        }
        for (size_t p = fdict.find('/'); p != std::string::npos; p = fdict.find('/', p + 1)) {
            size_t e = p + 1;
            while (e < fdict.size() && !is_delim(fdict[e])) ++e;
            const std::string name = fdict.substr(p + 1, e - p - 1);
            size_t i = e;
            while (i < fdict.size() && is_ws(fdict[i])) ++i;
            size_t j = i;
            while (j < fdict.size() && is_digit(fdict[j])) ++j;
            if (j == i || fonts.count(name)) continue;
            const auto font_obj = objs.find(std::atoi(fdict.substr(i, j - i).c_str()));
            if (font_obj == objs.end()) continue;
            Font f;
            f.two_byte = font_obj->second.dict.find("/Type0") != std::string::npos;
            const int tu = ref_after(font_obj->second.dict, "/ToUnicode");
            auto tu_obj = tu >= 0 ? objs.find(tu) : objs.end();
            if (tu_obj != objs.end()) parse_cmap(decoded(tu_obj->second), f);
            fonts[name] = std::move(f);
        }
    }
    return fonts;
}

std::string glyphs(const Font* font, const std::string& bytes) {
    std::string out;
    const bool two = font && font->two_byte;
    for (size_t i = 0; i < bytes.size(); i += two ? 2 : 1) {
        uint32_t code = static_cast<unsigned char>(bytes[i]);
        if (two) code = code << 8 | (i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0);
        const std::string* mapped = nullptr;
        if (font) {
            const auto it = font->map.find(code);
            if (it != font->map.end() && !it->second.empty()) mapped = &it->second;
        }
        if (mapped) {
            out += *mapped;
        } else if (code >= 0x283 && code <= 0x29C) {
            // EO.Pdf's Georgia small capitals map to U+0000; their glyph ids run a..z in order.
            out.push_back(static_cast<char>('A' + (code - 0x283)));
        } else if (!two && code >= 0x20 && code < 0x7F) {
            out.push_back(static_cast<char>(code));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::string hex_bytes(const std::string& hex) {
    std::string clean;
    for (char c : hex) {
        if (std::isxdigit(static_cast<unsigned char>(c))) clean.push_back(c);
    }
    if (clean.size() % 2) clean.push_back('0');
    std::string out;
    for (size_t i = 0; i < clean.size(); i += 2) out.push_back(static_cast<char>(hex_value(clean.substr(i, 2))));
    return out;
}

// Literal "( ... )" string starting at s[i] == '('; i ends after the closing parenthesis.
std::string literal_string(const std::string& s, size_t& i) {
    std::string out;
    int depth = 0;
    for (++i; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            switch (n) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                default:
                    if (n >= '0' && n <= '7') {
                        int v = n - '0';
                        for (int k = 0; k < 2 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; ++k) v = v * 8 + (s[++i] - '0');
                        out.push_back(static_cast<char>(v));
                    } else if (n != '\r' && n != '\n') {
                        out.push_back(n);
                    }
            }
        } else if (c == '(') {
            ++depth;
            out.push_back(c);
        } else if (c == ')') {
            if (depth-- == 0) {
                ++i;
                return out;
            }
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void extract_text(const std::string& content, const std::map<std::string, Font>& fonts, std::vector<PdfText>& out) {
    struct Operand {
        enum { Number, Name, String, Array } kind = Number;
        double number = 0;
        std::string text;                  // name, or the string's raw bytes
        std::vector<std::string> strings;  // Array: the strings inside, in order
    };
    std::vector<Operand> ops;
    const Font* font = nullptr;
    double x = 0, y = 0;
    PdfText cur;
    auto flush = [&] {
        bool visible = false;
        for (char c : cur.text) visible = visible || !is_ws(c);
        if (visible) out.push_back(cur);
        cur.text.clear();
        cur.x = x;
        cur.y = y;
    };
    auto num = [&](size_t from_end) { return ops.size() >= from_end ? ops[ops.size() - from_end].number : 0.0; };

    size_t i = 0;
    while (i < content.size()) {
        const char c = content[i];
        if (is_ws(c)) {
            ++i;
        } else if (c == '%') {
            while (i < content.size() && content[i] != '\n' && content[i] != '\r') ++i;
        } else if (c == '/') {
            size_t e = i + 1;
            while (e < content.size() && !is_delim(content[e])) ++e;
            ops.push_back({Operand::Name, 0, content.substr(i + 1, e - i - 1), {}});
            i = e;
        } else if (c == '<' && i + 1 < content.size() && content[i + 1] == '<') {
            i += 2;  // inline dictionary (marked content): its contents become ordinary operands
        } else if (c == '>' && i + 1 < content.size() && content[i + 1] == '>') {
            i += 2;
        } else if (c == '<') {
            const size_t e = content.find('>', i);
            const size_t end = e == std::string::npos ? content.size() : e;
            ops.push_back({Operand::String, 0, hex_bytes(content.substr(i + 1, end - i - 1)), {}});
            i = end + 1;
        } else if (c == '(') {
            ops.push_back({Operand::String, 0, literal_string(content, i), {}});
        } else if (c == '[') {
            Operand arr;
            arr.kind = Operand::Array;
            ++i;
            while (i < content.size() && content[i] != ']') {
                if (content[i] == '<') {
                    const size_t e = content.find('>', i);
                    const size_t end = e == std::string::npos ? content.size() : e;
                    arr.strings.push_back(hex_bytes(content.substr(i + 1, end - i - 1)));
                    i = end + 1;
                } else if (content[i] == '(') {
                    arr.strings.push_back(literal_string(content, i));
                } else {
                    ++i;
                }
            }
            ++i;
            ops.push_back(std::move(arr));
        } else if (c == ']' || c == '{' || c == '}' || c == ')' || c == '>') {
            ++i;
        } else if (is_digit(c) || c == '-' || c == '+' || c == '.') {
            size_t e = i + 1;
            while (e < content.size() && (is_digit(content[e]) || content[e] == '.')) ++e;
            ops.push_back({Operand::Number, std::atof(content.substr(i, e - i).c_str()), {}, {}});
            i = e;
        } else {
            size_t e = i + 1;
            while (e < content.size() && !is_delim(content[e])) ++e;
            const std::string op = content.substr(i, e - i);
            i = e;
            if (op == "BI") {  // inline image: skip its binary data up to "EI"
                const size_t ei = content.find("EI", i);
                i = ei == std::string::npos ? content.size() : ei + 2;
            } else if (op == "Tf") {
                for (size_t k = ops.size(); k-- > 0;) {
                    if (ops[k].kind == Operand::Name) {
                        auto it = fonts.find(ops[k].text);
                        font = it == fonts.end() ? nullptr : &it->second;
                        break;
                    }
                }
            } else if (op == "Tm" && ops.size() >= 6) {
                x = num(2);
                y = num(1);
                flush();
            } else if (op == "Td" || op == "TD") {
                const double tx = num(2), ty = num(1);
                x += tx;
                y += ty;
                if (ty != 0) flush();  // a new line; a pure horizontal move stays in the same run
            } else if (op == "T*" || op == "BT" || op == "ET") {
                flush();
            } else if (op == "Tj" || op == "'" || op == "\"") {
                if (op != "Tj") flush();
                if (!ops.empty() && ops.back().kind == Operand::String) cur.text += glyphs(font, ops.back().text);
            } else if (op == "TJ") {
                if (!ops.empty() && ops.back().kind == Operand::Array) {
                    for (const auto& s : ops.back().strings) cur.text += glyphs(font, s);
                }
            }
            ops.clear();
        }
    }
    flush();
}

// A string value after `key` in a dictionary: <hex> or (literal).
std::string string_after(const std::string& text, const std::string& key) {
    for (size_t p = text.find(key); p != std::string::npos; p = text.find(key, p + 1)) {
        size_t i = p + key.size();
        if (i < text.size() && !is_ws(text[i]) && text[i] != '<' && text[i] != '(') continue;  // /O vs /OE
        while (i < text.size() && is_ws(text[i])) ++i;
        if (i >= text.size()) return {};
        if (text[i] == '<') {
            const size_t e = text.find('>', i);
            return hex_bytes(text.substr(i + 1, (e == std::string::npos ? text.size() : e) - i - 1));
        }
        if (text[i] == '(') return literal_string(text, i);
        return {};
    }
    return {};
}

long number_after(const std::string& text, const std::string& key, long fallback) {
    for (size_t p = text.find(key); p != std::string::npos; p = text.find(key, p + 1)) {
        size_t i = p + key.size();
        if (i < text.size() && !is_ws(text[i])) continue;
        while (i < text.size() && is_ws(text[i])) ++i;
        size_t j = i + (i < text.size() && (text[i] == '-' || text[i] == '+') ? 1 : 0);
        const size_t digits = j;
        while (j < text.size() && is_digit(text[j])) ++j;
        if (j == digits) return fallback;
        return std::atol(text.substr(i, j - i).c_str());
    }
    return fallback;
}

std::string le_bytes(uint32_t v, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    return out;
}

// The PDF standard security handler, revisions 2 and 3 (RC4, 40-128 bit), opened with the empty
// user password: about 15% of House trade reports are "encrypted" this way only to restrict
// permissions, and any reader opens them without asking for a password.
bool decrypt_objects(const std::string& pdf, std::map<int, Object>& objs, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const int enc = ref_after(pdf, "/Encrypt");
    const auto enc_obj = enc >= 0 ? objs.find(enc) : objs.end();
    const std::string dict = enc_obj != objs.end() ? enc_obj->second.dict : balanced_dict(pdf, "/Encrypt");
    if (dict.find("/Standard") == std::string::npos) return fail("the PDF uses an encryption this reader doesn't support");
    const long v = number_after(dict, "/V", 0), r = number_after(dict, "/R", 0);
    if ((v != 1 && v != 2) || (r != 2 && r != 3)) {
        return fail("the PDF uses an encryption this reader doesn't support (V" + std::to_string(v) + ", R" + std::to_string(r) + ")");
    }
    const size_t n = v == 1 ? 5 : static_cast<size_t>(std::max(40L, std::min(128L, number_after(dict, "/Length", 40))) / 8);
    const std::string o = string_after(dict, "/O"), u = string_after(dict, "/U");
    const auto p = static_cast<uint32_t>(static_cast<int32_t>(number_after(dict, "/P", 0)));
    std::string id0;
    const size_t id = pdf.rfind("/ID");
    if (id != std::string::npos) {
        const size_t open = pdf.find('[', id);
        size_t i = open == std::string::npos ? pdf.size() : open + 1;
        while (i < pdf.size() && is_ws(pdf[i])) ++i;
        if (i < pdf.size() && pdf[i] == '<') {
            const size_t e = pdf.find('>', i);
            id0 = hex_bytes(pdf.substr(i + 1, (e == std::string::npos ? pdf.size() : e) - i - 1));
        } else if (i < pdf.size() && pdf[i] == '(') {
            id0 = literal_string(pdf, i);
        }
    }
    if (o.size() < 32 || u.size() < 16) return fail("the PDF's encryption settings are incomplete");

    static const unsigned char kPad[32] = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
                                           0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
                                           0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};
    const std::string pad(reinterpret_cast<const char*>(kPad), sizeof kPad);
    std::string hash = md5(pad + o.substr(0, 32) + le_bytes(p, 4) + id0);
    if (r >= 3) {
        for (int i = 0; i < 50; ++i) hash = md5(hash.substr(0, n));
    }
    const std::string key = hash.substr(0, n);

    bool user_ok;
    if (r == 2) {
        user_ok = u.size() >= 32 && rc4(key, pad) == u.substr(0, 32);
    } else {
        std::string x = rc4(key, md5(pad + id0));
        for (int i = 1; i <= 19; ++i) {
            std::string k = key;
            for (char& c : k) c = static_cast<char>(c ^ i);
            x = rc4(k, x);
        }
        user_ok = x == u.substr(0, 16);
    }
    if (!user_ok) return fail("the PDF needs a password to open");

    for (auto& [num, obj] : objs) {
        if (!obj.has_stream || num == enc || obj.dict.find("/XRef") != std::string::npos) continue;
        const std::string object_key = md5(key + le_bytes(static_cast<uint32_t>(num), 3) + le_bytes(static_cast<uint32_t>(obj.generation), 2));
        obj.stream = rc4(object_key.substr(0, std::min<size_t>(n + 5, 16)), obj.stream);
    }
    return true;
}

}  // namespace

std::vector<PdfText> pdf_text_fragments(const std::string& pdf, std::string* error) {
    std::vector<PdfText> out;
    if (pdf.compare(0, 5, "%PDF-") != 0) {
        if (error) *error = "not a PDF file";
        return out;
    }
    auto objs = read_objects(pdf);
    if (objs.empty()) {
        if (error) *error = "no objects found in the PDF";
        return out;
    }
    if (pdf.find("/Encrypt") != std::string::npos && !decrypt_objects(pdf, objs, error)) return out;
    const auto fonts = collect_fonts(objs);
    for (const auto& [num, o] : objs) {
        if (!o.has_stream) continue;
        const std::string& d = o.dict;
        // Font programs, images and metadata are never page content.
        if (d.find("/Length1") != std::string::npos || d.find("/Length2") != std::string::npos ||
            d.find("/Subtype /Image") != std::string::npos || d.find("/Subtype/Image") != std::string::npos ||
            d.find("/Metadata") != std::string::npos || d.find("/ObjStm") != std::string::npos) {
            continue;
        }
        const std::string content = decoded(o);
        if (content.find("Tj") == std::string::npos && content.find("TJ") == std::string::npos) continue;
        if (content.find("begincmap") != std::string::npos) continue;
        extract_text(content, fonts, out);
    }
    return out;
}

}  // namespace strojanka
