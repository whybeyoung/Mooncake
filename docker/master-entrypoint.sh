#!/usr/bin/env bash
# 若用 `sh ./master-entrypoint.sh` 调用，shebang 会被忽略；必须用 bash（pipefail、数组、[[ ]] 等）
if [ -z "${BASH_VERSION:-}" ]; then
  if command -v bash >/dev/null 2>&1; then
    exec bash "$0" "$@"
  fi
  echo "master-entrypoint.sh requires bash (e.g. bash $0)" >&2
  exit 2
fi
###
 # Mooncake Master 启动脚本：通过环境变量覆盖 mooncake_master 的全部 gflags。
 # 用法与 docker/storage-agent-entrypoint.sh 一致：默认展开参数，末尾 "$@" 可追加或覆盖。
###
set -euo pipefail

: "${MC_MASTER_BIN:=/opt/mooncake/build/mooncake-store/src/mooncake_master}"

# 配置文件。若路径不存在或不可读，则不传 --config_path（仅使用下方命令行参数）。
# 设为空字符串表示始终不使用 YAML。
: "${MC_CONFIG_PATH:=/opt/mooncake/conf/master.yaml}"

# 已弃用：与 rpc_port / rpc_thread_num 兼容
: "${MC_PORT:=50051}"
: "${MC_MAX_THREADS:=32}"

: "${MC_ENABLE_METRIC_REPORTING:=true}"
: "${MC_METRICS_PORT:=9003}"

: "${MC_DEFAULT_KV_LEASE_TTL:=5000}"
: "${MC_DEFAULT_KV_SOFT_PIN_TTL:=1800000}"
: "${MC_ALLOW_EVICT_SOFT_PINNED_OBJECTS:=true}"
: "${MC_EVICTION_RATIO:=0.1}"
: "${MC_EVICTION_HIGH_WATERMARK_RATIO:=0.8}"

: "${MC_RPC_THREAD_NUM:=0}"
: "${MC_RPC_PORT:=0}"
: "${MC_RPC_ADDRESS:=0.0.0.0}"
: "${MC_RPC_CONN_TIMEOUT_SECONDS:=0}"
: "${MC_RPC_ENABLE_TCP_NO_DELAY:=true}"

: "${MC_ENABLE_HA:=false}"
: "${MC_ENABLE_OFFLOAD:=true}"
: "${MC_ETCD_ENDPOINTS:=}"
: "${MC_CLIENT_TTL:=10}"

: "${MC_ROOT_FS_DIR:=}"
: "${MC_GLOBAL_FILE_SEGMENT_SIZE:=9223372036854775807}"
: "${MC_CLUSTER_ID:=mooncake_cluster}"

: "${MC_MEMORY_ALLOCATOR:=offset}"
: "${MC_ALLOCATION_STRATEGY:=random}"

: "${MC_ENABLE_HTTP_METADATA_SERVER:=false}"
: "${MC_HTTP_METADATA_SERVER_PORT:=8080}"
: "${MC_HTTP_METADATA_SERVER_HOST:=0.0.0.0}"

: "${MC_PUT_START_DISCARD_TIMEOUT_SEC:=30}"
: "${MC_PUT_START_RELEASE_TIMEOUT_SEC:=600}"
: "${MC_ENABLE_DISK_EVICTION:=true}"
: "${MC_QUOTA_BYTES:=0}"

: "${MC_SNAPSHOT_BACKUP_DIR:=}"
: "${MC_ENABLE_SNAPSHOT_RESTORE:=false}"
: "${MC_ENABLE_SNAPSHOT:=false}"
: "${MC_SNAPSHOT_INTERVAL_SECONDS:=600}"
: "${MC_SNAPSHOT_CHILD_TIMEOUT_SECONDS:=300}"
: "${MC_SNAPSHOT_RETENTION_COUNT:=2}"
: "${MC_SNAPSHOT_BACKEND_TYPE:=}"

: "${MC_MAX_TOTAL_FINISHED_TASKS:=10000}"
: "${MC_MAX_TOTAL_PENDING_TASKS:=10000}"
: "${MC_MAX_TOTAL_PROCESSING_TASKS:=10000}"
: "${MC_PENDING_TASK_TIMEOUT_SEC:=300}"
: "${MC_PROCESSING_TASK_TIMEOUT_SEC:=300}"
: "${MC_MAX_RETRY_ATTEMPTS:=10}"

# glog (google::InitGoogleLogging runs when --log_dir is non-empty in mooncake_master)
: "${MC_LOG_DIR:=/var/log/mooncake}"
: "${MC_GLOG_MINLOGLEVEL:=2}"
: "${MC_GLOG_LOGTOSTDERR:=1}"

case "$(printf '%s' "${MC_ENABLE_HA}" | tr '[:upper:]' '[:lower:]')" in
  true|1|yes|on)
    if [[ -z "${MC_ETCD_ENDPOINTS}" ]]; then
      echo "MC_ETCD_ENDPOINTS must be set when MC_ENABLE_HA=true" >&2
      exit 1
    fi
    ;;
