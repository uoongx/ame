/*
 * 所属模块：core 公共基础层——极简 JSON 的序列化与解析实现（json.h 的配对实现）。
 * 本文件主体思路：实现 Json 的 Obj 键访问（保序线性查找、不存在则创建）、
 *   dump 序列化（递归输出，字符串转义）与 parse 解析
 *   （基于字符指针的递归下降 Parser，覆盖对象/数组/字符串/数字/字面量）。
 * 关键算法/数据结构：递归下降解析；\u 转义仅处理 BMP 并按 UTF-8 编码输出；
 *   数字使用有界、locale 无关的解析；整数值（|x|<1e15）按整数格式输出。
 * 依赖关系：依赖本模块 json.h 与标准库；被需要 JSON 读写的模块（快照/配置/日志）使用。
 */
#include "ame/core/json.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ame {

// 伪代码：
//   步骤1：若自身不是 Obj，则重置为空 Obj（清空原有 obj 内容）；
//   步骤2：线性扫描保序 obj 查找 key，命中返回其值引用；
//   步骤3：未命中则追加 (key, Null) 并返回新值引用。
Json& Json::operator[](const std::string& key) {
  if (kind != Obj) { kind = Obj; obj.clear(); }
  for (auto& kv : obj)
    if (kv.first == key) return kv.second;
  obj.emplace_back(key, Json{});
  return obj.back().second;
}

// 伪代码：非 Obj 返回 nullptr；否则线性查找 key，命中返回值指针，未命中返回 nullptr。
const Json* Json::find(const std::string& key) const {
  if (kind != Obj) return nullptr;
  for (auto& kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

// 通过 operator[] 定位键位（不存在则创建）并写入值。
void Json::set(const std::string& key, Json v) { (*this)[key] = std::move(v); }

// ---------- dump ----------
// 伪代码（字符串转义输出）：
//   步骤1：输出开引号；
//   步骤2：逐字符处理——引号/反斜杠/\n/\r/\t 用转义序列，
//          其余控制字符（<0x20）输出 \u00xx，UTF-8 字节原样透传；
//   步骤3：输出闭引号。
// UTF-8 序列合法性校验：返回序列长度（1~4），非法返回 0。
static int utf8_seq_len(const std::string& s, size_t i) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80) return 1;
  int len;
  uint32_t cp;
  if ((c >> 5) == 0x6) { len = 2; cp = c & 0x1F; }
  else if ((c >> 4) == 0xE) { len = 3; cp = c & 0x0F; }
  else if ((c >> 3) == 0x1e) { len = 4; cp = c & 0x07; }
  else return 0;
  if (i + len > s.size()) return 0;
  for (int j = 1; j < len; ++j) {
    unsigned char cc = (unsigned char)s[i + j];
    if ((cc & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
      (len == 4 && cp < 0x10000)) return 0;                  // 过长编码
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0;                // 孤立代理对
  if (cp > 0x10FFFF) return 0;
  return len;
}

static void dump_esc(const std::string& s, std::string& out) {
  out += '"';
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"': out += "\\\""; ++i; continue;
      case '\\': out += "\\\\"; ++i; continue;
      case '\n': out += "\\n"; ++i; continue;
      case '\r': out += "\\r"; ++i; continue;
      case '\t': out += "\\t"; ++i; continue;
      default: break;
    }
    if (c < 0x20) {
      constexpr char hex[] = "0123456789abcdef";
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
      ++i;
      continue;
    }
    int len = utf8_seq_len(s, i);
    if (len == 0) {
      out += "\xEF\xBF\xBD";  // U+FFFD 替换非法字节（外部 API 拒绝非法 UTF-8）
      ++i;
      continue;
    }
    out.append(s, i, len);  // 合法 UTF-8 序列原样透传
    i += len;
  }
  out += '"';
}

