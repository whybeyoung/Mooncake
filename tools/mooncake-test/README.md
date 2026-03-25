# Mooncake Test Tool

A performance testing tool for Mooncake KVStore, providing HTTP-based API for simulating inference engine calls.

## Overview

This tool provides a lightweight HTTP server that wraps the Mooncake KVStore client, enabling performance testing of put/get/exist operations. It includes built-in performance metrics collection using t-digest algorithm for accurate percentile estimation.

## Build

### Prerequisites

- CMake 3.16 or higher
- C++20 compatible compiler (GCC 11+, Clang 14+)
- Mooncake repository with all dependencies

### Build Steps

```bash
cd <mooncake-repo-root>
cmake -S . -B build \
  -DWITH_TE=ON \
  -DWITH_STORE=ON \
  -DBUILD_MOONCAKE_TEST=ON
cmake --build build -j$(nproc) --target mooncake_test
```

## Usage

### Command Line Options

| Option | Default | Description |
|--------|---------|-------------|
| `--http-host` | `0.0.0.0` | HTTP listen host |
| `--http-port` | `18080` | HTTP listen port for this tool's KV API (named to avoid clashing with `mooncake_client`'s `--http_port`) |
| `--real-client-address` | `127.0.0.1:50052` | Real client RPC address |
| `--ipc-socket-path` | (auto-derived) | IPC socket path (empty means auto-derived from `--real-client-address`) |
| `--mem-pool-size` | `268435456` | Memory pool size in bytes (256MB) |
| `--local-buffer-size` | `268435456` | Local buffer size in bytes (256MB) |
| `--min-put-size-mb` | `1.0` | Minimum `size` (MB) allowed for `put` / `batch_put` (inclusive) |
| `--max-put-size-mb` | `100.0` | Maximum `size` (MB) allowed for `put` / `batch_put` (inclusive) |

### Start the Service

```bash
./mooncake_test \
    --http-port=18080 \
    --real-client-address="127.0.0.1:50052" \
    --mem-pool-size=268435456 \
    --local-buffer-size=268435456 \
    --min-put-size-mb=1.0 \
    --max-put-size-mb=100.0
```

## API Reference

### Health Check

**Endpoint:** `GET /health`

**Response:**
```json
{
    "ready": true,
    "status": "healthy",
    "real_client_address": "127.0.0.1:50052",
    "ipc_socket_path": "@mooncake_client_50052.sock"
}
```

### KV Operations

**Endpoint:** `POST /api/kv`

**Request Body:**

| Field | Type | Description |
|-------|------|-------------|
| `operator` | string | Operation type: `put`, `get`, `exist`, `batch_put`, `batch_get`, `batch_exist` |
| `key` | string | Key (comma-separated for batch operations) |
| `size` | string | Data size in MB (for `put` / `batch_put`); must be within `[min-put-size-mb, max-put-size-mb]` (defaults: 1.0..100.0) |

#### Single Put

**Request:**
```json
{
    "operator": "put",
    "key": "test_key_1",
    "size": "2.1"
}
```

**Response:**
```json
{
    "operator": "put",
    "key": "test_key_1",
    "code": 0,
    "duration_ms": 2.35,
    "size_mb": 2.0,
    "value_bytes": 2097152
}
```

#### Single Get

**Request:**
```json
{
    "operator": "get",
    "key": "test_key_1"
}
```

**Response:**
```json
{
    "operator": "get",
    "key": "test_key_1",
    "code": 0,
    "size": 2202009,
    "duration_ms": 1.82
}
```

#### Single Exist

**Request:**
```json
{
    "operator": "exist",
    "key": "test_key_1"
}
```

**Response:**
```json
{
    "operator": "exist",
    "key": "test_key_1",
    "exists": true,
    "code": 1,
    "duration_ms": 0.95
}
```

#### Batch Put

**Request:**
```json
{
    "operator": "batch_put",
    "key": "test_key_1,test_key_2,test_key_3",
    "size": "1.0"
}
```

**Response:**
```json
{
    "operator": "batch_put",
    "keys": ["test_key_1", "test_key_2", "test_key_3"],
    "total": 3,
    "success": 3,
    "duration_ms": 5.21,
    "size_mb": 1.0,
    "value_bytes": 1048576
}
```

#### Batch Get

**Request:**
```json
{
    "operator": "batch_get",
    "key": "test_key_1,test_key_2,test_key_3"
}
```

**Response:**
```json
{
    "operator": "batch_get",
    "keys": ["test_key_1", "test_key_2", "test_key_3"],
    "total": 3,
    "success": 3,
    "duration_ms": 4.15
}
```

#### Batch Exist

