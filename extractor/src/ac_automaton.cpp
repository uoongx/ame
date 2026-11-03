/*
 * =============================================================================
 * 模块定位：M1 要素提取器 —— AC 自动机（Aho-Corasick）实现。
 *
 * 本文件主体思路：
 *   为 load_keyword_dict 加载的大词表（领域词/实体词/分词合并词，可达 70 万级）
 *   提供一遍扫描的多模式匹配，替代逐词条 std::string::find 循环。
 *   转移边按 Unicode 码点（非字节）组织，避免中文 UTF-8 按字节错配。
 *
 * 关键算法/数据结构：
 *   - 节点紧凑结构：转移边为按码点排序的 vector<pair<码点,目标>>，二分查找，
 *     内存随词条数线性（实测见报告）；失败指针 BFS 构建；outlink 输出链跳读。
 *   - 重叠策略：报告全部命中（含重叠，如 失眠/失眠症 同文本时两者都输出），
 *     由提取层按词去重——与旧逐词实现的输出集合一致（对拍依据）。
 *
 * 依赖关系：依赖本模块头文件 ame/extractor/extractor.h；被 extract() 与
 *   基准工具 bench/scripts/bench_ac.cpp 使用。
 * =============================================================================
 */
#include "ame/extractor/extractor.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <new>
#include <stdexcept>
#include <sys/stat.h>

namespace ame {

// UTF-8 解码为码点序列（非法字节跳过为 U+FFFD 单点，保证单调推进）
static std::vector<uint32_t> ac_decode(const std::string& s) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 0x2) { cp = c & 0x07; len = 4; }
    else { cp = 0xFFFD; len = 1; }
    if (i + len > s.size()) { cp = 0xFFFD; len = 1; }
    for (size_t j = 1; j < len; ++j)
      cp = (cp << 6) | ((unsigned char)s[i + j] & 0x3F);
    cps.push_back(cp);
    i += len;
  }
  return cps;
}

// 二分查转移边（边按码点排序），命中返回目标节点，无则 -1
int RuleExtractor::ac_find(const AcNode& nd, uint32_t cp) {
  auto it = std::lower_bound(nd.next.begin(), nd.next.end(), cp,
                             [](const auto& p, uint32_t v) { return p.first < v; });
  return (it != nd.next.end() && it->first == cp) ? it->second : -1;
}

// ac_insert 伪代码：沿码点序列走 Trie，缺边则保序插入新节点，终结节点记词条 index
void RuleExtractor::ac_insert(uint32_t root, const uint32_t* cps, size_t n, int word_idx) {
  int cur = (int)root;
  for (size_t i = 0; i < n; ++i) {
    uint32_t cp = cps[i];
    int nxt = ac_find(ac_nodes_[cur], cp);
    if (nxt < 0) {
      nxt = (int)ac_nodes_.size();
      ac_nodes_.push_back({});
      auto& edges = ac_nodes_[cur].next;
      auto pos = std::lower_bound(edges.begin(), edges.end(), cp,
                                  [](const auto& p, uint32_t v) { return p.first < v; });
      edges.insert(pos, {cp, nxt});  // 保序插入
    }
    cur = nxt;
  }
  ac_nodes_[cur].out = word_idx;
}

// ac_build_fail 伪代码（BFS 构建失败指针与输出链）：
//   步骤1：根的子节点 fail=0 入队；
//   步骤2：逐节点出队——对每条边 (cp,v)：从当前节点 fail 链上找首个含 cp 转移的
//          祖先，v.fail=其 cp 目标（无则 0）；
//   步骤3：输出链 outlink：v 非终结时指向 v.fail 的 outlink（若 fail 终结则为 fail）。
void RuleExtractor::ac_build_fail() {
  std::deque<int> q;
  for (auto& [cp, v] : ac_nodes_[0].next) {
    ac_nodes_[v].fail = 0;
    q.push_back(v);
  }
  while (!q.empty()) {
    int u = q.front();
    q.pop_front();
    for (auto& [cp, v] : ac_nodes_[u].next) {
      q.push_back(v);
      int f = ac_nodes_[u].fail;
      while (f && ac_find(ac_nodes_[f], cp) < 0) f = ac_nodes_[f].fail;
      int t = f >= 0 ? ac_find(ac_nodes_[f], cp) : -1;
      ac_nodes_[v].fail = t > 0 ? t : 0;
      // 输出链：fail 终结 → fail；否则继承 fail 的 outlink
      int fl = ac_nodes_[v].fail;
      ac_nodes_[v].outlink =
          (ac_nodes_[fl].out >= 0) ? fl : ac_nodes_[fl].outlink;
    }
  }
}

