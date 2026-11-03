#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 两阶段聚合降本组合 A/B 配对分析：
#   a 组基线 = 逐条 top-k=10（lme_agg5_two_stage.ndjson，5 题子集）
#   c 组     = 预过滤(N=20)+批量提取(5条/次)（lme_agg10_batch.ndjson，14 题）
# 输出：每题 judge 对比翻转表 + 汇总；另从 c 组 ndjson 统计调用量字段（若有）。
import json, sys, os

RUNS = os.path.join(os.path.dirname(__file__), "..", "runs")

def load_ndjson(name):
    path = os.path.join(RUNS, name)
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            qid = r.get("question_id") or r.get("id")
            if qid:
                out[qid] = r
    return out

a = load_ndjson("lme_agg5_two_stage.ndjson")
c = load_ndjson("lme_agg10_batch.ndjson")

def judge_of(r):
    j = r.get("judge")
    if isinstance(j, dict):
        return j.get("score", j.get("correct"))
    return j

print(f"group_a_questions={len(a)}  group_c_questions={len(c)}")
print(f"{'question_id':<22} {'a(turn-top10)':>12} {'c(pre+batch)':>12}  flips")
flips_up, flips_down, both = 0, 0, 0
a_scores, c_scores = [], []
for qid in sorted(set(a) | set(c)):
    ja = judge_of(a[qid]) if qid in a else None
    jc = judge_of(c[qid]) if qid in c else None
    mark = ""
    if ja is not None and jc is not None:
        a_scores.append(ja); c_scores.append(jc)
        if jc > ja: flips_up += 1; mark = "up: improved"
        elif jc < ja: flips_down += 1; mark = "down: degraded"
        else: mark = "= unchanged"
    print(f"{qid:<22} {str(ja):>12} {str(jc):>12}  {mark}")
print()
if a_scores:
    print(f"paired_questions={len(a_scores)}  group_a_mean_judge={sum(a_scores)/len(a_scores):.3f}  group_c_mean_judge={sum(c_scores)/len(c_scores):.3f}")
    print(f"Improved {flips_up} questions / degraded {flips_down} questions")
if c:
    allc = [judge_of(r) for r in c.values() if judge_of(r) is not None]
    if allc:
        print(f"group c total ({len(allc)} questions), mean judge={sum(allc)/len(allc):.3f}  hits={sum(1 for x in allc if x>0)}")
