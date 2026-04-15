# Smoke Test — connect_basic

## Purpose

Verify that `connect_basic_example` completes the full connection lifecycle:
`eop_connect()` -> verify handle -> `eop_disconnect()`.

## Prerequisites

- Project built with CMake (`cmake --build build`)
- `nc` (netcat) installed, or a running EOP server

## Steps

### Option A: Using netcat (no server needed)

1. Start a TCP listener on port 9026:

```bash
nc -l -p 9026 &
```

2. Run the example:

```bash
./build/client/connect_basic_example 127.0.0.1 9026
```

3. Expected output:

```
Connecting to 127.0.0.1:9026...
Connected successfully.
Disconnected.
```

4. Exit code should be `0`.

### Option B: Using the EOP server

1. Start the EOP server (once available):

```bash
docker compose up -d eop-server
```

2. Run the example:

```bash
./build/client/connect_basic_example 127.0.0.1 9026
```

3. Expected output is the same as Option A.

## Error case

If no server is listening, the example should print an error and exit with code `1`:

```
Connecting to 127.0.0.1:9026...
Connection failed: Connection failed after poll
```
