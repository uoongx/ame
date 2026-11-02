#include "../bench/cli/args.h"
#include "test_util.hpp"

#include <initializer_list>

namespace {
ame::bench_cli::Cli parse(std::initializer_list<std::string> values) {
  std::vector<std::string> storage{"ame_bench"};
  storage.insert(storage.end(), values.begin(), values.end());
  std::vector<char*> argv;
  for (auto& value : storage) argv.push_back(value.data());
  return ame::bench_cli::parse_args(static_cast<int>(argv.size()), argv.data());
}

bool rejects(std::initializer_list<std::string> values) {
  try {
    parse(values);
    return false;
  } catch (const std::invalid_argument&) {
    return true;
  }
}
}  // namespace

int main() {
  const auto cli = parse({"--dataset", "locomo", "--limit", "10", "--k", "4",
                         "--concurrency", "2", "--entity-pass-threshold", "-1",
                         "--final-alpha", "0.25", "--final-beta", "0.75",
                         "--abstain-categories", "5,12,-3", "--entity-profile", "off"});
  CHECK(cli.dataset == "locomo");
  CHECK(cli.limit == 10 && cli.cfg.k == 4 && cli.cfg.concurrency == 2);
  CHECK(cli.entity_pass == -1);
  CHECK(cli.has_final && cli.cfg.final_mix);
  CHECK_NEAR(cli.cfg.final_alpha, 0.25, 1e-12);
  CHECK_NEAR(cli.cfg.final_beta, 0.75, 1e-12);
  CHECK((cli.cfg.abstention_categories == std::vector<int>{5, 12, -3}));
  CHECK(!cli.cfg.use_entity_profile);
  CHECK(parse({"--limit", "1", "--limit", "2"}).limit == 2);
  CHECK(parse({"--unknown", "value", "--limit", "3"}).limit == 3);
  CHECK(rejects({"--limit"}));
  CHECK(rejects({"--dataset", "--data", "fixture.json"}));
  for (const auto& option : {"--k", "--agg-batch"}) {
    CHECK(rejects({option, "0"}));
    CHECK(rejects({option, "-1"}));
  }
  CHECK(parse({"--limit", "0"}).limit == 0);
  CHECK(parse({"--limit", "-1"}).limit == -1);
  for (const auto& value : {"", "no", "12junk", "1.5", "2147483648", "-2147483649"}) {
    CHECK(rejects({"--limit", value}));
  }
  for (const auto& value : {"", "NaN", "inf", "-inf", "1e9999", "0.5junk"}) {
    CHECK(rejects({"--final-alpha", value}));
  }
  for (const auto& value : {"", ",5", "5,", "5,,6", "5,6oops", "5,9999999999999"}) {
    CHECK(rejects({"--abstain-categories", value}));
  }
  return AME_TEST_REPORT();
}
