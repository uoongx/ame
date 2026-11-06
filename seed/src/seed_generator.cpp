/*
 * =============================================================================
 * 所属模块：M5 种子生成器（seed）
 * 定位：联想链的软入口，把任意形式的用户查询转换为图上的一组带权种子节点。
 *
 * 本文件主体思路：
 *   实现 SeedGenerator::gen_seeds。按"硬种子优先、软种子补充"的策略，把
 *   SeedQuery 中的五类线索（uid 直连、强标识符、精确关键词、地理坐标、
 *   自由文本）逐一落到图节点上，汇入同一个 SeedSet。所有候选经统一的去重
 *   合并入口 add：同一节点多次命中时保留最大权重及其来源。
 *
 * 关键算法/数据结构说明：
 *   - 线性扫描去重合并：种子规模小（硬种子若干 + ANN topK 5~10），直接
 *     O(n) 扫描 SeedSet 查重，命中时取 max 权重，无需哈希表。
 *   - 地理通道：调用 M9 的 geo_lookup 在 1km 半径内取最近 Place，权重按
 *     haversine 距离线性衰减（max(0.3, 1 - d/1000)）。
 *   - 软种子通道：M13 embed 文本向量后经 M9 ann_search 召回 topK，
 *     相似度低于 soft_min_sim（默认 0.5）的结果丢弃，权重即相似度。
 *
 * 依赖关系：
 *   依赖 core 公共层（haversine_m 距离函数、Node/NodeKind/Extraction/Vec
 *   等类型）、extractor(M1) 的文本要素提取、keyword(M3) 的关键词归一化与
 *   精确查找、storage(M9) 的节点/属性/geo/ANN 查询、vector(M13) 的文本
 *   向量嵌入。被 engine(M12) 装配调用。
 * =============================================================================
 */
#include "ame/seed/seed_generator.h"

#include "ame/extractor/extractor.h"
#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "ame/vector/vector_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace ame {

