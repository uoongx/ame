/*
 * llm 词典蒸馏：批量请求、宽容解析和一次空响应重试。
 * pair 可以跨 chunk；完整处理一个 word 后才写入完成检查点。
 * 中断时未完成的 word 在续跑时重新处理，客户端缓存可复用此前请求。
 */
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ame/core/json.h"
#include "ame/core/log.h"
#include "ame/llm/components.h"

namespace ame {
namespace {
using WordSet = std::unordered_set<std::string>;
using KeptPairs = std::unordered_map<std::string, WordSet>;
using PairScores = std::unordered_map<std::string, double>;
using WordScores = std::unordered_map<std::string, PairScores>;
constexpr size_t kChunkBatches = 100;
constexpr int kValidationCheckpointVersion = 2;

static std::string string_field(const Json& value, const char* key) {
  const Json* field = value.find(key);
  return field ? field->as_str() : "";
}

static bool read_json(const std::string& path, Json& value) {
  std::ifstream input(path);
  if (!input) return false;
  std::stringstream contents;
  contents << input.rdbuf();
  bool ok = false;
  const Json parsed = Json::parse(contents.str(), &ok);
  value = ok ? parsed : Json{};
  return ok;
}

// Linux rename 可原子覆盖；写失败时保留原检查点。
static bool write_json(const std::string& path, const Json& value) {
  std::ofstream output(path + ".tmp", std::ios::binary | std::ios::trunc);
  output << value.dump();
  output.close();
  if (!output) return false;
  return std::rename((path + ".tmp").c_str(), path.c_str()) == 0;
}

static Json parse_response(const std::string& response, Json::Kind kind) {
  const char open = kind == Json::Arr ? '[' : '{';
  const char close = kind == Json::Arr ? ']' : '}';
  const size_t begin = response.find(open), end = response.rfind(close);
  if (begin == std::string::npos || end == std::string::npos || end <= begin) return {};
  bool ok = false;
  Json value = Json::parse(response.substr(begin, end - begin + 1), &ok);
  return ok && value.kind == kind ? value : Json{};
}

static std::vector<std::string> chat_with_retry(
    ILlmClient& client, const std::vector<ChatRequest>& requests, int concurrency) {
  auto responses = client.chat_batch(requests, concurrency);
  responses.resize(requests.size());
  std::vector<size_t> retry_indices;
  std::vector<ChatRequest> retry_requests;
  for (size_t i = 0; i < responses.size(); ++i) {
    if (!responses[i].empty()) continue;
    retry_indices.push_back(i);
    retry_requests.push_back(requests[i]);
  }
  if (retry_requests.empty()) return responses;
  auto retried = client.chat_batch(retry_requests, concurrency);
  retried.resize(retry_indices.size());
  for (size_t i = 0; i < retry_indices.size(); ++i) responses[retry_indices[i]] = retried[i];
  return responses;
}

static ChatRequest json_request(const std::string& system, std::string user) {
  ChatOptions options;
  options.json = true;
  options.max_tokens = 4096;
  return {{{"system", system}, {"user", std::move(user)}}, options};
}

// 词条按 word 更新，避免旧文件或重复模型结果产生重复条目。
struct EntryStore {
  Json entries = Json::array();
  std::unordered_map<std::string, size_t> positions;

  bool contains(const std::string& word) const { return positions.count(word) != 0; }

