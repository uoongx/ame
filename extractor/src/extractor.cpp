/*
 * =============================================================================
 * 模块定位：M1 要素提取器 —— RuleExtractor 的规则实现，自由文本 → 结构化记忆要素。
 *
 * 本文件主体思路：
 *   在不引入 LLM 的前提下，用纯启发式规则从一段中文文本中提取五类要素：
 *   地点（地名词典最长优先匹配）、实体（"老X/小X"称呼模式 + 物品词典）、
 *   关键词（分类词典匹配并去重）、事件类型（取第一个 action 类关键词）、
 *   事件时间（相对时间词换算为时间戳）。词典可通过 add_place/add_keyword
 *   在运行时扩展。
 *
 * 关键算法/数据结构：
 *   无复杂算法。核心是子串匹配（std::string::find）配合"按词条长度降序排序
 *   实现最长优先匹配"；中文按 UTF-8 编码处理，每个汉字占 3 字节，称呼模式
 *   截取前缀后 3 字节作为一个汉字人名。词典用 vector 顺序表存储。
 *
 * 依赖关系：
 *   依赖本模块头文件 ame/extractor/extractor.h（接口与数据结构）、
 *   M10 时间服务 ame/time/time_service.h（TimeService::now_utc 取当前 UTC
 *   时间作为相对时间词换算的基准）。
 * =============================================================================
 */
#include "ame/extractor/extractor.h"

#include "ame/time/time_service.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_set>

