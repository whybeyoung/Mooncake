#!/usr/bin/env bash

set -euo pipefail

STORE_URL="${STORE_URL:-http://127.0.0.1:18080}"
MASTER_METRICS_URL="${MASTER_METRICS_URL:-http://127.0.0.1:9003/metrics}"
TTL_SECONDS="${TTL_SECONDS:-3.0}"
GROUP_COUNT="${GROUP_COUNT:-20}"
ROUNDS="${ROUNDS:-5}"
CONCURRENCY="${CONCURRENCY:-8}"
PAYLOAD_MB="${PAYLOAD_MB:-1.0}"
ROUND_SLEEP_SECONDS="${ROUND_SLEEP_SECONDS:-1.0}"
FINAL_WAIT_SECONDS="${FINAL_WAIT_SECONDS:-0.8}"
KEY_PREFIX="${KEY_PREFIX:-groupttl$(date +%s)}"
KEEP_DATA="${KEEP_DATA:-0}"
MASTER_HTTP_BASE=""
RUN_PREFIX=""

usage() {
    cat <<'EOF'
Usage:
  scripts/test_group_ttl_with_curl.sh [options]

Options:
  --store-url URL             HTTP address of mooncake_store_test.
  --metrics-url URL           Master /metrics endpoint.
  --master-http-base URL      Master HTTP base URL. Default: metrics-url with trailing /metrics removed.
  --ttl-seconds SEC           Expected master default_kv_lease_ttl, in seconds.
  --group-count N             Number of two-key groups to create.
  --rounds N                  Number of hot-key access rounds.
  --concurrency N             Parallel curl workers per round.
  --payload-mb MB             Payload size used by put / batch_put.
  --round-sleep SEC           Sleep between rounds; should be < ttl-seconds.
  --final-wait SEC            Sleep after the last round before final checks.
  --key-prefix PREFIX         Prefix for generated keys.
  --keep-data                 Skip cleanup.
  -h, --help                  Show this help.

Environment variables with the same names are also supported:
  STORE_URL MASTER_METRICS_URL TTL_SECONDS GROUP_COUNT ROUNDS CONCURRENCY
  PAYLOAD_MB ROUND_SLEEP_SECONDS FINAL_WAIT_SECONDS KEY_PREFIX KEEP_DATA

Recommended master flags for this test:
  mooncake_master --enable_group_ttl=true --default_kv_lease_ttl=3000

Test logic:
  1. Create N groups: <sanitized-prefix>gX_hot and <sanitized-prefix>gX_cold
  2. Create N control keys without '_' so they are not grouped
  3. Repeatedly GET only the hot keys
  4. Verify cold keys still exist after repeated hot-key accesses
  5. Compare master_group_ttl_collateral_lease_renewals_total before/after

Important:
  Lease TTL in Mooncake does not mean immediate key disappearance.
  Expired objects become eviction candidates, but may still remain visible
  until memory pressure triggers eviction.
EOF
}