  void put(const Json& entry) {
    const std::string word = string_field(entry, "word");
    const auto existing = positions.find(word);
    if (!word.empty() && existing != positions.end()) {
      entries.arr[existing->second] = entry;
      return;
    }
    if (!word.empty()) positions[word] = entries.arr.size();
    entries.push(entry);
  }
};

static EntryStore load_entries(const Json& root, const WordSet& ignore = {}) {
  EntryStore result;
  const Json* entries = root.find("entries");
  if (!entries) return result;
  for (const auto& entry : entries->arr) {
    if (ignore.count(string_field(entry, "word"))) continue;
    result.put(entry);
  }
  return result;
}

static WordSet entry_words(const Json& entries) {
  WordSet words;
  for (const auto& entry : entries.arr) words.insert(string_field(entry, "word"));
  return words;
}

static void add_vocab_frequencies(const Json* values, PairScores& frequencies) {
  if (!values) return;
  for (const auto& pair : values->arr) {
    if (pair.arr.size() < 2) continue;
    const std::string word = pair.arr[0].as_str();
    if (word.size() < 3 || word.find('\'') != std::string::npos) continue;
    frequencies[word] += pair.arr[1].as_num();
  }
}

static std::vector<std::string> top_vocab(const std::vector<std::string>& paths, int top_n) {
  PairScores frequencies;
  for (const auto& path : paths) {
    Json root;
    if (!read_json(path, root)) continue;
    add_vocab_frequencies(root.find("conv_vocab_top"), frequencies);
    add_vocab_frequencies(root.find("question_vocab_top"), frequencies);
  }
  std::vector<std::pair<double, std::string>> ranked;
  for (const auto& [word, frequency] : frequencies) ranked.emplace_back(frequency, word);
  std::sort(ranked.begin(), ranked.end(), std::greater<>());
  std::vector<std::string> words;
  for (size_t i = 0; i < ranked.size() && static_cast<int>(i) < top_n; ++i)
    words.push_back(ranked[i].second);
  return words;
}

static Json string_array(const Json* source) {
  Json result = Json::array();
  if (!source) return result;
  for (const auto& value : source->arr) result.push(Json::string(value.as_str()));
  return result;
}

static Json near_array(const Json& entry) {
  const Json* near = entry.find("syn_near");
  if (!near) near = entry.find("related");
  Json result = Json::array();
  if (!near) return result;
  for (const auto& relation : near->arr) {
    const Json* word = relation.find("word");
    if (!word) continue;
    const Json* score = relation.find("score");
    Json item = Json::object();
    item["word"] = Json::string(word->as_str());
    item["score"] = Json::number(score ? score->as_num(0.5) : 0.5);
    result.push(std::move(item));
  }
  return result;
}

static Json normalize_entry(const std::string& word, const Json& entry) {
  Json result = Json::object();
  result["word"] = Json::string(word);
  Json strong = string_array(entry.find("syn_strong"));
  if (strong.arr.empty()) strong = string_array(entry.find("synonyms"));
  result["syn_strong"] = std::move(strong);
  result["antonyms"] = string_array(entry.find("antonyms"));
  result["syn_near"] = near_array(entry);
  return result;
}

static bool write_lexicon(const std::string& path, int top_n, const EntryStore& entries) {
  Json root = Json::object();
  root["version"] = Json::string("v1");
  root["top_n"] = Json::number(top_n);
  root["entries"] = entries.entries;
  return write_json(path, root);
}

static ChatRequest build_words_request(const std::vector<std::string>& words,
                                       size_t begin, size_t end) {
  std::string user = "为下列每个英文词生成词典条目。输出 JSON 对象：词为键，值为 "
                     "{\"syn_strong\":[同义词，含词形变化如 painting→paint], "
                     "\"syn_near\":[{\"word\":近义/上下位/强关联词,\"score\":0~1}], "
                     "\"antonyms\":[反义词]}。"
                     "syn_strong 只放可互换的真同义；近义但不可互换的放 syn_near。"
                     "专有名词（人名/地名/品牌）返回空词条。只输出 JSON。\n词表：\n";
  for (size_t i = begin; i < end; ++i) user += words[i] + "\n";
  return json_request("你是词典编纂器，严格输出 JSON。", std::move(user));
}

static std::vector<ChatRequest> build_words_chunk(const std::vector<std::string>& words,
                                                size_t begin, size_t end, size_t batch) {
  std::vector<ChatRequest> requests;
  for (size_t i = begin; i < end; i += batch)
    requests.push_back(build_words_request(words, i, std::min(i + batch, end)));
  return requests;
}

static int merge_generated_entries(const std::vector<std::string>& responses,
                                   EntryStore& output, int& failed_batches) {
  int added = 0;
  for (const auto& response : responses) {
    const Json object = parse_response(response, Json::Obj);
    if (object.kind != Json::Obj) { ++failed_batches; continue; }
    for (const auto& [word, value] : object.obj) {
      if (!output.contains(word)) ++added;
      output.put(normalize_entry(word, value));
    }
  }
  return added;
}

struct PairItem { std::string word, other; };
struct PairSpan { size_t begin, end; };
struct PairWork {
  std::vector<PairItem> pairs;
  std::unordered_map<std::string, size_t> completion_end;

  void add(const std::string& word, const std::vector<std::string>& candidates) {
    if (candidates.empty()) return;
    for (const auto& candidate : candidates) pairs.push_back({word, candidate});
    completion_end[word] = pairs.size();
  }