// 伪代码（递归序列化）：
//   步骤1：按 kind 分派——Null/Bool 写字面量；Str 走 dump_esc；
//   步骤2：Num 若为整数值且 |x|<1e15 用十进制整数输出，否则保留 9 位有效数字；
//   步骤3：Arr/Obj 递归序列化子元素，逗号分隔，方/花括号闭合。
static void dump_number(double number, std::string& out) {
  if (!std::isfinite(number)) throw std::invalid_argument("JSON numbers must be finite");
  double integral;
  if (std::modf(number, &integral) == 0.0 && std::fabs(number) < 1e15) {
    out += std::to_string(static_cast<long long>(number));
    return;
  }
  std::ostringstream formatted;
  formatted.imbue(std::locale::classic());
  formatted << std::setprecision(9) << number;
  out += formatted.str();
}

static void dump_rec(const Json& j, std::string& out, size_t depth) {
  if ((j.kind == Json::Arr || j.kind == Json::Obj) && depth >= Json::kMaxNestingDepth)
    throw std::length_error("JSON nesting depth exceeds limit");
  switch (j.kind) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: dump_number(j.num, out); break;
    case Json::Str: dump_esc(j.str, out); break;
    case Json::Arr: {
      out += '[';
      for (size_t i = 0; i < j.arr.size(); ++i) {
        if (i) out += ',';
        dump_rec(j.arr[i], out, depth + 1);
      }
      out += ']';
      break;
    }
    case Json::Obj: {
      out += '{';
      for (size_t i = 0; i < j.obj.size(); ++i) {
        if (i) out += ',';
        dump_esc(j.obj[i].first, out);
        out += ':';
        dump_rec(j.obj[i].second, out, depth + 1);
      }
      out += '}';
      break;
    }
  }
}

// 序列化入口：创建空串，递归输出后返回。
std::string Json::dump() const {
  std::string out;
  dump_rec(*this, out, 0);
  return out;
}

// ---------- parse ----------
// Keep the parser's historical permissive escape handling: an unknown hex
// digit contributes zero, and unknown simple escapes keep the escaped byte.
static unsigned hex_digit(char c) {
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
  return 0;
}

static void append_bmp(unsigned cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
    return;
  }
  if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 63));
    return;
  }
  out += static_cast<char>(0xE0 | (cp >> 12));
  out += static_cast<char>(0x80 | ((cp >> 6) & 63));
  out += static_cast<char>(0x80 | (cp & 63));
}

struct Parser {
  const char* p;
  const char* end;
  bool ok = true;

  void ws() { while (p < end && std::isspace((unsigned char)*p)) ++p; }  // 跳过空白字符
  // 跳过空白后若当前字符为 c 则消费并返回 true，否则返回 false
  bool eat(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }
  // 跳过空白后返回当前字符（越界返回 '\0'）
  char peek() { ws(); return p < end ? *p : '\0'; }

  // 伪代码：
  //   步骤1：跳过空白，越界则置失败并返回 Null；
  //   步骤2：按首字符分派——'{' 解析对象、'[' 解析数组、'"' 解析字符串、
  //          't'/'f' 匹配 true/false 字面量、'n' 匹配 null；
  //   步骤3：其余情况一律按数字解析。
  Json parse_value(size_t depth = 0) {
    ws();
    if (p >= end) { ok = false; return {}; }
    char c = *p;
    if ((c == '{' || c == '[') && depth >= Json::kMaxNestingDepth) {
      ok = false;
      return {};
    }
    if (c == '{') return parse_obj(depth + 1);
    if (c == '[') return parse_arr(depth + 1);
    if (c == '"') { Json j; j.kind = Json::Str; j.str = parse_str(); return j; }
    if (c == 't') { expect("true"); Json j; j.kind = Json::Bool; j.b = true; return j; }
    if (c == 'f') { expect("false"); Json j; j.kind = Json::Bool; j.b = false; return j; }
    if (c == 'n') { expect("null"); return {}; }
    return parse_num();
  }