log() {
    printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

fail() {
    log "ERROR: $*"
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

json_field() {
    local field="$1"
    python3 -c '
import json
import sys

field = sys.argv[1]
data = json.load(sys.stdin)
value = data[field]
if isinstance(value, bool):
    print("true" if value else "false")
else:
    print(value)
' "$field"
}

kv_post() {
    local body="$1"
    curl -fsS -X POST "${STORE_URL}/api/kv" \
        -H 'Content-Type: application/json' \
        -d "$body"
}

health_check() {
    curl -fsS "${STORE_URL}/health" >/dev/null
    curl -fsS "${MASTER_METRICS_URL}" >/dev/null
    curl -fsS "${MASTER_HTTP_BASE}/get_all_keys" >/dev/null
}

metric_value() {
    local name="$1"
    local value
    value="$(curl -fsS "${MASTER_METRICS_URL}" | awk -v metric="$name" '$1 == metric {print $2; found=1} END {if (!found) print 0}')"
    printf '%s\n' "${value:-0}"
}

float_to_sleep() {
    python3 - "$1" <<'PY'
import sys
print(f"{float(sys.argv[1]):.3f}")
PY
}

put_key() {
    local key="$1"
    local response code
    response="$(kv_post "{\"operator\":\"put\",\"key\":\"${key}\",\"size\":\"${PAYLOAD_MB}\"}")"
    code="$(printf '%s' "$response" | json_field code)"
    [[ "$code" == "0" ]] || fail "put failed for key=${key}, response=${response}"
}

get_key() {
    local key="$1"
    local response code
    response="$(kv_post "{\"operator\":\"get\",\"key\":\"${key}\"}")"
    code="$(printf '%s' "$response" | json_field code)"
    [[ "$code" == "0" ]] || fail "get failed for key=${key}, response=${response}"
}

list_all_keys() {
    curl -fsS "${MASTER_HTTP_BASE}/get_all_keys"
}

key_exists_without_renew() {
    local key="$1"
    local all_keys
    all_keys="$(list_all_keys)"
    grep -Fqx "$key" <<<"$all_keys"
}

reset_perf() {
    curl -fsS -X POST "${STORE_URL}/api/reset" >/dev/null
}

cleanup_keys() {
    if [[ "${KEEP_DATA}" == "1" ]]; then
        log "KEEP_DATA=1, skip cleanup"
        return
    fi

    log "cleanup skipped: generated keys use prefix ${RUN_PREFIX}"
}

run_parallel_hot_gets() {
    local started=0
    local i key
    for ((i = 1; i <= GROUP_COUNT; i++)); do
        key="${RUN_PREFIX}g${i}_hot"
        (
            get_key "$key"
        ) &
        started=$((started + 1))
        if (( started % CONCURRENCY == 0 )); then
            wait
        fi
    done
    wait
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --store-url)
            STORE_URL="$2"
            shift 2
            ;;
        --metrics-url)
            MASTER_METRICS_URL="$2"
            shift 2
            ;;
        --ttl-seconds)
            TTL_SECONDS="$2"
            shift 2
            ;;
        --master-http-base)
            MASTER_HTTP_BASE="$2"
            shift 2
            ;;
        --group-count)
            GROUP_COUNT="$2"
            shift 2
            ;;
        --rounds)
            ROUNDS="$2"
            shift 2
            ;;
        --concurrency)
            CONCURRENCY="$2"
            shift 2
            ;;
        --payload-mb)
            PAYLOAD_MB="$2"
            shift 2
            ;;
        --round-sleep)
            ROUND_SLEEP_SECONDS="$2"
            shift 2
            ;;
        --final-wait)
            FINAL_WAIT_SECONDS="$2"
            shift 2
            ;;
        --key-prefix)
            KEY_PREFIX="$2"
            shift 2
            ;;
        --keep-data)
            KEEP_DATA=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown argument: $1"
            ;;
    esac
done

need_cmd curl
need_cmd python3

if [[ -z "${MASTER_HTTP_BASE}" ]]; then
    MASTER_HTTP_BASE="${MASTER_METRICS_URL%/metrics}"
fi

RUN_PREFIX="${KEY_PREFIX//_/}"
[[ -n "${RUN_PREFIX}" ]] || fail "key prefix becomes empty after removing underscores"

python3 - "$TTL_SECONDS" "$ROUND_SLEEP_SECONDS" "$FINAL_WAIT_SECONDS" <<'PY'
import sys
ttl = float(sys.argv[1])
round_sleep = float(sys.argv[2])
final_wait = float(sys.argv[3])
if ttl <= 0:
    raise SystemExit("ttl-seconds must be > 0")
if round_sleep <= 0:
    raise SystemExit("round-sleep must be > 0")
if round_sleep >= ttl:
    raise SystemExit("round-sleep must be smaller than ttl-seconds")
