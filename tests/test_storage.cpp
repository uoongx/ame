/*
 * M9 storage 单元测试：内存邻接表（出/入边）、按类型过滤、JSON 快照 save/load 往返一致
 */
#include "ame/storage/storage.h"
#include "ame/core/json.h"
#include "test_util.hpp"
#include "test_temp_dir.hpp"

#include <limits>
#include <sys/stat.h>

using namespace ame;

static void test_private_temporary_directory() {
  std::filesystem::path removed;
  {
    const ame_test::TempDirectory first, second;
    CHECK(first.root() != second.root());
    CHECK(first.root().parent_path() == std::filesystem::current_path());
    struct stat info {};
    CHECK(::stat(first.root().c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0700);
    ame_test::write_text_file(first.path("fixture.txt"), "private fixture");
    CHECK(std::filesystem::exists(first.path("fixture.txt")));
    CHECK(!std::filesystem::exists(second.path("fixture.txt")));
    const std::vector<std::string> invalid = {"", ".", "..", "../escape", "/absolute",
                                             "nested/file", "nested\\file", std::string("a\0b", 3)};
    for (const auto& leaf : invalid) {
      bool rejected = false;
      try { first.path(leaf); } catch (const std::invalid_argument&) { rejected = true; }
      CHECK(rejected);
    }
    removed = first.root();
  }
  CHECK(!std::filesystem::exists(removed));
}

static void test_adjacency_and_snapshot() {
  Storage s;
  Node a, b, c;
  a.uid = "n1"; a.kind = NodeKind::Entity; a.name = "老张"; a.type = "person";
  b.uid = "n2"; b.kind = NodeKind::Event; b.text = "爆胎了"; b.ts = 1700000000;
  b.embedding = {1, 2, 3};
  c.uid = "n3"; c.kind = NodeKind::Place; c.name = "望京";
  c.lat = 39.9965; c.lng = 116.4730; c.geohash = geohash_encode(c.lat, c.lng, 6);
  s.put_node(a); s.put_node(b); s.put_node(c);
  s.put_edge(Edge{"n2", "n1", RelType::INVOLVES, 1.0, {{"role", "人物"}}});
  s.put_edge(Edge{"n2", "n3", RelType::HAPPENED_AT, 1.0, {}});

  // 邻接表：正向 + 反向
  CHECK(s.neighbors("n2").size() == 2);
  CHECK(s.neighbors("n1").size() == 1);
  CHECK(s.neighbors("n1", {RelType::INVOLVES}).size() == 1);
  CHECK(s.neighbors("n1", {RelType::NEXT}).empty());
  CHECK(s.get_node("n1")->name == "老张");
  CHECK(s.get_node("nope") == nullptr);

  // 快照 save/load 往返一致
  std::string path = ame_test::temp_path("snapshot.json");
  CHECK(s.save(path) == Err::Ok);
  Storage s2;
  CHECK(s2.load(path) == Err::Ok);
  CHECK(s2.node_count() == s.node_count());
  CHECK(s2.edge_count() == s.edge_count());
  const Node* b2 = s2.get_node("n2");
  CHECK(b2 && b2->text == "爆胎了" && b2->ts == 1700000000);
  CHECK(b2->embedding.size() == 3 && b2->embedding[2] == 3.0f);
  CHECK(s2.find_edge("n2", "n1", RelType::INVOLVES) != nullptr);
  const Node* c2 = s2.get_node("n3");
  CHECK(c2 && c2->geohash == c.geohash);
}

static void test_upsert_and_geo() {
  Storage s;
  Node p1, p2;
  p1.uid = "p1"; p1.kind = NodeKind::Place; p1.name = "朝阳公园";
  p1.lat = 39.9449; p1.lng = 116.4781;
  p2.uid = "p2"; p2.kind = NodeKind::Place; p2.name = "咖啡馆";
  p2.lat = 39.9455; p2.lng = 116.4790;
  s.put_node(p1); s.put_node(p2);

  // upsert 累加权重
  s.upsert_edge(Edge{"p1", "p2", RelType::NEAR, 1.0, {}}, true);
  s.upsert_edge(Edge{"p1", "p2", RelType::NEAR, 1.0, {}}, true);
  CHECK_NEAR(s.find_edge("p1", "p2", RelType::NEAR)->weight, 2.0, 1e-9);
  CHECK(s.edge_count() == 1);  // 同键边合并，不重复

  // geo_lookup 暴力距离
  CHECK(s.geo_lookup(39.945, 116.478, 500.0).size() == 2);
  CHECK(s.geo_lookup(40.1, 116.5, 50.0).empty());  // 远处无结果
  // geohash 前缀一致（同区域）
  CHECK(geohash_encode(p1.lat, p1.lng, 6).substr(0, 4) ==
        geohash_encode(p2.lat, p2.lng, 6).substr(0, 4));
}

static Json snapshot_with_node(Json node) {
  Json snapshot = Json::object();
  snapshot["nodes"] = Json::array();
  snapshot["nodes"].push(std::move(node));
  return snapshot;
}

static void test_snapshot_defaults_and_numeric_boundaries() {
  Storage storage;
  Json node = Json::object();
  node["uid"] = Json::string("node");
  CHECK(storage.from_json(snapshot_with_node(node)) == Err::Ok);
  const Node* restored = storage.get_node("node");
  CHECK(restored && restored->kind == NodeKind::Event && restored->precision == Precision::Second);
  CHECK(restored->importance == 0.5 && restored->status == 0 && restored->embedding.empty());
  node["ts"] = Json::number(-std::ldexp(1.0, 63));
  node["status"] = Json::number(static_cast<double>(std::numeric_limits<int>::max()) + 0.75);
  node["mention_count"] = Json::number(-1.75);
  node["precision"] = Json::string("wrong type");
  node["importance"] = Json::string("wrong type");
  node["embedding"] = Json::array();
  node["embedding"].push(Json::number(std::numeric_limits<float>::max()));
  CHECK(storage.from_json(snapshot_with_node(node)) == Err::Ok);
  restored = storage.get_node("node");
  CHECK(restored->ts == std::numeric_limits<TimePoint>::min());
  CHECK(restored->status == std::numeric_limits<int>::max() && restored->mention_count == -1);
  CHECK(restored->precision == Precision::Year && restored->importance == 0.5);
  CHECK(restored->embedding.at(0) == std::numeric_limits<float>::max());
  double highest_timestamp = std::nextafter(std::ldexp(1.0, 63), 0.0);
  node["ts"] = Json::number(highest_timestamp);
  CHECK(storage.from_json(snapshot_with_node(node)) == Err::Ok);
  CHECK(storage.get_node("node")->ts == static_cast<TimePoint>(highest_timestamp));
}

static void test_invalid_snapshot_preserves_graph() {
  Storage storage;
  Node original, other;
  original.uid = "original"; original.kind = NodeKind::Entity; original.name = "kept";
  other.uid = "other";
  storage.put_node(original); storage.put_node(other);
  storage.put_edge(Edge{original.uid, other.uid, RelType::INVOLVES, 1.0, {}});
  const std::vector<std::pair<std::string, double>> invalid = {
      {"kind", std::ldexp(1.0, 31)},
      {"precision", -std::ldexp(1.0, 31) - 1.0},
      {"status", std::numeric_limits<double>::quiet_NaN()},
      {"mention_count", std::numeric_limits<double>::infinity()},
      {"ts", std::ldexp(1.0, 63)},
      {"ts", std::nextafter(-std::ldexp(1.0, 63), -std::numeric_limits<double>::infinity())},
      {"embedding", static_cast<double>(std::numeric_limits<float>::max()) * 2.0},
      {"embedding", std::numeric_limits<double>::quiet_NaN()}};
  for (const auto& [field, value] : invalid) {
    Json valid = Json::object();
    valid["uid"] = Json::string("valid prefix");
    Json snapshot = snapshot_with_node(valid);
    Json bad = Json::object();
    bad["uid"] = Json::string("bad");
    bad[field] = Json::number(value);
    if (field == "embedding") {
      bad[field] = Json::array();
      bad[field].push(Json::number(value));
    }
    snapshot["nodes"].push(std::move(bad));
    CHECK(storage.from_json(snapshot) == Err::ParseError);
    CHECK(storage.node_count() == 2 && storage.edge_count() == 1);
    CHECK(storage.find_by_name(NodeKind::Entity, "kept")->uid == original.uid);
    CHECK(storage.find_edge(original.uid, other.uid, RelType::INVOLVES));
    CHECK(!storage.get_node("valid prefix"));
  }
}

int main() {
  test_private_temporary_directory();
  test_adjacency_and_snapshot();
  test_upsert_and_geo();
  test_snapshot_defaults_and_numeric_boundaries();
  test_invalid_snapshot_preserves_graph();
  return AME_TEST_REPORT();
}
