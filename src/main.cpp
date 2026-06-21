/**
 * @file main.cpp
 * @brief Chronos Trading Engine — public demo of the HFT pipeline.
 *
 * Architecture:
 *   [Log Files] → TimeReplayer ──→ engine.pushTick() ──→ StrategyEngine.run()
 *                                           │                    │
 *                                    [shared OrderQueue]         │
 *                                           │              OrderGateway.run()
 *                                    [Live Market Data]          │
 *                                                          FillCallback:
 *                                                          ├─ engine.pushFill()
 *                                                          └─ zmq.publishFill()
 *
 * Two modes:
 *   --replay <dir>   Replay historical ticks from binary logs (default)
 *   --live           Live market data via exchange testnet
 */

#include "app_config.hpp"

// L1: Core types
#include <chronos/core/config.hpp>
#include <chronos/core/types.hpp>

// L2: Logging & replay infrastructure
#include <chronos/backtest/time_replayer.hpp>
#include <chronos/logging/log_reader.hpp>
#include <chronos/logging/log_writer.hpp>
#include <chronos/logging/zmq_bridge.hpp>

// L3: Pipeline components
#include <chronos/trading/strategy_engine.hpp>
#include <chronos/risk/risk_engine.hpp>
#include <chronos/execution/order_gateway.hpp>

// L4: Strategy implementations
#include <chronos/strategies/grid_strategy.hpp>

#ifdef CHRONOS_HAS_LIVE_MODE
// L5: Live market data (requires IO layer: Boost.Beast + OpenSSL)
#include <chronos/execution/binance_http_client.hpp>
#include <chronos/execution/binance_user_stream.hpp>
#include <chronos/market_data/any_gateway.hpp>
#include <chronos/market_data/gateway_v2_factory.hpp>
#endif

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace chronos;
using namespace chronos::trading;
using namespace chronos::execution;
using namespace chronos::logging;
using namespace chronos::market_data;
using namespace chronos::strategies;

