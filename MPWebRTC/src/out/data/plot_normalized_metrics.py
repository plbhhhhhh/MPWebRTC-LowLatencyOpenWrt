#!/usr/bin/env python3
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib import font_manager as fm

DATA_DIR = Path(__file__).resolve().parent
OUT_DIR = DATA_DIR / "plots"
TARGET_LINE_INDEX = 91  # 1-based line 92

THROUGHPUT_MAX_BPS = 4_000_000.0
QP_MAX = 50.0
STALL_MAX = 2.0

METRICS = ["吞吐量", "QP", "卡顿时间"]
BRANCH_ORDER = ["native", "extend", "rr"]
BRANCH_STYLE = {
    "native": ("单路径WebRTC", "#1f77b4", "xx"),
    "extend": ("本文方案", "#ff7f0e", "//"),
    "rr": ("轮询方案", "#2ca02c", "\\\\"),
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
        # Fallback to common names; may still fail if fonts are absent.
        plt.rcParams["font.sans-serif"] = [
            "Noto Sans CJK SC",
            "WenQuanYi Zen Hei",
            "SimHei",
            "Microsoft YaHei",
            "DejaVu Sans",
        ]
        print("[font] no local chinese font file found, using fallback names")
    plt.rcParams["axes.unicode_minus"] = False
    # Enlarge all text to 2x for better readability in exported figures.
    plt.rcParams["font.size"] = 20
    plt.rcParams["axes.labelsize"] = 20
    plt.rcParams["xtick.labelsize"] = 20
    plt.rcParams["ytick.labelsize"] = 20
    plt.rcParams["legend.fontsize"] = 20


def read_line92_metrics(csv_path: Path):
    with csv_path.open("r", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    if len(rows) <= TARGET_LINE_INDEX:
        raise ValueError(f"{csv_path.name}: 行数不足 92 行")
    row = rows[TARGET_LINE_INDEX]
    if len(row) < 5:
        raise ValueError(f"{csv_path.name}: 第92行列数不足 5 列")

    throughput_bps = float(row[1])
    stall_time = float(row[3])
    qp = float(row[4])

    return [
        clamp01(throughput_bps / THROUGHPUT_MAX_BPS),
        clamp01(qp / QP_MAX),
        clamp01(stall_time / STALL_MAX),
    ]


def main():
    configure_chinese_font()

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    grouped = {}
    for csv_path in sorted(DATA_DIR.glob("*.csv")):
        name = csv_path.name
        if "," not in name:
            continue
        prefix, suffix_with_ext = name.split(",", 1)
        suffix = suffix_with_ext.rsplit(".", 1)[0]
        branch_key = branch_key_from_suffix(suffix)
        if branch_key == "unknown":
            continue
        grouped.setdefault(prefix, {})[branch_key] = csv_path

    if not grouped:
        raise RuntimeError("未在 out/data 下找到可用 CSV")

    for prefix, branch_files in grouped.items():
        if not all(k in branch_files for k in BRANCH_ORDER):
            print(f"[skip] {prefix}: 分支不完整，已有 {sorted(branch_files.keys())}")
            continue

        values = {
            branch: read_line92_metrics(branch_files[branch]) for branch in BRANCH_ORDER
        }

        x = np.arange(len(METRICS))
        width = 0.24

        plt.figure(figsize=(8.6, 4.8))
        for i, branch in enumerate(BRANCH_ORDER):
            label, color, hatch = BRANCH_STYLE[branch]
            plt.bar(
                x + (i - 1) * width,
                values[branch],
                width=width,
                label=label,
                color=color,
                hatch=hatch,
                edgecolor="black",
                linewidth=0.8,
            )

        plt.ylim(0, 1.05)
        plt.xticks(x, METRICS)
        plt.ylabel("归一化值")
        plt.grid(axis="y", linestyle="--", linewidth=0.6, alpha=0.5)
        plt.legend()
        plt.tight_layout()

        out_file = OUT_DIR / f"{prefix}_normalized_bar.png"
        plt.savefig(out_file, dpi=180)
        plt.close()
        print(f"[ok] {out_file}")


if __name__ == "__main__":
    main()
