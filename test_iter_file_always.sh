#!/usr/bin/env bash
set -Eeuo pipefail

# ----- 파라미터 세트 -----
block_nums=(1 2 4 8 16)
block_copies=(128000)
file_sizes=(30)   # GiB
seed=-1

LOG_DIR="${LOG_DIR:-./logs}"
mkdir -p "$LOG_DIR"

DATE_DIR="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$LOG_DIR/$DATE_DIR"
mkdir -p "$RUN_DIR"

BASELINE_LOG_FILE="$RUN_DIR/blockcopy.log"
RPC_LOG_FILE="$RUN_DIR/rpc.log"
echo "로그 디렉토리: $RUN_DIR"

mnt="/mnt/nvme1"
a="$mnt/a.txt"
b="$mnt/b.txt"

# ----- 필요 명령/바이너리 점검 -----
need_cmds=(sudo cp sync)
for c in "${need_cmds[@]}"; do
  command -v "$c" >/dev/null 2>&1 || { echo "필요 명령 없음: $c" >&2; exit 1; }
done
for bin in ./create_file ./client ./baseline ./compare; do
  [[ -x "$bin" ]] || { echo "실행 파일 없음 또는 실행 불가: $bin" >&2; exit 1; }
done

# ----- 캐시 동기화/정리 -----
# 시스템 전체에 영향. 원치 않으면: DROP_CACHES=0 ./test.sh
DROP_CACHES="${DROP_CACHES:-1}"
flush_caches() {
  sudo sync
  if [[ "$DROP_CACHES" == "1" ]]; then
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  fi
}

echo "=== 테스트 시작 ==="
echo "params: seed=$seed, block_nums=${block_nums[*]}, block_copies=${block_copies[*]}, file_sizes=${file_sizes[*]}"
echo

echo "block_num,iteration,num_block_copies,file_size,read_ns,write_ns,prep_ns,end_ns,io_ns,total_time" > "$BASELINE_LOG_FILE"
echo "block_num,iteration,num_block_copies,file_size,server_read_ns,server_write_ns,server_other_ns,prep_ns,end_ns,fiemap_ns,rpc_ns,io_ns,total_time" > "$RPC_LOG_FILE"

i=1
while true; do
    echo "iteration: $i >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
    ((i++))
    for fs in "${file_sizes[@]}"; do
        for bc in "${block_copies[@]}"; do
            for bn in "${block_nums[@]}"; do
                # iterations = block_copy / block_num (정수)
                if (( bc % bn != 0 )); then
                    echo "[SKIP] bc($bc) 가 bn($bn)으로 나누어떨어지지 않음"
                    continue
                fi
                it=$(( bc / bn ))
                (( it > 0 )) || { echo "[SKIP] iterations==0 (bc=$bc, bn=$bn)"; continue; }

                case_id="fs${fs}G_bc${bc}_bn${bn}_it${it}"
                echo "----- START $case_id -----"

                # (1) 파일 생성 및 복제(초기화)
                echo "[create] sudo ./create_file $a $fs --gib"
                sudo ./create_file "$a" "$fs" --gib
                flush_caches

                echo "[copy] sudo cp $a $b"
                sudo cp "$a" "$b"
                echo "Done"
                flush_caches

                # (2) client: a.txt
                echo "[client] sudo ./client eternity2 $a -n $it -b $bn -s $seed"
                sudo ./client eternity2 "$a" -n "$it" -b "$bn" -s "$seed" -t \
                    > >(stdbuf -oL tee -a "$RPC_LOG_FILE" >/dev/null) \
                    2> >(stdbuf -eL tee -a "$RPC_LOG_FILE" >&2)
                flush_caches

                # (3) baseline: b.txt
                echo "[baseline] sudo ./baseline $b -n $it -b $bn -s $seed"
                sudo ./baseline "$b" -n "$it" -b "$bn" -s "$seed" -t \
                    > >(stdbuf -oL tee -a "$BASELINE_LOG_FILE" >/dev/null) \
                    2> >(stdbuf -eL tee -a "$BASELINE_LOG_FILE" >&2)
                flush_caches

                # (4) 비교
                #echo "[compare] sudo ./compare $a $b"
                #sudo ./compare "$a" "$b"
                #   flush_caches

                echo "----- END   $case_id -----"
                echo
            done
        done
    done
done

echo "=== 모든 조합 처리 완료 ==="