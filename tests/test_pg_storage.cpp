#include "ame/storage/storage.h"
#include "ame/core/uid.h"
#include "ame/engine/engine.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <stdexcept>

using namespace ame;

static int failures = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                \
    }                                                            \
  } while (0)

static Vec vec128(float a, float b) {
  Vec v(128, 0.0f);
  v[0] = a;
  v[1] = b;
  return v;
}

static bool has_original_embedding(Storage& storage) {
  const Node* event = storage.get_node("ev1");
  return event && event->embedding.size() == 128;
}

static void verify_rollback(Storage& storage, const Json& snap) {
  // Roll back both SQL tables and retain the original name index and cache
  // when pgvector rejects a node midway through a replacement.
  if (snap.find("nodes") == nullptr || snap.find("nodes")->arr.empty()) {
    CHECK(false);
    return;
  }
  Json invalid = snap;
  auto& row = invalid["nodes"].arr.back();
  row["embedding"] = Json::array();
  row["embedding"].push(Json::number(1.0));
  CHECK(storage.from_json(invalid) == Err::IoError);
  CHECK(storage.node_count() == 3 && storage.edge_count() == 1);
  CHECK(storage.find_by_name(NodeKind::Entity, "Caroline") != nullptr);
  CHECK(has_original_embedding(storage));

  Node* edit = storage.get_node_mut("ev1");
  if (edit) {
    edit->embedding.resize(1);
    CHECK(storage.put_node(*edit) == Err::IoError);
    CHECK(has_original_embedding(storage));
  }
}

static void storage_roundtrip(const std::string& conn, const std::string& prefix) {
  Storage st;
  CHECK(st.pg_active() == false);
  CHECK(st.connect_pg(conn, prefix + "graph") == Err::Ok);
  CHECK(st.pg_active());
  if (!st.pg_active()) return;
  Storage competing;
  CHECK(competing.connect_pg(conn, prefix + "graph") == Err::IoError);
  CHECK(st.from_json(Json::object()) == Err::Ok);
  Node e1; e1.uid = "ev1"; e1.kind = NodeKind::Event; e1.text = "caroline moved to Paris";
  e1.name = "e1"; e1.ts = 100; e1.embedding = vec128(1, 0);
  Node e2; e2.uid = "ev2"; e2.kind = NodeKind::Event; e2.text = "jon adopted a dog";
  e2.name = "e2"; e2.ts = 200; e2.embedding = vec128(0, 1);
  Node p1; p1.uid = "p1"; p1.kind = NodeKind::Entity; p1.type = "person"; p1.name = "Caroline";
  p1.attrs["id:passport"] = "X123";
  CHECK(st.put_node(e1) == Err::Ok);
  CHECK(st.put_node(e2) == Err::Ok);
  CHECK(st.put_node(p1) == Err::Ok);
  CHECK(st.node_count() == 3);

  const Node* g = st.get_node("ev1");
  CHECK(g && g->text == "caroline moved to Paris" && g->embedding.size() == 128);
  CHECK(st.find_by_name(NodeKind::Entity, "Caroline") != nullptr);
  CHECK(st.find_by_attr(NodeKind::Entity, "id:passport", "X123") != nullptr);
  CHECK(st.find_by_attr(NodeKind::Entity, "id:passport", "NOPE") == nullptr);
  CHECK(st.nodes_of_kind(NodeKind::Event).size() == 2);
  CHECK(st.put_edge(Edge{"ev1", "p1", RelType::INVOLVES, 1.0, {{"role", "主体"}}}) == Err::Ok);
  CHECK(st.upsert_edge(Edge{"ev1", "p1", RelType::INVOLVES, 0.5, {}}, true) == Err::Ok);
  CHECK(st.put_edge(Edge{"ev1", "evX", RelType::NEXT, 1.0, {}}) == Err::NotFound);
  const Edge* fe = st.find_edge("ev1", "p1", RelType::INVOLVES);
  CHECK(fe && fe->weight > 1.49 && fe->weight < 1.51);
  CHECK(st.edge_count() == 1);
  auto nb = st.neighbors("p1");
  CHECK(nb.size() == 1);
  if (nb.size() == 1) {
    CHECK(nb[0].second->uid == std::string("ev1"));
    CHECK(nb[0].first.from == "p1" && nb[0].first.to == "ev1");
    CHECK(nb[0].first.attrs.count("_reversed") == 1);
  }
  auto ann = st.ann_search(vec128(1, 0), 2);
  CHECK(ann.size() == 1 && ann[0].first == "ev1" && ann[0].second > 0.99);
  auto lex = st.lexical_event_candidates({"paris", "dog"}, 10);
  CHECK(lex.size() == 2);
  auto lex1 = st.lexical_event_candidates({"paris"}, 10);
  CHECK(lex1.size() == 1 && lex1[0]->uid == std::string("ev1"));
  CHECK(st.lexical_event_candidates({"zzz_nothing"}, 10).empty());
  Json snap = st.to_json();
  Storage st2;
  CHECK(st2.connect_pg(conn, prefix + "copy") == Err::Ok);
  CHECK(st2.from_json(snap) == Err::Ok);
  CHECK(st2.node_count() == 3 && st2.edge_count() == 1);
  CHECK(st2.find_by_name(NodeKind::Entity, "Caroline") != nullptr);

  verify_rollback(st2, snap);
  CHECK(st2.from_json(Json::object()) == Err::Ok);
  CHECK(st2.node_count() == 0);

  CHECK(st.from_json(Json::object()) == Err::Ok);
  CHECK(st.node_count() == 0);
}

