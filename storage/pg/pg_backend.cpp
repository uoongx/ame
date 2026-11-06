#include "pg_backend.h"

#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <algorithm>

namespace ame {
namespace {

constexpr size_t kCacheEvictionDivisor = 10;  // Evict roughly one tenth when full.
constexpr const char* kNodeCols =
    "uid,kind,type,name,text,ts,precision,importance,lat,lng,geohash,"
    "status,mention_count,attrs,embedding";
constexpr const char* kEdgeCols = "from_uid,to_uid,rel_type,weight,attrs";

std::string i64(long long v) { return std::to_string(v); }
template <typename Number>
std::string number_text(Number value) {
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) throw std::runtime_error("Unable to format database value");
  return {buffer, result.ptr};
}

template <typename Number>
Number parse_number(std::string_view text) {
  Number value{};
  const char* end = text.data() + text.size();
  auto result = std::from_chars(text.data(), end, value);
  if (result.ec != std::errc{} || result.ptr != end)
    throw std::runtime_error("Invalid numeric value in PostgreSQL");
  return value;
}

class Transaction {
 public:
  explicit Transaction(PGconn* conn) : connection_(conn), active_(command("BEGIN")) {}
  ~Transaction() { if (active_) command("ROLLBACK"); }
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  bool active() const { return active_; }
  bool commit() {
    if (!command("COMMIT")) return false;
    active_ = false;
    return true;
  }
 private:
  bool command(const char* sql) {
    std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection_, sql), &PQclear);
    return result && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
  }
  PGconn* connection_;
  bool active_;
};

std::string ilike_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '%' || c == '_' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

void PgBackend::enter_call() const {
  if (depth_ == 0) {
    evict_if_needed();
    ++tick_;
  }
  ++depth_;
}

Storage::NodePins::CallScope::CallScope(const PgBackend& backend)
    : lock_(backend.mu_), depth_(backend.depth_) {
  backend.enter_call();
}

Storage::NodePins::CallScope::~CallScope() noexcept { --depth_; }

void PgBackend::evict_if_needed() const {
  if (cap_ == 0 || cache_.size() < cap_) return;
  const size_t target = cap_ - cap_ / kCacheEvictionDivisor - 1;
  std::vector<std::pair<uint64_t, const std::string*>> ord;
  ord.reserve(cache_.size());
  for (auto& kv : cache_) ord.emplace_back(kv.second.used, &kv.first);
  std::sort(ord.begin(), ord.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  size_t n_evict = cache_.size() - target;
  for (size_t i = 0; i < n_evict && i < ord.size(); ++i) cache_.erase(*ord[i].second);
}

PgBackend::Result PgBackend::exec(const std::string& sql, const std::vector<std::string>& params) const {
  if (params.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return {};
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& param : params) {
    if (param.find('\0') != std::string::npos) return {};
    values.push_back(param.c_str());
  }
  Result result(PQexecParams(conn_.get(), sql.c_str(), static_cast<int>(params.size()),
                             nullptr, values.data(), nullptr, nullptr, 0));
  if (!result) return {};
  auto status = PQresultStatus(result.get());
  if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK) return result;
  const char* state = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
  std::fprintf(stderr, "[pg] Query failed: %s (%s)\n", PQresStatus(status), state ? state : "unknown");
  return {};
}

Err PgBackend::connect(const std::string& conninfo, const std::string& ns) {
  if (conninfo.find('\0') != std::string::npos || ns.find('\0') != std::string::npos) return Err::InvalidArg;
  conn_.reset(PQconnectdb(conninfo.c_str()));
  if (!conn_ || PQstatus(conn_.get()) != CONNECTION_OK) return Err::IoError;
  ns_ = ns.empty() ? "default" : ns;
  auto owner = exec("SELECT pg_try_advisory_lock(hashtextextended($1, 0))", {"ame:graph:" + ns_});
  if (!owner || std::string_view(PQgetvalue(owner.get(), 0, 0)) != "t") return Err::IoError;
  if (const char* configured = std::getenv("AME_PG_CACHE_NODES")) {
    try {
      cap_ = parse_number<size_t>(configured);
    } catch (const std::runtime_error&) {
      return Err::InvalidArg;
    }
  }
  Err status = init_schema();
  return status == Err::Ok ? load_index() : status;
}