namespace {
using Words = std::unordered_set<std::string>;
using Frequencies = std::unordered_map<std::string, int>;
using Expansions = std::unordered_map<std::string, std::pair<std::string, double>>;
using Events = std::vector<const Node*>;
using TokenizedEvents = std::vector<Words>;

// Ties retain the earlier channel; only a larger weight replaces its source.
void add_seed(SeedSet& seeds, const std::string& uid, double weight,
              const std::string& source) {
  if (uid.empty()) return;
  auto existing = std::find_if(seeds.begin(), seeds.end(),
                               [&](const Seed& seed) { return seed.node_uid == uid; });
  if (existing == seeds.end()) { seeds.push_back({uid, weight, source}); return; }
  if (weight > existing->weight) {
    existing->weight = weight;
    existing->source = source;
  }
}
void add_node_seed(SeedSet& seeds, const Node* node, const std::string& source) {
  if (node) add_seed(seeds, node->uid, 1.0, source);
}
void collect_hard_seeds(const SeedQuery& q, Storage& storage, KeywordManager& kw,
                        SeedSet& seeds) {
  if (!q.uid.empty()) add_node_seed(seeds, storage.get_node(q.uid), "hard");
  for (const auto& [key, value] : q.identifiers)
    add_node_seed(seeds, storage.find_by_attr(NodeKind::Entity, "id:" + key, value), "hard");
  for (const auto& word : q.keywords)
    add_node_seed(seeds, kw.find(kw.normalize(word)), "hard");
}
void collect_geo_seed(const SeedQuery& q, Storage& storage, SeedSet& seeds) {
  if (!q.has_geo) return;
  auto places = storage.geo_lookup(q.lat, q.lng, 1000.0);
  if (places.empty()) return;
  const Node& place = *places.front();
  double distance = haversine_m(q.lat, q.lng, place.lat, place.lng);
  add_seed(seeds, place.uid, std::max(0.3, 1.0 - distance / 1000.0), "geo");
}
void collect_entity_seed(const ExtractedEntity& entity, Storage& storage,
                         KeywordManager& kw, SeedSet& seeds, Words& name_canons) {
  std::string canon = kw.alias_of(entity.surface);
  const Node* node = canon.empty() ? nullptr : storage.find_by_name(NodeKind::Entity, canon);
  if (!node) node = storage.find_by_name(NodeKind::Entity, entity.surface);
  add_node_seed(seeds, node, "text");
  if (canon.empty()) return;
  add_node_seed(seeds, kw.find(canon), "text");
  name_canons.insert(canon);
}
Words collect_text_seeds(const std::string& text, IExtractor* extractor,
                          Storage& storage, KeywordManager& kw, SeedSet& seeds) {
  Words name_canons;
  if (!extractor) return name_canons;
  Extraction extracted = extractor->extract(text);
  for (const auto& entity : extracted.entities)
    collect_entity_seed(entity, storage, kw, seeds, name_canons);
  for (const auto& keyword : extracted.keywords)
    add_node_seed(seeds, kw.find(kw.normalize(keyword.word)), "text");
  if (!extracted.place.name.empty())
    add_node_seed(seeds, storage.find_by_name(NodeKind::Place, extracted.place.name), "text");
  return name_canons;
}
void collect_soft_seeds(const SeedQuery& q, Storage& storage, VectorService& vec,
                        SeedSet& seeds) {
  Vec embedding = vec.embed(q.text);
  for (const auto& [uid, similarity] : storage.ann_search(embedding, q.soft_topk)) {
    if (similarity < q.soft_min_sim) continue;
    add_seed(seeds, uid, similarity, "soft");
  }
}
Words tokenize_ascii(const std::string& text) {
  Words words;
  std::string current;
  auto flush = [&] {
    if (current.size() >= 3) words.insert(current);
    current.clear();
  };
  for (unsigned char ch : text) {
    if (ch < 128 && std::isalnum(ch)) current += (char)std::tolower(ch);
    else flush();
  }
  flush();
  return words;
}
Words ascii_content(const std::string& text) {
  static const Words stop = {
      "the", "and", "for", "are", "was", "were", "has", "have", "had", "did",
      "does", "what", "when", "where", "which", "who", "whom", "whose", "how",
      "with", "from", "that", "this", "these", "those", "they", "them", "their",
      "would", "could", "should", "about", "into", "your", "you", "his", "her",
      "she", "him", "not", "but", "all", "any", "can", "will", "just"};
  Words content;
  for (const auto& word : tokenize_ascii(text))
    if (!stop.count(word)) content.insert(word);
  return content;
}
TokenizedEvents tokenize_events(const Events& events, const Words& content,
                                Frequencies& frequencies) {
  TokenizedEvents tokens;
  tokens.reserve(events.size());
  for (const Node* event : events) {
    tokens.push_back(tokenize_ascii(event->text));
    for (const auto& word : content)
      if (tokens.back().count(word)) ++frequencies[word];
  }
  return tokens;
}
void add_variants(const std::vector<std::string>& variants, const std::string& word,
                  const Words& content, Expansions& expansions) {
  for (const auto& variant : variants)
    if (!content.count(variant)) expansions[variant] = {word, 0.6};
}
bool has_direct_ascii_match(const std::string& word, const std::string& normalized,
                             Storage& storage, KeywordManager& kw, Frequencies& df) {
  bool direct = df[word] > 0 || df[normalized] > 0 || kw.find(normalized) != nullptr;
  std::string capitalized = word;
  capitalized[0] = (char)std::toupper((unsigned char)capitalized[0]);
  return direct || storage.find_by_name(NodeKind::Entity, capitalized) != nullptr;
}
void expand_ascii_word(const std::string& word, const Words& content, bool conditional,
                        Storage& storage, KeywordManager& kw, Frequencies& df,
                        Expansions& expansions) {
  const std::string normalized = kw.normalize(word);
  if (conditional && has_direct_ascii_match(word, normalized, storage, kw, df)) return;
  auto strong = kw.variants(normalized, 3);
  if (!conditional || !strong.empty()) {
    add_variants(strong, word, content, expansions);
    return;
  }
  for (const auto& [variant, score] : kw.near_words(normalized, 3)) {
    (void)score;
    if (!content.count(variant)) expansions[variant] = {word, 0.4};
  }
}
int token_frequency(const std::string& word, const TokenizedEvents& events) {
  return (int)std::count_if(events.begin(), events.end(),
                            [&](const Words& tokens) { return tokens.count(word) != 0; });
}
int substring_frequency(const std::string& word, const Events& events) {
  return (int)std::count_if(events.begin(), events.end(), [&](const Node* event) {
    return event->text.find(word) != std::string::npos;
  });
}
struct LexicalWeights {
  std::unordered_map<std::string, double> idf;
  double total = 0;
};
double inverse_frequency(size_t event_count, const std::string& word,
                          const Frequencies& frequencies) {
  auto found = frequencies.find(word);
  int frequency = found == frequencies.end() ? 0 : found->second;
  return std::log(1.0 + (double)event_count / (1.0 + frequency));
}
template <typename Terms>
LexicalWeights lexical_weights(const Terms& words, const Expansions& expansions,
                                 size_t event_count, const Frequencies& frequencies) {
  LexicalWeights weights;
  for (const auto& word : words) {
    weights.idf[word] = inverse_frequency(event_count, word, frequencies);
    weights.total += weights.idf[word];
  }
  for (const auto& [variant, source] : expansions) {
    (void)source;
    weights.idf[variant] = inverse_frequency(event_count, variant, frequencies);
  }
  return weights;
}
struct LexicalScore {
  int matches = 0;
  bool rare_hit = false;
  double sum = 0;
  void add(const std::string& word, double weight) {
    ++matches;
    if (word.size() >= 6) rare_hit = true;
    sum += weight;
  }
  double weight(double total) const { return 0.3 + 0.7 * (total > 0 ? sum / total : 0); }
};
template <typename Terms, typename Match>
LexicalScore score_lexical_terms(const Terms& words, const Expansions& expansions,
                                  const LexicalWeights& weights, Match matches) {
  LexicalScore score;
  for (const auto& word : words)
    if (matches(word)) score.add(word, weights.idf.at(word));
  for (const auto& [variant, source] : expansions)
    if (matches(variant)) score.add(variant, source.second * weights.idf.at(variant));
  return score;
}
constexpr int kCandidateLimit = 2000;
enum class LexicalChannel { Ascii, Cjk };

template <typename Terms>
Events lexical_candidates(Storage& storage, const Terms& terms, KeywordManager& keywords,
                          LexicalChannel channel) {
  if (!storage.pg_active()) return storage.nodes_of_kind(NodeKind::Event);
  std::vector<std::string> words(terms.begin(), terms.end());
  for (const auto& term : terms) {
    const auto normalized = keywords.normalize(term);
    const auto variants = keywords.variants(normalized, channel == LexicalChannel::Ascii ? 3 : 8);
    words.insert(words.end(), variants.begin(), variants.end());
    if (channel == LexicalChannel::Ascii) {
      for (const auto& entry : keywords.near_words(normalized, 3)) words.push_back(entry.first);
    }
  }
  return storage.lexical_event_candidates(words, kCandidateLimit);
}

void collect_ascii_lexical(const SeedQuery& q, Storage& storage, KeywordManager& kw,
                            SeedSet& seeds) {
  Words content = ascii_content(q.text);
  if (content.empty()) return;
  auto events = lexical_candidates(storage, content, kw, LexicalChannel::Ascii);
  Frequencies frequencies;
  auto tokens = tokenize_events(events, content, frequencies);
  Expansions expansions;
  for (const auto& word : content)
    expand_ascii_word(word, content, q.conditional_normalize, storage, kw, frequencies, expansions);
  for (const auto& [variant, source] : expansions) {
    (void)source;
    frequencies[variant] += token_frequency(variant, tokens);
  }
  auto weights = lexical_weights(content, expansions, events.size(), frequencies);
  std::vector<std::pair<double, std::string>> scored;
  for (size_t i = 0; i < events.size(); ++i) {
    auto score = score_lexical_terms(content, expansions, weights,
                                     [&](const std::string& word) { return tokens[i].count(word); });
    if (score.matches >= 2 || (score.matches == 1 && score.rare_hit))
      scored.emplace_back(score.weight(weights.total), events[i]->uid);
  }
  std::sort(scored.begin(), scored.end(), std::greater<>());
  for (size_t i = 0; i < scored.size() && i < (size_t)q.lex_topk; ++i)
    add_seed(seeds, scored[i].second, scored[i].first, "lex");
}
std::vector<std::string> cjk_content(const std::string& text, IExtractor* extractor,
                                     const Words& name_canons) {
  Extraction extracted = extractor ? extractor->extract(text) : Extraction{};
  std::vector<std::string> words;
  for (const auto& keyword : extracted.keywords)
    if (!keyword.word.empty() && (unsigned char)keyword.word[0] >= 128)
      words.push_back(keyword.word);
  for (const auto& canon : name_canons)
    if (std::find(words.begin(), words.end(), canon) == words.end()) words.push_back(canon);
  return words;
}
void expand_cjk_word(const std::string& word, const Events& events, bool conditional,
                      KeywordManager& kw, Frequencies& df, Expansions& expansions) {
  std::string normalized = kw.normalize(word);
  df[word] += substring_frequency(word, events);
  bool direct = df[word] > 0 || kw.find(normalized) != nullptr;
  if (conditional && direct) return;
  for (const auto& variant : kw.variants(normalized, 8)) {
    if (variant == word) continue;
    expansions[variant] = {word, 0.6};
    // Preserve per-query-word accumulation for overlapping synonym groups.
    df[variant] += substring_frequency(variant, events);
  }
}
void collect_cjk_words(const SeedQuery& q, const std::vector<std::string>& words,
                        const Events& events, KeywordManager& kw, SeedSet& seeds) {
  Frequencies frequencies;
  Expansions expansions;
  for (const auto& word : words)
    expand_cjk_word(word, events, q.conditional_normalize, kw, frequencies, expansions);
  auto weights = lexical_weights(words, expansions, events.size(), frequencies);
  for (const Node* event : events) {
    auto score = score_lexical_terms(words, expansions, weights, [&](const std::string& word) {
      return event->text.find(word) != std::string::npos;
    });
    if (score.matches >= 1 && score.sum > 0)
      add_seed(seeds, event->uid, score.weight(weights.total), "lex");
  }
}
void append_codepoint(unsigned cp, std::string& out) {
  if (cp < 0x80) { out += (char)cp; return; }
  if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
  }
  out += (char)(0x80 | (cp & 0x3F));
}
size_t codepoint_prefix(unsigned char ch, unsigned& cp) {
  if (ch < 0x80) { cp = ch; return 1; }
  if ((ch & 0xE0) == 0xC0) { cp = ch & 0x1F; return 2; }
  if ((ch & 0xF0) == 0xE0) { cp = ch & 0x0F; return 3; }
  if ((ch & 0xF8) == 0xF0) { cp = ch & 0x07; return 4; }
  return 0;
}
bool read_codepoint(const std::string& text, size_t& offset, unsigned& cp) {
  size_t length = codepoint_prefix((unsigned char)text[offset], cp);
  bool valid = length != 0 && length <= text.size() - offset;
  for (size_t i = 1; valid && i < length; ++i) {
    unsigned char ch = (unsigned char)text[offset + i];
    if ((ch & 0xC0) != 0x80) valid = false;
    else cp = (cp << 6) | (ch & 0x3F);
  }
  offset += valid ? length : 1;
  return valid;
}
std::vector<std::string> cjk_grams(const std::string& text) {
  std::vector<unsigned> codepoints;
  for (size_t offset = 0; offset < text.size();) {
    unsigned cp = 0;
    if (read_codepoint(text, offset, cp) && cp > 0x2E7F) codepoints.push_back(cp);
  }
  std::vector<std::string> grams;
  for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
    std::string gram;
    append_codepoint(codepoints[i], gram);
    append_codepoint(codepoints[i + 1], gram);
    grams.push_back(gram);
  }
  if (grams.empty() && codepoints.size() == 1) {
    std::string gram;
    append_codepoint(codepoints[0], gram);
    grams.push_back(gram);
  }
  return grams;
}
void collect_cjk_fallback(const std::string& text, Storage& storage, SeedSet& seeds) {
  auto grams = cjk_grams(text);
  if (grams.empty()) return;
  auto events = storage.pg_active() ? storage.lexical_event_candidates(grams, kCandidateLimit)
                                    : storage.nodes_of_kind(NodeKind::Event);
  Frequencies frequencies;
  for (const auto& gram : grams) frequencies[gram] += substring_frequency(gram, events);
  const Expansions no_expansions;
  auto weights = lexical_weights(grams, no_expansions, events.size(), frequencies);
  for (const Node* event : events) {
    auto score = score_lexical_terms(grams, no_expansions, weights, [&](const std::string& gram) {
      return event->text.find(gram) != std::string::npos;
    });
    if ((score.matches >= 2 || grams.size() == 1) && score.sum > 0)
      add_seed(seeds, event->uid, score.weight(weights.total), "lex");
  }
}
void collect_cjk_lexical(const SeedQuery& q, IExtractor* extractor, Storage& storage,
                          KeywordManager& kw, const Words& name_canons, SeedSet& seeds) {
  auto words = cjk_content(q.text, extractor, name_canons);
  if (words.empty()) {
    collect_cjk_fallback(q.text, storage, seeds);
    return;
  }
  auto events = lexical_candidates(storage, words, kw, LexicalChannel::Cjk);
  collect_cjk_words(q, words, events, kw, seeds);
}
}  // namespace

SeedGenerator::SeedGenerator(Storage& storage, VectorService& vec, KeywordManager& kw,
                             IExtractor* extractor)
    : storage_(storage), vec_(vec), kw_(kw), extractor_(extractor) {}

SeedSet SeedGenerator::gen_seeds(const SeedQuery& q) {
  SeedSet seeds;
  collect_hard_seeds(q, storage_, kw_, seeds);
  collect_geo_seed(q, storage_, seeds);
  if (q.text.empty()) return seeds;
  auto name_canons = collect_text_seeds(q.text, extractor_, storage_, kw_, seeds);
  collect_soft_seeds(q, storage_, vec_, seeds);
  collect_ascii_lexical(q, storage_, kw_, seeds);
  collect_cjk_lexical(q, extractor_, storage_, kw_, name_canons, seeds);
  return seeds;
}

}  // namespace ame
