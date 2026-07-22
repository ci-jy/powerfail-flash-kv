#!/usr/bin/env python3
"""Plot wear-benchmark results (docs/wear_results.json) to docs/wear_results.png."""
import json
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
data = json.loads((ROOT / "docs" / "wear_results.json").read_text())
variants = data["variants"]
geo = data["geometry"]
colors = ["#c0392b", "#7f8c8d", "#e67e22", "#27ae60"]

fig, axes = plt.subplots(1, 3, figsize=(15, 4.6))

ax = axes[0]
n = len(variants)
width = 0.8 / (n - 1)
for i, v in enumerate(variants[1:], start=1):
    xs = [s + (i - 1) * width - 0.4 + width / 2 for s in range(geo["sectors"])]
    ax.bar(xs, v["erase_counts"], width, label=v["name"], color=colors[i])
ax.set_xlabel("sector")
ax.set_ylabel(f"erase count after {data['fixed_writes']:,} writes")
ax.set_title("Erase-count distribution (log store variants)")
ax.legend(fontsize=8)

ax = axes[1]
names = [v["name"].replace("pfkv ", "pfkv\n").replace(" + ", "\n+ ") for v in variants]
wa = [v["write_amplification"] for v in variants]
bars = ax.bar(names, wa, color=colors)
ax.set_yscale("log")
ax.set_ylabel("write amplification (flash bytes / user bytes)")
ax.set_title("Write amplification")
for b, val in zip(bars, wa):
    ax.text(b.get_x() + b.get_width() / 2, val, f"{val:.1f}", ha="center", va="bottom", fontsize=9)
ax.tick_params(axis="x", labelsize=8)

ax = axes[2]
life = [v["writes_to_first_wearout"] for v in variants]
bars = ax.bar(names, life, color=colors)
ax.set_yscale("log")
ax.set_ylabel(f"writes until a sector hits {geo['endurance']:,} erases")
ax.set_title("Lifetime to first worn-out sector")
for b, val in zip(bars, life):
    ax.text(b.get_x() + b.get_width() / 2, val, f"{val:,}", ha="center", va="bottom", fontsize=8)
ax.tick_params(axis="x", labelsize=8)

fig.suptitle(
    f"{geo['sectors']} x {geo['sector_size']} B NOR sectors, 96 static calibration keys + hot counters",
    fontsize=11,
)
fig.tight_layout()
out = ROOT / "docs" / "wear_results.png"
fig.savefig(out, dpi=110)
print(f"wrote {out}")