esac

args=()
if [[ -n "${MC_CONFIG_PATH}" ]]; then
  if [[ -f "${MC_CONFIG_PATH}" && -r "${MC_CONFIG_PATH}" ]]; then
    args+=( "--config_path=${MC_CONFIG_PATH}" )
  else
    echo "warning: MC_CONFIG_PATH=${MC_CONFIG_PATH} missing or not readable; omitting --config_path (flags/env in this script only)" >&2
  fi
fi

args+=(
  "--port=${MC_PORT}"
  "--max_threads=${MC_MAX_THREADS}"
  "--enable_metric_reporting=${MC_ENABLE_METRIC_REPORTING}"
  "--metrics_port=${MC_METRICS_PORT}"
  "--default_kv_lease_ttl=${MC_DEFAULT_KV_LEASE_TTL}"
  "--default_kv_soft_pin_ttl=${MC_DEFAULT_KV_SOFT_PIN_TTL}"
  "--allow_evict_soft_pinned_objects=${MC_ALLOW_EVICT_SOFT_PINNED_OBJECTS}"
  "--eviction_ratio=${MC_EVICTION_RATIO}"
  "--eviction_high_watermark_ratio=${MC_EVICTION_HIGH_WATERMARK_RATIO}"
  "--rpc_thread_num=${MC_RPC_THREAD_NUM}"
  "--rpc_port=${MC_RPC_PORT}"
  "--rpc_address=${MC_RPC_ADDRESS}"
  "--rpc_conn_timeout_seconds=${MC_RPC_CONN_TIMEOUT_SECONDS}"
  "--rpc_enable_tcp_no_delay=${MC_RPC_ENABLE_TCP_NO_DELAY}"
  "--enable_ha=${MC_ENABLE_HA}"
  "--enable_offload=${MC_ENABLE_OFFLOAD}"
  "--etcd_endpoints=${MC_ETCD_ENDPOINTS}"
  "--client_ttl=${MC_CLIENT_TTL}"
  "--root_fs_dir=${MC_ROOT_FS_DIR}"
  "--global_file_segment_size=${MC_GLOBAL_FILE_SEGMENT_SIZE}"
  "--cluster_id=${MC_CLUSTER_ID}"
  "--memory_allocator=${MC_MEMORY_ALLOCATOR}"
  "--allocation_strategy=${MC_ALLOCATION_STRATEGY}"
  "--enable_http_metadata_server=${MC_ENABLE_HTTP_METADATA_SERVER}"
  "--http_metadata_server_port=${MC_HTTP_METADATA_SERVER_PORT}"
  "--http_metadata_server_host=${MC_HTTP_METADATA_SERVER_HOST}"
  "--put_start_discard_timeout_sec=${MC_PUT_START_DISCARD_TIMEOUT_SEC}"
  "--put_start_release_timeout_sec=${MC_PUT_START_RELEASE_TIMEOUT_SEC}"
  "--enable_disk_eviction=${MC_ENABLE_DISK_EVICTION}"
  "--quota_bytes=${MC_QUOTA_BYTES}"
  "--snapshot_backup_dir=${MC_SNAPSHOT_BACKUP_DIR}"
  "--enable_snapshot_restore=${MC_ENABLE_SNAPSHOT_RESTORE}"
  "--enable_snapshot=${MC_ENABLE_SNAPSHOT}"
  "--snapshot_interval_seconds=${MC_SNAPSHOT_INTERVAL_SECONDS}"
  "--snapshot_child_timeout_seconds=${MC_SNAPSHOT_CHILD_TIMEOUT_SECONDS}"
  "--snapshot_retention_count=${MC_SNAPSHOT_RETENTION_COUNT}"
  "--snapshot_backend_type=${MC_SNAPSHOT_BACKEND_TYPE}"
  "--max_total_finished_tasks=${MC_MAX_TOTAL_FINISHED_TASKS}"
  "--max_total_pending_tasks=${MC_MAX_TOTAL_PENDING_TASKS}"
  "--max_total_processing_tasks=${MC_MAX_TOTAL_PROCESSING_TASKS}"
  "--pending_task_timeout_sec=${MC_PENDING_TASK_TIMEOUT_SEC}"
  "--processing_task_timeout_sec=${MC_PROCESSING_TASK_TIMEOUT_SEC}"
  "--max_retry_attempts=${MC_MAX_RETRY_ATTEMPTS}"
  "--minloglevel=${MC_GLOG_MINLOGLEVEL}"
  "--logtostderr=${MC_GLOG_LOGTOSTDERR}"
  "--log_dir=${MC_LOG_DIR}"
)

mkdir -p "${MC_LOG_DIR}"

exec "${MC_MASTER_BIN}" "${args[@]}" "$@"
