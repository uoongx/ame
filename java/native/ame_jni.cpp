#include <jni.h>

#include "ame/core/json.h"
#include "ame/engine/engine.h"

#include <cmath>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using ame::Json;

struct JavaError : std::runtime_error {
  const char* type;
  JavaError(const char* type, const std::string& message) : std::runtime_error(message), type(type) {}
};
struct PendingJavaException final : std::exception {
  const char* what() const noexcept override { return "A Java exception is already pending"; }
};

[[noreturn]] void invalid(const char* message) {
  throw JavaError("java/lang/IllegalArgumentException", message);
}
[[noreturn]] void io_error(const char* message) {
  throw JavaError("java/io/IOException", message);
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
  else if (cp <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

bool high_surrogate(uint32_t cp) { return cp >= 0xd800 && cp <= 0xdbff; }
bool low_surrogate(uint32_t cp) { return cp >= 0xdc00 && cp <= 0xdfff; }
uint32_t combine_surrogates(uint32_t high, uint32_t low) {
  return 0x10000 + ((high - 0xd800) << 10) + (low - 0xdc00);
}

uint32_t read_utf16(const jchar* chars, jsize length, jsize& position) {
  const uint32_t cp = chars[position++];
  if (low_surrogate(cp)) invalid("String contains an unpaired UTF-16 surrogate");
  if (!high_surrogate(cp)) return cp;
  if (position == length || !low_surrogate(chars[position]))
    invalid("String contains an unpaired UTF-16 surrogate");
  return combine_surrogates(cp, chars[position++]);
}

struct InvalidUtf8 : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Utf8Prefix {
  unsigned char first, last, mask;
  size_t continuation;
  uint32_t minimum;
};

Utf8Prefix utf8_prefix(unsigned char first) {
  static constexpr Utf8Prefix prefixes[] = {
      {0xc2, 0xdf, 0x1f, 1, 0x80},
      {0xe0, 0xef, 0x0f, 2, 0x800},
      {0xf0, 0xf4, 0x07, 3, 0x10000}};
  for (const auto& prefix : prefixes)
    if (first >= prefix.first && first <= prefix.last) return prefix;
  throw InvalidUtf8("Invalid UTF-8 returned by the engine");
}

// Both snapshot input and JVM output must reject overlong sequences, surrogate
// code points and values above U+10FFFF using the same decoder.
uint32_t read_utf8(const std::string& input, size_t& position) {
  const auto first = static_cast<unsigned char>(input.at(position++));
  if (first < 0x80) return first;
  const auto prefix = utf8_prefix(first);
  if (input.size() - position < prefix.continuation)
    throw InvalidUtf8("Truncated UTF-8 returned by the engine");
  uint32_t cp = first & prefix.mask;
  for (size_t n = 0; n < prefix.continuation; ++n) {
    const auto next = static_cast<unsigned char>(input[position++]);
    if ((next & 0xc0) != 0x80) throw InvalidUtf8("Invalid UTF-8 continuation");
    cp = (cp << 6) | (next & 0x3f);
  }
  if (cp < prefix.minimum || cp > 0x10ffff || high_surrogate(cp) || low_surrogate(cp))
    throw InvalidUtf8("Invalid Unicode scalar value");
  return cp;
}

void append_utf16(std::vector<jchar>& out, uint32_t cp) {
  if (cp <= 0xffff) {
    out.push_back(static_cast<jchar>(cp));
    return;
  }
  cp -= 0x10000;
  out.push_back(static_cast<jchar>(0xd800 + (cp >> 10)));
  out.push_back(static_cast<jchar>(0xdc00 + (cp & 0x3ff)));
}

// JNI's GetStringUTFChars/NewStringUTF use modified UTF-8, which is not the UTF-8
// stored by AME. Convert Unicode scalar values explicitly, including emoji and NUL.
std::string from_java(JNIEnv* env, jstring value, bool optional = false) {
  if (!value) {
    if (optional) return {};
    invalid("String argument must not be null");
  }
  const jsize length = env->GetStringLength(value);
  const jchar* chars = env->GetStringChars(value, nullptr);
  if (!chars) throw PendingJavaException{};
  struct Release {
    JNIEnv* env;
    jstring value;
    const jchar* chars;
    ~Release() { env->ReleaseStringChars(value, chars); }
  } release{env, value, chars};
  std::string out;
  out.reserve(static_cast<size_t>(length));
  for (jsize i = 0; i < length;) append_utf8(out, read_utf16(chars, length, i));
  return out;
}

jstring to_java(JNIEnv* env, const std::string& value) {
  std::vector<jchar> out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size();) append_utf16(out, read_utf8(value, i));
  if (out.size() > static_cast<size_t>(std::numeric_limits<jsize>::max()))
    throw std::bad_alloc();
  static const jchar empty = 0;
  jstring result = env->NewString(out.empty() ? &empty : out.data(), static_cast<jsize>(out.size()));
  if (!result) throw PendingJavaException{};
  return result;
}

void throw_java(JNIEnv* env, const char* type, const char* message) noexcept {
  if (env->ExceptionCheck()) return;
  try {
    jclass cls = env->FindClass(type);
    if (!cls) return;
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(Ljava/lang/String;)V");
    if (!ctor) { env->DeleteLocalRef(cls); return; }
    jstring text = to_java(env, message);
    auto error = static_cast<jthrowable>(env->NewObject(cls, ctor, text));
    if (error) {
      env->Throw(error);
      env->DeleteLocalRef(error);
    }
    env->DeleteLocalRef(text);
    env->DeleteLocalRef(cls);
  } catch (...) {
    if (!env->ExceptionCheck()) {
      jclass cls = env->FindClass("java/lang/IllegalStateException");
      if (cls) { env->ThrowNew(cls, "Native AME operation failed"); env->DeleteLocalRef(cls); }
    }
  }
}

template <typename Result, typename Function>
Result boundary(JNIEnv* env, Result fallback, Function&& function) noexcept {
  try { return function(); }
  catch (const PendingJavaException&) {}
  catch (const JavaError& error) { throw_java(env, error.type, error.what()); }
  catch (const std::bad_alloc&) { throw_java(env, "java/lang/OutOfMemoryError", "Native AME allocation failed"); }
  catch (const fs::filesystem_error&) { throw_java(env, "java/io/IOException", "AME filesystem operation failed"); }
  catch (const std::exception& error) { throw_java(env, "java/lang/IllegalStateException", error.what()); }
  catch (...) { throw_java(env, "java/lang/IllegalStateException", "Unknown native AME failure"); }
  return fallback;
}

// The core JSON reader is deliberately permissive (unknown escapes, BMP-only
// \u handling). Snapshot I/O needs strict corruption detection and surrogate-pair
// decoding, so keep that additional validation at this external file boundary.
class SnapshotParser {
 public:
  explicit SnapshotParser(const std::string& input) : input_(input) {}
  Json parse() {
    Json result = value(0);
    whitespace();
    if (position_ != input_.size()) fail();
    return result;
  }

 private:
  const std::string& input_;
  size_t position_ = 0;
  [[noreturn]] static void fail() { io_error("Invalid JSON in AME graph.json"); }
  char peek() const { return position_ < input_.size() ? input_[position_] : '\0'; }
  bool consume(char c) {
    if (peek() != c) return false;
    ++position_;
    return true;
  }
  void whitespace() {
    static constexpr std::string_view spaces = " \t\r\n";
    while (spaces.find(peek()) != std::string_view::npos) ++position_;
  }
  bool take(char c) {
    whitespace();
    return consume(c);
  }
  static bool digit(char c) { return c >= '0' && c <= '9'; }
  uint32_t hex4() {
    if (input_.size() - position_ < 4) fail();
    uint32_t result = 0;
    const char* begin = input_.data() + position_;
    const auto parsed = std::from_chars(begin, begin + 4, result, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != begin + 4) fail();
    position_ += 4;
    return result;
  }
  uint32_t unicode_escape() {
    const uint32_t cp = hex4();
    if (low_surrogate(cp)) fail();
    if (!high_surrogate(cp)) return cp;
    if (input_.compare(position_, 2, "\\u") != 0) fail();
    position_ += 2;
    const uint32_t low = hex4();
    if (!low_surrogate(low)) fail();
    return combine_surrogates(cp, low);
  }
  void escape(std::string& result) {
    if (position_ == input_.size()) fail();
    const char c = input_[position_++];
    if (c == 'u') {
      append_utf8(result, unicode_escape());
      return;
    }
    static constexpr std::string_view encoded = "\"\\/bfnrt";
    static constexpr std::string_view decoded = "\"\\/\b\f\n\r\t";
    const auto index = encoded.find(c);
    if (index == std::string_view::npos) fail();
    result += decoded[index];
  }
  void character(std::string& result) {
    if (static_cast<unsigned char>(peek()) < 0x20) fail();
    if (consume('\\')) {
      escape(result);
      return;
    }
    try {
      append_utf8(result, read_utf8(input_, position_));
    } catch (const InvalidUtf8&) {
      fail();
    }
  }
  std::string string() {
    if (!take('"')) fail();
    std::string result;
    while (position_ < input_.size()) {
      if (consume('"')) return result;
      character(result);
    }
    fail();
  }
  void digits() {
    if (!digit(peek())) fail();
    do { ++position_; } while (digit(peek()));
  }
  void integer() {
    consume('-');
    if (!consume('0')) digits();
  }
  void exponent() {
    if (!consume('e') && !consume('E')) return;
    if (!consume('+')) consume('-');
    digits();
  }
  Json number() {
    const size_t start = position_;
    integer();
    if (consume('.')) digits();
    exponent();
    double result = 0;
    const char* end = input_.data() + position_;
    const auto parsed = std::from_chars(input_.data() + start, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(result)) fail();
    return Json::number(result);
  }
  Json object(int depth) {
    ++position_;  // value() already matched the opening delimiter.
    Json result = Json::object();
    if (take('}')) return result;
    do {
      std::string key = string();
      if (result.find(key) || !take(':')) fail();
      result[key] = value(depth + 1);
      if (take('}')) return result;
    } while (take(','));
    fail();
  }
  Json array(int depth) {
    ++position_;
    Json result = Json::array();
    if (take(']')) return result;
    do {
      result.push(value(depth + 1));
      if (take(']')) return result;
    } while (take(','));
    fail();
  }
  Json literal(std::string_view word, Json result) {
    if (input_.compare(position_, word.size(), word) != 0) fail();
    position_ += word.size();
    return result;
  }
  Json value(int depth) {
    if (depth > 128) fail();
    whitespace();
    switch (peek()) {
      case '"': return Json::string(string());
      case '{': return object(depth);
      case '[': return array(depth);
      case 't': return literal("true", Json::boolean(true));
      case 'f': return literal("false", Json::boolean(false));
      case 'n': return literal("null", Json{});
      default: return number();
    }
  }
};

// Engine::load accepts very sparse JSON. The JVM boundary must not silently turn
// an existing malformed snapshot into an empty engine and overwrite it on close.
const Json& required(const Json& object, const char* key, Json::Kind kind) {
  const Json* value = object.find(key);
  if (!value || value->kind != kind) io_error("Invalid AME snapshot structure");
  return *value;
}
void string_attrs(const Json* attrs) {
  if (!attrs) return;
  if (attrs->kind != Json::Obj) io_error("Invalid AME snapshot attributes");
  for (const auto& pair : attrs->obj)
    if (pair.second.kind != Json::Str) io_error("Invalid AME snapshot attribute value");
}
void numeric_field(const Json& object, const char* key, bool integral = false,
                   double minimum = -std::numeric_limits<double>::max(),
                   double maximum = std::numeric_limits<double>::max()) {
  const Json* value = object.find(key);
  if (!value) return;
  if (value->kind != Json::Num || !std::isfinite(value->num) ||
      value->num < minimum || value->num > maximum ||
      (integral && std::floor(value->num) != value->num))
    io_error("Invalid AME snapshot numeric field");
}
void text_field(const Json& object, const char* key) {
  const Json* value = object.find(key);
  if (value && value->kind != Json::Str) io_error("Invalid AME snapshot text field");
}
void validate_embedding(const Json* vector) {
  if (!vector) return;
  if (vector->kind != Json::Arr) io_error("Invalid AME snapshot embedding");
  for (const auto& item : vector->arr)
    if (item.kind != Json::Num || !std::isfinite(item.num) ||
        std::fabs(item.num) > std::numeric_limits<float>::max())
      io_error("Invalid AME snapshot embedding value");
}
void validate_node(const Json& node, std::unordered_set<std::string>& ids) {
  const auto& uid = required(node, "uid", Json::Str).str;
  if (uid.empty() || !ids.insert(uid).second) io_error("Invalid or duplicate AME node identifier");
  required(node, "kind", Json::Num);
  numeric_field(node, "kind", true, 0, 7);
  for (const char* field : {"name", "text", "type", "geohash"}) text_field(node, field);
  // Staying within +/-2^53 also preserves integer timestamps in AME's JSON numbers.
  numeric_field(node, "ts", true, -9007199254740991.0, 9007199254740991.0);
  numeric_field(node, "precision", true, 0, 3);
  numeric_field(node, "status", true, 0, 1);
  numeric_field(node, "mention_count", true, 0, std::numeric_limits<int>::max());
  for (const char* field : {"importance", "lat", "lng"}) numeric_field(node, field);
  validate_embedding(node.find("embedding"));
  string_attrs(node.find("attrs"));
}
void validate_edge(const Json& edge, const std::unordered_set<std::string>& ids) {
  if (!ids.count(required(edge, "from", Json::Str).str) ||
      !ids.count(required(edge, "to", Json::Str).str)) io_error("AME snapshot edge has a missing endpoint");
  if (!ame::rel_from_name(required(edge, "type", Json::Str).str)) io_error("Invalid AME snapshot relationship");
  numeric_field(edge, "weight");
  string_attrs(edge.find("attrs"));
}
void validate_slot(const Json& slot) {
  required(slot, "slot", Json::Str);
  required(slot, "value", Json::Str);
  text_field(slot, "src");
  numeric_field(slot, "valid_from", true, -9007199254740991.0, 9007199254740991.0);
  numeric_field(slot, "valid_to", true, -9007199254740991.0, 9007199254740991.0);
}
void validate_slots(const Json* slots, const std::unordered_set<std::string>& ids) {
  if (!slots) return;
  if (slots->kind != Json::Obj) io_error("Invalid AME snapshot slots");
  for (const auto& pair : slots->obj) {
    if (!ids.count(pair.first) || pair.second.kind != Json::Arr) io_error("Invalid AME slot owner");
    for (const auto& slot : pair.second.arr) validate_slot(slot);
  }
}
void validate_snapshot(const Json& root) {
  if (root.kind != Json::Obj) io_error("AME snapshot must be an object");
  const Json* graph = root.find("graph");
  if (!graph) graph = &root;  // Engine's original bare graph format remains readable.
  if (graph->kind != Json::Obj) io_error("Invalid AME snapshot graph");
  const auto& nodes = required(*graph, "nodes", Json::Arr);
  const auto& edges = required(*graph, "edges", Json::Arr);
  std::unordered_set<std::string> ids;
  for (const auto& node : nodes.arr) validate_node(node, ids);
  for (const auto& edge : edges.arr) validate_edge(edge, ids);
  string_attrs(root.find("aliases"));
  validate_slots(root.find("slots"), ids);
}

// One writable engine per directory, across JVMs as well as within this JVM.
// A persistent lock file is harmless: the OS lock is released when the process exits.
struct DirectoryLock {
  int handle = -1;
  explicit DirectoryLock(const fs::path& path) {
    handle = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (handle < 0) io_error("Unable to open AME directory lock");
    if (::flock(handle, LOCK_EX | LOCK_NB) != 0) {
      ::close(handle);
      handle = -1;
      io_error("AME directory is already open by another engine");
    }
  }
  DirectoryLock(const DirectoryLock&) = delete;
  DirectoryLock& operator=(const DirectoryLock&) = delete;
  void release() noexcept {
    if (handle >= 0) { ::flock(handle, LOCK_UN); ::close(handle); handle = -1; }
  }
  ~DirectoryLock() { release(); }
};

struct Session {
  fs::path snapshot;
  DirectoryLock directory_lock;
  std::mutex mutex;
  ame::Engine engine;
  bool closed = false;
  explicit Session(fs::path path)
      : snapshot(std::move(path)), directory_lock(snapshot.parent_path() / ".ame.lock") {}
};
std::mutex registry_mutex;
std::unordered_map<jlong, std::shared_ptr<Session>> sessions;
jlong next_handle = 1;

std::shared_ptr<Session> find_session(jlong handle) {
  std::lock_guard<std::mutex> lock(registry_mutex);
  auto found = sessions.find(handle);
  if (found == sessions.end()) throw JavaError("java/lang/IllegalStateException", "AME engine is closed or handle is invalid");
  return found->second;
}
void require_open(const Session& session) {
  if (session.closed) throw JavaError("java/lang/IllegalStateException", "AME engine is closed");
}

void load(Session& session) {
  if (!fs::exists(session.snapshot)) return;
  if (!fs::is_regular_file(session.snapshot)) io_error("AME graph.json is not a regular file");
  std::ifstream file(session.snapshot, std::ios::binary);
  if (!file) io_error("Unable to open AME graph.json");
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad()) io_error("Unable to read AME graph.json");
  Json root = SnapshotParser(content).parse();
  validate_snapshot(root);
  const Json* graph = root.find("graph");
  if (session.engine.storage().from_json(graph ? *graph : root) != ame::Err::Ok)
    io_error("Unable to load AME graph.json");
  if (const Json* slots = root.find("slots")) session.engine.entities().slots_from_json(*slots);
  if (const Json* aliases = root.find("aliases")) session.engine.keywords().aliases_from_json(*aliases);
}

