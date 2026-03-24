#!/usr/bin/env bash
###
 # @Author: chengji2 chengji2@iflytek.com
 # @Date: 2026-03-19 20:29:34
 # @Description: 
### 
set -euo pipefail

: "${MC_METADATA_SERVER:=http://127.0.0.1:8080/metadata}"
: "${MC_ENABLE_HA:=false}"
: "${MC_ETCD_ENDPOINTS:=}"
: "${MC_MASTER_SERVER_ADDRESS:=127.0.0.1:50051}"
: "${MC_PROTOCOL:=tcp}"
: "${MC_DEVICE_NAMES:=}"
: "${MC_CLIENT_PORT:=50052}"
: "${MC_GLOBAL_SEGMENT_SIZE:=64GB}"
: "${MC_ENABLE_OFFLOAD:=true}"
: "${MC_ENABLE_HTTP_SERVER:=true}"
: "${MC_HTTP_PORT:=9300}"

# glog (mooncake_client)
: "${MC_LOG_DIR:=/var/log/mooncake}"
: "${MC_GLOG_MINLOGLEVEL:=2}"
: "${MC_GLOG_LOGTOSTDERR:=1}"

# real_client initializes FileStorage from MOONCAKE_OFFLOAD_* environment
# variables, so the entrypoint must export a usable absolute path in addition to
# passing --enable_offload=true.
: "${MOONCAKE_OFFLOAD_FILE_STORAGE_PATH:=/data/file_storage}"
export MOONCAKE_OFFLOAD_FILE_STORAGE_PATH

# File offload requires an existing writable directory; this keeps
# FileStorageConfig::Validate() from failing at startup.
case "${MC_ENABLE_OFFLOAD,,}" in
  true|1|yes|on)
    mkdir -p "${MOONCAKE_OFFLOAD_FILE_STORAGE_PATH}"
    ;;
esac

master_entry="${MC_MASTER_SERVER_ADDRESS}"
case "${MC_ENABLE_HA,,}" in
  true|1|yes|on)
    if [[ -z "${MC_ETCD_ENDPOINTS}" ]]; then
      echo "MC_ETCD_ENDPOINTS must be set when MC_ENABLE_HA=true" >&2
      exit 1
    fi
    master_entry="etcd://${MC_ETCD_ENDPOINTS}"
    ;;
esac

args=(
  "--metadata_server=${MC_METADATA_SERVER}"
  "--master_server_address=${master_entry}"
  "--protocol=${MC_PROTOCOL}"
  "--device_names=${MC_DEVICE_NAMES}"
  "--port=${MC_CLIENT_PORT}"
  "--global_segment_size=${MC_GLOBAL_SEGMENT_SIZE}"
  "--enable_offload=${MC_ENABLE_OFFLOAD}"
  "--enable_http_server=${MC_ENABLE_HTTP_SERVER}"
  "--http_port=${MC_HTTP_PORT}"
  "--minloglevel=${MC_GLOG_MINLOGLEVEL}"
  "--logtostderr=${MC_GLOG_LOGTOSTDERR}"
  "--log_dir=${MC_LOG_DIR}"
)

mkdir -p "${MC_LOG_DIR}"

exec /opt/mooncake/build/mooncake-store/src/mooncake_client "${args[@]}" "$@"
