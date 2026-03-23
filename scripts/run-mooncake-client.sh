#!/usr/bin/env bash
set -euo pipefail

: "${MC_CLIENT_BIN:=./build/mooncake-store/src/mooncake_client}"
: "${MC_HOST:=127.0.0.1}"
: "${MC_METADATA_SERVER:=http://127.0.0.1:8080/metadata}"
: "${MC_MASTER_SERVER_ADDRESS:=127.0.0.1:50051}"
: "${MC_PROTOCOL:=tcp}"
: "${MC_DEVICE_NAMES:=}"
: "${MC_CLIENT_PORT:=50052}"
: "${MC_GLOBAL_SEGMENT_SIZE:=4GB}"
: "${MC_CLIENT_THREADS:=1}"
: "${MC_ENABLE_OFFLOAD:=false}"
: "${MC_ENABLE_HTTP_SERVER:=false}"
: "${MC_HTTP_PORT:=9300}"

args=(
  "--host=${MC_HOST}"
  "--metadata_server=${MC_METADATA_SERVER}"
  "--master_server_address=${MC_MASTER_SERVER_ADDRESS}"
  "--protocol=${MC_PROTOCOL}"
  "--device_names=${MC_DEVICE_NAMES}"
  "--port=${MC_CLIENT_PORT}"
  "--global_segment_size=${MC_GLOBAL_SEGMENT_SIZE}"
  "--threads=${MC_CLIENT_THREADS}"
  "--enable_offload=${MC_ENABLE_OFFLOAD}"
  "--enable_http_server=${MC_ENABLE_HTTP_SERVER}"
  "--http_port=${MC_HTTP_PORT}"
)

exec "${MC_CLIENT_BIN}" "${args[@]}" "$@"
