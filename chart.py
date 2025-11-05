import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

# --- 설정 ---
CSV_PATH = "10_nocache.csv"
OUT_DIR = "charts"
os.makedirs(OUT_DIR, exist_ok=True)

# --- 데이터 로드 ---
df = pd.read_csv(CSV_PATH)

# 숫자형 보정(필요 시)
num_cols = [
    "block_num", "iteration", "num_block_copies", "file_size",
    "fiemap_time", "rpc_time", "io_time", "client_total_time", "baseline_time"
]
time_cols = ["fiemap_time", "rpc_time", "io_time", "client_total_time", "baseline_time"]

for c in num_cols:
    if c in df.columns:
        df[c] = pd.to_numeric(df[c], errors="coerce")

# (file_size, num_block_copies) 조합별 처리
groups = df.groupby(["file_size", "num_block_copies"], dropna=False)

for (fs, nbc), g in groups:
    # 1) 같은 (block_num, iteration)에서 중복 레코드가 있다면 먼저 평균으로 압축
    per_iter = (
        g.groupby(["block_num", "iteration"], as_index=False)[time_cols]
         .mean()
    )

    # 2) 그 다음, iteration을 지우고 block_num 단위 평균 (iteration 간 공평 가중)
    agg = (
        per_iter.groupby("block_num", as_index=False)[["fiemap_time", "rpc_time", "io_time", "baseline_time", "client_total_time"]]
                .mean()
                .sort_values("block_num")
    )
    if agg.empty:
        continue

    block_nums = agg["block_num"].to_numpy()
    fiemap = agg["fiemap_time"].to_numpy()
    rpc = agg["rpc_time"].to_numpy()
    io = agg["io_time"].to_numpy()
    base = agg["baseline_time"].to_numpy()
    total_left = fiemap + rpc + io

    x = np.arange(len(block_nums), dtype=float)
    width = 0.35

    # 좌측(스택: fiemap/rpc/io), 우측(baseline)
    fig, ax = plt.subplots(figsize=(12, 6))

    left_pos = x - width/2
    right_pos = x + width/2

    p1 = ax.bar(left_pos, fiemap, width, label="fiemap_time")
    p2 = ax.bar(left_pos, rpc, width, bottom=fiemap, label="rpc_time")
    p3 = ax.bar(left_pos, io, width, bottom=fiemap+rpc, label="io_time")
    p4 = ax.bar(right_pos, base, width, label="baseline_time")


    ax.bar_label(p1, labels=[f"{v:.2f}" for v in fiemap], label_type="center", fontsize=8)
    ax.bar_label(p2, labels=[f"{v:.2f}" for v in rpc], label_type="center", fontsize=8)
    ax.bar_label(p3, labels=[f"{v:.2f}" for v in io], label_type="center", fontsize=8)

    for xpos, total in zip(left_pos, total_left):
        ax.text(
            xpos, total + (max(total_left) * 0.01),  # 막대 위쪽 약간 위
            f"{total:.2f}",
            ha="center", va="bottom", fontsize=9, fontweight="bold", color="black"
        )


    ax.bar_label(p4, labels=[f"{v:.2f}" for v in base], label_type="edge", fontsize=8, padding=3)

    # 축/라벨/제목
    ax.set_title(f"file_size={fs} GiB, num_block_copies={nbc}")
    ax.set_xlabel("block_num")
    ax.set_ylabel("time (s)")
    ax.set_xticks(x)
    ax.set_xticklabels(block_nums.astype(int), rotation=0)
    ax.legend(loc="best")

    ax.grid(axis="y", linestyle="--", alpha=0.3)

    plt.tight_layout()

    # 파일 저장
    safe_fs = str(fs).replace(".", "_")
    safe_nbc = str(nbc).replace(".", "_")
    out_path = os.path.join(OUT_DIR, f"chart_fs{safe_fs}_nbc{safe_nbc}.png")
    plt.savefig(out_path, dpi=150)
    plt.close(fig)

print(f"그래프 저장 완료: {OUT_DIR}/ 아래에 PNG 파일들이 생성되었습니다.")
