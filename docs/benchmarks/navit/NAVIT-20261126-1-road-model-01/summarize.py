#!/usr/bin/env python3
"""Summarise workflow-*.csv (NAVIT-20261126-1-road-model-01).

usage: python3 -I summarize.py evidence/workflow-baseline.csv evidence/workflow-A.csv evidence/workflow-B.csv
Groups: long (8 inter-city cases) / city (20 pairs), per mode. Median and P95
(nearest-rank) of plan_ms over all repeats; route length ratio vs the first file.
"""
import csv
import math
import statistics
import sys


def load(path):
    rows = [r for r in csv.DictReader(l for l in open(path, encoding="utf-8") if not l.startswith("#"))]
    for r in rows:
        r["group"] = "city" if r["case"].startswith("城区") else "long"
    return rows


def p95(v):
    v = sorted(v)
    return v[max(0, math.ceil(0.95 * len(v)) - 1)]


def main(paths):
    data = {p: load(p) for p in paths}
    base = data[paths[0]]
    base_len = {(r["case"], r["mode"]): float(r["route_km"]) for r in base if r["ok"] == "1"}
    print("file,group,mode,runs,ok,plan_med_ms,plan_p95_ms,alg_med_ms,len_ratio_med,len_ratio_min,len_ratio_max")
    for p, rows in data.items():
        for g in ("city", "long"):
            for m in ("general", "driving"):
                sel = [r for r in rows if r["group"] == g and r["mode"] == m]
                ok = [r for r in sel if r["ok"] == "1"]
                plan = [float(r["plan_ms"]) for r in ok]
                alg = [float(r["algorithm_ms"]) for r in ok]
                ratios = [float(r["route_km"]) / base_len[(r["case"], m)] for r in ok
                          if base_len.get((r["case"], m))]
                print(f"{p.split('/')[-1]},{g},{m},{len(sel)},{len(ok)},"
                      f"{statistics.median(plan):.1f},{p95(plan):.1f},{statistics.median(alg):.2f},"
                      f"{statistics.median(ratios):.5f},{min(ratios):.5f},{max(ratios):.5f}")


if __name__ == "__main__":
    main(sys.argv[1:])