void save(Session& session) {
  Json root = Json::object();
  root["graph"] = session.engine.storage().to_json();
  root["slots"] = session.engine.entities().slots_to_json();
  root["aliases"] = session.engine.keywords().aliases_to_json();
  fs::path temporary = session.snapshot;
  temporary += ".tmp";
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code ignored; fs::remove(path, ignored); }
  } cleanup{temporary};
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) io_error("Unable to create AME snapshot temporary file");
    const std::string content = root.dump();
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.flush();
    if (!file) io_error("Unable to write AME snapshot");
    file.close();
    if (!file) io_error("Unable to close AME snapshot file");
  }
  fs::rename(temporary, session.snapshot);
}
}  // namespace

extern "C" {
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_8; }

JNIEXPORT jlong JNICALL Java_io_ame_AmeEngine_nativeOpen(JNIEnv* env, jclass, jstring directory) {
  return boundary<jlong>(env, 0, [&] {
    const std::string text = from_java(env, directory);
    if (text.empty() || text.find('\0') != std::string::npos) invalid("Directory must be a nonempty path without NUL");
    fs::path path = fs::absolute(fs::u8path(text));
    fs::create_directories(path);
    auto session = std::make_shared<Session>(path / "graph.json");
    load(*session);
    std::lock_guard<std::mutex> lock(registry_mutex);
    if (next_handle == std::numeric_limits<jlong>::max()) throw std::runtime_error("AME handle space exhausted");
    const jlong handle = next_handle++;
    sessions.emplace(handle, std::move(session));
    return handle;
  });
}

