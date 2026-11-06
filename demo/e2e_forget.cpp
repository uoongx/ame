// Exercise memory management without an external model.
#include "ame/engine/engine.h"

#include <cstdio>
#include <string>

using namespace ame;

static int failures = 0;

static void check(bool ok, const char* name) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}

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
  check(r1.code == Err::Ok && r2.code == Err::Ok, "S0 write two turns");
  check(recall_has(e, "password", "zephyrus"), "S1 recall private event");
  check(recall_has(e, "drink", "quokka"), "S2 recall other event");

  check(e.block_event(r1.event_id) == Err::Ok, "S3 block event");
  check(!recall_has(e, "password", "zephyrus"), "S4 blocked event is excluded");
  check(recall_has(e, "drink", "quokka"), "S5 unrelated event remains visible");
  check(e.storage().get_node(r1.event_id) != nullptr, "S6 blocked data is retained");

  check(e.unblock_event(r1.event_id) == Err::Ok, "S7 unblock event");
  check(recall_has(e, "password", "zephyrus"), "S8 unblocked data is visible");

  check(e.delete_event(r1.event_id) == Err::Ok, "S9 delete event");
  check(!recall_has(e, "password", "zephyrus"), "S10 deleted data is excluded");
  check(e.storage().get_node(r1.event_id) == nullptr, "S11 deleted node is absent");
  check(e.delete_event(r1.event_id) == Err::NotFound, "S12 repeat deletion returns NotFound");

  e.clear_memory();
  check(e.stats().turns == 0, "S13 clearing removes private events");
  check(!recall_has(e, "drink", "quokka"), "S14 cleared graph has no matches");
  check(e.remember_turn("user", "new day new memory", 300).code == Err::Ok,
        "S15 cleared engine accepts new events");

  if (failures == 0) std::printf("E2E FORGET: ALL PASS\n");
  return failures == 0 ? 0 : 1;
}
