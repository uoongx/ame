/*
 * M1 extractor 单元测试：规则提取的实体/地点/关键词/时间词解析
 */
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <limits>
#include <tuple>
#include <filesystem>
#include <unordered_set>

#include "ame/extractor/dat_trie.h"
#include "ame/extractor/extractor.h"
#include "ame/time/time_service.h"
#include "test_util.hpp"
#include "test_temp_dir.hpp"

using namespace ame;

static void test_extract_basic() {
  RuleExtractor ext;
  Extraction ex = ext.extract("老张昨天在朝阳公园跑步，压力很大");

  // 实体：称呼模式命中
  bool found_lz = false;
  for (auto& e : ex.entities)
    if (e.surface == "老张" && e.entity_type == "person") found_lz = true;
  CHECK(found_lz);

  // 地点：词典命中带坐标
  CHECK(ex.place.name == "朝阳公园");
  CHECK(ex.place.has_coord);

  // 关键词：action + concept
  bool has_run = false, has_press = false;
  for (auto& k : ex.keywords) {
    if (k.word == "跑步" && k.category == "action") has_run = true;
    if (k.word == "压力" && k.category == "concept") has_press = true;
  }
  CHECK(has_run && has_press);

  // 事件类型取首个 action 关键词
  CHECK(ex.event.type == "跑步");

  // 时间词：昨天 → now-86400，精度 Day
  TimePoint now = TimeService::now_utc();
  CHECK(std::llabs((long long)(ex.event.time - (now - 86400))) < 60);
  CHECK(ex.event.precision == Precision::Day);
}

static void test_extract_time_words() {
  RuleExtractor ext;
  TimePoint now = TimeService::now_utc();
  CHECK(std::llabs((long long)(ext.extract("老张前天聚餐").event.time - (now - 2 * 86400))) < 60);
  CHECK(ext.extract("老张去年体检").event.precision == Precision::Year);
  CHECK(ext.extract("老张上个月维修").event.precision == Precision::Month);
}

static void test_extract_multi_entity_and_object() {
  RuleExtractor ext;
  Extraction ex = ext.extract("老张和老王在公司谈项目");
  CHECK(ex.entities.size() == 2);  // 老张、老王
  Extraction ex2 = ext.extract("老张的车在望京爆胎了");
  bool has_car = false;
  for (auto& e : ex2.entities)
    if (e.surface == "车" && e.entity_type == "object") has_car = true;
  CHECK(has_car);
  CHECK(ex2.place.name == "望京");
}

static void test_extract_multiword_proper() {
  // 连续大写词合并为复合专名（"New York" 不拆两个实体）
  RuleExtractor ext;
  Extraction ex = ext.extract("I flew to New York yesterday");
  bool has_ny = false, has_new = false, has_york = false;
  for (auto& e : ex.entities) {
    if (e.surface == "New York") has_ny = true;
    if (e.surface == "New") has_new = true;
    if (e.surface == "York") has_york = true;
  }
  CHECK(has_ny && !has_new && !has_york);
}

static void test_load_keyword_dict() {
  // 运行时加载中文领域词典（小文件模拟 domain/segmentation 格式）
  const std::string path = ame_test::temp_path("domain.txt");
  ame_test::write_text_file(path, "北京烤鸭\t9999\tfood\n糖葫芦\t8888\tfood\n不起眼的词\t1\tfood\n");
  RuleExtractor ext;
  CHECK(ext.load_keyword_dict(path, "food", 100) == 3);
  Extraction ex = ext.extract("老张中午吃了北京烤鸭");
  bool hit = false;
  for (auto& k : ex.keywords)
    if (k.word == "北京烤鸭" && k.category == "food") hit = true;
  CHECK(hit);
  // cap 截断
  RuleExtractor ext2;
  CHECK(ext2.load_keyword_dict(path, "food", 1) == 1);
  // 文件不存在
  RuleExtractor ext3;
  CHECK(ext3.load_keyword_dict(ame_test::temp_path("nonexistent.txt"), "x", 10) == -1);
}