JNIEXPORT jstring JNICALL Java_io_ame_AmeEngine_nativeRememberTurn(
    JNIEnv* env, jclass, jlong handle, jstring speaker, jstring text, jlong epoch_seconds,
    jstring message_id, jstring session_id) {
  return boundary<jstring>(env, nullptr, [&] {
    const std::string content = from_java(env, text);
    if (content.empty()) invalid("Text must not be empty");
    if (epoch_seconds < 0 || epoch_seconds > 9007199254740991LL) invalid("Timestamp must be a nonnegative epoch second value");
    const std::string who = from_java(env, speaker, true);
    const std::string message = from_java(env, message_id, true);
    const std::string conversation = from_java(env, session_id, true);
    auto session = find_session(handle);
    std::lock_guard<std::mutex> lock(session->mutex);
    require_open(*session);
    auto result = session->engine.remember_turn(who, content, epoch_seconds, message, conversation);
    if (result.code != ame::Err::Ok) throw std::runtime_error("AME failed to remember the event");
    Json response = Json::object();
    response["code"] = Json::number(static_cast<int>(result.code));
    response["event_id"] = Json::string(result.event_id);
    Json entities = Json::array();
    for (const auto& entity : result.resolved_entities) {
      Json item = Json::object();
      item["surface"] = Json::string(entity.first);
      item["uid"] = Json::string(entity.second);
      entities.push(std::move(item));
    }
    response["resolved_entities"] = std::move(entities);
    return to_java(env, response.dump());
  });
}

