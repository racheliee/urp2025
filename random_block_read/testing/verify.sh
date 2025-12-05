#!/usr/bin/env bash
set -Eeuo pipefail

# ============================================================================
# Quick Verification Script for Random Block Copy
# Runs a minimal test to verify the system is working correctly
# ============================================================================

echo "=== Random Block Copy Verification Test ==="
echo ""

# ----- Configuration -----
server_host="${1:-eternity2}"
test_file="${2:-/mnt/nvme1/test_verify.txt}"
file_size_gb=1
iterations=100
batch_size=10
block_num=1
seed=12345

# ----- Setup -----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "$SCRIPT_DIR")"

CLIENT_BIN="$PARENT_DIR/client_random"
SERVER_BIN="$PARENT_DIR/server_random"
CREATE_BIN="$PARENT_DIR/create_file"

echo "Configuration:"
echo "  Server: $server_host"
echo "  Test file: $test_file"
echo "  File size: ${file_size_gb} GiB"
echo "  Iterations: $iterations"
echo "  Batch size: $batch_size"
echo "  Block number: $block_num"
echo "  Seed: $seed"
echo ""

# ----- Check Binaries -----
echo "Step 1: Checking binaries..."

if [[ ! -x "$CLIENT_BIN" ]]; then
  echo "ERROR: Client binary not found: $CLIENT_BIN"
  echo "Please run 'make all' first"
  exit 1
fi
echo "  ✓ Client binary found: $CLIENT_BIN"

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "ERROR: Server binary not found: $SERVER_BIN"
  echo "Please run 'make all' first"
  exit 1
fi
echo "  ✓ Server binary found: $SERVER_BIN"

if [[ ! -x "$CREATE_BIN" ]]; then
  echo "WARNING: create_file binary not found: $CREATE_BIN"
  echo "Will skip file creation step"
  CREATE_FILE=0
else
  echo "  ✓ create_file binary found: $CREATE_BIN"
  CREATE_FILE=1
fi
echo ""

# ----- Check Server Connection -----
echo "Step 2: Checking server connection..."
if ! ping -c 1 -W 2 "$server_host" &>/dev/null; then
  echo "ERROR: Cannot reach server: $server_host"
  echo "Please check:"
  echo "  1. Server hostname is correct"
  echo "  2. Network connection is working"
  echo "  3. Server is running: sudo $SERVER_BIN"
  exit 1
fi
echo "  ✓ Server $server_host is reachable"
echo ""

# ----- Check/Create Test File -----
echo "Step 3: Checking test file..."
if [[ ! -f "$test_file" ]]; then
  if [[ "$CREATE_FILE" == "0" ]]; then
    echo "ERROR: Test file does not exist: $test_file"
    echo "Please create it manually or provide create_file binary"
    exit 1
  fi
  
  echo "  Creating test file (${file_size_gb} GiB)..."
  sudo "$CREATE_BIN" "$test_file" "$file_size_gb" --gib
  if [[ $? -ne 0 ]]; then
    echo "ERROR: Failed to create test file"
    exit 1
  fi
  echo "  ✓ Test file created: $test_file"
else
  file_size=$(stat -c%s "$test_file" 2>/dev/null || stat -f%z "$test_file" 2>/dev/null)
  file_size_mb=$((file_size / 1024 / 1024))
  echo "  ✓ Test file exists: $test_file (${file_size_mb} MB)"
fi
echo ""

# ----- Check RPC Server is Running -----
echo "Step 4: Checking if RPC server is running..."
if ! rpcinfo -p "$server_host" 2>/dev/null | grep -q "blockcopy"; then
  echo "WARNING: RPC server does not appear to be running on $server_host"
  echo "Please ensure the server is running:"
  echo "  ssh $server_host 'sudo $SERVER_BIN'"
  echo ""
  read -p "Do you want to continue anyway? (y/N) " -n 1 -r
  echo
  if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    exit 1
  fi
else
  echo "  ✓ RPC server is running on $server_host"
fi
echo ""

# ----- Run Quick Test -----
echo "Step 5: Running quick test..."
echo "Command: sudo $CLIENT_BIN $server_host $test_file -n $iterations -b $block_num -s $seed -batch $batch_size -l"
echo ""

start_time=$(date +%s)
if sudo "$CLIENT_BIN" "$server_host" "$test_file" \
  -n "$iterations" -b "$block_num" -s "$seed" -batch "$batch_size" -l; then
  end_time=$(date +%s)
  elapsed=$((end_time - start_time))
  
  echo ""
  echo "=== Verification PASSED ==="
  echo "Test completed successfully in ${elapsed}s"
  echo ""
  echo "System is working correctly!"
  echo ""
  echo "Next steps:"
  echo "  - Run full test suite: ./test.sh"
  echo "  - Adjust parameters in test.sh for your needs"
  echo "  - Monitor server performance"
  echo ""
  exit 0
else
  echo ""
  echo "=== Verification FAILED ==="
  echo "Test did not complete successfully"
  echo ""
  echo "Troubleshooting:"
  echo "  1. Check server logs on $server_host"
  echo "  2. Verify device path in server_random.h matches your NVMe device"
  echo "  3. Ensure server has permissions to access the device"
  echo "  4. Check network connectivity: ping $server_host"
  echo "  5. Verify RPC service: rpcinfo -p $server_host"
  echo ""
  exit 1
fi