static void test_ac_automaton() {
  // AC 多模式/重叠/UTF-8 边界
  const std::string path = ame_test::temp_path("ac.txt");
  ame_test::write_text_file(path, "失眠\t1\n失眠症\t2\n压力\t3\nsleep\t4\n");
  RuleExtractor ext;
  CHECK(ext.load_keyword_dict(path, "concept", 100) == 4);
  auto hits = ext.ac_match("他最近失眠症状加重，压力也大，can't sleep");
  std::unordered_set<std::string> got;
  for (auto& [pos, w, cat] : hits) got.insert(w);
  // 重叠模式全部报告（策略：不取最长，全集输出，提取层按词去重）
  CHECK(got.count("失眠") && got.count("失眠症"));
  CHECK(got.count("压力"));
  CHECK(got.count("sleep"));           // ASCII 同样走码点转移
  CHECK(!got.count("失") && !got.count("眠症"));  // 边界：不拆词
  // 位置：失眠 起始于码点 3（他=0 最=1 近=2 失=3）
  bool pos_ok = false;
  for (auto& [pos, w, cat] : hits)
    if (w == "失眠") pos_ok = (pos == 3);
  CHECK(pos_ok);
}

// AC 二进制持久化：往返命中一致 + 校验不匹配自动重建
static void test_ac_persistence() {
  const std::string path = ame_test::temp_path("ac_persistence.txt");
  ame_test::write_text_file(path, "失眠\t1\n失眠症\t2\n压力\t3\n");
  const std::string cache = ame_test::temp_path("ac_persistence.cache");
  RuleExtractor ext;
  // 首次：cache 不存在 → 构建并覆写
  CHECK(ext.load_keyword_dict_cached(path, "concept", 100, cache) == 3);
  CHECK(std::filesystem::exists(cache));
  // 第二次：直接加载（命中一致）
  RuleExtractor ext2;
  CHECK(ext2.load_keyword_dict_cached(path, "concept", 100, cache) == 3);
  auto h1 = ext.ac_match("他失眠了，失眠症严重");
  auto h2 = ext2.ac_match("他失眠了，失眠症严重");
  std::unordered_set<std::string> s1, s2;
  for (auto& [p, w, c] : h1) s1.insert(w);
  for (auto& [p, w, c] : h2) s2.insert(w);
  CHECK(s1 == s2 && !s1.empty());
  // 词典变更（mtime/size 变化）→ 校验失败自动重建
  ame_test::write_text_file(path, "焦虑\t4\n", std::ios::app);
  RuleExtractor ext3;
  CHECK(ext3.load_keyword_dict_cached(path, "concept", 100, cache) == 4);
  CHECK(!ext3.ac_match("焦虑").empty());
}

// DAT 双数组 Trie：小词典正确性 + 与 AC 对拍
static void test_dat_trie() {
  DatTrie d;
  d.build({"回家的路", "回家路上", "心理保健", "心理健康", "失眠", "失眠症"});
  auto m1 = d.match("回家路上");
  CHECK(m1.count("回家路上") && !m1.count("回家的路"));  // 兄弟槽位不合并（顺序 bug 修复）
  auto m2 = d.match("他失眠症犯了");
  CHECK(m2.count("失眠") && m2.count("失眠症"));
  // 与 AC 对拍（同词表同文本）
  const std::string path = ame_test::temp_path("dat.txt");
  ame_test::write_text_file(path, "回家的路\n回家路上\n心理保健\n心理健康\n失眠\n失眠症\n");
  RuleExtractor ext;
  CHECK(ext.load_keyword_dict(path, "x", 100) == 6);
  std::unordered_set<std::string> ac;
  for (auto& [p, w, c] : ext.ac_match("回家路上，心理健康")) ac.insert(w);
  CHECK(ac == d.match("回家路上，心理健康"));
}

