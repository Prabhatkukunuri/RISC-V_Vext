"""Chart for run_benchmarks.py (needs matplotlib): instructions executed, scalar vs. vector."""
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
SCALAR, VECTOR = "#2a78d6", "#eb6834"  # categorical slots 1 and 2

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 10, "text.color": INK,
    "axes.labelcolor": INK_2, "axes.edgecolor": AXIS, "xtick.color": MUTED,
    "ytick.color": INK_2, "figure.facecolor": SURFACE, "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
})


def instructions_chart(results, path):
    rows = results[::-1]
    h = 0.36
    fig, ax = plt.subplots(figsize=(9, 4.2))
    scalar = [e["scalar"]["instructions"] for e in rows]
    vector = [e["vector"]["instructions"] for e in rows]
    ys = range(len(rows))
    ax.barh([i + h / 2 for i in ys], scalar, height=h - 0.04, color=SCALAR, label="Scalar")
    ax.barh([i - h / 2 for i in ys], vector, height=h - 0.04, color=VECTOR, label="Vector")
    ax.set_xscale("log")
    ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:,.0f}"))
    for i, (s, v) in enumerate(zip(scalar, vector)):
        ax.text(s * 1.08, i + h / 2, f"{s:,}", va="center", fontsize=9, color=INK_2)
        ax.text(v * 1.08, i - h / 2, f"{v:,}   {s / v:.1f}x speedup", va="center",
                fontsize=9, color=INK)
    ax.set_yticks(list(ys), [e["title"] for e in rows])
    ax.set_xlim(10, max(scalar) * 8)
    ax.set_xlabel("Assembly instructions executed (log scale)")
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    ax.grid(axis="x", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)
    leg = ax.legend(frameon=False, ncol=2, loc="lower left", bbox_to_anchor=(0, 1.0),
                    handlelength=1.0)
    for text in leg.get_texts():
        text.set_color(INK_2)
    fig.suptitle("Instructions executed: scalar vs. vector", x=0.01, ha="left",
                 fontsize=12.5, fontweight="bold", y=0.99)
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)