// ac_match 伪代码（一遍扫描全部命中）：
//   步骤1：文本解码为码点序列；状态 cur=0；
//   步骤2：逐码点——沿 fail 回退到可转移状态再转移；
//   步骤3：到达状态含输出（out/outlink 链）则逐个输出 (位置, 词, 类别)；
//   重叠命中全部报告（策略：不取最长，全集输出，提取层按词去重）。
std::vector<std::tuple<size_t, std::string, std::string>> RuleExtractor::ac_match(
    const std::string& text) const {
  std::vector<std::tuple<size_t, std::string, std::string>> out;
  if (!ac_built_ || ac_nodes_.size() <= 1) return out;
  auto cps = ac_decode(text);
  int cur = 0;
  for (size_t i = 0; i < cps.size(); ++i) {
    uint32_t cp = cps[i];
    while (cur && ac_find(ac_nodes_[cur], cp) < 0) cur = ac_nodes_[cur].fail;
    int t = ac_find(ac_nodes_[cur], cp);
    cur = t >= 0 ? t : 0;
    // 输出链遍历：自身终结 + 祖先后缀终结
    for (int v = cur; v; v = ac_nodes_[v].outlink) {
      int w = ac_nodes_[v].out;
      if (w >= 0)
        out.emplace_back(i + 1 - (size_t)ac_lens_[w], ac_words_[w], ac_cats_[w]);
    }
  }
  return out;
}

// load_keyword_dict 伪代码（AC 版）：
//   步骤1：打不开文件返回 -1；
//   步骤2：逐行取首列词，cap 内全部插入 Trie（ac_nodes_[0] 为根，首次惰性建根）；
//   步骤3：全部插入后构建失败指针（ac_built_=true），返回词条数。
int RuleExtractor::load_keyword_dict(const std::string& path, const std::string& category,
                                     int cap) {
  std::ifstream f(path);
  if (!f) return -1;
  if (ac_nodes_.empty()) ac_nodes_.push_back({});  // 根节点
  std::string line;
  int n = 0;
  while (n < cap && std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t tab = line.find('\t');
    std::string w = line.substr(0, tab);
    if (w.empty()) continue;
    auto cps = ac_decode(w);
    if (cps.empty()) continue;
    ac_words_.push_back(w);
    ac_cats_.push_back(category);
    ac_lens_.push_back((int)cps.size());
    ac_insert(0, cps.data(), cps.size(), (int)ac_words_.size() - 1);
    ++n;
  }
  if (n > 0) {
    ac_build_fail();
    ac_built_ = true;
  }
  return n;
}

// ac_stats 伪代码：汇总节点/边计数并估算内存
//   （AcNode 对象 36B + 每条边 8B + vector 容量冗余按 1.3 倍估算）。
void RuleExtractor::ac_stats(size_t& nodes, size_t& edges, size_t& approx_bytes) const {
  nodes = ac_nodes_.size();
  edges = 0;
  for (auto& nd : ac_nodes_) edges += nd.next.size();
  approx_bytes = (size_t)((nodes * sizeof(AcNode) + edges * 8) * 1.3);
}

