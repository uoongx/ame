// PostgreSQL graph storage and bounded node cache.
#pragma once

#include <libpq-fe.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ame/core/json.h"
#include "ame/core/types.h"

namespace ame {

class PgBackend {
 public:
  PgBackend() = default;
  ~PgBackend() = default;
  PgBackend(const PgBackend&) = delete;
  PgBackend& operator=(const PgBackend&) = delete;
  Err connect(const std::string& conninfo, const std::string& ns);

  Err put_node(Node n);
  Err upsert_edge(const Edge& e, bool add_weight);
  Err delete_node(const std::string& uid);
  const Edge* find_edge(const std::string& from, const std::string& to, RelType t) const;
  const Node* get_node(const std::string& uid) const;
  Node* get_node_mut(const std::string& uid);
  std::vector<std::pair<Edge, const Node*>> neighbors(
      const std::string& uid, const std::vector<RelType>* rel_types) const;
  std::vector<std::pair<std::string, double>> ann_search(const Vec& embedding, int topk) const;
  std::vector<const Node*> geo_lookup(double lat, double lng, double radius_m) const;
  const Node* find_by_name(NodeKind kind, const std::string& name) const;
  std::vector<const Node*> find_all_by_name(NodeKind kind, const std::string& name) const;
  const Node* find_by_attr(NodeKind kind, const std::string& key, const std::string& val) const;
  std::vector<const Node*> nodes_of_kind(NodeKind k) const;
  std::vector<const Node*> lexical_event_candidates(const std::vector<std::string>& words,
                                                    int limit) const;
  size_t node_count() const;
  size_t edge_count() const;
  size_t cache_size() const { std::lock_guard<std::recursive_mutex> lock(mu_); return cache_.size(); }
  size_t index_size() const { std::lock_guard<std::recursive_mutex> lock(mu_); return index_.size(); }
  std::vector<Node> all_nodes() const;
  std::vector<Edge> all_edges() const;
  Err replace_graph(const std::vector<Node>& nodes, const std::vector<Edge>& edges);
  void begin_operation();
  void end_operation() noexcept;

 private:
  struct CallScope {
    const PgBackend* self;
    explicit CallScope(const PgBackend* s);
    ~CallScope();
  };
  void evict_if_needed() const;
  struct ResultDeleter { void operator()(PGresult* r) const noexcept { PQclear(r); } };
  struct ConnectionDeleter { void operator()(PGconn* c) const noexcept { PQfinish(c); } };
  using Result = std::unique_ptr<PGresult, ResultDeleter>;
  Result exec(const std::string& sql, const std::vector<std::string>& params) const;
  Node row_to_node(PGresult* r, int row) const;
  Edge row_to_edge(PGresult* r, int row) const;
  const Node* materialize(Node n) const;
  std::vector<const Node*> query_nodes(const std::string& sql,
                                       const std::vector<std::string>& params) const;
  static std::string vec_literal(const Vec& v);        // -> '[f1,f2,...]'
  static Vec parse_vec(const char* s);
  static Json attrs_to_json(const Attrs& a);
  static Attrs attrs_from_json(const char* s);

  Err init_schema();
  Err load_index();
  bool persist_node(const Node& n);
  bool persist_edge(const Edge& e, bool add_weight);
  void enter_call() const;
  struct CacheEntry {
    std::unique_ptr<Node> node;
    std::unique_ptr<Node> edit;
    uint64_t used = 0;
  };
  struct IndexEntry {
    NodeKind kind = NodeKind::Event;
    std::string name;
  };

  std::unique_ptr<PGconn, ConnectionDeleter> conn_;
  std::string ns_;
  size_t cap_ = 50000;
  mutable uint64_t tick_ = 0;
  mutable int depth_ = 0;
  mutable std::recursive_mutex mu_;
  mutable std::unordered_map<std::string, CacheEntry> cache_;
  mutable std::unordered_map<std::string, IndexEntry> index_;
  mutable std::unordered_map<std::string, std::vector<std::string>> name_index_;  // "kind|name"->uids
  mutable std::unique_ptr<Edge> edge_cache_;
};

}  // namespace ame
