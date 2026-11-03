// 三本词典合并灌库耗时与幂等实测（lexicon_zh + zh2 + hf 20万词）
#include <chrono>
#include <cstdio>
#include "ame/bench/bench.h"
#include "ame/engine/engine.h"
using namespace ame;
static double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
int main() {
  Engine e;
  e.remember("我昨晚又睡不着了");
  e.remember("老张今天很愉快");
  const char* base = "/mnt/d/agent/memory/ame/bench/data/";
  double t0 = now();
  int a1 = 0, a2 = 0, a3 = 0;
  apply_lexicon(e, std::string(base) + "lexicon_zh.json", &a1);
  double t1 = now();
  apply_lexicon(e, std::string(base) + "lexicon_zh2.json", &a2);
  double t2 = now();
  apply_lexicon(e, std::string(base) + "lexicon_hf.json", &a3);
  double t3 = now();
  printf("zh:   %8d entries %5.1fs\n", a1, t1 - t0);
  printf("zh2:  %8d entries %5.1fs\n", a2, t2 - t1);
  printf("hf:   %8d entries %5.1fs\n", a3, t3 - t2);
  size_t edges = e.storage().edge_count();
  double t4 = now();
  apply_lexicon(e, std::string(base) + "lexicon_hf.json", nullptr);
  double t5 = now();
  printf("hf second import: %.1fs edges %zu -> %zu (idempotent=%s)\n", t5 - t4, edges,
         e.storage().edge_count(), edges == e.storage().edge_count() ? "yes" : "no");
  printf("normalize(unable to sleep)=%s (expected insomnia)\n", e.keywords().normalize("睡不着").c_str());
  printf("normalize(hair processing)=%s normalize(insomnia)=%s\n", e.keywords().normalize("制发").c_str(),
         e.keywords().normalize("失眠").c_str());
  return 0;
}