  bool complete(const std::string& word, size_t processed_end) const {
    const auto end = completion_end.find(word);
    return end != completion_end.end() && end->second <= processed_end;
  }
};

static std::vector<std::string> strong_candidates(const Json& entry) {
  std::vector<std::string> result;
  const Json* strong = entry.find("syn_strong");
  if (!strong) return result;
  for (const auto& candidate : strong->arr) {
    const std::string word = candidate.as_str();
    if (!word.empty()) result.push_back(word);
  }
  return result;
}

static void add_unique_candidate(std::vector<std::string>& words, WordSet& seen,
                                 const std::string& word) {
  if (!word.empty() && seen.insert(word).second) words.push_back(word);
}

static std::vector<std::string> score_candidates(const Json& entry) {
  std::vector<std::string> result;
  WordSet seen;
  for (const auto& candidate : strong_candidates(entry)) add_unique_candidate(result, seen, candidate);
  const Json* near = entry.find("syn_near");
  if (!near) return result;
  for (const auto& relation : near->arr)
    add_unique_candidate(result, seen, string_field(relation, "word"));
  return result;
}

static PairWork validation_work(const Json& entries, const EntryStore& completed) {
  PairWork work;
  WordSet seen;
  for (const auto& entry : entries.arr) {
    const std::string word = string_field(entry, "word");
    if (word.empty() || completed.contains(word) || !seen.insert(word).second) continue;
    work.add(word, strong_candidates(entry));
  }
  return work;
}

static bool has_all_scores(const Json* row, const std::vector<std::string>& candidates) {
  if (!row || row->kind != Json::Obj) return false;
  for (const auto& word : candidates) {
    const Json* score = row->find(word);
    if (!score || score->kind != Json::Num) return false;
  }
  return true;
}

static PairWork scoring_work(const Json& entries, const Json& completed) {
  PairWork work;
  WordSet seen;
  for (const auto& entry : entries.arr) {
    const std::string word = string_field(entry, "word");
    if (word.empty() || !seen.insert(word).second) continue;
    auto candidates = score_candidates(entry);
    if (!has_all_scores(completed.find(word), candidates)) work.add(word, candidates);
  }
  return work;
}

enum class PairMode { Validate, Score };

static ChatRequest build_pair_request(const PairWork& work, PairSpan span, PairMode mode) {
  const bool validate = mode == PairMode::Validate;
  std::string user = validate
      ? "You are a strict synonym validator. For each pair A|B below, answer: in most "
        "sentences, can A be directly replaced by B without changing the meaning? "
        "Reply with a JSON array only: [{\"a\":A,\"b\":B,\"ok\":true/false},...] "
        "covering every pair in order.\nPairs:\n"
      : "Rate semantic similarity of each pair A|B for conversational English on a "
        "1-10 scale (10=interchangeable synonyms, 7-9=highly similar usually "
        "replaceable, 4-6=related but usually not replaceable, 1-3=weakly related). "
        "Reply with a JSON array only: [{\"a\":A,\"b\":B,\"score\":N},...] covering "
        "every pair in order.\nPairs:\n";
  for (size_t i = span.begin; i < span.end; ++i)
    user += work.pairs[i].word + "|" + work.pairs[i].other + "\n";
  return json_request(validate ? "你是严格的同义词校验器，只输出 JSON。"
                               : "你是严格的语义近似评分器，只输出 JSON。", std::move(user));
}

struct PairChunk {
  std::vector<ChatRequest> requests;
  std::vector<PairSpan> spans;
};

static PairChunk build_pair_chunk(const PairWork& work, size_t begin, size_t end,
                                  size_t batch, PairMode mode) {
  PairChunk chunk;
  for (size_t i = begin; i < end; i += batch) {
    const PairSpan span{i, std::min(i + batch, end)};
    chunk.spans.push_back(span);
    chunk.requests.push_back(build_pair_request(work, span, mode));
  }
  return chunk;
}

static WordSet kept_verdicts(const Json& array) {
  WordSet kept;
  for (const auto& verdict : array.arr) {
    const Json* a = verdict.find("a"), *b = verdict.find("b"), *ok = verdict.find("ok");
    if (!a || !b || !ok) continue;
    if (ok->as_bool()) kept.insert(a->as_str() + "|" + b->as_str());
  }
  return kept;
}

static bool merge_validation_batch(const std::string& response, const PairWork& work,
                                   PairSpan span, KeptPairs& kept) {
  const Json array = parse_response(response, Json::Arr);
  const bool parsed = array.kind == Json::Arr;
  const WordSet verdicts = kept_verdicts(array);
  for (size_t i = span.begin; i < span.end; ++i) {
    const auto& pair = work.pairs[i];
    // 无法解析时 fail-open；有效数组内缺失或 false 的判定仍按原策略剔除。
    if (!parsed || verdicts.count(pair.word + "|" + pair.other)) kept[pair.word].insert(pair.other);
  }
  return !parsed;
}

static Json validated_entry(const Json& source, const WordSet& kept) {
  Json result = Json::object();
  result["word"] = Json::string(string_field(source, "word"));
  Json strong = Json::array();
  for (const auto& candidate : strong_candidates(source))
    if (kept.count(candidate)) strong.push(Json::string(candidate));
  result["syn_strong"] = std::move(strong);
  result["antonyms"] = string_array(source.find("antonyms"));
  const Json* near = source.find("syn_near");
  result["syn_near"] = near && near->kind == Json::Arr ? *near : Json::array();
  return result;
}

static void commit_validated_words(const Json& entries, const PairWork& work, size_t end,
                                   KeptPairs& kept, EntryStore& output) {
  for (const auto& entry : entries.arr) {
    const std::string word = string_field(entry, "word");
    if (output.contains(word) || !work.complete(word, end)) continue;
    output.put(validated_entry(entry, kept[word]));
    kept.erase(word);
  }
}

static EntryStore load_validation_checkpoint(const std::string& path, const Json& entries) {
  Json root;
  read_json(path, root);
  const Json* version = root.find("checkpoint_version");
  if (version && version->as_num() == kValidationCheckpointVersion) return load_entries(root);
  // 旧格式无法区分已拒绝与未处理的 pair，当前输入词必须重验；保留无关历史词。
  return load_entries(root, entry_words(entries));
}

static bool write_validation_checkpoint(const std::string& path, const EntryStore& output) {
  Json root = Json::object();
  root["version"] = Json::string("v1");
  root["source"] = Json::string("distilled");
  root["validated"] = Json::boolean(true);
  root["checkpoint_version"] = Json::number(kValidationCheckpointVersion);
  root["entries"] = output.entries;
  return write_json(path, root);
}

static void carry_forward_entries(const Json& entries, EntryStore& output) {
  for (const auto& entry : entries.arr) {
    const std::string word = string_field(entry, "word");
    if (word.empty() || output.contains(word)) continue;
    output.put(entry);
  }
}

static PairScores parsed_scores(const Json& array) {
  PairScores scores;
  for (const auto& item : array.arr) {
    const Json* a = item.find("a"), *b = item.find("b"), *score = item.find("score");
    if (!a || !b || !score) continue;
    scores[a->as_str() + "|" + b->as_str()] = score->as_num(5.0);
  }
  return scores;
}

static double pair_score(const PairScores& scores, const PairItem& pair) {
  auto score = scores.find(pair.word + "|" + pair.other);
  if (score != scores.end()) return score->second;
  score = scores.find(pair.other);
  return score == scores.end() ? 5.0 : score->second;
}

static bool merge_score_batch(const std::string& response, const PairWork& work,
                              PairSpan span, WordScores& output) {
  const Json array = parse_response(response, Json::Arr);
  const PairScores scores = parsed_scores(array);
  for (size_t i = span.begin; i < span.end; ++i) {
    const auto& pair = work.pairs[i];
    output[pair.word][pair.other] = pair_score(scores, pair);
  }
  return array.kind != Json::Arr;
}

static Json normalized_scores(const PairScores& scores) {
  Json row = Json::object();
  for (const auto& [word, score] : scores) row[word] = Json::number(score / 10.0);
  return row;
}

static void commit_scored_words(const PairWork& work, size_t end, WordScores& scores, Json& output) {
  for (auto word = scores.begin(); word != scores.end();) {
    if (!work.complete(word->first, end)) { ++word; continue; }
    output[word->first] = normalized_scores(word->second);
    word = scores.erase(word);
  }
}

static Json load_score_checkpoint(const std::string& path) {
  Json root;
  read_json(path, root);
  const Json* scores = root.find("sims");
  return scores && scores->kind == Json::Obj ? *scores : Json::object();
}

static bool write_score_checkpoint(const std::string& path, const Json& scores) {
  Json root = Json::object();
  root["source"] = Json::string("distilled-sim");
  root["sims"] = scores;
  return write_json(path, root);
}

}  // namespace

int build_lexicon_json(ILlmClient& client, const std::vector<std::string>& vocab_paths,
                       const std::string& out_path, int top_n, int batch, int concurrency) {
  if (batch <= 0) return -1;
  Json previous;
  read_json(out_path, previous);
  EntryStore output = load_entries(previous);
  const auto all_words = top_vocab(vocab_paths, top_n);
  if (all_words.empty()) return -1;
  std::vector<std::string> words;
  for (const auto& word : all_words)
    if (!output.contains(word)) words.push_back(word);
  int added = 0, failed_batches = 0;
  const size_t chunk_size = static_cast<size_t>(batch) * kChunkBatches;
  for (size_t begin = 0; begin < words.size(); begin += chunk_size) {
    const size_t end = std::min(begin + chunk_size, words.size());
    const auto requests = build_words_chunk(words, begin, end, batch);
    added += merge_generated_entries(chat_with_retry(client, requests, concurrency), output, failed_batches);
    if (!write_lexicon(out_path, top_n, output)) return -1;
    AME_LOGI("lexicon: progress ", end, "/", words.size(), " words, failed batches ", failed_batches);
  }
  if (words.empty() && !write_lexicon(out_path, top_n, output)) return -1;
  AME_LOGI("lexicon: added ", added, " entries, total ", output.positions.size());
  return added;
}

int validate_lexicon_json(ILlmClient& client, const std::string& in_path,
                          const std::string& out_path, int batch, int concurrency) {
  if (batch <= 0) return -1;
  Json root;
  if (!read_json(in_path, root)) return -1;
  const Json* entries = root.find("entries");
  if (!entries) return -1;
  EntryStore output = load_validation_checkpoint(out_path, *entries);
  const PairWork work = validation_work(*entries, output);
  KeptPairs kept;
  int failed_batches = 0;
  const size_t chunk_size = static_cast<size_t>(batch) * kChunkBatches;
  for (size_t begin = 0; begin < work.pairs.size(); begin += chunk_size) {
    const size_t end = std::min(begin + chunk_size, work.pairs.size());
    const auto chunk = build_pair_chunk(work, begin, end, batch, PairMode::Validate);
    const auto responses = chat_with_retry(client, chunk.requests, concurrency);
    for (size_t i = 0; i < responses.size(); ++i)
      failed_batches += merge_validation_batch(responses[i], work, chunk.spans[i], kept);
    commit_validated_words(*entries, work, end, kept, output);
    if (!write_validation_checkpoint(out_path, output)) return -1;
    AME_LOGI("validate: progress ", end, "/", work.pairs.size(), " pairs, failed batches ", failed_batches);
  }
  carry_forward_entries(*entries, output);
  if (!write_validation_checkpoint(out_path, output)) return -1;
  return static_cast<int>(work.pairs.size());
}

int score_word_sims(ILlmClient& client, const std::string& in_path,
                    const std::string& out_path, int batch, int concurrency) {
  if (batch <= 0) return -1;
  Json root;
  if (!read_json(in_path, root)) return -1;
  const Json* entries = root.find("entries");
  if (!entries) return -1;
  Json output = load_score_checkpoint(out_path);
  const PairWork work = scoring_work(*entries, output);
  WordScores scores;
  int failed_batches = 0;
  const size_t chunk_size = static_cast<size_t>(batch) * kChunkBatches;
  for (size_t begin = 0; begin < work.pairs.size(); begin += chunk_size) {
    const size_t end = std::min(begin + chunk_size, work.pairs.size());
    const auto chunk = build_pair_chunk(work, begin, end, batch, PairMode::Score);
    const auto responses = chat_with_retry(client, chunk.requests, concurrency);
    for (size_t i = 0; i < responses.size(); ++i)
      failed_batches += merge_score_batch(responses[i], work, chunk.spans[i], scores);
    commit_scored_words(work, end, scores, output);
    if (!write_score_checkpoint(out_path, output)) return -1;
    AME_LOGI("score-sims: progress ", end, "/", work.pairs.size(), " pairs, failed batches ", failed_batches);
  }
  return static_cast<int>(work.pairs.size());
}

}  // namespace ame
