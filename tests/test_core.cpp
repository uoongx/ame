/*
 * core 单元测试：uid 生成唯一性、RelType 传导系数、JSON 序列化往返
 */
#include <unordered_set>
#include <limits>
#include <locale>
#include <stdexcept>
#include <iomanip>

#include "ame/core/json.h"
#include "ame/core/log.h"
#include "ame/core/types.h"
#include "ame/core/uid.h"
#include "test_util.hpp"

using namespace ame;

static void test_uid_uniqueness() {
  std::unordered_set<std::string> seen;
  for (int i = 0; i < 10000; ++i) {
    std::string u = gen_uid();
    CHECK(u.size() == 9 && u[0] == 'E');
    CHECK(seen.insert(u).second);
  }
}

static void test_conduction_coeff() {
  CHECK_NEAR(conduction_coeff(RelType::REL_CAUSE), 0.9, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_INFER), 0.9, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_PART), 0.8, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_SIMILAR), 0.7, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_COOCCUR), 0.6, 1e-9);
  CHECK_NEAR(conduction_coeff(RelType::REL_OPPOSITE), 0.5, 1e-9);
}

static void test_rel_name_roundtrip() {
  for (int i = 0; i <= (int)RelType::LIFE_PHASE; ++i) {
    RelType t = (RelType)i;
    auto back = rel_from_name(rel_name(t));
    CHECK(back.has_value() && *back == t);
  }
  CHECK(!rel_from_name("NO_SUCH_REL").has_value());
  CHECK(rel_undirected(RelType::NEAR));
  CHECK(rel_undirected(RelType::REL_COOCCUR));
  CHECK(!rel_undirected(RelType::INVOLVES));
}

static void test_json_roundtrip() {
  Json j = Json::object();
  j["name"] = Json::string("老张@朝阳公园");
  j["n"] = Json::number(42);
  j["f"] = Json::number(0.5);
  j["flag"] = Json::boolean(true);
  Json arr = Json::array();
  arr.push(Json::number(1));
  arr.push(Json::string("失眠"));
  j["arr"] = std::move(arr);

  bool ok = false;
  Json back = Json::parse(j.dump(), &ok);
  CHECK(ok);
  CHECK(back.find("name")->as_str() == "老张@朝阳公园");
  CHECK(back.find("n")->as_num() == 42);
  CHECK(back.find("f")->as_num() == 0.5);
  CHECK(back.find("flag")->as_bool());
  CHECK(back.find("arr")->arr.size() == 2);
  CHECK(back.find("arr")->arr[1].as_str() == "失眠");

  bool ok2 = true;
  Json::parse("{broken json", &ok2);
  CHECK(!ok2);
}

static void test_json_numbers_and_controls() {
  for (const auto* invalid : {"1e9999", "NaN", "nan", "inf", "-inf", "12trailing"}) {
    bool ok = true;
    Json::parse(invalid, &ok);
    CHECK(!ok);
  }
  for (double value : {std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
    bool rejected = false;
    try { Json::number(value).dump(); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
  }
  struct DecimalComma : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
  };
  const auto old_locale = std::locale();
  std::locale::global(std::locale(old_locale, new DecimalComma));
  CHECK(Json::number(1234.5).dump() == "1234.5");
  bool ok = false;
  CHECK(Json::parse("1234.5", &ok).num == 1234.5);
  CHECK(ok);
  std::locale::global(old_locale);
  CHECK(Json::string(std::string("\0\x01\x1f", 3)).dump() == "\"\\u0000\\u0001\\u001f\"");
  CHECK(Json::number(1e100).dump() == "1e+100");
}

static void test_log_message() {
  CHECK(log_message("count=", 42, ", text=", std::string("100% %s %n"))
        == "count=42, text=100% %s %n");
  CHECK(log_message("progress=", std::fixed, std::setprecision(1), 12.34, "%")
        == "progress=12.3%");
  const std::string long_text(4096, 'x');
  CHECK(log_message(long_text) == long_text);
  CHECK(log_message("", 0, '/', static_cast<size_t>(10)) == "0/10");
}

static void test_json_escape_compatibility() {
  bool ok = false;
  const auto decoded = Json::parse(
      R"("\n\t\r\"\\\/\u0000\u007f\u0080\u07FF\u0800\u4e2d")", &ok);
  CHECK(ok);
  std::string expected = "\n\t\r\"\\/";
  expected.push_back('\0');
  expected += "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xe4\xb8\xad";
  CHECK(decoded.str == expected);
  // This refactor preserves the legacy permissive decoder, including unknown
  // escapes and non-hex digits. Strict JSON validation is a separate contract.
  CHECK(Json::parse(R"("\q\u00g1")", &ok).str == std::string("q\x01", 2));
  CHECK(ok);
  for (const auto* invalid : {R"("\u12)", R"("unfinished)", R"("trailing\)"}) {
    CHECK(Json::parse(invalid, &ok).is_null());
    CHECK(!ok);
  }
}

static std::string nested_json(size_t depth, bool mixed) {
  std::string prefix, suffix;
  for (size_t i = 0; i < depth; ++i) {
    const bool object = mixed && i % 2 == 0;
    prefix += object ? "{\"v\":" : "[";
    suffix += object ? "}" : "]";
  }
  return prefix + "0" + std::string(suffix.rbegin(), suffix.rend());
}

static void test_json_depth_limit() {
  bool ok = false;
  for (bool mixed : {false, true}) {
    const auto text = nested_json(Json::kMaxNestingDepth, mixed);
    auto value = Json::parse(text, &ok);
    CHECK(ok);
    CHECK(value.dump() == text);
    const auto too_deep = nested_json(Json::kMaxNestingDepth + 1, mixed);
    CHECK(Json::parse(too_deep, &ok).is_null());
    CHECK(!ok);
    Json outer = Json::object();
    outer["v"] = std::move(value);
    bool rejected = false;
    try { outer.dump(); } catch (const std::length_error&) { rejected = true; }
    CHECK(rejected);
  }
  // Empty containers must consume depth too; no scalar leaf is required.
  const auto limit = Json::kMaxNestingDepth;
  const std::string empty_arrays = std::string(limit, '[') + std::string(limit, ']');
  CHECK(Json::parse(empty_arrays, &ok).dump() == empty_arrays);
  CHECK(ok);
  CHECK(Json::parse("[" + empty_arrays + "]", &ok).is_null());
  CHECK(!ok);
  // Rejection happens before recursively traversing the entire input.
  CHECK(Json::parse(nested_json(10000, true), &ok).is_null());
  CHECK(!ok);
}

int main() {
  test_uid_uniqueness();
  test_conduction_coeff();
  test_rel_name_roundtrip();
  test_json_roundtrip();
  test_json_numbers_and_controls();
  test_json_escape_compatibility();
  test_json_depth_limit();
  test_log_message();
  return AME_TEST_REPORT();
}