static void bounded_cache(const std::string& conn, const std::string& prefix) {
  setenv("AME_PG_CACHE_NODES", "3", 1);
  Storage st3;
  CHECK(st3.connect_pg(conn, prefix + "lru") == Err::Ok);
  CHECK(st3.from_json(Json::object()) == Err::Ok);
  for (int i = 0; i < 6; ++i) {
    Node n;
    n.uid = "lru" + std::to_string(i);
    n.kind = NodeKind::Entity;
    n.name = "LRU" + std::to_string(i);
    n.text = "body" + std::to_string(i);
    CHECK(st3.put_node(n) == Err::Ok);
  }
  CHECK(st3.pg_index_size() == 6);
  bool all_ok = true;
  for (int i = 0; i < 6; ++i) {
    const Node* n = st3.get_node("lru" + std::to_string(i));
    if (!n || n->text != "body" + std::to_string(i)) all_ok = false;
  }
  CHECK(all_ok);
  CHECK(st3.pg_cache_size() <= 4);
  {
    auto pins = st3.pin_nodes();
    const Node* first = st3.get_node("lru0");
    for (int i = 1; i < 6; ++i) CHECK(st3.get_node("lru" + std::to_string(i)) != nullptr);
    CHECK(first && first->text == "body0");
  }
  CHECK(st3.get_node("lru0") != nullptr);
  CHECK(st3.pg_cache_size() <= 4);
  CHECK(st3.find_by_name(NodeKind::Entity, "LRU5") != nullptr);
  CHECK(st3.pg_index_size() == 6);
  CHECK(st3.from_json(Json::object()) == Err::Ok);
  CHECK(st3.pg_index_size() == 0 && st3.pg_cache_size() == 0);
  unsetenv("AME_PG_CACHE_NODES");
}

static void required_write_failure(const std::string& connection, const std::string& prefix) {
  Engine engine;
  CHECK(engine.open_postgres(connection, prefix + "reject") == Err::Ok);
  if (!engine.storage().pg_active()) return;
  // PostgreSQL text cannot store an embedded NUL. The caller must observe a
  // failure when a required graph write rejects the original turn.
  bool failed = false;
  try {
    auto result = engine.remember_turn("user", std::string("before\0after", 12), 123);
    failed = result.code != Err::Ok;
  } catch (const std::runtime_error&) {
    failed = true;
  }
  CHECK(failed);
  CHECK(engine.clear_memory() == Err::Ok);
}

static void shared_graph(const std::string& conn, const std::string& prefix) {
  Engine t1, t2, s1;
  CHECK(t1.open_postgres(conn, prefix + "tenant_t1") == Err::Ok);
  CHECK(t2.open_postgres(conn, prefix + "tenant_t2") == Err::Ok);
  CHECK(s1.open_postgres(conn, prefix + "tenant_t1_session_s1") == Err::Ok);
  for (Storage* st : {&t1.storage(), &t2.storage(), &s1.storage()})
    CHECK(st->from_json(Json::object()) == Err::Ok);
  CHECK(t1.remember_turn("user", "tenant one knowledge zephyrus", 100).code == Err::Ok);
  CHECK(t2.remember_turn("user", "tenant two secret wombat", 100).code == Err::Ok);
  auto rp = s1.remember_turn("user", "session private quokka", 100);
  CHECK(rp.code == Err::Ok);
  SeedQuery q1; q1.text = "zephyrus";
  SeedQuery q2; q2.text = "wombat";
  SeedQuery q3; q3.text = "quokka";
  auto has = [](Engine& e, SeedQuery& q, const char* needle) {
    for (auto& ev : e.recall(q, {}, 2, 5, false).events)
      if (ev.event && ev.event->text.find(needle) != std::string::npos) return true;
    return false;
  };
  CHECK(!has(s1, q1, "zephyrus"));
  CHECK(!has(s1, q2, "wombat"));
  s1.attach_tenant(&t1);
  CHECK(has(s1, q1, "zephyrus"));
  CHECK(!has(s1, q2, "wombat"));
  CHECK(has(s1, q3, "quokka"));
  s1.detach_tenant();
  CHECK(!has(s1, q1, "zephyrus"));
  CHECK(s1.block_event(rp.event_id) == Err::Ok);
  CHECK(!has(s1, q3, "quokka"));
  CHECK(s1.storage().get_node(rp.event_id) != nullptr);
  CHECK(s1.unblock_event(rp.event_id) == Err::Ok);
  CHECK(has(s1, q3, "quokka"));
  CHECK(s1.delete_event(rp.event_id) == Err::Ok);
  CHECK(!has(s1, q3, "quokka"));
  CHECK(s1.storage().get_node(rp.event_id) == nullptr);
  CHECK(s1.delete_event(rp.event_id) == Err::NotFound);
  for (Engine* engine : {&t1, &t2, &s1}) CHECK(engine->clear_memory() == Err::Ok);
}

int main() {
  const char* connection = std::getenv("AME_PG_CONN");
  if (!connection || !*connection) {
    std::printf("SKIP test_pg_storage: AME_PG_CONN is unset\n");
    return 77;
  }
  const std::string prefix = "ame_test_" + gen_uid() + "_";
  storage_roundtrip(connection, prefix);
  bounded_cache(connection, prefix);
  shared_graph(connection, prefix);
  required_write_failure(connection, prefix);

  if (failures == 0) std::printf("PASS test_pg_storage\n");
  return failures == 0 ? 0 : 1;
}