// ---------------- 二进制持久化 ----------------
// 格式：magic "AMEAC" + u32 version(1) + u64 dict_size + u64 dict_mtime
//       u32 nwords（逐词：u16 词长+词字节 + u16 类别长+类别字节 + i32 码点长）
//       u32 nnodes（逐节点：i32 out + i32 fail + i32 outlink + u32 nedges + nedges×(u32,i32)）
namespace {
template <typename T>
void wr(std::ofstream& f, const T& v) { f.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
template <typename T>
bool rd(std::ifstream& f, T& v) { return (bool)f.read(reinterpret_cast<char*>(&v), sizeof(T)); }
constexpr char kAcMagic[6] = "AMEAC";
constexpr uint32_t kAcVersion = 1;
}  // namespace

// ac_save 伪代码：写头（magic/version/词典校验）→ 词表 → 节点表（含 fail/outlink/排序边）。
bool RuleExtractor::ac_save(const std::string& cache_path, uint64_t dict_size,
                            uint64_t dict_mtime) const {
  std::ofstream f(cache_path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(kAcMagic, 5);
  wr(f, kAcVersion);
  wr(f, dict_size);
  wr(f, dict_mtime);
  uint32_t nw = (uint32_t)ac_words_.size();
  wr(f, nw);
  for (size_t i = 0; i < ac_words_.size(); ++i) {
    uint16_t wl = (uint16_t)ac_words_[i].size();
    uint16_t cl = (uint16_t)ac_cats_[i].size();
    wr(f, wl);
    f.write(ac_words_[i].data(), wl);
    wr(f, cl);
    f.write(ac_cats_[i].data(), cl);
    wr(f, ac_lens_[i]);
  }
  uint32_t nn = (uint32_t)ac_nodes_.size();
  wr(f, nn);
  for (auto& nd : ac_nodes_) {
    wr(f, nd.out);
    wr(f, nd.fail);
    wr(f, nd.outlink);
    uint32_t ne = (uint32_t)nd.next.size();
    wr(f, ne);
    for (auto& [cp, tgt] : nd.next) {
      wr(f, cp);
      wr(f, tgt);
    }
  }
  return f.good();
}

namespace {
// Bounds cover the supported large dictionaries while rejecting allocation requests
// that are disproportionate to the actual cache payload.
constexpr uint64_t kMaxAcCacheBytes = 256ULL * 1024 * 1024;
constexpr uint32_t kMaxAcWords = 2 * 1024 * 1024;
constexpr uint32_t kMaxAcNodes = 8 * 1024 * 1024;
constexpr uint32_t kMaxAcEdges = 8 * 1024 * 1024;

class AcCacheReader {
 public:
  explicit AcCacheReader(const std::string& path) : file_(path, std::ios::binary | std::ios::ate) {
    if (!file_) return;
    auto size = file_.tellg();
    if (size < 0 || (uint64_t)size > kMaxAcCacheBytes) return;
    remaining_ = (uint64_t)size;
    file_.seekg(0);
    valid_ = (bool)file_;
  }
  bool bytes(char* destination, size_t count) {
    if (!valid_ || count > remaining_) return false;
    if (!file_.read(destination, (std::streamsize)count)) return false;
    remaining_ -= count;
    return true;
  }
  template <typename T> bool read(T& value) {
    return bytes(reinterpret_cast<char*>(&value), sizeof(value));
  }
  bool count(uint32_t& value, size_t minimum_bytes, uint32_t maximum) {
    if (!read(value)) return false;
    return value <= maximum && value <= remaining_ / minimum_bytes;
  }
  bool string(std::string& value) {
    uint16_t length = 0;
    if (!read(length) || length > remaining_) return false;
    value.resize(length);
    return bytes(value.data(), length);
  }
  bool finished() const { return valid_ && remaining_ == 0; }

 private:
  std::ifstream file_;
  uint64_t remaining_ = 0;
  bool valid_ = false;
};

bool read_cache_header(AcCacheReader& reader, uint64_t dict_size, uint64_t dict_mtime) {
  char magic[5];
  uint32_t version = 0;
  uint64_t size = 0, mtime = 0;
  if (!reader.bytes(magic, sizeof(magic)) || std::memcmp(magic, kAcMagic, 5) != 0) return false;
  if (!reader.read(version) || version != kAcVersion) return false;
  if (!reader.read(size) || !reader.read(mtime)) return false;
  return size == dict_size && mtime == dict_mtime;
}

bool read_word_table(AcCacheReader& reader, std::vector<std::string>& words,
                     std::vector<std::string>& categories, std::vector<int>& lengths) {
  uint32_t count = 0;
  if (!reader.count(count, 8, kMaxAcWords)) return false;
  words.resize(count);
  categories.resize(count);
  lengths.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!reader.string(words[i]) || !reader.string(categories[i]) || !reader.read(lengths[i]))
      return false;
    if (words[i].empty() || lengths[i] <= 0) return false;
    if ((size_t)lengths[i] != ac_decode(words[i]).size()) return false;
  }
  return true;
}

template <typename Node>
bool read_cache_node(AcCacheReader& reader, Node& node, uint32_t& edges_left) {
  uint32_t edges = 0;
  if (!reader.read(node.out) || !reader.read(node.fail) || !reader.read(node.outlink)) return false;
  if (!reader.count(edges, 8, edges_left)) return false;
  edges_left -= edges;
  node.next.resize(edges);
  for (auto& edge : node.next)
    if (!reader.read(edge.first) || !reader.read(edge.second)) return false;
  return true;
}

template <typename Nodes>
bool read_node_table(AcCacheReader& reader, Nodes& nodes) {
  uint32_t count = 0, edges_left = kMaxAcEdges;
  if (!reader.count(count, 16, kMaxAcNodes) || count == 0) return false;
  nodes.resize(count);
  for (auto& node : nodes)
    if (!read_cache_node(reader, node, edges_left)) return false;
  return true;
}

bool valid_node_index(int index, size_t count) {
  return index >= 0 && (size_t)index < count;
}

template <typename Node>
bool valid_transitions(const Node& node, size_t node_count) {
  uint32_t previous = 0;
  bool first = true;
  for (const auto& [codepoint, target] : node.next) {
    if (!first && codepoint <= previous) return false;
    if (target == 0 || !valid_node_index(target, node_count)) return false;
    previous = codepoint;
    first = false;
  }
  return true;
}

template <typename Node>
bool valid_node_fields(const Node& node, size_t node_count, size_t word_count) {
  if (!valid_node_index(node.fail, node_count) || !valid_node_index(node.outlink, node_count))
    return false;
  if (node.out < -1) return false;
  if (node.out >= 0 && (size_t)node.out >= word_count) return false;
  return valid_transitions(node, node_count);
}

template <typename Node>
bool visit_trie_children(const Node& node, size_t depth, std::vector<size_t>& depths,
                          std::vector<size_t>& pending) {
  for (const auto& [codepoint, target] : node.next) {
    (void)codepoint;
    if (depths[target] != 0) return false;  // A second parent or transition cycle.
    depths[target] = depth + 1;
    pending.push_back((size_t)target);
  }
  return true;
}

template <typename Nodes>
bool trie_depths(const Nodes& nodes, std::vector<size_t>& depths) {
  depths.assign(nodes.size(), 0);
  depths[0] = 1;  // Zero is reserved for unvisited nodes.
  std::vector<size_t> pending{0};
  for (size_t i = 0; i < pending.size(); ++i) {
    size_t node = pending[i];
    if (!visit_trie_children(nodes[node], depths[node], depths, pending)) return false;
  }
  return pending.size() == nodes.size();
}

template <typename Nodes>
bool valid_suffix_links(const Nodes& nodes, size_t index, const std::vector<size_t>& depths,
                         const std::vector<int>& word_lengths) {
  const auto& node = nodes[index];
  // Strictly decreasing trie depth proves both linked chains terminate at root.
  if (depths[node.fail] >= depths[index] || depths[node.outlink] >= depths[index]) return false;
  const auto& failure = nodes[node.fail];
  int expected_output = failure.out >= 0 ? node.fail : failure.outlink;
  if (node.outlink != expected_output) return false;
  if (node.out >= 0 && (size_t)word_lengths[node.out] != depths[index] - 1) return false;
  return true;
}

template <typename Nodes>
bool validate_automaton(const Nodes& nodes, const std::vector<int>& word_lengths) {
  if (nodes.empty()) return false;
  const auto& root = nodes[0];
  if (root.fail != 0 || root.outlink != 0 || root.out != -1) return false;
  for (const auto& node : nodes)
    if (!valid_node_fields(node, nodes.size(), word_lengths.size())) return false;
  std::vector<size_t> depths;
  if (!trie_depths(nodes, depths)) return false;
  for (size_t i = 1; i < nodes.size(); ++i)
    if (!valid_suffix_links(nodes, i, depths, word_lengths)) return false;
  return true;
}
}  // namespace

