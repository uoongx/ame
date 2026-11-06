#include "ame/engine/engine.h"

#include <cstdio>
#include <string>

using namespace ame;

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

static bool recall_has(Engine& e, const char* query, const char* needle) {
  SeedQuery q;
  q.text = query;
  for (auto& ev : e.recall(q, {}, 2, 5, false).events)
    if (ev.event && ev.event->text.find(needle) != std::string::npos) return true;
  return false;
}

int main() {
  Engine e;
  auto r1 = e.remember_turn("user", "my password is zephyrus", 100);
  auto r2 = e.remember_turn("user", "favorite drink is quokka", 200);
  CHECK(r1.code == Err::Ok && r2.code == Err::Ok);
  CHECK(recall_has(e, "zephyrus", "zephyrus"));
  CHECK(recall_has(e, "quokka", "quokka"));


  CHECK(e.block_event(r1.event_id) == Err::Ok);
  CHECK(!recall_has(e, "zephyrus", "zephyrus"));
  CHECK(recall_has(e, "quokka", "quokka"));
  CHECK(e.storage().get_node(r1.event_id) != nullptr);
  CHECK(e.unblock_event(r1.event_id) == Err::Ok);
  CHECK(recall_has(e, "zephyrus", "zephyrus"));


  size_t before = e.storage().node_count();
  if (r1.resolved_entities.empty()) return 1;
  const auto owner = r1.resolved_entities.front().second;
  CHECK(e.entities().put_slot(owner, "trip", "old", 100, r1.event_id) == Err::Ok);
  CHECK(e.entities().put_slot(owner, "food", "tea", 200, r2.event_id) == Err::Ok);
  CHECK(e.delete_event(r1.event_id) == Err::Ok);
  CHECK(!recall_has(e, "zephyrus", "zephyrus"));
  CHECK(e.storage().get_node(r1.event_id) == nullptr);
  CHECK(e.storage().node_count() == before - 1);
  CHECK(e.get_entity_profile(owner).current_slot("trip").empty());
  CHECK(e.get_entity_profile(owner).current_slot("food") == "tea");
  CHECK(e.delete_event(r1.event_id) == Err::NotFound);
  CHECK(recall_has(e, "quokka", "quokka"));

  auto entity = e.register_entity("person", "Unaffiliated");
  CHECK(e.delete_event(entity) == Err::NotFound);
  CHECK(e.block_event(entity) == Err::NotFound);
  SeedQuery private_query;
  private_query.text = "quokka";
  auto retained = e.recall(private_query);
  CHECK(!retained.events.empty());
  {
    Engine tenant;
    CHECK(e.attach_tenant(&tenant) == Err::Ok);
    CHECK(tenant.attach_tenant(&e) == Err::InvalidArg);
    CHECK(e.attach_tenant(&e) == Err::InvalidArg);
  }
  CHECK(recall_has(e, "quokka", "quokka"));
  CHECK(e.clear_memory() == Err::Ok);
  CHECK(!retained.events.empty() && retained.events.front().event->kind == NodeKind::Event);
  CHECK(e.stats().events == 0);
  if (failures == 0) std::printf("PASS test_forget\n");
  return failures == 0 ? 0 : 1;
}