**Request:**
```json
{
    "operator": "batch_exist",
    "key": "test_key_1,test_key_2,test_key_3"
}
```

**Response:**
```json
{
    "operator": "batch_exist",
    "keys": ["test_key_1", "test_key_2", "test_key_3"],
    "total": 3,
    "exists": 3,
    "duration_ms": 2.33
}
```

### Performance Metrics

**Endpoint:** `GET /api/performance`

**Response:**
```json
[
    {
        "tag": "put",
        "avgMs": 2.28,
        "minMs": 1.90,
        "maxMs": 2.66,
        "p90Ms": 2.60,
        "p95Ms": 2.64,
        "p99Ms": 2.66,
        "qps": 438.15,
        "totalDurationMs": 4.56,
        "totalTimeSpanMs": 4.56,
        "totalRequests": 2
    },
    {
        "tag": "mix",
        "avgMs": 2.15,
        "minMs": 0.95,
        "maxMs": 5.21,
        "p90Ms": 4.85,
        "p95Ms": 5.10,
        "p99Ms": 5.21,
        "qps": 512.34,
        "totalDurationMs": 23.45,
        "totalTimeSpanMs": 23.45,
        "totalRequests": 10
    }
]
```

**Metrics Description:**

| Field | Description | Unit |
|-------|-------------|------|
| `tag` | Operation type (`put`, `get`, `exist`, `batch_put`, `batch_get`, `batch_exist`, `mix`) | - |
| `avgMs` | Average response time | ms |
| `minMs` | Minimum response time | ms |
| `maxMs` | Maximum response time | ms |
| `p90Ms` | 90th percentile response time (t-digest estimate, ±0.2% ~ 0.5% error) | ms |
| `p95Ms` | 95th percentile response time | ms |
| `p99Ms` | 99th percentile response time | ms |
| `qps` | Queries per second | req/s |
| `totalDurationMs` | Sum of all request durations | ms |
| `totalTimeSpanMs` | Time span from first to last request | ms |
| `totalRequests` | Total number of requests | count |

**Note:** The `mix` tag represents aggregated statistics across all operation types.

### Reset Performance Data

**Endpoint:** `POST /api/reset`

**Response:**
```json
{
    "status": "reset_success",
    "message": "Performance data has been reset"
}
```

## Architecture

```
+------------------+     HTTP      +-------------------+     RPC      +----------------+
|  Test Client     | ----------->  |  HttpKVService    | -----------> |  Mooncake      |
|  (curl, wrk,     |  POST /api/kv |  (mooncake_test)  |  DummyClient |  KVStore       |
|   JMeter, etc.)  |               |                   |              |                |
+------------------+               +-------------------+              +----------------+
                                          |
                                          v
                                   +-------------------+
                                   |  PerfCollector    |
                                   |  (t-digest based) |
                                   +-------------------+
```

## Example Usage

### Using curl

```bash
# Health check
curl http://localhost:18080/health

# Single put (2MB random data)
curl -X POST http://localhost:18080/api/kv \
    -H "Content-Type: application/json" \
    -d '{"operator": "put", "key": "test_key_1", "size": "2.0"}'

# Single get
curl -X POST http://localhost:18080/api/kv \
    -H "Content-Type: application/json" \
    -d '{"operator": "get", "key": "test_key_1"}'

# Batch put (3 keys, 1MB each)
curl -X POST http://localhost:18080/api/kv \
    -H "Content-Type: application/json" \
    -d '{"operator": "batch_put", "key": "key1,key2,key3", "size": "1.0"}'

# Get performance metrics
curl http://localhost:18080/api/performance

# Reset performance data
curl -X POST http://localhost:18080/api/reset
```

### Performance Testing Script

```bash
#!/bin/bash

BASE_URL="http://localhost:18080"

# Run 100 put operations
for i in $(seq 1 100); do
    curl -s -X POST "$BASE_URL/api/kv" \
        -H "Content-Type: application/json" \
        -d "{\"operator\": \"put\", \"key\": \"perf_key_$i\", \"size\": \"1.0\"}" > /dev/null
done

# Run 100 get operations
for i in $(seq 1 100); do
    curl -s -X POST "$BASE_URL/api/kv" \
        -H "Content-Type: application/json" \
        -d "{\"operator\": \"get\", \"key\": \"perf_key_$i\"}" > /dev/null
done

# Get performance report
curl -s "$BASE_URL/api/performance" | python3 -m json.tool
```

## Code Style

This project follows the coding style guidelines defined in [CODING_STYLE.md](CODING_STYLE.md).

## License

Copyright (c) 2024 Mooncake Project. All rights reserved.