void RuleExtractor::swap_ac_state(RuleExtractor& other) noexcept {
  ac_words_.swap(other.ac_words_);
  ac_cats_.swap(other.ac_cats_);
  ac_lens_.swap(other.ac_lens_);
  ac_nodes_.swap(other.ac_nodes_);
  std::swap(ac_built_, other.ac_built_);
}

bool RuleExtractor::ac_load(const std::string& cache_path, uint64_t dict_size,
                             uint64_t dict_mtime) {
  try {
    AcCacheReader reader(cache_path);
    if (!read_cache_header(reader, dict_size, dict_mtime)) return false;
    RuleExtractor loaded;
    if (!read_word_table(reader, loaded.ac_words_, loaded.ac_cats_, loaded.ac_lens_)) return false;
    if (!read_node_table(reader, loaded.ac_nodes_) || !reader.finished()) return false;
    if (!validate_automaton(loaded.ac_nodes_, loaded.ac_lens_)) return false;
    loaded.ac_built_ = true;
    swap_ac_state(loaded);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
}

int RuleExtractor::load_keyword_dict_cached(const std::string& path,
                                            const std::string& category, int cap,
                                            const std::string& cache_path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return -1;
  uint64_t size = (uint64_t)st.st_size, mtime = (uint64_t)st.st_mtime;
  if (ac_load(cache_path, size, mtime)) return (int)ac_words_.size();
  // Rebuild in isolation: failed cache reads must not append to an old dictionary.
  RuleExtractor rebuilt;
  int count = rebuilt.load_keyword_dict(path, category, cap);
  if (count <= 0) return count;
  swap_ac_state(rebuilt);
  ac_save(cache_path, size, mtime);  // Best effort; the rebuilt dictionary is usable.
  return count;
}

}  // namespace ame
