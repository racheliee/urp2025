import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import sys
from datetime import datetime

def latest_log_folder():
    if "finegrained_read_logs" not in os.listdir("../testing"):
        print("no 'finegrained_read_logs' folder in testing directory")
        sys.exit()

    log_folders = os.listdir("../testing/finegrained_read_logs")
    log_folders.sort(reverse=True)

    if log_folders:
        latest_log_folder=log_folders[0]
        return latest_log_folder
    else:
        latest_log=None
        print("Nothing saved in 'finegrained_read_logs' folder")
        return None

# --- 설정 ---
MAIN_TITLE = "Finegrained Read Performance (per copy)"  # ← 여기서 제목을 직접 정하세요
LOG_DIR = "../testing/finegrained_read_logs/" + latest_log_folder() #baseline과 RPC 로그가  위치한 folder
BASELINE_CSV = LOG_DIR+"/"+"baseline.log"
RPC_CSV = LOG_DIR+"/"+"rpc.log"
OUT_DIR = "./outputs"
os.makedirs(OUT_DIR, exist_ok=True)

AUTO_DETECT_NS = False    # 중앙값이 크면(ns) 초로 변환
FORCE_NS_TO_S  = False   # True면 *_ns를 무조건 /1e9

# 스택 컬럼 순서 (요청 순서)
PARTS = ["read_ns", "write_ns", "io_ns", "fiemap_ns", "rpc_ns", "other_ns"]

# Matplotlib 기본 color cycle을 PARTS에 고정 매핑
_DEFAULT_CYCLE = plt.rcParams['axes.prop_cycle'].by_key().get('color', [])
if not _DEFAULT_CYCLE:
    _DEFAULT_CYCLE = ["#1f77b4","#ff7f0e","#2ca02c","#d62728",
                      "#9467bd","#8c564b","#e377c2","#7f7f7f",
                      "#bcbd22","#17becf"]
COLOR_MAP = {part: _DEFAULT_CYCLE[i % len(_DEFAULT_CYCLE)] for i, part in enumerate(PARTS)}

# ------ 유틸 ------
def to_numeric(df, cols):
    for c in cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")
    return df

def ns_to_s_if_needed_df(df, cols):
    for col in cols:
        if col not in df.columns:
            continue
        if FORCE_NS_TO_S:
            df[col] = df[col] / 1e9
        elif AUTO_DETECT_NS:
            med = np.nanmedian(df[col].to_numpy(dtype=float))
            if np.isfinite(med) and med > 1e6:  # ns로 판단
                df[col] = df[col] / 1e9
    return df

def ensure_columns(df, cols, fill=0.0):
    for c in cols:
        if c not in df.columns:
            df[c] = fill
    return df

# ------ CSV 로드 ------
baseline = pd.read_csv(BASELINE_CSV)
rpc      = pd.read_csv(RPC_CSV)

to_numeric(baseline, ["block_num","iteration","num_block_copies","file_size","total_time"])
to_numeric(rpc,      ["block_num","iteration","num_block_copies","file_size","total_time"])

# RPC → 공통 이름으로 리매핑
rpc.rename(columns={
    "server_read_ns": "read_ns",
    "server_write_ns": "write_ns",
    "server_other_ns": "other_ns",
}, inplace=True)

# 누락 가능 컬럼들 생성(0으로)
maybe_cols = ["read_ns","write_ns","io_ns","fiemap_ns","rpc_ns","other_ns","prep_ns","end_ns"]
baseline = ensure_columns(baseline, maybe_cols)
rpc      = ensure_columns(rpc,      maybe_cols)

# 단위 보정 (합치기 전에 각 성분을 초로 맞춤)
ns_to_s_if_needed_df(baseline, maybe_cols + ["total_time"])
ns_to_s_if_needed_df(rpc,      maybe_cols + ["total_time"])

# other_ns <- other_ns + prep_ns + end_ns (요청사항)
baseline["other_ns"] = baseline[["other_ns","prep_ns","end_ns"]].sum(axis=1, skipna=True)
rpc["other_ns"]      = rpc[["other_ns","prep_ns","end_ns"]].sum(axis=1, skipna=True)

# 최종적으로 사용할 컬럼만 보장
baseline = ensure_columns(baseline, PARTS)
rpc      = ensure_columns(rpc, PARTS)

