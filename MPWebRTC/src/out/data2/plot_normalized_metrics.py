#!/usr/bin/env python3
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib import font_manager as fm
DATA_DIR = Path(__file__).resolve().parent
OUT_DIR = DATA_DIR / "plots"

LINE_AVG_INDEX = 91  # 1-based line 92
LINE_STALL_INDEX = 90  # 1-based line 91

THROUGHPUT_MAX_BPS = 5_000_000.0
FPS_MAX = 30.0
QP_MAX = 80.0
STALL_DURATION_S = 180.0

METRICS = ["吞吐量", "帧率", "QP", "卡顿时间"]
# 从左到右：原生WebRTC、轮询方案、本文方案
BRANCH_ORDER = ["native", "rr", "extend"]
BRANCH_STYLE = {
    "native": ("原生WebRTC", "#1f77b4", "xx"),
    "rr": ("轮询方案", "#2ca02c", "\\\\"),
    "extend": ("本文方案", "#ff7f0e", "//"),
}


def clamp01(v: float) -> float:
    return max(0.0, min(1.0, v))


def branch_key_from_suffix(suffix: str) -> str:
    lower = suffix.lower()
    if lower.startswith("native"):
        return "native"
    if lower.startswith("extend"):
        return "extend"
    if lower.startswith("rr"):
        return "rr"
    return "unknown"


def configure_chinese_font():
    preferred_keywords = [
        "notosanscjk",
        "notoserifcjk",
        "sourcehansans",
        "sourcehanserif",
        "wqy-zenhei",
        "wenquanyi",
        "simhei",
        "microsoftyahei",
        "msyh",
        "pingfang",
    ]
    search_dirs = [
        Path.home() / ".fonts",
        Path.home() / ".local" / "share" / "fonts",
        Path("/usr/share/fonts"),
        Path("/usr/local/share/fonts"),
    ]

    candidates = []
    for d in search_dirs:
        if not d.exists():
            continue
        for pattern in ("*.ttf", "*.otf", "*.ttc"):
            candidates.extend(d.rglob(pattern))

    selected_font_name = None
    for path in candidates:
        lower_name = path.name.lower().replace(" ", "")
        if any(keyword in lower_name for keyword in preferred_keywords):
            try:
                fm.fontManager.addfont(str(path))
                selected_font_name = fm.FontProperties(fname=str(path)).get_name()
                break
            except Exception:
                continue

    if selected_font_name:
        plt.rcParams["font.sans-serif"] = [selected_font_name, "DejaVu Sans"]
        print(f"[font] using local chinese font: {selected_font_name}")
    else:
        plt.rcParams["font.sans-serif"] = [
            "Noto Sans CJK SC",
            "WenQuanYi Zen Hei",
            "SimHei",
            "Microsoft YaHei",
            "DejaVu Sans",
        ]
        print("[font] no local chinese font file found, using fallback names")
    plt.rcParams["axes.unicode_minus"] = False
    plt.rcParams["font.size"] = 20
    plt.rcParams["axes.labelsize"] = 20
    plt.rcParams["xtick.labelsize"] = 20
    plt.rcParams["ytick.labelsize"] = 20
    plt.rcParams["legend.fontsize"] = 20


def read_metrics(csv_path: Path):
    with csv_path.open("r", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    if len(rows) <= LINE_AVG_INDEX:
        raise ValueError(f"{csv_path.name}: 行数不足 92 行")
    if len(rows) <= LINE_STALL_INDEX:
        raise ValueError(f"{csv_path.name}: 行数不足 91 行")

    avg_row = rows[LINE_AVG_INDEX]
    stall_row = rows[LINE_STALL_INDEX]
    if len(avg_row) < 5:
        raise ValueError(f"{csv_path.name}: 第92行列数不足 5 列")
    if len(stall_row) < 4:
        raise ValueError(f"{csv_path.name}: 第91行列数不足 4 列")

    throughput_bps = float(avg_row[1])
    fps = float(avg_row[2])
    qp = float(avg_row[4])
    stall_time_s = float(stall_row[3])

    normalized = [
        clamp01(throughput_bps / THROUGHPUT_MAX_BPS),
        clamp01(fps / FPS_MAX),
        clamp01(qp / QP_MAX),
        clamp01(stall_time_s / STALL_DURATION_S),
    ]
    labels = [
        f"{throughput_bps / 1_000_000:.1f}",
        f"{fps:.1f}",
        f"{qp:.1f}",
        f"{stall_time_s / STALL_DURATION_S * 100:.1f}",
    ]
    return normalized, labels


def main():
    configure_chinese_font()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    grouped = {}
    for csv_path in sorted(DATA_DIR.glob("*.csv")):
        name = csv_path.name
        if "," not in name or name.startswith("."):
            continue
        prefix, suffix_with_ext = name.split(",", 1)
        suffix = suffix_with_ext.rsplit(".", 1)[0]
        branch_key = branch_key_from_suffix(suffix)
        if branch_key == "unknown":
            continue
        grouped.setdefault(prefix, {})[branch_key] = csv_path

    if not grouped:
        raise RuntimeError("未在 out/data2 下找到可用 CSV")

    generated = 0
    for prefix, branch_files in sorted(grouped.items()):
        if not all(k in branch_files for k in BRANCH_ORDER):
            print(f"[skip] {prefix}: 分支不完整，已有 {sorted(branch_files.keys())}")
            continue

        normalized_values = {}
        label_values = {}
        for branch in BRANCH_ORDER:
            normalized, labels = read_metrics(branch_files[branch])
            normalized_values[branch] = normalized
            label_values[branch] = labels

        # x 轴从左到右：吞吐量、帧率、QP、卡顿时间；每组三根柱为三种方案。
        x = np.arange(len(METRICS))
        width = 0.2

        plt.figure(figsize=(10.5, 5.0))
        max_bar_height = 0.0
        for i, branch in enumerate(BRANCH_ORDER):
            legend_label, color, hatch = BRANCH_STYLE[branch]
            bars = plt.bar(
                x + (i - 1) * width,
                normalized_values[branch],
                width=width,
                label=legend_label,
                color=color,
                hatch=hatch,
                edgecolor="black",
                linewidth=0.8,
            )
            for bar, text in zip(bars, label_values[branch]):
                height = bar.get_height()
                max_bar_height = max(max_bar_height, height)
                plt.text(
                    bar.get_x() + bar.get_width() / 2,
                    height + 0.02,
                    text,
                    ha="center",
                    va="bottom",
                    fontsize=16,
                )

        plt.ylim(0, max(1.2, max_bar_height + 0.12))
        plt.xticks(x, METRICS)
        plt.ylabel("归一化值")
        plt.grid(axis="y", linestyle="--", linewidth=0.6, alpha=0.5)
        plt.legend()
        plt.tight_layout()

        out_file = OUT_DIR / f"{prefix}_normalized_bar.png"
        plt.savefig(out_file, dpi=180)
        plt.close()
        generated += 1
        print(f"[ok] {out_file}")

    print(f"[done] generated {generated} figure(s)")


if __name__ == "__main__":
    main()
