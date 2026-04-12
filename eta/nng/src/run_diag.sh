#!/bin/bash
ETAI=/mnt/c/Users/lewis/develop/eta/out/wsl-clang-release/eta/interpreter/etai
STDLIB=/mnt/c/Users/lewis/develop/eta/stdlib
PARENT=/mnt/c/Users/lewis/develop/eta/eta/nng/src/parent_test.eta
WORKER=/mnt/c/Users/lewis/develop/eta/eta/nng/src/worker_test.eta
SOCK=ipc:///tmp/eta_diag_test.sock

rm -f /tmp/eta_diag_test.sock /tmp/parent_out.txt /tmp/worker_out.txt

echo "[shell] starting parent..."
ETA_MODULE_PATH=$STDLIB timeout 15 $ETAI $PARENT > /tmp/parent_out.txt 2>&1 &
PAR_PID=$!
echo "[shell] parent pid=$PAR_PID"
sleep 0.4

echo "[shell] starting worker..."
ETA_MODULE_PATH=$STDLIB timeout 10 $ETAI $WORKER --mailbox $SOCK > /tmp/worker_out.txt 2>&1 &
WRK_PID=$!
echo "[shell] worker pid=$WRK_PID"

wait $PAR_PID
PAR_EXIT=$?
echo "[shell] parent_exit=$PAR_EXIT"
echo "=== parent stdout/stderr ==="
cat /tmp/parent_out.txt
echo "=== end parent ==="

wait $WRK_PID
WRK_EXIT=$?
echo "[shell] worker_exit=$WRK_EXIT"
echo "=== worker stdout/stderr ==="
cat /tmp/worker_out.txt
echo "=== end worker ==="

