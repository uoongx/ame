// ============================================================================
// 模块：M6 扩散引擎（diffusion）——联想记忆的"思考主体"，从种子节点出发沿
//       联想图做加权能量扩散，产出各节点的激活能量与可解释传播路径。
//
// 本文件主体思路：实现 DiffusionEngine。核心为 diffuse() 的逐层波前扩展：
// 种子入队为第 0 跳波前，每层把波前节点的出边邻居算入下一层，边传导系数
// 与 1/(hop+1) 衰减共同决定子节点能量，直至深度耗尽、budget 用尽或能量枯竭。
//
// 关键算法/数据结构：加权逐层 BFS（vector 作波前队列，unordered_set 判重）。
//   - 边传导 = conduction_coeff(类型) × weight × 可选类型乘子；
//     weight > 5 的高频边按 5 + log1p(w-5) 软化。
//   - 深度硬限 [2,3]；budget 限制已扩展节点总数；child_e < 0.01 能量剪枝。
//   - 每节点 paths 按能量降序保留 top3，排序截断维护。
//   - 同一节点在同一 hop 层内只入队一次（能量仍可多次累加）。
//
// 依赖关系：依赖 M9 storage（Storage::get_node / neighbors 邻接表查询）；
// 依赖 M3 keyword（KeywordManager，休眠关键词判定的预留入口，当前直接查
// 节点 status）；依赖 core 公共层（conduction_coeff、RelType、NodeKind）。
// ============================================================================
#include "ame/diffusion/diffusion_engine.h"

#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace ame {

// 构造函数：保存存储引擎引用与可选的关键词管理器指针（kw 可空，空则查节点 status）。
DiffusionEngine::DiffusionEngine(Storage& storage, KeywordManager* kw)
    : storage_(storage), kw_(kw) {}

// 伪代码：
// 步骤1：深度钳位到 [2,3]；初始化结果表 result、当前波前 cur、已扩展集合 expanded。
// 步骤2：装入种子——若种子节点在库中不存在则跳过；否则把 weight 累加进
//        result 并生成一条单点路径，同时以 hop=0 加入当前波前。
// 步骤3：逐层扩散循环，直到波前为空：
//   3.1 对波前中每个节点 f：若 hop 已达深度上限则跳过；若已扩展节点数达到
//       budget 则整层停止扩展；若 f 已扩展过则跳过，否则标记并计数。
//   3.2 遍历 f 的邻居（可按 rel_types 过滤通道）：
//       - 若邻居是休眠关键词（Keyword 且 status==1）则跳过；
//       - 计算边传导 trans = 类型系数 × 边权 × 类型乘子（weights 空则乘 1）；
//         若边权 > 5 则按 5 + log1p(w-5) 软化高频边；
//       - 子能量 child_e = f.energy × trans × (hop+1)/(hop+2)，即叠加
//         1/(hop+1) 衰减的增量因子；child_e < 0.01 视为枯竭，剪枝；
//       - 在 f 的路径上追加本步边，更新路径能量；
//       - 把 child_e 累加进邻居的结果能量，路径插入 paths 后按能量降序
//         排序并截断到 top3；
//       - 若该邻居在本层尚未入队且下一跳不超深度，则加入下一层波前
//         （同层去重，但能量与路径可多次累加）。
//   3.3 下一层波前接替当前波前，回到 3。
// 步骤4：返回 result（node_uid -> 能量 + top3 路径）。
DiffusionResult DiffusionEngine::diffuse(const std::vector<DiffusionSeed>& seeds, int depth,
                                         const std::vector<RelType>* rel_types,
                                         const std::unordered_map<int, double>* weights,
                                         int budget) const {
  return diffuse_beam(seeds, depth, rel_types, weights, budget, 0, nullptr);  // beam=0 即原 BFS
}

// 伪代码（束搜索 best-first，引导式扩散阶段 1）：
// 与 diffuse 同流程，仅两处扩展：
//   ① beam 剪枝：beam_width>0 时每层先把波前按能量降序截断到 beam_width；
//   ② 边传导分：gate_fn 非空时由门控函数给出（监督边门控），否则规则传导。
// 能量衰减/路径记录/top3/budget/休眠隔离语义与 diffuse 完全一致。
namespace {

struct Frontier {
  std::string uid;
  double energy;
  int hop;
  Path path;
};

// One traversal owns its result, expansion budget and per-layer frontier state.
class DiffusionTraversal {
 public:
  DiffusionTraversal(const Storage& storage, int depth, const std::vector<RelType>* rel_types,
                     const std::unordered_map<int, double>* weights, int budget, int beam_width,
                     const DiffusionEngine::GateFn& gate)
      : storage_(storage), depth_(depth), rel_types_(rel_types), weights_(weights),
        budget_(budget), beam_width_(beam_width), gate_(gate) {}

