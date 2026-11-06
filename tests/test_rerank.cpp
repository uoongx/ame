/*
 * M7 rerank 单元测试：融合打分公式、filters（时间范围）、α/β/γ 可调、explain 生成
 */
#include "ame/rerank/reranker.h"
#include "ame/storage/storage.h"
#include "ame/time/time_service.h"
#include "ame/vector/vector_service.h"
#include "test_util.hpp"

using namespace ame;

static void test_fuse_and_filters() {
  Storage s;
  TimeService ts(&s);
  VectorService vs(&s);
  Reranker rk(s, ts, vs);
  Node ev;
  ev.uid = "ev"; ev.kind = NodeKind::Event; ev.text = "测试事件";
  ev.ts = TimeService::now_utc();
  ev.importance = 0.8;
  ev.embedding = vs.embed("测试事件");
  s.put_node(ev);
  DiffusionResult dr;
  Path p;
  p.steps.push_back({"seed", "ev", RelType::CONTAINS});
  p.energy = 0.5;
  dr["ev"].energy = 0.5;
  dr["ev"].paths.push_back(p);
  dr["seed"].energy = 1.0;  // 非 Event，应被过滤

  auto out = rk.rerank(dr, vs.embed("测试事件"), {}, 10);
  CHECK(out.size() == 1);
  // 事件级归一化：max_event_energy=0.5（"seed" 非事件不计），graph=0.5/0.5=1.0
  // final = 0.5*1.0 + 0.3*~1.0 + 0.2*~1.0*0.8 ≈ 0.96
  CHECK_NEAR(out[0].score, 0.5 * 1.0 + 0.3 * 1.0 + 0.2 * 1.0 * 0.8, 0.05);
  CHECK(!out[0].explain.empty());

  // filters：时间范围排除
  RecallFilters f;
  f.time_to = ev.ts - 1;
  CHECK(rk.rerank(dr, vs.embed("测试事件"), f, 10).empty());

  // α/β/γ 可调
  rk.set_weights({1.0, 0.0, 0.0});
  auto out2 = rk.rerank(dr, vs.embed("完全不同的东西"), {}, 10);
  CHECK_NEAR(out2[0].score, 1.0, 1e-6);  // 纯图扩散分（事件级归一）
}

static void test_blocked_event_is_hidden_before_limit() {
  Storage storage;
  TimeService time(&storage);
  VectorService vectors(&storage);
  Reranker reranker(storage, time, vectors);
  Node hidden, visible;
  hidden.uid = "hidden";
  hidden.kind = NodeKind::Event;
  hidden.text = "private evidence";
  hidden.attrs["blocked"] = "1";
  visible.uid = "visible";
  visible.kind = NodeKind::Event;
  visible.text = "public evidence";
  CHECK(storage.put_node(hidden) == Err::Ok);
  CHECK(storage.put_node(visible) == Err::Ok);
  DiffusionResult diff;
  diff[hidden.uid].energy = 2.0;
  diff[visible.uid].energy = 1.0;
  Path path;
  path.steps.push_back({hidden.uid, visible.uid, RelType::NEXT});
  diff[visible.uid].paths.push_back(path);
  auto result = reranker.rerank(diff, {}, {}, 1);
  CHECK(result.size() == 1);
  if (result.empty()) return;
  CHECK(result.front().event->uid == visible.uid);
  CHECK(result.front().explain.find(hidden.text) == std::string::npos);
}

int main() {
  test_fuse_and_filters();
  test_blocked_event_is_hidden_before_limit();
  return AME_TEST_REPORT();
}
