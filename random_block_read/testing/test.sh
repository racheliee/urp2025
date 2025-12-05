#!/usr/bin/env bash
set -Eeuo pipefail

# ============================================================================
# Random Block Copy Test Script
# Tests both baseline and batched RPC versions with different batch sizes
# ============================================================================

# ----- Test Parameters -----
block_nums=(1 2 4 8 16)           # Number of blocks per operation
iterations=(10000)                # Number of random copy operations
batch_sizes=(1 10 50 100 200 500 1000)  # Batch sizes to test (1 = no batching)
file_sizes=(10)                   # File sizes in GiB
seed=12345                        # Fixed seed for reproducibility
server_host="${SERVER_HOST:-eternity2}"  # RPC server hostname

# ----- Directory Setup -----
LOG_DIR="${LOG_DIR:-./testing}"
mkdir -p "$LOG_DIR"

DATE_DIR="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$LOG_DIR/$DATE_DIR"
mkdir -p "$RUN_DIR"

BASELINE_LOG_FILE="$RUN_DIR/baseline.log"
RPC_LOG_FILE="$RUN_DIR/rpc_batched.log"
SUMMARY_FILE="$RUN_DIR/summary.txt"

echo "=== Random Block Copy Test ==="
echo "Log directory: $RUN_DIR"
echo "Server: $server_host"
echo ""

# ----- Test File Paths -----
mnt="/mnt/nvme1"
test_file="$mnt/random_test.txt"

# ----- Binary Checks -----
need_cmds=(sudo sync)
for c in "${need_cmds[@]}"; do
  command -v "$c" >/dev/null 2>&1 || { 
    echo "ERROR: Required command not found: $c" >&2
    exit 1
  }
done

# Check for required binaries in parent directory
parent_dir="$(dirname "$(pwd)")"
CLIENT_BIN="$parent_dir/client_random"
BASELINE_BIN="$parent_dir/baseline_random"
CREATE_BIN="$parent_dir/create_file"

for bin in "$CLIENT_BIN" "$CREATE_BIN"; do
  [[ -x "$bin" ]] || { 
    echo "ERROR: Binary not found or not executable: $bin" >&2
    echo "Please compile the binaries first with 'make all'" >&2
    exit 1
  }
done

# Baseline is optional
if [[ ! -x "$BASELINE_BIN" ]]; then
  echo "WARNING: Baseline binary not found: $BASELINE_BIN"
  echo "Will skip baseline tests"
  SKIP_BASELINE=1
else
  SKIP_BASELINE=0
fi

# ----- Cache Flushing -----
DROP_CACHES="${DROP_CACHES:-1}"
flush_caches() {
  sudo sync
  if [[ "$DROP_CACHES" == "1" ]]; then
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    echo "[$(date +%H:%M:%S)] Cache flushed"
  fi
}

# ----- Server Check -----
check_server() {
  echo "Checking RPC server connection..."
  if ! ping -c 1 -W 2 "$server_host" &>/dev/null; then
    echo "ERROR: Cannot reach server: $server_host" >&2
    exit 1
  fi
  echo "Server $server_host is reachable"
}

# ----- CSV Headers -----
cat > "$BASELINE_LOG_FILE" << EOF
block_num,iteration,num_block_copies,file_size_gb,read_time_s,write_time_s,prep_time_s,end_time_s,io_time_s,total_time_s
EOF

cat > "$RPC_LOG_FILE" << EOF
block_num,iteration,num_block_copies,file_size_gb,batch_size,server_read_s,server_write_s,server_other_s,prep_s,end_s,fiemap_s,rpc_s,io_s,total_s
EOF

# ----- Test Summary Header -----
{
  echo "=========================================="
  echo "Random Block Copy Test Summary"
  echo "=========================================="
  echo "Date: $(date)"
  echo "Server: $server_host"
  echo "Seed: $seed"
  echo ""
  echo "Test Parameters:"
  echo "  Block numbers: ${block_nums[*]}"
  echo "  Iterations: ${iterations[*]}"
  echo "  Batch sizes: ${batch_sizes[*]}"
  echo "  File sizes: ${file_sizes[*]} GiB"
  echo ""
  echo "=========================================="
  echo ""
} > "$SUMMARY_FILE"

# ----- Main Test Loop -----
echo "=== Starting Tests ==="
echo "Parameters: seed=$seed"
echo "  block_nums: ${block_nums[*]}"
echo "  iterations: ${iterations[*]}"
echo "  batch_sizes: ${batch_sizes[*]}"
echo "  file_sizes: ${file_sizes[*]}"
echo ""