JNIEXPORT jstring JNICALL Java_io_ame_AmeEngine_nativeRecall(JNIEnv* env, jclass, jlong handle, jstring query, jint limit) {
  return boundary<jstring>(env, nullptr, [&] {
    if (limit < 1 || limit > 100) invalid("Recall limit must be between 1 and 100");
    ame::SeedQuery seeds;
    seeds.text = from_java(env, query);
    if (seeds.text.empty()) invalid("Recall query must not be empty");
    auto session = find_session(handle);
    std::lock_guard<std::mutex> lock(session->mutex);
    require_open(*session);
    auto result = session->engine.recall(seeds, {}, 2, limit, true);
    if (result.code != ame::Err::Ok && result.code != ame::Err::NotFound)
      throw std::runtime_error("AME recall failed");
    Json response = Json::array();
    for (const auto& event : result.events) {
      Json item = Json::object();
      item["event_id"] = Json::string(event.event->uid);
      item["text"] = Json::string(event.event->text);
      item["ts"] = Json::number(static_cast<double>(event.event->ts));
      item["score"] = Json::number(event.score);
      item["explain"] = Json::string(event.explain);
      response.push(std::move(item));
    }
    return to_java(env, response.dump());
  });
}

JNIEXPORT jstring JNICALL Java_io_ame_AmeEngine_nativeStats(JNIEnv* env, jclass, jlong handle) {
  return boundary<jstring>(env, nullptr, [&] {
    auto session = find_session(handle);
    std::lock_guard<std::mutex> lock(session->mutex);
    require_open(*session);
    const auto stats = session->engine.stats();
    Json response = Json::object();
    response["turns"] = Json::number(static_cast<double>(stats.turns));
    response["total_chars"] = Json::number(static_cast<double>(stats.total_chars));
    response["events"] = Json::number(static_cast<double>(stats.events));
    response["nodes"] = Json::number(static_cast<double>(stats.nodes));
    return to_java(env, response.dump());
  });
}

JNIEXPORT void JNICALL Java_io_ame_AmeEngine_nativeSave(JNIEnv* env, jclass, jlong handle) {
  boundary<int>(env, 0, [&] {
    auto session = find_session(handle);
    std::lock_guard<std::mutex> lock(session->mutex);
    require_open(*session);
    save(*session);
    return 0;
  });
}

JNIEXPORT void JNICALL Java_io_ame_AmeEngine_nativeClose(JNIEnv* env, jclass, jlong handle) {
  boundary<int>(env, 0, [&] {
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      auto found = sessions.find(handle);
      if (found == sessions.end()) return 0;  // close is idempotent, including after a failed save.
      session = std::move(found->second);
      sessions.erase(found);
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->closed = true;
    struct UnlockDirectory {
      DirectoryLock& lock;
      ~UnlockDirectory() { lock.release(); }
    } unlock{session->directory_lock};
    // The handle is already gone. Both success and exception release the engine;
    // operations that acquired a shared_ptr earlier see closed while holding the same mutex.
    save(*session);
    return 0;
  });
}

JNIEXPORT jint JNICALL Java_io_ame_AmeEngine_nativeVersion(JNIEnv*, jclass) { return 1; }
}  // extern "C"
