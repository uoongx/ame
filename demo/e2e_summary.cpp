// Offline compatibility check for the Android summary event format.
#include "ame/core/json.h"
#include "ame/engine/engine.h"

#include <iostream>
#include <string>

int main() {
  ame::Engine engine;
  ame::Json summary = ame::Json::object();
  summary["l1"] = ame::Json::string("The planetarium opens at nine.");
  summary["l2"] = ame::Json::string("The planetarium opens at nine on weekends.");
  summary["keywords"] = ame::Json::array();
  summary["keywords"].push(ame::Json::string("planetarium"));
  summary["l3"] = ame::Json::string("Original planetarium opening hours");
  const std::string encoded = "MEMSUM:" + summary.dump();
  auto result = engine.remember_turn("summary", encoded, 100, "guide-1", "visitor");
  if (result.code != ame::Err::Ok) return 1;
  ame::SeedQuery query;
  query.text = "planetarium";
  for (const auto& hit : engine.recall(query, {}, 2, 5, false).events) {
    if (hit.event && hit.event->text == encoded && hit.event->attrs.at("speaker") == "summary") {
      std::cout << "PASS summary event round trip\n";
      return 0;
    }
  }
  std::cerr << "Unable to recall stored summary event\n";
  return 1;
}
