#!/usr/bin/env bash
set -Eeuo pipefail

# ----- 파라미터 세트 -----
byte_sizes=(8 16 32 64 128)
block_copies=(1048576)
file_sizes=(30)   # GiB
seed=-1

HOME_DIR="${HOME_DIR:-../finegrained_block_read}"

LOG_DIR="${LOG_DIR:-./write_logs}"
mkdir -p "$LOG_DIR"

DATE_DIR="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$LOG_DIR/$DATE_DIR"
mkdir -p "$RUN_DIR"

BASELINE_LOG_FILE="$RUN_DIR/baseline.log"
RPC_LOG_FILE="$RUN_DIR/rpc.log"
echo "로그 디렉토리: $RUN_DIR"

mnt="/mnt/nvme1"
a="$mnt/a.txt"
b="$mnt/b.txt"

gcc $HOME_DIR/create_file.c -o $HOME_DIR/create_file

# ----- 필요 명령/바이너리 점검 -----
need_cmds=(sudo cp sync)
for c in "${need_cmds[@]}"; do
  command -v "$c" >/dev/null 2>&1 || { echo "필요 명령 없음: $c" >&2; exit 1; }
done
for bin in $HOME_DIR/create_file $HOME_DIR/client_write $HOME_DIR/baseline_write ./compare; do
  [[ -x "$bin" ]] || { echo "실행 파일 없음 또는 실행 불가: $bin" >&2; exit 1; }
done

# ----- 캐시 동기화/정리 -----
# 시스템 전체에 영향. 원치 않으면: DROP_CACHES=0 ./test.sh
DROP_CACHES="${DROP_CACHES:-1}"
flush_caches() {
  sudo sync
  if [[ "$DROP_CACHES" == "1" ]]; then
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    echo "Cache flushed"
  fi
}

echo "=== 테스트 시작 ==="
echo "params: seed=$seed, block_nums=${block_nums[*]}, block_copies=${block_copies[*]}, file_sizes=${file_sizes[*]}"
echo

echo "block_num,iteration,num_block_copies,file_size,read_ns,write_ns,io_ns,total_time" >> "$BASELINE_LOG_FILE"
echo "block_num,iteration,num_block_copies,file_size,server_read_ns,server_write_ns,server_other_ns,fiemap_ns,rpc_ns,io_ns,total_time" >> "$RPC_LOG_FILE"

# (1) 파일 생성 및 복제(초기화)
fs=${file_sizes[0]}
echo "[create] sudo ./create_file $a $fs --gib"
sudo $HOME_DIR/create_file "$a" "$fs" --gib
flush_caches

echo "[copy] sudo cp $a $b"
sudo cp "$a" "$b"
flush_caches


for round in $(seq 1 35); do
    echo "================ ROUND $round / 35 ================"

    for fs in "${file_sizes[@]}"; do
        for bc in "${block_copies[@]}"; do
            for bn in "${byte_sizes[@]}"; do
                # iterations = block_copy / block_num (정수)
                if (( bc % bn != 0 )); then
                    echo "[SKIP] bc($bc) 가 bn($bn)으로 나누어떨어지지 않음"
                    continue
                fi
                it=$(( bc / bn ))
                (( it > 0 )) || { echo "[SKIP] iterations==0 (bc=$bc, bn=$bn)"; continue; }

                case_id="fs${fs}G_bc${bc}_bn${bn}_it${it}"
                echo "----- START $case_id -----"

                # (2) client: a.txt
                echo "[client] sudo $HOME_DIR/client_write 10.0.0.2 $a -n $it -b $bn -s $seed"
                sudo $HOME_DIR/client_write 10.0.0.2 "$a" -n "$it" -b "$bn" -s "$seed" -t \
                    > >(stdbuf -oL tee -a "$RPC_LOG_FILE" >/dev/null) \
                    2> >(stdbuf -eL tee -a "$RPC_LOG_FILE" >&2)
                flush_caches

                # (3) baseline: b.txt
                echo "[baseline] sudo $HOME_DIR/baseline_write $b -n $it -b $bn -s $seed"
                sudo $HOME_DIR/baseline_write "$b" -n "$it" -b "$bn" -s "$seed" -t \
                    > >(stdbuf -oL tee -a "$BASELINE_LOG_FILE" >/dev/null) \
                    2> >(stdbuf -eL tee -a "$BASELINE_LOG_FILE" >&2)
                flush_caches

                echo "----- END   $case_id -----"
                echo
            done
        done
    done
done

echo "=== 모든 조합 처리 완료 ==="
