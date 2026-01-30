
#!/usr/bin/env bash
set -Eeuo pipefail

# =============================================================================
# Random Block Copy Test Framework
# - Runs baseline_random and client_random (RPC)
# - Verifies correctness after every test using deterministic replay
# - Generates logs + summary
# =============================================================================


# ====== Parameters ======
block_nums=(1 2 4 8 16)
iterations=(10000)
#iterations=(1280)
file_sizes=(30)   # GiB
seed=1234

SERVER_HOST="${SERVER_HOST:-10.0.0.2}"

# ====== Paths ======
LOG_DIR="${LOG_DIR:-./random_logs}"
mkdir -p "$LOG_DIR"

DATE_DIR="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$LOG_DIR/$DATE_DIR"
mkdir -p "$RUN_DIR"

BASELINE_LOG="$RUN_DIR/baseline.log"
RPC_LOG="$RUN_DIR/rpc.log"
SUMMARY="$RUN_DIR/summary.txt"

mnt="/mnt/nvme1"
TEST_FILE="$mnt/test.bin"
BEFORE_FILE="$mnt/before.bin"
REF_DIR="$mnt/reference"
OUT_DIR="$mnt/output"
mkdir -p "$REF_DIR" "$OUT_DIR"


# ====== Binaries ======
parent_dir="$(dirname "$(pwd)")"
CLIENT_BIN="$parent_dir/random_block_read/client_random"
BASELINE_BIN="$parent_dir/random_block_read/baseline_random"

mnt="/mnt/nvme1"
a="$mnt/a.txt"
b="$mnt/b.txt"

grandparent_dir="$(dirname "$(dirname "$(pwd)")")"
gcc -O2 -Wall -o $parent_dir/create_file $parent_dir/create_file.c
CREATE_BIN="$parent_dir/create_file"


# ====== Colors ======
GREEN="\033[1;32m"
RED="\033[1;31m"
BLUE="\033[1;34m"
YELLOW="\033[1;33m"
NC="\033[0m"

# =============================================================================
echo -e "${BLUE}=== Random Block Copy Testing Framework ===${NC}"
echo "Log directory: $RUN_DIR"
echo "Server: $SERVER_HOST"
echo "Seed: $seed"
echo ""

# ====== Prerequisite checks ======
need_cmds=(sudo sync cmp)
for c in "${need_cmds[@]}"; do
  command -v "$c" >/dev/null || { echo >&2 "ERROR: Missing command: $c"; exit 1; }
done

for bin in "$CLIENT_BIN" "$BASELINE_BIN" "$CREATE_BIN"; do
  [[ -x "$bin" ]] || { echo >&2 "ERROR: Missing binary: $bin"; exit 1; }
done

# ====== CSV headers ======
echo "block_num,iterations,total_blocks,file_size_gb,read_s,write_s,io_s,total_s" > "$BASELINE_LOG"
echo "block_num,iterations,total_blocks,file_size_gb,server_read_s,server_write_s,server_other_s,fiemap_s,rpc_s,io_s,total_s" > "$RPC_LOG"

# ====== Summary header ======
cat > "$SUMMARY" << EOF
==========================================
Random Block Copy Test Summary
==========================================
Date: $(date)
Server: $SERVER_HOST
Seed: $seed

Block numbers: ${block_nums[*]}
Iterations: ${iterations[*]}
File sizes: ${file_sizes[*]} GiB

==========================================

EOF

# ====== Cache flushing ======
DROP_CACHES="${DROP_CACHES:-1}"
flush_caches() {
  sudo sync
  if [[ "$DROP_CACHES" == "1" ]]; then
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
  fi
}

# ====== Prepare test file ======
for fs in "${file_sizes[@]}"; do
  echo -e "${BLUE}Creating test file (${fs} GiB)...${NC}"
  sudo "$CREATE_BIN" "$TEST_FILE" "$fs" --gib
done

echo -e "${BLUE}Saving pristine copy...${NC}"
sudo cp "$TEST_FILE" "$BEFORE_FILE"
flush_caches
echo ""

# =============================================================================
# Helper: deterministic correctness check
# =============================================================================
:<<'COMMENT'
verify_correctness() {
    local bn="$1"
    local iter="$2"

    echo "Generating reference correctness file..."
    sudo cp "$BEFORE_FILE" "$REFERENCE_FILE"
    sudo "$BASELINE_BIN" "$REFERENCE_FILE" -n "$(( iter / bn ))" -b "$bn" -s "$seed" -t >/dev/null

    echo "Comparing output..."
    if sudo cmp "$TEST_FILE" "$REFERENCE_FILE" >/dev/null 2>&1; then
        echo -e "${GREEN}[OK] Correctness VERIFIED${NC}"
        echo "correctness,bn${bn},iter${iter},OK" >> "$SUMMARY"
    else
        echo -e "${RED}[ERROR] MISMATCH DETECTED${NC}"
        echo "correctness,bn${bn},iter${iter},FAIL" >> "$SUMMARY"

        echo "Showing first few differing bytes:"
        sudo diff <(xxd "$TEST_FILE") <(xxd "$REFERENCE_FILE") | head -n 20 || true
    fi
}
COMMENT