  // 逐字符匹配字面量 kw（true/false/null），任一字符不匹配则置 ok=false 并返回
  void expect(const char* kw) {
    while (*kw) {
      if (p >= end || *p != *kw) { ok = false; return; }
      ++p; ++kw;
    }
  }

  // Decode one BMP escape, preserving the existing permissive decoder.
  void parse_unicode_escape(std::string& out) {
    if (end - p < 4) { ok = false; return; }
    unsigned cp = 0;
    for (int i = 0; i < 4; ++i) cp = (cp << 4) | hex_digit(*p++);
    append_bmp(cp, out);
  }

  void parse_escape(std::string& out) {
    const char escaped = *p++;
    switch (escaped) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'u': parse_unicode_escape(out); break;
      default: out += escaped; break;
    }
  }

  std::string parse_str() {
    std::string s;
    if (p >= end || *p != '"') { ok = false; return s; }
    ++p;
    while (p < end) {
      char c = *p++;
      if (c == '"') return s;
      if (c == '\\' && p < end) {
        parse_escape(s);
        if (!ok) return s;
      } else {
        s += c;
      }
    }
    ok = false;
    return s;
  }

  // 有界解析，拒绝溢出、非有限值和未消费任何字符的输入。
  Json parse_num() {
    Json j; j.kind = Json::Num;
    constexpr std::string_view number_chars = "0123456789+-.eE";
    const char* token_end = p;
    while (token_end < end && number_chars.find(*token_end) != std::string_view::npos) ++token_end;
    // Copy only this token, not the remaining document. Classic-locale streams also
    // support Android's libc++, which may not provide floating-point from_chars.
    std::istringstream number(std::string(p, token_end));
    number.imbue(std::locale::classic());
    number >> std::noskipws >> j.num;
    if (!number || number.peek() != std::char_traits<char>::eof() || !std::isfinite(j.num)
        || (p < end && *p == '+')) {
      ok = false;
      j.kind = Json::Null;
    }
    p = token_end;
    return j;
  }

  // 伪代码：
  //   步骤1：消费 '['，若紧跟 ']' 返回空数组；
  //   步骤2：循环——解析一个元素追加到 arr；
  //   步骤3：遇 ']' 结束返回；否则要求 ',' 分隔符，缺失则置失败返回。
  Json parse_arr(size_t depth) {
    Json j = Json::array();
    ++p;  // '['
    if (eat(']')) return j;
    while (ok) {
      j.arr.push_back(parse_value(depth));
      if (eat(']')) return j;
      if (!eat(',')) { ok = false; return j; }
    }
    return j;
  }

  // 伪代码：
  //   步骤1：消费 '{'，若紧跟 '}' 返回空对象；
  //   步骤2：循环——要求 '"' 开始解析键字符串，再要求 ':'，解析值后按键序追加到 obj；
  //   步骤3：遇 '}' 结束返回；否则要求 ','，缺失则置失败返回。
  Json parse_obj(size_t depth) {
    Json j = Json::object();
    ++p;  // '{'
    if (eat('}')) return j;
    while (ok) {
      ws();
      if (peek() != '"') { ok = false; return j; }
      std::string key = parse_str();
      if (!eat(':')) { ok = false; return j; }
      j.obj.emplace_back(std::move(key), parse_value(depth));
      if (eat('}')) return j;
      if (!eat(',')) { ok = false; return j; }
    }
    return j;
  }
};

// 伪代码：
//   步骤1：构造 Parser 并解析一个值；
//   步骤2：跳过尾部空白，仅当全程无错误且恰好读到末尾才算成功；
//   步骤3：经 okp 传出成功标志；失败返回 Null，成功返回解析结果。
Json Json::parse(const std::string& s, bool* okp) {
  Parser ps{s.data(), s.data() + s.size(), true};
  Json j = ps.parse_value();
  ps.ws();
  bool ok = ps.ok && ps.p == ps.end;
  if (okp) *okp = ok;
  if (!ok) return {};
  return j;
}

}  // namespace ame
