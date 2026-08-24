#!/usr/bin/env bash
set -euo pipefail

: "${ROLE:?}"
: "${HARNESS_REPO:?}"
: "${TRANSPORT_REPO:?}"
: "${TRANSPORT_MODE:?}"
: "${RUN_DIR:?}"
: "${RUN_ID:?}"
: "${TOTAL_COUNT:?}"
: "${MEASURED_COUNT:?}"
: "${WARMUP:?}"
: "${SLOTS:?}"
: "${RATE:?}"
: "${MESSAGE_TYPE:?}"
: "${FORWARD_PORT:?}"
: "${RETURN_PORT:?}"
: "${PEER_PRIVATE:?}"
: "${SNDBUF:?}"
: "${RCVBUF:?}"
: "${BATCH_SIZE:?}"
: "${BATCH_TIMEOUT_US:?}"
: "${FEC_K:?}"
: "${FEC_TIMEOUT_US:?}"
: "${START_DELAY_MS:?}"
: "${LATENCY_OUTPUT:?}"

mkdir -p "$RUN_DIR"
in_shm="/${RUN_ID}_in"
forward_shm="/${RUN_ID}_forward"
relay_shm="/${RUN_ID}_relay"
return_shm="/${RUN_ID}_return"
pids=()
success=false

cleanup() {
  if [[ "$success" == "false" ]]; then
    for pid in "${pids[@]}"; do
      kill "$pid" 2>/dev/null || true
    done
  fi
  rm -f "/dev/shm/${in_shm#/}" "/dev/shm/${forward_shm#/}" "/dev/shm/${relay_shm#/}" "/dev/shm/${return_shm#/}"
}
trap cleanup EXIT INT TERM

launch() {
  local cpu="$1"
  local log="$2"
  shift 2
  taskset -c "$cpu" "$@" >"$log" 2>&1 &
  pids+=("$!")
}

wait_shm() {
  local name="$1"
  local end="$((SECONDS + 10))"
  while (( SECONDS < end )); do
    [[ -e "/dev/shm/${name#/}" ]] && return 0
    sleep 0.001
  done
  printf '%s\n' "shared memory timeout: $name" >&2
  return 1
}

sender_args=(--slots "$SLOTS" --count "$TOTAL_COUNT" --idle-ms 30000 --sndbuf "$SNDBUF" --batch-size "$BATCH_SIZE" --batch-timeout-us "$BATCH_TIMEOUT_US")
receiver_args=(--slots "$SLOTS" --count "$TOTAL_COUNT" --rcvbuf "$RCVBUF" --batch-size "$BATCH_SIZE" --idle-ms 30000)
if [[ "$TRANSPORT_MODE" == "current" ]]; then
  sender_args+=(--fec-k "$FEC_K" --fec-timeout-us "$FEC_TIMEOUT_US")
elif [[ "$TRANSPORT_MODE" != "baseline" || "$FEC_K" != "0" ]]; then
  printf '%s\n' "baseline transport requires FEC_K=0" >&2
  exit 2
fi

if [[ "$ROLE" == "rx" ]]; then
  : "${CPU_FORWARD_RECEIVER:?}"
  : "${CPU_RELAY:?}"
  : "${CPU_RETURN_SENDER:?}"
  rm -f "/dev/shm/${forward_shm#/}" "/dev/shm/${relay_shm#/}"
  launch "$CPU_FORWARD_RECEIVER" "$RUN_DIR/forward_receiver.log" "$TRANSPORT_REPO/harness/bin/receiver" --out-shm "$forward_shm" --port "$FORWARD_PORT" "${receiver_args[@]}"
  wait_shm "$forward_shm"
  launch "$CPU_RELAY" "$RUN_DIR/relay.log" "$HARNESS_REPO/harness/bin/relay" --in-shm "$forward_shm" --out-shm "$relay_shm" --slots "$SLOTS" --count "$TOTAL_COUNT" --idle-ms 30000
  wait_shm "$relay_shm"
  launch "$CPU_RETURN_SENDER" "$RUN_DIR/return_sender.log" "$TRANSPORT_REPO/harness/bin/sender" --in-shm "$relay_shm" --dst "$PEER_PRIVATE:$RETURN_PORT" "${sender_args[@]}"
elif [[ "$ROLE" == "tx" ]]; then
  : "${CPU_PRODUCER:?}"
  : "${CPU_FORWARD_SENDER:?}"
  : "${CPU_RETURN_RECEIVER:?}"
  : "${CPU_CONSUMER:?}"
  rm -f "/dev/shm/${in_shm#/}" "/dev/shm/${return_shm#/}"
  launch "$CPU_RETURN_RECEIVER" "$RUN_DIR/return_receiver.log" "$TRANSPORT_REPO/harness/bin/receiver" --out-shm "$return_shm" --port "$RETURN_PORT" "${receiver_args[@]}"
  wait_shm "$return_shm"
  consumer_args=(--shm "$return_shm" --slots "$SLOTS" --count "$MEASURED_COUNT" --skip "$WARMUP" --idle-ms 30000)
  if [[ "$LATENCY_OUTPUT" == "disk" ]]; then
    consumer_args+=(--csv "$RUN_DIR/latency.bin")
  fi
  launch "$CPU_CONSUMER" "$RUN_DIR/consumer.log" "$HARNESS_REPO/harness/bin/consumer" "${consumer_args[@]}"
  launch "$CPU_PRODUCER" "$RUN_DIR/producer.log" "$HARNESS_REPO/harness/bin/producer" --shm "$in_shm" --slots "$SLOTS" --count "$TOTAL_COUNT" --rate "$RATE" --type "$MESSAGE_TYPE" --start-delay-ms "$START_DELAY_MS"
  wait_shm "$in_shm"
  launch "$CPU_FORWARD_SENDER" "$RUN_DIR/forward_sender.log" "$TRANSPORT_REPO/harness/bin/sender" --in-shm "$in_shm" --dst "$PEER_PRIVATE:$FORWARD_PORT" "${sender_args[@]}"
else
  printf '%s\n' "ROLE must be tx or rx" >&2
  exit 2
fi

status=0
for pid in "${pids[@]}"; do
  wait "$pid" || status=1
done
if (( status != 0 )); then
  exit "$status"
fi
printf '%s\n' "complete" >"$RUN_DIR/done"
success=true