static bool extracted_entity(const Extraction& ex, const std::string& name) {
  return std::any_of(ex.entities.begin(), ex.entities.end(),
                     [&](const ExtractedEntity& entity) { return entity.surface == name; });
}
static int keyword_count(const Extraction& ex, const std::string& word) {
  return (int)std::count_if(ex.keywords.begin(), ex.keywords.end(),
                            [&](const ExtractedKeyword& keyword) { return keyword.word == word; });
}
static void test_extraction_stage_priority() {
  RuleExtractor ext;
  ext.add_place("公园", 0, 0);
  ext.add_keyword("车", "attr");
  const std::string path = ame_test::temp_path("stage_words.txt");
  ame_test::write_text_file(path, "跑步\n失眠\npressure\n");
  CHECK(ext.load_keyword_dict(path, "concept", 10) == 3);
  auto ex = ext.extract("小王和老张昨天在朝阳公园骑自行车跑步，失眠 PRESSURE");
  CHECK(ex.place.name == "朝阳公园");
  CHECK(ex.entities[0].surface == "老张" && ex.entities[1].surface == "小王");
  CHECK(extracted_entity(ex, "自行车") && extracted_entity(ex, "车"));
  CHECK(keyword_count(ex, "车") == 0);
  CHECK(keyword_count(ex, "跑步") == 1 && keyword_count(ex, "失眠") == 1);
  CHECK(keyword_count(ex, "pressure") == 1);
  CHECK(ex.event.type == "跑步");
}
static void test_english_token_boundaries() {
  RuleExtractor ext;
  CHECK(extracted_entity(ext.extract("we met New  York"), "New  York"));
  auto spaced = ext.extract("we met New   York");
  CHECK(extracted_entity(spaced, "New") && extracted_entity(spaced, "York"));
  auto punctuation = ext.extract("we met New. York");
  CHECK(extracted_entity(punctuation, "New") && !extracted_entity(punctuation, "York"));
  CHECK(keyword_count(punctuation, "york") == 1);
  auto repeated = ext.extract("we met Alice and Alice");
  CHECK(repeated.entities.size() == 1 && repeated.entities[0].surface == "Alice");
  auto mixed = ext.extract("老张 and Alice enjoy coffee. Museum");
  CHECK(extracted_entity(mixed, "老张") && extracted_entity(mixed, "Alice"));
  CHECK(keyword_count(mixed, "coffee") == 1 && keyword_count(mixed, "museum") == 1);
}

