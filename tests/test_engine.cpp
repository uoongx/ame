/*
 * M12 engine 单元测试（网关层）：register/remember/lookup/merge/split 动词与错误码
 */
#include "ame/engine/engine.h"
#include "test_util.hpp"

using namespace ame;

static void test_lookup() {
  Engine e;
  e.remember("老张昨天在朝阳公园跑步");
  e.remember("老张今天在公司加班，压力很大");
  e.remember("老张晚上总是睡不着");

  // lookup by keyword
  LookupQuery lq;
  lq.keywords = {"跑步"};
  auto r1 = e.lookup(lq);
  CHECK(r1.size() == 1);
  // lookup by entity
  const Node* ent = e.storage().find_by_name(NodeKind::Entity, "老张");
  CHECK(ent != nullptr);
  LookupQuery lq2;
  lq2.entity_uids = {ent->uid};
  CHECK(e.lookup(lq2).size() == 3);
  // lookup by time
  LookupQuery lq3;
  lq3.time_from = TimeService::now_utc() - 3600;
  CHECK(e.lookup(lq3).size() >= 2);
}

static void test_gateway_verbs() {
  Engine e;
  // register
  std::string uid = e.register_entity("person", "老张", {{"phone", "13800000009"}});
  CHECK(!uid.empty());
  // remember 空文本 → 统一错误码
  auto bad = e.remember("");
  CHECK(bad.code == Err::InvalidArg);
  // recall 无种子命中 → NotFound
  SeedQuery sq;
  sq.text = "";
  auto rr = e.recall(sq);
  CHECK(rr.code == Err::NotFound);
  // merge / split 经网关
  std::string a = e.register_entity("person", "老王");
  std::string b = e.register_entity("person", "老王");
  CHECK(e.merge_entities(a, b) == Err::Ok);
  auto card = e.entities().get_entity(a);
  CHECK(card.uid == b);
}

// 引擎统计接口：turns/events=事件数、total_chars=事件文本字符合计、nodes=图节点总数
static void test_engine_stats() {
  Engine e;
  auto s0 = e.stats();
  CHECK(s0.turns == 0 && s0.events == 0 && s0.total_chars == 0 && s0.nodes == 0);
  e.remember_turn("user", "我今天去爬山了", 1700000000, "D1:1", "s1");
  e.remember_turn("assistant", "天气不错吧", 1700000060, "D1:2", "s1");
  auto s1 = e.stats();
  CHECK(s1.turns == 2 && s1.events == 2);
  CHECK(s1.total_chars == (long)(std::string("我今天去爬山了").size() +
                                 std::string("天气不错吧").size()));
  CHECK(s1.nodes > 2);  // 事件 + speaker 实体 + 关键词节点
}

static void test_lookup_combined_filters_and_limits() {
  Engine engine;
  Storage& storage = engine.storage();
  Node actor, place, keyword;
  actor.uid = "actor"; actor.kind = NodeKind::Entity;
  place.uid = "place"; place.kind = NodeKind::Place;
  keyword.uid = "keyword"; keyword.kind = NodeKind::Keyword; keyword.name = "失眠";
  storage.put_node(actor); storage.put_node(place); storage.put_node(keyword);
  engine.keywords().add_alias("sleep", "失眠");
  for (int i = 1; i <= 3; ++i) {
    Node event;
    event.uid = "event" + std::to_string(i);
    event.kind = NodeKind::Event;
    event.ts = i * 100;
    event.importance = 1.0 / i;
    if (i == 1) event.attrs["valid_to"] = "150";
    storage.put_node(event);
    storage.put_edge(Edge{event.uid, actor.uid, RelType::INVOLVES, 1.0, {}});
    storage.put_edge(Edge{event.uid, place.uid, RelType::HAPPENED_AT, 1.0, {}});
    storage.put_edge(Edge{event.uid, keyword.uid, RelType::CONTAINS, 1.0, {}});
  }
  LookupQuery query;
  query.entity_uids = {"missing", actor.uid};
  query.place_uid = place.uid;
  query.keywords = {"unmatched", "SLEEP"};
  query.time_from = 100;
  query.time_to = 300;
  query.current_only = true;
  auto results = engine.lookup(query);
  CHECK(results.size() == 2 && results[0]->uid == "event3" && results[1]->uid == "event2");
  query.order_by = "importance";
  query.limit = 1;
  CHECK(engine.lookup(query).at(0)->uid == "event2");
  query.current_only = false;
  CHECK(engine.lookup(query).at(0)->uid == "event1");
  query.limit = 0;
  CHECK(engine.lookup(query).empty());
  query.limit = -1;
  CHECK(engine.lookup(query).empty());
  query.limit = 10;
  query.place_uid = "wrong place";
  CHECK(engine.lookup(query).empty());
  query.place_uid = place.uid;
  query.entity_uids = {"wrong actor"};
  CHECK(engine.lookup(query).empty());
  query.entity_uids = {actor.uid};
  query.keywords = {"unmatched"};
  CHECK(engine.lookup(query).empty());
}

int main() {
  test_lookup();
  test_gateway_verbs();
  test_engine_stats();
  test_lookup_combined_filters_and_limits();
  return AME_TEST_REPORT();
}