  DiffusionResult run(const std::vector<DiffusionSeed>& seeds) {
    auto current = seed_frontier(seeds);
    while (!current.empty()) {
      trim_beam(current);
      current = expand_layer(current);
    }
    return std::move(result_);
  }

 private:
  std::vector<Frontier> seed_frontier(const std::vector<DiffusionSeed>& seeds) {
    std::vector<Frontier> frontier;
    for (const auto& seed : seeds) {
      if (!storage_.get_node(seed.node_uid)) continue;
      auto& energy = result_[seed.node_uid];
      energy.energy += seed.weight;
      Path path;
      path.energy = seed.weight;
      energy.paths.push_back(path);
      frontier.push_back({seed.node_uid, seed.weight, 0, path});
    }
    return frontier;
  }

  void trim_beam(std::vector<Frontier>& frontier) const {
    if (beam_width_ <= 0 || frontier.size() <= static_cast<size_t>(beam_width_)) return;
    std::sort(frontier.begin(), frontier.end(),
              [](const Frontier& a, const Frontier& b) { return a.energy > b.energy; });
    frontier.resize(beam_width_);
  }

  std::vector<Frontier> expand_layer(const std::vector<Frontier>& current) {
    std::vector<Frontier> next;
    std::unordered_set<std::string> queued;
    for (const auto& frontier : current) {
      if (frontier.hop >= depth_) continue;
      if (expanded_count_ >= budget_) break;
      if (!expanded_.insert(frontier.uid).second) continue;
      ++expanded_count_;
      expand_frontier(frontier, next, queued);
    }
    return next;
  }

  double transmission(const Edge& edge, const Node* head, const Node& tail, int hop) const {
    if (gate_ && head) return gate_(edge, *head, tail, hop);
    double multiplier = 1.0;
    if (weights_) {
      auto it = weights_->find(static_cast<int>(edge.type));
      if (it != weights_->end()) multiplier = it->second;
    }
    double weight = edge.weight;
    if (weight > 5.0) weight = 5.0 + std::log1p(weight - 5.0);
    return conduction_coeff(edge.type) * weight * multiplier;
  }

  void record_path(const std::string& uid, const Path& path) {
    auto& energy = result_[uid];
    energy.energy += path.energy;
    energy.paths.push_back(path);
    std::sort(energy.paths.begin(), energy.paths.end(),
              [](const Path& a, const Path& b) { return a.energy > b.energy; });
    if (energy.paths.size() > 3) energy.paths.resize(3);
  }

  void expand_frontier(const Frontier& frontier, std::vector<Frontier>& next,
                        std::unordered_set<std::string>& queued) {
    auto neighbors = storage_.neighbors(frontier.uid, rel_types_);
    // Degree includes dormant/pruned edges, as in the original normalization.
    double degree = static_cast<double>(neighbors.size());
    const Node* head = storage_.get_node(frontier.uid);
    for (const auto& [edge, node] : neighbors) {
      if (node->kind == NodeKind::Keyword && node->status == 1) continue;
      double transfer = transmission(edge, head, *node, frontier.hop);
      double energy = frontier.energy * transfer * static_cast<double>(frontier.hop + 1) /
                      static_cast<double>(frontier.hop + 2) / degree;
      if (energy < 0.01) continue;
      Path path = frontier.path;
      path.steps.push_back({frontier.uid, node->uid, edge.type});
      path.energy = energy;
      record_path(node->uid, path);
      // Accumulate every incoming path, but expand only the first one in this layer.
      if (queued.insert(node->uid).second)
        next.push_back({node->uid, energy, frontier.hop + 1, path});
    }
  }

  const Storage& storage_;
  int depth_;
  const std::vector<RelType>* rel_types_;
  const std::unordered_map<int, double>* weights_;
  int budget_;
  int beam_width_;
  const DiffusionEngine::GateFn& gate_;
  DiffusionResult result_;
  std::unordered_set<std::string> expanded_;
  int expanded_count_ = 0;
};

}  // namespace

DiffusionResult DiffusionEngine::diffuse_beam(const std::vector<DiffusionSeed>& seeds,
                                              int depth,
                                              const std::vector<RelType>* rel_types,
                                              const std::unordered_map<int, double>* weights,
                                              int budget, int beam_width,
                                              const GateFn& gate_fn) const {
  DiffusionTraversal traversal(storage_, std::clamp(depth, 2, 3), rel_types, weights,
                               budget, beam_width, gate_fn);
  return traversal.run(seeds);
}

}  // namespace ame