if final_wait <= 0 or final_wait >= ttl:
    raise SystemExit("final-wait must be > 0 and smaller than ttl-seconds")
PY

trap cleanup_keys EXIT

log "checking service health"
health_check || fail "health check failed, please start mooncake_store_test and master metrics first"

log "resetting perf data"
reset_perf

lease_metric_before="$(metric_value master_group_ttl_collateral_lease_renewals_total)"
group_metric_before="$(metric_value master_group_ttl_group_count)"

log "creating ${GROUP_COUNT} grouped key pairs and ${GROUP_COUNT} control keys"
for ((i = 1; i <= GROUP_COUNT; i++)); do
    put_key "${RUN_PREFIX}g${i}_hot"
    put_key "${RUN_PREFIX}g${i}_cold"
    put_key "${RUN_PREFIX}control${i}"
done

group_metric_after_put="$(metric_value master_group_ttl_group_count)"
group_metric_delta="$(python3 - "$group_metric_after_put" "$group_metric_before" <<'PY'
import sys
print(int(float(sys.argv[1]) - float(sys.argv[2])))
PY
)"
log "group count delta after put: ${group_metric_delta}"
if (( group_metric_delta < GROUP_COUNT )); then
    fail "group count delta is smaller than expected, likely due to key naming colliding into the same group: actual=${group_metric_delta}, expected_at_least=${GROUP_COUNT}"
fi

for ((round = 1; round <= ROUNDS; round++)); do
    log "round ${round}/${ROUNDS}: concurrent GET on hot keys"
    run_parallel_hot_gets
    if (( round < ROUNDS )); then
        sleep "$(float_to_sleep "$ROUND_SLEEP_SECONDS")"
    fi
done

log "waiting ${FINAL_WAIT_SECONDS}s before final checks"
sleep "$(float_to_sleep "$FINAL_WAIT_SECONDS")"

log "checking grouped cold keys should still exist"
for ((i = 1; i <= GROUP_COUNT; i++)); do
    key="${RUN_PREFIX}g${i}_cold"
    key_exists_without_renew "$key" || fail "group peer key should still exist: ${key}"
done

log "checking control keys visibility for reference"
visible_controls=0
for ((i = 1; i <= GROUP_COUNT; i++)); do
    key="${RUN_PREFIX}control${i}"
    if key_exists_without_renew "$key"; then
        visible_controls=$((visible_controls + 1))
    fi
done

lease_metric_after="$(metric_value master_group_ttl_collateral_lease_renewals_total)"
lease_metric_delta="$(python3 - "$lease_metric_after" "$lease_metric_before" <<'PY'
import sys
print(int(float(sys.argv[1]) - float(sys.argv[2])))
PY
)"
expected_min_renewals=$((GROUP_COUNT * ROUNDS))

perf_json="$(curl -fsS "${STORE_URL}/api/performance")"

log "validation passed"
printf '\n'
printf 'Summary\n'
printf '  key_prefix: %s\n' "$KEY_PREFIX"
printf '  run_prefix: %s\n' "$RUN_PREFIX"
printf '  grouped_pairs: %s\n' "$GROUP_COUNT"
printf '  rounds: %s\n' "$ROUNDS"
printf '  expected_min_collateral_renewals: %s\n' "$expected_min_renewals"
printf '  actual_collateral_renewals_delta: %s\n' "$lease_metric_delta"
printf '  master_group_ttl_group_count_before: %s\n' "$group_metric_before"
printf '  master_group_ttl_group_count_after_put: %s\n' "$group_metric_after_put"
printf '  visible_control_keys_after_test: %s\n' "$visible_controls"
printf '\n'
printf 'Store performance JSON\n%s\n' "$perf_json"

if (( lease_metric_delta < expected_min_renewals )); then
    fail "collateral renewal metric is lower than expected minimum: actual=${lease_metric_delta}, expected_min=${expected_min_renewals}"
fi
