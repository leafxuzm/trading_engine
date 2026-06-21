# Chronos Trading Engine

**Ultra-low latency quantitative trading platform** — public demo of the HFT pipeline.

Sub-50μs market-data-to-strategy-decision latency, lock-free architecture, C++20.

## Architecture

```
┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ Market Data  │───→│   Strategy   │───→│     Risk     │───→│    Order     │
│   Gateway    │    │   Engine     │    │   Engine     │    │   Gateway    │
│ (AnyGateway) │    │ (poll loop)  │    │ (<1μs check) │    │ (auto-fill)  │
└──────┬───────┘    └──────┬───────┘    └──────┬───────┘    └──────┬───────┘
       │                   │                   │                   │
       │         ┌─────────┴─────────┐         │                   │
       │         │   MPMC Queue      │         │              FillCallback
       │         │ (lock-free, 65536)│         │         ┌─────────┴─────────┐
       │         └───────────────────┘         │         │  engine.pushFill() │
       │                                       │         │  zmq.publishFill() │
       │                                       │         └───────────────────┘
       │                                       │
  ┌────┴────┐                            ┌────┴────┐
  │  ZMQ    │                            │  Log    │
  │ Bridge  │                            │ Writer  │
  └─────────┘                            └─────────┘
```

### Single-process, multi-threaded

All core trading runs in one process. Logging and backtesting are separate processes connected via ZeroMQ PUB-SUB.

### Dependency Layering

```
   core/  (types, config, Decimal)
     ↓
   io/   (transport, protocol, security)  ← transport-agnostic design
     ↓
   market_data/ + execution/ + trading/ + risk/
     ↓
   strategies/  ← GridStrategy, etc.
     ↓
   logging/ + backtest/
```

Lower layers never depend on upper layers. `io/` is transport-agnostic — WebSocket (Binance/OKX), raw TCP (SSE STEP), and UDP multicast (SZSE MDDP) all implement the same interfaces.

## Performance Targets

| Component | Operation | P99 Target |
|-----------|-----------|------------|
| MPMC Queue | push / pop | <50ns |
| OrderBook V2 | best bid/ask (hot) | <10ns |
| OrderBook V2 | top 5 levels (hot) | <10ns |
| OrderBook V2 | full 20 levels (cold) | <50ns |
| OrderBook V2 | update | <100ns |
| OrderIDGenerator | nextID() | <10ns |
| RiskEngine | checkOrder() | <1μs |
| StrategyEngine | onTick() | <10μs |
| MarketDataGateway | tick → queue | <100μs |
| OrderGateway | order send | <500μs |
| **End-to-end** | **tick → decision** | **<50μs** |
| **System throughput** | tick processing | **>1M/sec** |

*Benchmarked on Apple M1 Max, -O3 -march=native -mtune=native, Clang 17.*

## Design Patterns

### 1. Hot/Cold Data Separation
Frequently accessed fields (top 5 price levels) in compact cache-aligned structs; cold data in separate storage. `OrderBookV2` hot path reads from L1 cache only.

### 2. Lock-Free Optimistic Reads (Seqlock)
Atomic 64-bit version counter. Reader retries if version changed during read. Zero writer-blocking. Used in `OrderBook` V1/V2 hot data paths.

### 3. Double Buffering (RCU-style)
Two data copies; writer updates the dark buffer, then atomic pointer swap makes it visible. Used in `OrderBookV2` cold data (full 20-level depth).

### 4. Process Isolation via ZMQ
Latency-critical core in one process; I/O-heavy logging/backtest in separate processes connected by ZMQ PUB-SUB.

### 5. Fixed-Point Arithmetic
All financial values use `Decimal` = `fpm::fixed<int64_t, __int128, 32>` (64-bit, 32 fractional bits). Conversion to/from `double` only at API boundaries.

## Quick Start

### Prerequisites (macOS)

```bash
brew install cmake boost openssl zeromq pkg-config
```

### Build

```bash
# Clone the project family
git clone https://github.com/Leafxu/libchronos-deps.git
git clone <private>/libchronos.git        # core library (private repo)
git clone https://github.com/Leafxu/trading_engine.git

# Build (libchronos-deps and libchronos are pulled in automatically)
cd trading_engine
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
```

### Run

```bash
# File replay mode (historical data)
./trading_engine --config=../config/example.yaml --replay=./logs --date=20240617

# Live mode (requires IO layer: Boost + OpenSSL)
./trading_engine --config=../config/example.yaml --live --exchange=binance
```

### Configuration

See `config/example.yaml` for the complete YAML configuration format with all options documented.

## Project Family

| Repository | Visibility | Purpose |
|------------|------------|---------|
| [libchronos-deps](https://github.com/Leafxu/libchronos-deps) | Public | Third-party dependency aggregation |
| libchronos | Private | Core library (`libchronos.a`) — all algorithm implementations |
| [trading_engine](https://github.com/Leafxu/trading_engine) | Public | Application demo — pipeline orchestration |

## Design Philosophy

> **"懂C++ ≠ 懂延迟"** — understanding C++ is necessary but not sufficient for low-latency programming. Latency comes from understanding the hardware: cache hierarchy, store buffers, branch predictors, and memory ordering. A technically correct C++ program can still destroy cache locality, trigger false sharing, or stall on unnecessary barriers.

Key principles in this codebase:

1. **Measure, don't assume.** Every component has a latency budget and a benchmark.
2. **Cold data must not pollute hot cache lines.** `alignas(64)` on all shared-memory data structures.
3. **Lock-free where it matters.** CAS loops over mutexes when contention is low; MPMC queue for producer-consumer decoupling.
4. **No allocation on the hot path.** Pre-allocated object pools, fixed-size arrays, no `std::vector` in tick processing.
5. **Explicit memory ordering.** Every `std::atomic` has an explicit `std::memory_order` — no implicit sequential consistency.

## License

MIT — see [LICENSE](LICENSE).