generate_reference() {
  local bn="$1"
  local iter="$2"

  local ref_file="$REF_DIR/ref_bn${bn}_iter${iter}.bin"

  echo "[REF] Generating reference (bn=$bn, iter=$iter)"
  sudo rm -f "$ref_file"

  sudo cp "$BEFORE_FILE" "$TEST_FILE"
  flush_caches

  sudo "$BASELINE_BIN" "$TEST_FILE" \
    -n "$iter" \
    -b "$bn" \
    -s "$seed" \
    -t \
    > /dev/null

  # 결과 파일 덤프
  sudo dd if="$TEST_FILE" of="$ref_file" bs=4K status=none
}

compare_with_reference() {
  local mode="$1"   # baseline | rpc
  local bn="$2"
  local iter="$3"

  local ref_file="$REF_DIR/ref_bn${bn}_iter${iter}.bin"
  local out_file="$OUT_DIR/${mode}_bn${bn}_iter${iter}.bin"

  # 결과 덤프
  sudo dd if="$TEST_FILE" of="$out_file" bs=4K status=none

  if cmp -s "$ref_file" "$out_file"; then
    echo "[OK] $mode correctness VERIFIED"
    rm -f "$out_file" 

  else
    echo "[ERROR] $mode MISMATCH DETECTED"
    echo "Files:"
    echo "  REF : $ref_file"
    echo "  OUT : $out_file"

    # 처음 몇 바이트만 보여주기
    cmp -l "$ref_file" "$out_file" | head -n 20
    exit 1
  fi
}




# =============================================================================
# Main Test Loop
# =============================================================================

test_num=30
test_id=0
total_tests=$(( ${#iterations[@]} * ${#block_nums[@]} * test_num))

echo -e "${BLUE}Total tests to run: $total_tests${NC}"
echo ""


for iter in "${iterations[@]}"; do
  for bn in "${block_nums[@]}"; do

    # ------------------------------------------------------------
    # 1) reference 생성 (bn, iter 조합당 1회)
    # ------------------------------------------------------------
    generate_reference "$bn" "$iter"
    ref_file="$REF_DIR/ref_bn${bn}_iter${iter}.bin"

    echo "correctness check..."

    # 2) baseline correctness 1회
    sudo cp "$BEFORE_FILE" "$TEST_FILE"
    flush_caches
    sudo "$BASELINE_BIN" "$TEST_FILE" -n "$iter" -b "$bn" -s "$seed" -t
    compare_with_reference "baseline" "$bn" "$iter"

    # 3) rpc correctness 1회
    sudo cp "$BEFORE_FILE" "$TEST_FILE"
    flush_caches
    sudo "$CLIENT_BIN" "$SERVER_HOST" "$TEST_FILE" -n "$iter" -b "$bn" -s "$seed" -t
    compare_with_reference "rpc" "$bn" "$iter"

    echo "done!"

    for (( t=1; t<=test_num; t++ )); do
      test_id=$((test_id + 1))

      echo -e "${BLUE}=== Test Round $t / $test_num (bn=$bn, iter=$iter) ===${NC}"

      # ==========================================================
      # Baseline Test
      # ==========================================================
      echo -e "${YELLOW}--- [${test_id}/${total_tests}] Baseline ---${NC}"

      sudo cp "$BEFORE_FILE" "$TEST_FILE"
      flush_caches

      sudo "$BASELINE_BIN" "$TEST_FILE" \
        -n "$iter" \
        -b "$bn" \
        -s "$seed" \
        -t \
        >> "$BASELINE_LOG" 2>&1

      #compare_with_reference "baseline" "$bn" "$iter"

      # ==========================================================
      # RPC Test
      # ==========================================================
      echo -e "${YELLOW}--- [${test_id}/${total_tests}] RPC ---${NC}"

      sudo cp "$BEFORE_FILE" "$TEST_FILE"
      flush_caches

      sudo "$CLIENT_BIN" "$SERVER_HOST" "$TEST_FILE" \
        -n "$iter" \
        -b "$bn" \
        -s "$seed" \
        -t \
        >> "$RPC_LOG" 2>&1

      #compare_with_reference "rpc" "$bn" "$iter"

      flush_caches
      sleep 1
    done

    rm -f "$ref_file"

  done
done


echo -e "${GREEN}=== All tests completed ===${NC}"
echo "Logs stored in: $RUN_DIR"
echo "Summary file: $SUMMARY"
