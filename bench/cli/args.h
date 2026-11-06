#pragma once

#include "ame/bench/bench.h"

#include <charconv>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ame::bench_cli {

struct Cli {
  BenchConfig cfg;
  std::string dataset, data;
  int limit = 0;
  std::string dump_path, build_lexicon, validate_lexicon, score_sims;
  std::string lexicon_out = "bench/data/lexicon.json";
  std::string question_ids;
  int lexicon_topn = 2000;
  std::string save_graph, load_graph, name_alias;
  double entity_pass = -1.0;
  double rerank_guard = -1.0;
  std::string cheap_rerank, rerank_mode;
  std::string pg_conn;
  std::string pg_ns = "default";
  bool has_final = false;
};

template <typename Number>
Number number(std::string_view text) {
  if (text.empty()) throw std::invalid_argument("number must not be empty");
  Number result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
      || !std::isfinite(static_cast<double>(result))) {
    throw std::invalid_argument("invalid or out-of-range number: " + std::string(text));
  }
  return result;
}

inline std::vector<int> categories(std::string_view text) {
  std::vector<int> result;
  while (true) {
    const auto comma = text.find(',');
    result.push_back(number<int>(text.substr(0, comma)));
    if (comma == std::string_view::npos) return result;
    text.remove_prefix(comma + 1);
  }
}

using Setter = std::function<void(const std::string&)>;
inline Setter assign(std::string& target) {
  return [&target](const std::string& value) { target = value; };
}
template <typename Number>
Setter assign(Number& target) {
  return [&target](const std::string& value) { target = number<Number>(value); };
}
inline Setter toggle(bool& target) {
  return [&target](const std::string& value) { target = value != "off"; };
}
inline Setter positive(int& target) {
  return [&target](const std::string& value) {
    const auto parsed = number<int>(value);
    if (parsed <= 0) throw std::invalid_argument("value must be a positive integer");
    target = parsed;
  };
}
inline Setter final_weight(Cli& cli, double& target) {
  return [&cli, &target](const std::string& value) {
    target = number<double>(value);
    cli.has_final = cli.cfg.final_mix = true;
  };
}

inline std::unordered_map<std::string, Setter> options(Cli& cli) {
  return {
      {"--dataset", assign(cli.dataset)},
      {"--data", assign(cli.data)},
      {"--pg-conn", assign(cli.pg_conn)},
      {"--pg-ns", assign(cli.pg_ns)},
      {"--limit", assign(cli.limit)},
      {"--k", positive(cli.cfg.k)},
      {"--abstain-threshold", assign(cli.cfg.abstain_threshold)},
      {"--extractor", assign(cli.cfg.extractor_mode)},
      {"--answer", assign(cli.cfg.answer_mode)},
      {"--judge", assign(cli.cfg.judge_mode)},
      {"--concurrency", assign(cli.cfg.concurrency)},
      {"--model", assign(cli.cfg.model)},
      {"--cache-dir", assign(cli.cfg.cache_dir)},
      {"--config", assign(cli.cfg.config_path)},
      {"--out", assign(cli.cfg.out_json)},
      {"--dump", assign(cli.dump_path)},
      {"--build-lexicon", assign(cli.build_lexicon)},
      {"--validate-lexicon", assign(cli.validate_lexicon)},
      {"--score-sims", assign(cli.score_sims)},
      {"--lexicon-out", assign(cli.lexicon_out)},
      {"--lexicon-topn", assign(cli.lexicon_topn)},
      {"--question-ids", assign(cli.question_ids)},
      {"--entity-profile", toggle(cli.cfg.use_entity_profile)},
      {"--conditional-normalize", toggle(cli.cfg.conditional_normalize)},
      {"--answer-mode", assign(cli.cfg.answer_mode_agg)},
      {"--agg-prefilter-n", assign(cli.cfg.agg_prefilter_n)},
      {"--agg-batch", positive(cli.cfg.agg_batch)},
      {"--save-graph", assign(cli.save_graph)},
      {"--load-graph", assign(cli.load_graph)},
      {"--name-alias", assign(cli.name_alias)},
      {"--entity-pass-threshold", assign(cli.entity_pass)},
      {"--rerank-guard", assign(cli.rerank_guard)},
      {"--cheap-rerank", assign(cli.cheap_rerank)},
      {"--rerank", assign(cli.rerank_mode)},
      {"--final-alpha", final_weight(cli, cli.cfg.final_alpha)},
      {"--final-beta", final_weight(cli, cli.cfg.final_beta)},
      {"--abstain-categories", [&cli](const std::string& value) {
        cli.cfg.abstention_categories = categories(value);
      }},
  };
}

// Preserve unknown-option handling; known options require a complete, valid value.
inline Cli parse_args(int argc, char** argv) {
  Cli cli;
  const auto setters = options(cli);
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    const auto found = setters.find(option);
    if (found == setters.end()) continue;
    if (i + 1 == argc || std::string_view(argv[i + 1]).find("--") == 0) {
      throw std::invalid_argument(option + " requires a value");
    }
    try {
      found->second(argv[++i]);
    } catch (const std::invalid_argument& error) {
      throw std::invalid_argument(option + ": " + error.what());
    }
  }
  return cli;
}

}  // namespace ame::bench_cli