struct CacheTestNode {
  int out = -1;
  int fail = 0;
  int outlink = 0;
  std::vector<std::pair<uint32_t, int>> next;
};
struct CacheTestData {
  std::vector<std::string> words{"ab", "b"};
  std::vector<int> lengths{2, 1};
  std::vector<CacheTestNode> nodes{
      {-1, 0, 0, {{'a', 1}, {'b', 3}}}, {-1, 0, 0, {{'b', 2}}},
      {0, 3, 3, {}}, {1, 0, 0, {}}};
};
template <typename T> static void cache_write(std::ostream& out, T value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
static void cache_write_string(std::ostream& out, const std::string& value) {
  cache_write(out, (uint16_t)value.size());
  out.write(value.data(), (std::streamsize)value.size());
}
static void write_cache_fixture(const std::string& path, const CacheTestData& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("Cannot open cache fixture: " + path);
  out.write("AMEAC", 5);
  cache_write(out, uint32_t{1});
  cache_write(out, uint64_t{123});
  cache_write(out, uint64_t{456});
  cache_write(out, (uint32_t)data.words.size());
  for (size_t i = 0; i < data.words.size(); ++i) {
    cache_write_string(out, data.words[i]);
    cache_write_string(out, "concept");
    cache_write(out, data.lengths[i]);
  }
  cache_write(out, (uint32_t)data.nodes.size());
  for (const auto& node : data.nodes) {
    cache_write(out, node.out);
    cache_write(out, node.fail);
    cache_write(out, node.outlink);
    cache_write(out, (uint32_t)node.next.size());
    for (const auto& edge : node.next) {
      cache_write(out, edge.first);
      cache_write(out, edge.second);
    }
  }
  out.close();
  if (!out) throw std::runtime_error("Cannot write cache fixture: " + path);
}
static void expect_cache_rejected(RuleExtractor& ext, const CacheTestData& data) {
  const std::string path = ame_test::temp_path("bad_structure.cache");
  const auto before = ext.ac_match("ab b");
  write_cache_fixture(path, data);
  CHECK(!ext.ac_load(path, 123, 456));
  CHECK(ext.ac_match("ab b") == before);
}
static void test_ac_cache_structure_validation() {
  const std::string path = ame_test::temp_path("valid_structure.cache");
  write_cache_fixture(path, CacheTestData{});
  RuleExtractor ext;
  CHECK(ext.ac_load(path, 123, 456));
  CHECK(ext.ac_match("ab").size() == 2);  // Includes suffix output through outlink.
  CacheTestData bad;
  bad.nodes[2].out = 2;
  expect_cache_rejected(ext, bad);
  bad = CacheTestData{}; bad.nodes[2].fail = 2;
  expect_cache_rejected(ext, bad);
  bad = CacheTestData{}; bad.nodes[2].outlink = 2;
  expect_cache_rejected(ext, bad);
  bad = CacheTestData{}; bad.nodes[0].next[0].second = 99;
  expect_cache_rejected(ext, bad);
  bad = CacheTestData{}; bad.nodes[1].next[0].second = 1;
  expect_cache_rejected(ext, bad);  // Trie transition cycle.
  bad = CacheTestData{}; std::reverse(bad.nodes[0].next.begin(), bad.nodes[0].next.end());
  expect_cache_rejected(ext, bad);
  bad = CacheTestData{}; bad.nodes.push_back({});
  expect_cache_rejected(ext, bad);  // Unreachable node.
  bad = CacheTestData{}; bad.lengths[0] = 10;
  expect_cache_rejected(ext, bad);
  bad = CacheTestData{}; bad.nodes[3].out = 0;
  expect_cache_rejected(ext, bad);  // Output length differs from the trie depth.
}
static void patch_cache_count(const std::string& path, std::streamoff offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) throw std::runtime_error("Cannot open cache fixture: " + path);
  file.seekp(offset);
  cache_write(file, std::numeric_limits<uint32_t>::max());
  file.close();
  if (!file) throw std::runtime_error("Cannot patch cache fixture: " + path);
}
static void test_ac_cache_truncation_and_counts() {
  const std::string path = ame_test::temp_path("bad_payload.cache");
  CacheTestData data;
  RuleExtractor ext;
  write_cache_fixture(path, data);
  CHECK(ext.ac_load(path, 123, 456));
  const auto before = ext.ac_match("ab b");
  const auto size = std::filesystem::file_size(path);
  for (uintmax_t truncated : {uintmax_t{0}, uintmax_t{29}, size - 1}) {
    write_cache_fixture(path, data);
    std::filesystem::resize_file(path, truncated);
    CHECK(!ext.ac_load(path, 123, 456));
    CHECK(ext.ac_match("ab b") == before);
  }
  // Header is 25 bytes; words consume 17+16 bytes; node table starts at 62.
  for (std::streamoff count_offset : {25, 62, 78}) {
    write_cache_fixture(path, data);
    patch_cache_count(path, count_offset);
    CHECK(!ext.ac_load(path, 123, 456));
    CHECK(ext.ac_match("ab b") == before);
  }
  write_cache_fixture(path, data);
  CHECK(!ext.ac_load(path, 124, 456));
  CHECK(ext.ac_match("ab b") == before);
}
static void test_ac_cache_rebuild_replaces_old_state() {
  const std::string dict = ame_test::temp_path("rebuild_dict.txt");
  const std::string cache = ame_test::temp_path("rebuild_dict.cache");
  ame_test::write_text_file(dict, "oldword\n");
  RuleExtractor ext;
  CHECK(ext.load_keyword_dict_cached(dict, "concept", 10, cache) == 1);
  ame_test::write_text_file(dict, "replacement\n");
  ame_test::write_text_file(cache, "AMEAC");
  CHECK(ext.load_keyword_dict_cached(dict, "concept", 10, cache) == 1);
  CHECK(ext.ac_match("oldword").empty());
  CHECK(ext.ac_match("replacement").size() == 1);
  CHECK(ext.load_keyword_dict_cached(dict, "concept", 10, cache) == 1);
  CHECK(ext.ac_match("replacement").size() == 1);
  CHECK(ext.load_keyword_dict_cached(ame_test::temp_path("missing_dict"), "concept", 10, cache) == -1);
  CHECK(ext.ac_match("replacement").size() == 1);
}

int main() {
  test_extract_basic();
  test_extract_time_words();
  test_extract_multi_entity_and_object();
  test_extract_multiword_proper();
  test_load_keyword_dict();
  test_ac_automaton();
  test_ac_persistence();
  test_dat_trie();
  test_extraction_stage_priority();
  test_english_token_boundaries();
  test_ac_cache_structure_validation();
  test_ac_cache_truncation_and_counts();
  test_ac_cache_rebuild_replaces_old_state();
  return AME_TEST_REPORT();
}