namespace {

// ============================================================================
// Signal handling
// ============================================================================

std::atomic<bool> g_running{true};

void handleSignal(int sig) {
    std::fprintf(stderr, "\nSignal %d received, shutting down...\n", sig);
    g_running.store(false, std::memory_order_release);
}

// ============================================================================
// Stats printing
// ============================================================================

void printHeader() {
    std::printf("\n");
    std::printf("╔══════════════════════════════════════════════════╗\n");
    std::printf("║         Chronos Trading Engine v0.3              ║\n");
    std::printf("║         Pipeline: Engine → Risk → Gateway        ║\n");
    std::printf("╚══════════════════════════════════════════════════╝\n");
    std::printf("\n");
}

void printStatsPeriodic(const StrategyEngine::Stats& es,
                         const OrderGatewayStats& gs) {
    std::printf("\r| ticks: %8llu | orders: %6llu | fills: %6llu | "
                "GW sent: %6llu | GW fill: %6llu | "
                "rej: %4llu | drop: %4llu |",
                (unsigned long long)es.ticks_processed,
                (unsigned long long)es.orders_submitted,
                (unsigned long long)es.fills_processed,
                (unsigned long long)gs.orders_sent,
                (unsigned long long)gs.fills_received,
                (unsigned long long)es.orders_risk_rejected,
                (unsigned long long)es.orders_queue_dropped);
    std::fflush(stdout);
}

void printSummary(const StrategyEngine::Stats& es,
                  const OrderGatewayStats& gs,
                  const ZMQBridge& zmq,
                  const LogWriter& lw) {
    auto pl = [](const char* label, uint64_t v) {
        std::printf("  %-28s %8llu\n", label, (unsigned long long)v);
    };

    std::printf("\n");
    std::printf("═══════════════════════════════════════════\n");
    std::printf("          Session Summary                   \n");
    std::printf("═══════════════════════════════════════════\n");
    std::printf("── Engine ────────────────────────────────\n");
    pl("Ticks processed:",       es.ticks_processed);
    pl("Orders submitted:",      es.orders_submitted);
    pl("Orders risk-rejected:",  es.orders_risk_rejected);
    pl("Orders queue-dropped:",  es.orders_queue_dropped);
    pl("Fills processed:",       es.fills_processed);
    std::printf("── Order Gateway ─────────────────────────\n");
    pl("GW orders received:",    gs.orders_received);
    pl("GW orders sent:",        gs.orders_sent);
    pl("GW orders cancelled:",   gs.orders_cancelled);
    pl("GW orders modified:",    gs.orders_modified);
    pl("GW fills received:",     gs.fills_received);
    pl("GW exchange mismatch:",  gs.exchange_id_mismatch);
    pl("GW avg send latency:",   gs.avg_send_latency_us);
    std::printf("── Output ───────────────────────────────\n");
    pl("ZMQ ticks published:",   zmq.ticksPublished());
    pl("ZMQ fills published:",   zmq.fillsPublished());
    pl("ZMQ dropped:",           zmq.droppedCount());
    pl("Log ticks written:",     lw.ticksWritten());
    std::printf("═══════════════════════════════════════════\n\n");
}

// ============================================================================
// Strategy registration
// ============================================================================

void registerStrategies(StrategyEngine& engine, const AppConfig& cfg) {
    for (auto& sc : cfg.strategies) {
        if (sc.type == "grid" || sc.type == "GridStrategy") {
            GridStrategy::Config grid_cfg;
            grid_cfg.symbol_id   = sc.symbol_id;
            grid_cfg.grid_low    = sc.grid_low;
            grid_cfg.grid_high   = sc.grid_high;
            grid_cfg.grid_levels = sc.grid_levels;
            grid_cfg.quantity    = sc.quantity;

            auto strategy = std::make_unique<GridStrategy>(grid_cfg);
            std::printf("Strategy:      GridStrategy (symbol=%u, %.1f-%.1f, %d levels)\n",
                        grid_cfg.symbol_id, grid_cfg.grid_low,
                        grid_cfg.grid_high, grid_cfg.grid_levels);
            engine.registerStrategy(std::move(strategy));
        } else {
            std::fprintf(stderr, "Warning: unknown strategy type '%s', skipping\n",
                         sc.type.c_str());
        }
    }
}

// ============================================================================
// OrderGateway factory — shared queue + FillCallback → engine + ZMQ
// ============================================================================

std::unique_ptr<OrderGateway> createOrderGateway(StrategyEngine& engine,
                                                  ZMQBridge& zmq,
                                                  const ExchangeConfig& exCfg,
                                                  BinanceHttpClient* httpClient,
                                                  bool simulateFills) {
    auto fill_cb = [&engine, &zmq](const Fill& fill) {
        engine.pushFill(fill);
        zmq.publishFill(fill);
    };

    auto gw = std::make_unique<OrderGateway>(
        exCfg,
        engine.getOrderQueue(),
        std::move(fill_cb),
        0,
        simulateFills);

    if (httpClient && !simulateFills) {
        gw->setHttpClient(httpClient);
    }

    return gw;
}

// ============================================================================
// File replay mode (always available)
// ============================================================================

int runFileReplay(const AppConfig& cfg) {
    std::printf("Mode:          file replay\n");
    std::printf("Log directory: %s\n", cfg.log_dir.c_str());
    if (!cfg.date.empty()) std::printf("Date:          %s\n", cfg.date.c_str());

    LogFileSet logSet;
    if (!cfg.date.empty()) {
        logSet = openLogDirectory(cfg.log_dir, cfg.date);
    } else {
        logSet = openLogDirectory(cfg.log_dir);
    }

    if (!logSet.tick.isOpen()) {
        std::fprintf(stderr, "Error: no tick log found in '%s'\n", cfg.log_dir.c_str());
        return 1;
    }
    std::printf("Tick records:  %zu\n", logSet.tick.recordCount());

    // Logging output
    LogWriter logWriter;
    {
        LogConfig logCfg;
        logCfg.log_dir = cfg.log_cfg.output_dir;
        if (!logCfg.log_dir.empty()) {
            std::filesystem::create_directories(logCfg.log_dir);
            logWriter.initialize(logCfg.log_dir, logCfg);
            std::printf("Log output:    %s\n", logCfg.log_dir.c_str());
        }
    }

    // ZMQ bridge
    ZMQBridge zmqBridge;
    {
        logging::ZMQConfig zmqCfg;
        zmqCfg.bind_address = cfg.zmq_cfg.endpoint;
        zmqCfg.send_hwm = cfg.zmq_cfg.hwm;
        zmqCfg.linger_ms = cfg.zmq_cfg.linger;
        if (!zmqBridge.initialize(zmqCfg)) {
            std::fprintf(stderr, "Warning: ZMQ init failed, continuing without ZMQ\n");
        } else {
            std::printf("ZMQ endpoint:  %s\n", zmqCfg.bind_address.c_str());
        }
    }

    // StrategyEngine
    StrategyEngine engine;
    engine.updateRiskParameters(cfg.risk_params);
    engine.setAvailableCapital(toDecimal(cfg.initial_capital));
    registerStrategies(engine, cfg);

    // OrderGateway (simulated fills)
    ExchangeConfig simCfg;
    simCfg.name = "simulated";
    auto gateway = createOrderGateway(engine, zmqBridge, simCfg,
                                      nullptr, true);
    std::printf("Execution:     simulated (OrderGateway auto-fill)\n");
    std::printf("\nStarting pipeline...\n\n");

    engine.start();
    gateway->start();

    // TimeReplayer
    backtest::TimeReplayer replayer;
    if (logSet.tick.isOpen()) replayer.addStream(logSet.tick);

    uint64_t tick_count = 0;
    auto last_stats_time = std::chrono::steady_clock::now();

    replayer.setTickCallback([&](const Tick& tick) {
        if (!g_running.load(std::memory_order_acquire)) return;
        tick_count++;
        engine.pushTick(tick);
        zmqBridge.publishTick(tick);
        if (logWriter.isRunning()) logWriter.writeTick(tick);
    });

    replayer.setOrderCallback([](const OrderRequest&) {});
    replayer.setFillCallback([](const Fill&) {});

    // Replay loop
    while (g_running.load(std::memory_order_acquire) && !replayer.isExhausted()) {
        if (!replayer.advanceToNextEvent()) break;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_time).count() >= 2) {
            printStatsPeriodic(engine.getStats(), gateway->getStats());
            last_stats_time = now;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    auto final_engine_stats = engine.getStats();
    auto final_gw_stats = gateway->getStats();

    std::printf("\n\nShutting down...\n");
    gateway->stop();
    engine.stop();
    zmqBridge.stop();
    logWriter.flush();
    logWriter.stop();

    printSummary(final_engine_stats, final_gw_stats, zmqBridge, logWriter);
    std::printf("Shutdown complete. %llu ticks replayed.\n",
                (unsigned long long)tick_count);
    return 0;
}

// ============================================================================
// Live mode (requires CHRONOS_HAS_LIVE_MODE — IO layer)
// ============================================================================

#ifdef CHRONOS_HAS_LIVE_MODE

int runLive(const AppConfig& cfg) {
    std::printf("Mode:          live (market data from %s testnet)\n",
                cfg.exchange_name.c_str());

    // Logging
    LogWriter logWriter;
    {
        LogConfig logCfg;
        logCfg.log_dir = cfg.log_cfg.output_dir;
        if (!logCfg.log_dir.empty()) {
            std::filesystem::create_directories(logCfg.log_dir);
            logWriter.initialize(logCfg.log_dir, logCfg);
            std::printf("Log output:    %s\n", logCfg.log_dir.c_str());
        }
    }

    // ZMQ
    ZMQBridge zmqBridge;
    {
        logging::ZMQConfig zmqCfg;
        zmqCfg.bind_address = cfg.zmq_cfg.endpoint;
        zmqCfg.send_hwm = cfg.zmq_cfg.hwm;
        zmqCfg.linger_ms = cfg.zmq_cfg.linger;
        if (!zmqBridge.initialize(zmqCfg)) {
            std::fprintf(stderr, "Warning: ZMQ init failed\n");
        } else {
            std::printf("ZMQ endpoint:  %s\n", zmqCfg.bind_address.c_str());
        }
    }

    // StrategyEngine
    StrategyEngine engine;
    engine.updateRiskParameters(cfg.risk_params);
    engine.setAvailableCapital(toDecimal(cfg.initial_capital));
    registerStrategies(engine, cfg);

    // ExchangeConfig
    ExchangeConfig exCfg;
    exCfg.name = cfg.exchange_name;
    exCfg.websocket_url = cfg.exchange_url;
    exCfg.rest_url      = cfg.rest_url;
    exCfg.user_stream_url = cfg.user_stream_url;
    exCfg.proxy_host    = cfg.proxy_host;
    exCfg.proxy_port    = cfg.proxy_port;
    exCfg.api_key       = cfg.exchange_api_key;
    exCfg.api_secret    = cfg.exchange_api_secret;
    exCfg.symbols       = cfg.exchange_symbols;

    if (exCfg.websocket_url.empty()) {
        std::fprintf(stderr, "Error: no websocket_url in exchange config\n");
        return 1;
    }

    bool useRealExecution = !cfg.force_simulate &&
                            !cfg.exchange_api_key.empty() &&
                            !cfg.exchange_api_secret.empty();

    std::unique_ptr<BinanceHttpClient> httpClient;
    std::unique_ptr<BinanceUserStream> userStream;

    auto gateway = createOrderGateway(engine, zmqBridge, exCfg,
                                      httpClient.get(), !useRealExecution);

    if (useRealExecution) {
        std::printf("Execution:     REAL (Binance testnet, fake money)\n");
        httpClient = std::make_unique<BinanceHttpClient>();
        gateway->setHttpClient(httpClient.get());
        userStream = std::make_unique<BinanceUserStream>();
    } else {
        std::printf("Execution:     simulated\n");
    }

    // Market data gateway
    auto md_queue = std::make_unique<utils::MPMCQueue<Tick, 65536>>();
    std::printf("Exchange:      %s (%s)\n",
                cfg.exchange_name.c_str(), cfg.exchange_url.c_str());

    auto md_gateway = createGatewayV2(cfg.exchange_name, exCfg, *md_queue);
    if (!md_gateway) {
        std::fprintf(stderr, "Error: failed to create gateway for '%s'\n",
                     cfg.exchange_name.c_str());
        return 1;
    }

    for (auto& sym : cfg.exchange_symbols) {
        std::printf("Subscribing:   %s\n", sym.c_str());
        md_gateway.subscribe(sym);
    }
    if (cfg.exchange_symbols.empty()) {
        std::printf("Subscribing:   btcusdt (default)\n");
        md_gateway.subscribe("btcusdt");
    }

    std::printf("\nStarting pipeline...\n\n");

    engine.start();
    gateway->start();

    if (userStream) {
        std::printf("\nStarting Binance user data stream...\n");
        if (!userStream->start(exCfg, httpClient.get(), gateway.get())) {
            std::fprintf(stderr, "Warning: user data stream failed\n");
            gateway->setUserStream(nullptr);
        } else {
            gateway->setUserStream(userStream.get());
        }
    }

    md_gateway.start();

    // Bridge thread: market data queue → engine
    std::atomic<uint64_t> last_tick_ts{0};
    std::atomic<double>   last_tick_price{0};
    std::atomic<double>   last_tick_qty{0};
    std::atomic<int>      last_tick_side{0};
    std::atomic<uint32_t> last_tick_count{0};

    std::thread md_bridge([&] {
        while (g_running.load(std::memory_order_acquire)) {
            Tick tick;
            while (md_queue->try_pop(tick)) {
                // Print every 50th tick with full details
                auto cnt = ++last_tick_count;
                if (cnt % 50 == 0) {
                    std::printf("  [tick#%u] price=%.2f qty=%.4f side=%s sym=%u\n",
                                cnt, toDouble(tick.price), toDouble(tick.quantity),
                                tick.side == TickSide::BID ? "BID" : "ASK",
                                tick.symbol_id);
                }
                last_tick_price.store(toDouble(tick.price), std::memory_order_relaxed);
                last_tick_qty.store(toDouble(tick.quantity), std::memory_order_relaxed);
                last_tick_side.store(static_cast<int>(tick.side), std::memory_order_relaxed);
                last_tick_ts.store(tick.exchange_timestamp_us, std::memory_order_relaxed);

                engine.pushTick(tick);
                zmqBridge.publishTick(tick);
                if (logWriter.isRunning()) logWriter.writeTick(tick);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        auto p = last_tick_price.load(std::memory_order_relaxed);
        if (p > 0) {
            std::printf("  [latest] price=%.2f qty=%.4f side=%s\n",
                        p, last_tick_qty.load(std::memory_order_relaxed),
                        last_tick_side.load(std::memory_order_relaxed) == 0 ? "BID" : "ASK");
        }
        printStatsPeriodic(engine.getStats(), gateway->getStats());
    }

    std::printf("\n\nShutting down...\n");
    g_running.store(false);
    md_bridge.join();
    md_gateway.stop();
    if (userStream && userStream->isRunning()) userStream->stop();
    gateway->stop();
    engine.stop();
    zmqBridge.stop();
    logWriter.flush();
    logWriter.stop();

    printSummary(engine.getStats(), gateway->getStats(), zmqBridge, logWriter);
    std::printf("Shutdown complete.\n");
    return 0;
}

#else  // !CHRONOS_HAS_LIVE_MODE

int runLive(const AppConfig& cfg) {
    (void)cfg;
    std::fprintf(stderr, "Error: live mode not available — "
                          "libchronos built without IO layer (Boost/OpenSSL)\n");
    return 1;
}

#endif  // CHRONOS_HAS_LIVE_MODE

}  // namespace

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    AppConfig cfg;
    if (!parseArgs(argc, argv, cfg)) return 1;
    if (!loadConfig(cfg.config_path, cfg)) return 1;

    printHeader();

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    if (cfg.live_mode) {
        return runLive(cfg);
    }

    if (cfg.log_dir.empty()) {
        std::fprintf(stderr, "Error: --replay <dir> required for file replay mode\n");
        return 1;
    }

    return runFileReplay(cfg);
}