Err PgBackend::init_schema() {
  if (!exec("CREATE EXTENSION IF NOT EXISTS vector", {})) return Err::Internal;
  const bool trigram = static_cast<bool>(exec("CREATE EXTENSION IF NOT EXISTS pg_trgm", {}));
  const char* ddl[] = {
      "CREATE TABLE IF NOT EXISTS ame_nodes("
      "ns text NOT NULL, uid text NOT NULL, kind int NOT NULL, type text NOT NULL DEFAULT '',"
      "name text NOT NULL DEFAULT '', text text NOT NULL DEFAULT '', ts bigint NOT NULL DEFAULT 0,"
      "precision int NOT NULL DEFAULT 3, importance float8 NOT NULL DEFAULT 0.5,"
      "lat float8 NOT NULL DEFAULT 0, lng float8 NOT NULL DEFAULT 0,"
      "geohash text NOT NULL DEFAULT '', status int NOT NULL DEFAULT 0,"
      "mention_count int NOT NULL DEFAULT 0, attrs jsonb NOT NULL DEFAULT '{}',"
      "embedding vector(128), PRIMARY KEY(ns,uid))",
      "CREATE TABLE IF NOT EXISTS ame_edges("
      "ns text NOT NULL, from_uid text NOT NULL, to_uid text NOT NULL, rel_type int NOT NULL,"
      "weight float8 NOT NULL DEFAULT 1, attrs jsonb NOT NULL DEFAULT '{}',"
      "PRIMARY KEY(ns,from_uid,to_uid,rel_type))",
      "CREATE INDEX IF NOT EXISTS ame_nodes_name_idx ON ame_nodes(ns,kind,name)",
      "CREATE INDEX IF NOT EXISTS ame_nodes_kind_idx ON ame_nodes(ns,kind)",
      "CREATE INDEX IF NOT EXISTS ame_nodes_attrs_gin ON ame_nodes USING gin(attrs)",
      "CREATE INDEX IF NOT EXISTS ame_nodes_emb_hnsw ON ame_nodes USING hnsw(embedding vector_cosine_ops)",
      "CREATE INDEX IF NOT EXISTS ame_edges_from_idx ON ame_edges(ns,from_uid)",
      "CREATE INDEX IF NOT EXISTS ame_edges_to_idx ON ame_edges(ns,to_uid)",
  };
  for (auto* sql : ddl) {
    if (!exec(sql, {})) return Err::Internal;
  }
  if (trigram && !exec("CREATE INDEX IF NOT EXISTS ame_nodes_text_trgm "
                        "ON ame_nodes USING gin(text gin_trgm_ops)", {})) return Err::Internal;
  return Err::Ok;
}

Err PgBackend::load_index() {
  auto ri = exec("SELECT uid,kind,name FROM ame_nodes WHERE ns=$1", {ns_});
  if (!ri) return Err::IoError;
  for (int i = 0; i < PQntuples(ri.get()); ++i) {
    std::string uid = PQgetvalue(ri.get(), i, 0);
    IndexEntry ie;
    ie.kind = static_cast<NodeKind>(parse_number<int>(PQgetvalue(ri.get(), i, 1)));
    ie.name = PQgetvalue(ri.get(), i, 2);
    index_[uid] = ie;
    name_index_[i64(static_cast<int>(ie.kind)) + "|" + ie.name].push_back(uid);
  }
  return Err::Ok;
}

std::string PgBackend::vec_literal(const Vec& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += number_text(v[i]);
  }
  return s + "]";
}