# Check server connection
check_server

# Create test file(s)
for fs in "${file_sizes[@]}"; do
  echo "[$(date +%H:%M:%S)] Creating test file: $test_file (${fs} GiB)"
  sudo "$CREATE_BIN" "$test_file" "$fs" --gib
  flush_caches
  echo ""
done

# Test iteration counter
test_num=0
total_tests=$((${#file_sizes[@]} * ${#iterations[@]} * ${#block_nums[@]} * ${#batch_sizes[@]}))

if [[ "$SKIP_BASELINE" == "0" ]]; then
  total_tests=$((total_tests + ${#file_sizes[@]} * ${#iterations[@]} * ${#block_nums[@]}))
fi

echo "Total tests to run: $total_tests"
echo ""

# Run tests
for fs in "${file_sizes[@]}"; do
  for iter in "${iterations[@]}"; do
    for bn in "${block_nums[@]}"; do
      
      # ===== Baseline Test (if available) =====
      if [[ "$SKIP_BASELINE" == "0" ]]; then
        ((test_num++))
        case_id="baseline_fs${fs}G_iter${iter}_bn${bn}"
        
        echo "=========================================="
        echo "Test [$test_num/$total_tests]: $case_id"
        echo "=========================================="
        
        echo "[$(date +%H:%M:%S)] Running baseline..."
        sudo "$BASELINE_BIN" "$test_file" -n "$iter" -b "$bn" -s "$seed" -t -l \
          >> "$BASELINE_LOG_FILE" 2>&1
        
        flush_caches
        echo ""
      fi
      
      # ===== RPC Tests with Different Batch Sizes =====
      for batch in "${batch_sizes[@]}"; do
        ((test_num++))
        case_id="rpc_fs${fs}G_iter${iter}_bn${bn}_batch${batch}"
        
        echo "=========================================="
        echo "Test [$test_num/$total_tests]: $case_id"
        echo "=========================================="
        
        echo "[$(date +%H:%M:%S)] Running RPC client (batch size: $batch)..."
        sudo "$CLIENT_BIN" "$server_host" "$test_file" \
          -n "$iter" -b "$bn" -s "$seed" -batch "$batch" -t -l \
          >> "$RPC_LOG_FILE" 2>&1
        
        flush_caches
        echo ""
        
        # Brief pause to let server settle
        sleep 1
      done
    done
  done
done

# ----- Generate Summary Statistics -----
echo "=== Generating Summary Statistics ==="

{
  echo ""
  echo "=========================================="
  echo "Test Results Summary"
  echo "=========================================="
  echo ""
  
  if [[ "$SKIP_BASELINE" == "0" && -s "$BASELINE_LOG_FILE" ]]; then
    echo "Baseline Results:"
    echo "-----------------"
    tail -n +2 "$BASELINE_LOG_FILE" | while IFS=, read -r bn iter nbc fs_gb read write prep end io total; do
      throughput=$(echo "scale=2; $nbc * $bn * 4096 / ($total * 1024 * 1024)" | bc)
      printf "  bn=%2d, iter=%6d: total=%8.3fs, throughput=%8.2f MB/s\n" \
        "$bn" "$iter" "$total" "$throughput"
    done
    echo ""
  fi
  
  echo "RPC Batched Results:"
  echo "--------------------"
  tail -n +2 "$RPC_LOG_FILE" | while IFS=, read -r bn iter nbc fs_gb batch srv_r srv_w srv_o prep end fiemap rpc io total; do
    throughput=$(echo "scale=2; $nbc * $bn * 4096 / ($total * 1024 * 1024)" | bc)
    printf "  bn=%2d, batch=%4d, iter=%6d: total=%8.3fs, throughput=%8.2f MB/s, rpc=%6.3fs\n" \
      "$bn" "$batch" "$iter" "$total" "$throughput" "$rpc"
  done
  echo ""
  
  echo "=========================================="
  echo "Test completed: $(date)"
  echo "=========================================="
  
} | tee -a "$SUMMARY_FILE"

# ----- Cleanup Instructions -----
echo ""
echo "=== Test Complete ==="
echo "Results saved to: $RUN_DIR"
echo "  - Baseline log: $BASELINE_LOG_FILE"
echo "  - RPC log: $RPC_LOG_FILE"
echo "  - Summary: $SUMMARY_FILE"
echo ""
echo "To analyze results:"
echo "  cat $SUMMARY_FILE"
echo "  less $RPC_LOG_FILE"
echo ""
echo "To clean up test files:"
echo "  sudo rm $test_file"
echo ""