namespace ame {

// 构造函数：初始化内置地名词典与关键词词典。
// 伪代码：
//   步骤1：写入内置地名词典（北京一带样例坐标，词条 = 名称 + 经纬度）。
//   步骤2：写入内置关键词词典 word -> category
//          （category 取值 action|attr|topic|sentiment|concept）。
RuleExtractor::RuleExtractor() {
  // 内置地名词典（北京一带样例坐标）
  places_ = {
      {"朝阳公园", 39.9449, 116.4781}, {"三里屯", 39.9355, 116.4542},
      {"望京", 39.9965, 116.4730},     {"中关村", 39.9833, 116.3106},
      {"咖啡馆", 39.9455, 116.4790},   {"公司", 39.9087, 116.3975},
      {"健身房", 39.9440, 116.4770},   {"医院", 39.9370, 116.4600},
  };
  // 内置关键词词典 word -> category（action|attr|topic|sentiment|concept）
  kw_dict_ = {
      {"爆胎", "action"},   {"维修", "action"}, {"保养", "action"},
      {"聚餐", "action"},   {"谈项目", "action"}, {"加班", "action"},
      {"开会", "action"},   {"跑步", "action"}, {"体检", "action"},
      {"红色", "attr"},     {"二手", "attr"},   {"坏了", "attr"},
      {"工作", "topic"},    {"健康", "topic"},  {"理财", "topic"},
      {"开心", "sentiment"}, {"踩坑", "sentiment"}, {"值得", "sentiment"},
      {"失眠", "concept"},  {"睡不着", "concept"}, {"压力", "concept"}, {"焦虑", "concept"},
      {"拖延", "concept"},  {"性价比", "concept"}, {"咖啡", "concept"},
      {"感冒", "concept"},
  };
}

// 向地名词典追加一条词条（名称 + 经纬度），供 demo/测试扩展。
void RuleExtractor::add_place(const std::string& name, double lat, double lng) {
  places_.push_back({name, lat, lng});
}

// 向关键词词典追加一条词条（词 + 类别），供 demo/测试扩展。
void RuleExtractor::add_keyword(const std::string& word, const std::string& category) {
  kw_dict_.emplace_back(word, category);
}

namespace {
bool has_entity(const Extraction& ex, const std::string& surface) {
  return std::any_of(ex.entities.begin(), ex.entities.end(),
                     [&](const ExtractedEntity& entity) { return entity.surface == surface; });
}
bool entity_covers(const Extraction& ex, const std::string& word) {
  return std::any_of(ex.entities.begin(), ex.entities.end(), [&](const ExtractedEntity& entity) {
    return entity.surface.find(word) != std::string::npos;
  });
}
bool has_keyword(const Extraction& ex, const std::string& word) {
  return std::any_of(ex.keywords.begin(), ex.keywords.end(),
                     [&](const ExtractedKeyword& keyword) { return keyword.word == word; });
}
void add_entity(Extraction& ex, const std::string& name, const std::string& role,
                 const std::string& type) {
  if (!has_entity(ex, name)) ex.entities.push_back({name, role, type, {}});
}
void extract_honorific(const std::string& text, const std::string& prefix, Extraction& ex) {
  size_t pos = 0;
  while ((pos = text.find(prefix, pos)) != std::string::npos) {
    size_t start = pos + prefix.size();
    if (start + 3 <= text.size())
      add_entity(ex, prefix + text.substr(start, 3), "人物", "person");
    pos = start;
  }
}
void extract_objects(const std::string& text, Extraction& ex) {
  static const char* objects[] = {"自行车", "汽车", "车", "手机", "电脑"};
  for (const char* word : objects)
    if (text.find(word) != std::string::npos) add_entity(ex, word, "客体", "object");
}
void extract_dictionary_keywords(const std::string& text,
                                  std::vector<std::pair<std::string, std::string>> dict,
                                  Extraction& ex) {
  std::sort(dict.begin(), dict.end(),
            [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  for (const auto& [word, category] : dict) {
    if (text.find(word) == std::string::npos || entity_covers(ex, word)) continue;
    // Keep dictionary order and duplicate behavior; AC/English stages deduplicate later.
    ex.keywords.push_back({word, category});
  }
}
std::string lower_ascii(const std::string& text) {
  std::string lower = text;
  for (char& ch : lower)
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
  return lower;
}
void extract_ac_keywords(const RuleExtractor& extractor, const std::string& text,
                          Extraction& ex) {
  for (const auto& [pos, word, category] : extractor.ac_match(lower_ascii(text))) {
    (void)pos;
    if (entity_covers(ex, word) || has_keyword(ex, word)) continue;
    ex.keywords.push_back({word, category});
  }
}
std::string lower_token(const std::string& text) {
  std::string lower;
  for (char ch : text) lower += (char)std::tolower((unsigned char)ch);
  return lower;
}
bool has_lower_entity(const Extraction& ex, const std::string& lower) {
  return std::any_of(ex.entities.begin(), ex.entities.end(), [&](const ExtractedEntity& entity) {
    return lower_token(entity.surface) == lower;
  });
}
bool english_content_word(const std::string& lower) {
  static const std::unordered_set<std::string> stop = {
      "this", "that", "with", "have", "from", "they", "them", "what", "when",
      "where", "which", "there", "their", "would", "could", "should", "about",
      "just", "like", "know", "really", "yeah", "okay", "well", "then", "than",
      "some", "into", "over", "such", "only", "also", "very", "much", "been"};
  return lower.size() >= 4 && !stop.count(lower);
}
struct EnglishTokenState {
  bool sentence_start = true;
  bool previous_capitalized = false;
  size_t previous_end = 0;
};
bool merge_proper_name(const std::string& text, size_t begin, const std::string& token,
                        bool capitalized, const EnglishTokenState& state, Extraction& ex) {
  if (!capitalized || !state.previous_capitalized || begin <= state.previous_end) return false;
  if (begin - state.previous_end > 2 || ex.entities.empty()) return false;
  std::string joiner = text.substr(state.previous_end, begin - state.previous_end);
  if (joiner.find_first_not_of(' ') != std::string::npos) return false;
  ex.entities.back().surface += joiner + token;
  return true;
}
void classify_english_token(const std::string& token, const std::string& lower,
                             bool capitalized, bool sentence_start, Extraction& ex) {
  if (capitalized && token.size() >= 2 && !sentence_start && lower != "i") {
    add_entity(ex, token, "人物", "person");
    return;
  }
  if (!english_content_word(lower) || has_lower_entity(ex, lower) || has_keyword(ex, lower)) return;
  ex.keywords.push_back({lower, "attr"});
}
size_t english_token_end(const std::string& text, size_t begin) {
  size_t end = begin;
  while (end < text.size() && (unsigned char)text[end] < 128 &&
         std::isalnum((unsigned char)text[end])) ++end;
  return end;
}
size_t extract_english_token(const std::string& text, size_t begin,
                              EnglishTokenState& state, Extraction& ex) {
  size_t end = english_token_end(text, begin);
  std::string token = text.substr(begin, end - begin);
  std::string lower = lower_token(token);
  bool capitalized = std::isupper((unsigned char)token[0]) != 0;
  if (!merge_proper_name(text, begin, token, capitalized, state, ex))
    classify_english_token(token, lower, capitalized, state.sentence_start, ex);
  state.previous_capitalized = capitalized;
  state.previous_end = end;
  state.sentence_start = false;
  return end;
}
void extract_english(const std::string& text, Extraction& ex) {
  EnglishTokenState state;
  size_t offset = 0;
  while (offset < text.size()) {
    unsigned char ch = (unsigned char)text[offset];
    if (ch < 128 && std::isalpha(ch)) {
      offset = extract_english_token(text, offset, state, ex);
      continue;
    }
    if (ch == '.' || ch == '!' || ch == '?') state.sentence_start = true;
    ++offset;
  }
}
void extract_event_metadata(const std::string& text, Extraction& ex) {
  for (const auto& keyword : ex.keywords) {
    if (keyword.category != "action") continue;
    ex.event.type = keyword.word;
    break;
  }
  struct RelativeTime { const char* word; int days; Precision precision; };
  static const RelativeTime rules[] = {
      {"前天", 2, Precision::Day}, {"昨天", 1, Precision::Day},
      {"今天", 0, Precision::Day}, {"上个月", 30, Precision::Month},
      {"去年", 365, Precision::Year}};
  ex.event.time = TimeService::now_utc();
  ex.event.precision = Precision::Day;
  for (const auto& rule : rules) {
    if (text.find(rule.word) == std::string::npos) continue;
    ex.event.time -= (TimePoint)rule.days * 86400;
    ex.event.precision = rule.precision;
    break;
  }
}
}  // namespace

Extraction RuleExtractor::extract(const std::string& text) {
  Extraction ex;
  ex.event.text = text;
  std::sort(places_.begin(), places_.end(),
            [](const auto& a, const auto& b) { return a.name.size() > b.name.size(); });
  for (const auto& place : places_) {
    if (text.find(place.name) == std::string::npos) continue;
    ex.place = {place.name, place.lat, place.lng, true};
    break;
  }
  // Stage order is part of the matching contract: entities cover dictionary terms,
  // then AC terms deduplicate against the dictionary, before English token handling.
  extract_honorific(text, "老", ex);
  extract_honorific(text, "小", ex);
  extract_objects(text, ex);
  extract_dictionary_keywords(text, kw_dict_, ex);
  if (ac_built_) extract_ac_keywords(*this, text, ex);
  extract_english(text, ex);
  extract_event_metadata(text, ex);
  return ex;
}

}  // namespace ame