# 키 숫자형 안전화
for df in (baseline, rpc):
    for c in ["block_num","iteration","num_block_copies","file_size"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")

# ------ 그룹핑 ------
group_keys = ["file_size", "num_block_copies"]
b_groups = baseline.groupby(group_keys, dropna=False)
r_groups = rpc.groupby(group_keys, dropna=False)
common_groups = sorted(set(b_groups.groups.keys()) & set(r_groups.groups.keys()))

# ------ 메인 루프 ------
for (fs, nbc) in common_groups:
    b_g = b_groups.get_group((fs, nbc)).copy()
    r_g = r_groups.get_group((fs, nbc)).copy()

    # (block_num, iteration) 평균 → block_num 평균
    b_per_iter = b_g.groupby(["block_num","iteration"], as_index=False)[PARTS].mean().fillna(0.0)
    r_per_iter = r_g.groupby(["block_num","iteration"], as_index=False)[PARTS].mean().fillna(0.0)

    b_agg = b_per_iter.groupby("block_num", as_index=False)[PARTS].mean().fillna(0.0)
    r_agg = r_per_iter.groupby("block_num", as_index=False)[PARTS].mean().fillna(0.0)

    # 공통 block_num만
    agg = pd.merge(r_agg, b_agg, on="block_num", how="inner", suffixes=("_rpc", "_base")).sort_values("block_num")
    if agg.empty:
        continue

    block_nums = agg["block_num"].to_numpy()
    x = np.arange(len(block_nums), dtype=float)+1
    

    # 막대 간 여백
    width  = 0.30          # 막대 폭
    offset = width * 0.70  # 좌/우 간격

    # 스택 데이터
    rpc_stack  = [agg[p + "_rpc"].to_numpy()  for p in PARTS]
    base_stack = [agg[p + "_base"].to_numpy() for p in PARTS]
    rpc_total  = np.sum(rpc_stack, axis=0)
    base_total = np.sum(base_stack, axis=0)

    # ----- Plot -----
    fig, ax = plt.subplots(figsize=(12, 6))
    # 상단 제목(메인 타이틀)
    if MAIN_TITLE:
        fig.suptitle(MAIN_TITLE, fontsize=14, fontweight="bold", y=0.97)

    left_pos  = x - offset
    right_pos = x + offset

    def plot_stack(xs, stacks, width, parts):
        bottoms = np.zeros_like(stacks[0])
        for vals, part in zip(stacks, parts):
            bars = ax.bar(
                xs,
                vals,
                width,
                bottom=bottoms,
                color=COLOR_MAP[part],  # 동일 컬럼 동일 색
                label=part
            )
            
            # 각 스택 조각 값: (col: val)s 형식, 검은색, bold 아님
            for xi, v, bot in zip(xs, vals, bottoms):
                if v > 0 and part in ['read_ns', 'write_ns', 'rpc_ns']: #지분 큰 요소만
                    ax.text(
                        xi, bot + v/2.0,
                        f"({part[:part.index('_')]}: {v:.2f}$\mu s$)",
                        ha="center", va="center",
                        color="black", fontsize=8
                    )
            
            bottoms += vals
        return bottoms  # 최종 높이 반환

    # 좌측: RPC 스택
    top_rpc = plot_stack(left_pos, rpc_stack, width, PARTS)

    # 우측: BASELINE 스택
    top_base = plot_stack(right_pos, base_stack, width, PARTS)

    # 막대 맨 위에 전체 시간(검은색 Bold, s 단위)
    ymax = float(np.nanmax([top_rpc.max() if len(top_rpc) else 0, top_base.max() if len(top_base) else 0, 0.0]))
    bump = ymax * 0.015 if ymax > 0 else 0.01
    
    for xpos, total in zip(left_pos, rpc_total):
        ax.text(xpos, total + bump, f"{total:.2f}$\mu s$", ha="center", va="bottom",
                color="black", fontsize=10, fontweight="bold")
    for xpos, total in zip(right_pos, base_total):
        ax.text(xpos, total + bump, f"{total:.2f}$\mu s$", ha="center", va="bottom",
                color="black", fontsize=10, fontweight="bold")
    
    # 축 / 서브타이틀 / 격자
    ax.set_title(f"file_size={fs} GiB · total_bytes={nbc}", fontsize=11)
    ax.set_xlabel("bytes")
    ax.set_ylabel("time ($\mu s$)")

    # x축: block_num + 아래 줄에 L/R 안내
    #ax.set_xticklabels(x)
    ax.set_xticks(x)
    ax.set_xticklabels([f"{int(bn)}" for bn in block_nums])
    #ax.set_xticklabels([f"{int(bn)}\nL: RPC | R: BASELINE" for bn in block_nums])

    ax.grid(axis="y", linestyle="--", alpha=0.3)

    # 하단 전역 안내(여백 아래에도 한번 더)
    ax.annotate("Left: RPC   |   Right: BASELINE",
                xy=(0.5, -0.18), xycoords="axes fraction",
                ha="center", va="center", fontsize=9)

    # legend 중복 제거
    handles, labels = ax.get_legend_handles_labels()
    uniq = dict(zip(labels, handles))
    ax.legend(uniq.values(), uniq.keys())

    plt.tight_layout(rect=[0, 0.03, 1, 0.94])  # suptitle/annotation 여백
    safe_fs = str(int(fs))
    safe_nbc = str(nbc).replace(".", "_")
    out_path = os.path.join(OUT_DIR, f"chart_fs{safe_fs}GB_bytes{safe_nbc}-finegrained-read-{str(datetime.now().isoformat())[:10]}.png")
    plt.savefig(out_path, dpi=150)
    plt.close(fig)

print(f"그래프 생성 완료 → {OUT_DIR}/")