Vec PgBackend::parse_vec(const char* s) {
  std::string_view text(s);
  if (text.size() < 2 || text.front() != '[' || text.back() != ']')
    throw std::runtime_error("Invalid pgvector text");
  text.remove_prefix(1);
  text.remove_suffix(1);
  Vec values;
  while (!text.empty()) {
    auto comma = text.find(',');
    values.push_back(parse_number<float>(text.substr(0, comma)));
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return values;
}

Json PgBackend::attrs_to_json(const Attrs& a) {
  Json j = Json::object();
  for (auto& kv : a) j[kv.first] = Json::string(kv.second);
  return j;
}

Attrs PgBackend::attrs_from_json(const char* s) {
  Attrs a;
  bool ok = false;
  Json j = Json::parse(s ? s : "{}", &ok);
  if (ok)
    for (auto& kv : j.obj) a[kv.first] = kv.second.as_str();
  return a;
}

Node PgBackend::row_to_node(PGresult* r, int row) const {
  Node n;
  n.uid = PQgetvalue(r, row, 0);
  n.kind = static_cast<NodeKind>(parse_number<int>(PQgetvalue(r, row, 1)));
  n.type = PQgetvalue(r, row, 2);
  n.name = PQgetvalue(r, row, 3);
  n.text = PQgetvalue(r, row, 4);
  n.ts = parse_number<TimePoint>(PQgetvalue(r, row, 5));
  n.precision = static_cast<Precision>(parse_number<int>(PQgetvalue(r, row, 6)));
  n.importance = parse_number<double>(PQgetvalue(r, row, 7));
  n.lat = parse_number<double>(PQgetvalue(r, row, 8));
  n.lng = parse_number<double>(PQgetvalue(r, row, 9));
  n.geohash = PQgetvalue(r, row, 10);
  n.status = parse_number<int>(PQgetvalue(r, row, 11));
  n.mention_count = parse_number<int>(PQgetvalue(r, row, 12));
  n.attrs = attrs_from_json(PQgetvalue(r, row, 13));
  if (!PQgetisnull(r, row, 14)) n.embedding = parse_vec(PQgetvalue(r, row, 14));
  return n;
}

Edge PgBackend::row_to_edge(PGresult* r, int row) const {
  Edge e;
  e.from = PQgetvalue(r, row, 0);
  e.to = PQgetvalue(r, row, 1);
  e.type = static_cast<RelType>(parse_number<int>(PQgetvalue(r, row, 2)));
  e.weight = parse_number<double>(PQgetvalue(r, row, 3));
  e.attrs = attrs_from_json(PQgetvalue(r, row, 4));
  return e;
}

const Node* PgBackend::materialize(Node n) const {
  auto& slot = cache_[n.uid];
  slot.used = tick_;
  if (slot.node) {
    *slot.node = std::move(n);
  } else {
    slot.node = std::make_unique<Node>(std::move(n));
  }
  return slot.node.get();
}

std::vector<const Node*> PgBackend::query_nodes(const std::string& sql,
                                                const std::vector<std::string>& params) const {
  std::vector<const Node*> out;
  auto r = exec(sql, params);
  if (!r) throw std::runtime_error("Unable to query PostgreSQL nodes");
  int n = PQntuples(r.get());
  out.reserve(n);
  for (int i = 0; i < n; ++i) out.push_back(materialize(row_to_node(r.get(), i)));
  return out;
}

Err PgBackend::put_node(Node n) {
  if (n.uid.empty()) return Err::InvalidArg;
  CallScope scope(*this);
  if (!persist_node(n)) return Err::IoError;
  materialize(n);
  {
    auto old = index_.find(n.uid);
    if (old != index_.end() && (old->second.kind != n.kind || old->second.name != n.name)) {
      std::string okey = i64(static_cast<int>(old->second.kind)) + "|" + old->second.name;
      auto& ov = name_index_[okey];
      ov.erase(std::remove(ov.begin(), ov.end(), n.uid), ov.end());
    }
    index_[n.uid] = IndexEntry{n.kind, n.name};
    auto& v = name_index_[i64(static_cast<int>(n.kind)) + "|" + n.name];
    if (std::find(v.begin(), v.end(), n.uid) == v.end()) v.push_back(n.uid);
  }
  return Err::Ok;
}

Err PgBackend::upsert_edge(const Edge& e, bool add_weight) {
  if (e.from.empty() || e.to.empty()) return Err::InvalidArg;
  CallScope scope(*this);
  if (!index_.count(e.from) || !index_.count(e.to)) return Err::NotFound;
  return persist_edge(e, add_weight) ? Err::Ok : Err::IoError;
}

bool PgBackend::persist_node(const Node& n) {
  const char* sql =
      "INSERT INTO ame_nodes(ns,uid,kind,type,name,text,ts,precision,importance,lat,lng,"
      "geohash,status,mention_count,attrs,embedding) VALUES("
      "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15::jsonb,NULLIF($16,'')::vector) "
      "ON CONFLICT(ns,uid) DO UPDATE SET kind=EXCLUDED.kind,type=EXCLUDED.type,"
      "name=EXCLUDED.name,text=EXCLUDED.text,ts=EXCLUDED.ts,precision=EXCLUDED.precision,"
      "importance=EXCLUDED.importance,lat=EXCLUDED.lat,lng=EXCLUDED.lng,"
      "geohash=EXCLUDED.geohash,status=EXCLUDED.status,mention_count=EXCLUDED.mention_count,"
      "attrs=EXCLUDED.attrs,embedding=EXCLUDED.embedding";
  std::vector<std::string> p = {
      ns_, n.uid, i64(static_cast<int>(n.kind)), n.type, n.name, n.text, i64(n.ts), i64(static_cast<int>(n.precision)),
      number_text(n.importance), number_text(n.lat), number_text(n.lng), n.geohash, i64(n.status),
      i64(n.mention_count), attrs_to_json(n.attrs).dump(),
      n.embedding.empty() ? "" : vec_literal(n.embedding)};
  auto r = exec(sql, p);
  return static_cast<bool>(r);
}

bool PgBackend::persist_edge(const Edge& e, bool add_weight) {
  const char* sql =
      "INSERT INTO ame_edges(ns,from_uid,to_uid,rel_type,weight,attrs) "
      "VALUES($1,$2,$3,$4,$5,$6::jsonb) "
      "ON CONFLICT(ns,from_uid,to_uid,rel_type) DO UPDATE SET "
      "weight=CASE WHEN $7::boolean THEN ame_edges.weight+EXCLUDED.weight ELSE EXCLUDED.weight END,"
      "attrs=ame_edges.attrs||EXCLUDED.attrs";
  auto r = exec(sql, {ns_, e.from, e.to, i64(static_cast<int>(e.type)), number_text(e.weight),
                           attrs_to_json(e.attrs).dump(), add_weight ? "true" : "false"});
  return static_cast<bool>(r);
}

Err PgBackend::delete_node(const std::string& uid) {
  CallScope scope(*this);
  if (!index_.count(uid)) return Err::NotFound;
  if (!exec("WITH removed_edges AS (DELETE FROM ame_edges WHERE ns=$1 AND "
            "(from_uid=$2 OR to_uid=$2)) DELETE FROM ame_nodes WHERE ns=$1 AND uid=$2", {ns_, uid}))
    return Err::IoError;
  auto it = index_.find(uid);
  std::string key = i64(static_cast<int>(it->second.kind)) + "|" + it->second.name;
  if (auto ni = name_index_.find(key); ni != name_index_.end()) {
    auto& v = ni->second;
    v.erase(std::remove(v.begin(), v.end(), uid), v.end());
  }
  index_.erase(it);
  cache_.erase(uid);
  return Err::Ok;
}

const Edge* PgBackend::find_edge(const std::string& from, const std::string& to, RelType t) const {
  CallScope scope(*this);
  auto r = exec(
      std::string("SELECT ") + kEdgeCols +
          " FROM ame_edges WHERE ns=$1 AND from_uid=$2 AND to_uid=$3 AND rel_type=$4 LIMIT 1",
      {ns_, from, to, i64(static_cast<int>(t))});
  if (!r) throw std::runtime_error("Unable to read PostgreSQL graph");
  const Edge* out = nullptr;
  if (PQntuples(r.get()) > 0) {
    edge_cache_ = std::make_unique<Edge>(row_to_edge(r.get(), 0));
    out = edge_cache_.get();
  }
  return out;
}

const Node* PgBackend::get_node(const std::string& uid) const {
  CallScope scope(*this);
  auto it = cache_.find(uid);
  if (it != cache_.end()) {
    it->second.used = tick_;
    return it->second.node.get();
  }
  auto r = exec(std::string("SELECT ") + kNodeCols +
                         " FROM ame_nodes WHERE ns=$1 AND uid=$2 LIMIT 1",
                     {ns_, uid});
  if (!r) throw std::runtime_error("Unable to read PostgreSQL graph");
  const Node* out = PQntuples(r.get()) > 0 ? materialize(row_to_node(r.get(), 0)) : nullptr;
  return out;
}

Node* PgBackend::get_node_mut(const std::string& uid) {
  CallScope scope(*this);
  const Node* existing = get_node(uid);
  if (!existing) return nullptr;
  auto& edit = cache_.at(uid).edit;
  if (edit) *edit = *existing;
  else edit = std::make_unique<Node>(*existing);
  return edit.get();
}

std::vector<std::pair<Edge, const Node*>> PgBackend::neighbors(
    const std::string& uid, const std::vector<RelType>* rel_types) const {
  CallScope scope(*this);
  std::vector<std::pair<Edge, const Node*>> res;
  auto want = [rel_types](RelType t) {
    if (!rel_types) return true;
    for (auto x : *rel_types)
      if (x == t) return true;
    return false;
  };
  auto push = [this, &res, &want](const Edge& e, const std::string& other) {
    if (!want(e.type)) return;
    const Node* n = get_node(other);
    if (n) res.emplace_back(e, n);
  };
  auto ro = exec(std::string("SELECT ") + kEdgeCols + " FROM ame_edges WHERE ns=$1 AND from_uid=$2",
                      {ns_, uid});
  if (!ro) throw std::runtime_error("Unable to query outgoing edges");
  {
    for (int i = 0; i < PQntuples(ro.get()); ++i) {
      Edge e = row_to_edge(ro.get(), i);
      push(e, e.to);
    }
  }
  auto ri = exec(std::string("SELECT ") + kEdgeCols + " FROM ame_edges WHERE ns=$1 AND to_uid=$2",
                      {ns_, uid});
  if (!ri) throw std::runtime_error("Unable to query incoming edges");
  {
    for (int i = 0; i < PQntuples(ri.get()); ++i) {
      Edge e = row_to_edge(ri.get(), i);
      std::string other = e.from;
      std::swap(e.from, e.to);
      e.attrs["_reversed"] = "1";
      push(e, other);
    }
  }
  return res;
}

std::vector<std::pair<std::string, double>> PgBackend::ann_search(const Vec& embedding,
                                                                  int topk) const {
  std::vector<std::pair<std::string, double>> out;
  if (embedding.empty() || topk <= 0) return out;
  CallScope scope(*this);
  std::string lit = vec_literal(embedding);
  auto r = exec(
      "SELECT uid, 1-(embedding <=> $2::vector) AS sim FROM ame_nodes "
      "WHERE ns=$1 AND embedding IS NOT NULL "
      "ORDER BY embedding <=> $2::vector ASC LIMIT $3",
      {ns_, lit, i64(topk)});
  if (!r) throw std::runtime_error("Unable to read PostgreSQL graph");
  for (int i = 0; i < PQntuples(r.get()); ++i) {
    double sim = parse_number<double>(PQgetvalue(r.get(), i, 1));
    if (sim > 0) out.emplace_back(PQgetvalue(r.get(), i, 0), sim);
  }
  return out;
}

std::vector<const Node*> PgBackend::geo_lookup(double lat, double lng, double radius_m) const {
  CallScope scope(*this);
  auto places = query_nodes(std::string("SELECT ") + kNodeCols +
                                " FROM ame_nodes WHERE ns=$1 AND kind=$2",
                            {ns_, i64(static_cast<int>(NodeKind::Place))});
  std::vector<const Node*> out;
  for (auto* n : places)
    if (haversine_m(lat, lng, n->lat, n->lng) <= radius_m) out.push_back(n);
  std::sort(out.begin(), out.end(), [lat, lng](const Node* a, const Node* b) {
    return haversine_m(lat, lng, a->lat, a->lng) < haversine_m(lat, lng, b->lat, b->lng);
  });
  return out;
}

const Node* PgBackend::find_by_name(NodeKind kind, const std::string& name) const {
  CallScope scope(*this);
  auto it = name_index_.find(i64(static_cast<int>(kind)) + "|" + name);
  if (it == name_index_.end() || it->second.empty()) return nullptr;
  return get_node(it->second.front());
}

std::vector<const Node*> PgBackend::find_all_by_name(NodeKind kind,
                                                     const std::string& name) const {
  CallScope scope(*this);
  std::vector<const Node*> out;
  auto it = name_index_.find(i64(static_cast<int>(kind)) + "|" + name);
  if (it == name_index_.end()) return out;
  for (auto& uid : it->second)
    if (const Node* n = get_node(uid)) out.push_back(n);
  return out;
}

const Node* PgBackend::find_by_attr(NodeKind kind, const std::string& key,
                                    const std::string& val) const {
  CallScope scope(*this);
  Json cond = Json::object();
  cond[key] = Json::string(val);
  auto v = query_nodes(std::string("SELECT ") + kNodeCols +
                           " FROM ame_nodes WHERE ns=$1 AND kind=$2 AND attrs @> $3::jsonb LIMIT 1",
                       {ns_, i64(static_cast<int>(kind)), cond.dump()});
  return v.empty() ? nullptr : v[0];
}

std::vector<const Node*> PgBackend::nodes_of_kind(NodeKind k) const {
  CallScope scope(*this);
  return query_nodes(std::string("SELECT ") + kNodeCols + " FROM ame_nodes WHERE ns=$1 AND kind=$2",
                     {ns_, i64(static_cast<int>(k))});
}

std::vector<const Node*> PgBackend::lexical_event_candidates(
    const std::vector<std::string>& words, int limit) const {
  if (words.empty() || limit <= 0) return {};
  CallScope scope(*this);
  std::string sql = std::string("SELECT ") + kNodeCols +
                    " FROM ame_nodes WHERE ns=$1 AND kind=" + i64(static_cast<int>(NodeKind::Event)) +
                    " AND (text ILIKE '%'||$2||'%'";
  std::vector<std::string> p = {ns_};
  for (size_t i = 0; i < words.size(); ++i) {
    p.push_back(ilike_escape(words[i]));
    if (i > 0) sql += " OR text ILIKE '%'||$" + std::to_string(i + 2) + "||'%'";
  }
  sql += ") LIMIT $" + std::to_string(words.size() + 2);
  p.push_back(i64(limit));
  return query_nodes(sql, p);
}

size_t PgBackend::node_count() const {
  CallScope scope(*this);
  auto r = exec("SELECT count(*) FROM ame_nodes WHERE ns=$1", {ns_});
  if (!r) throw std::runtime_error("Unable to count PostgreSQL graph");
  size_t n = parse_number<size_t>(PQgetvalue(r.get(), 0, 0));
  return n;
}

size_t PgBackend::edge_count() const {
  CallScope scope(*this);
  auto r = exec("SELECT count(*) FROM ame_edges WHERE ns=$1", {ns_});
  if (!r) throw std::runtime_error("Unable to count PostgreSQL graph");
  size_t n = parse_number<size_t>(PQgetvalue(r.get(), 0, 0));
  return n;
}

Err PgBackend::replace_graph(const std::vector<Node>& nodes, const std::vector<Edge>& edges) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  decltype(index_) index;
  decltype(name_index_) names;
  for (const auto& node : nodes) {
    if (node.uid.empty() || !index.emplace(node.uid, IndexEntry{node.kind, node.name}).second)
      return Err::ParseError;
    names[i64(static_cast<int>(node.kind)) + "|" + node.name].push_back(node.uid);
  }
  for (const auto& edge : edges) {
    if (!index.count(edge.from) || !index.count(edge.to)) return Err::ParseError;
  }
  Transaction transaction(conn_.get());
  if (!transaction.active() || !exec("DELETE FROM ame_edges WHERE ns=$1", {ns_}) ||
      !exec("DELETE FROM ame_nodes WHERE ns=$1", {ns_})) return Err::IoError;
  for (const auto& node : nodes) {
    if (!persist_node(node)) return Err::IoError;
  }
  for (const auto& edge : edges) {
    if (!persist_edge(edge, false)) return Err::IoError;
  }
  if (!transaction.commit()) return Err::IoError;
  cache_.clear();
  edge_cache_.reset();
  index_.swap(index);
  name_index_.swap(names);
  return Err::Ok;
}

std::vector<Node> PgBackend::all_nodes() const {
  CallScope scope(*this);
  std::vector<Node> out;
  auto r = exec(std::string("SELECT ") + kNodeCols + " FROM ame_nodes WHERE ns=$1", {ns_});
  if (!r) throw std::runtime_error("Unable to export PostgreSQL graph");
  out.reserve(PQntuples(r.get()));
  for (int i = 0; i < PQntuples(r.get()); ++i) out.push_back(row_to_node(r.get(), i));
  return out;
}

std::vector<Edge> PgBackend::all_edges() const {
  CallScope scope(*this);
  std::vector<Edge> out;
  auto r = exec(std::string("SELECT ") + kEdgeCols + " FROM ame_edges WHERE ns=$1", {ns_});
  if (!r) throw std::runtime_error("Unable to export PostgreSQL graph");
  out.reserve(PQntuples(r.get()));
  for (int i = 0; i < PQntuples(r.get()); ++i) out.push_back(row_to_edge(r.get(), i));
  return out;
}

}  // namespace ame
