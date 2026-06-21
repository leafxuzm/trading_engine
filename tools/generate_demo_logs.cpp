#include <chronos/core/types.hpp>
#include <chronos/logging/log_writer.hpp>
#include <cstdio>

using namespace chronos;
using namespace chronos::logging;

int main() {
    LogConfig cfg;
    cfg.log_dir = "/tmp/chronos_demo_logs";
    cfg.buffer_size = 65536;
    cfg.flush_interval_ms = 100;
    cfg.retention_days = 7;
    cfg.enable_tick_logging = true;
    cfg.enable_order_logging = true;
    cfg.enable_fill_logging = true;

    LogWriter writer;
    if (!writer.initialize(cfg.log_dir, cfg)) {
        std::fprintf(stderr, "Failed to initialize log writer\n");
        return 1;
    }

    for (int i = 0; i < 1000; i++) {
        Tick t;
        t.price = toDecimal(100.0 + (i % 100) * 0.01);
        t.quantity = toDecimal(0.1 + (i % 10) * 0.05);
        t.symbol_id = 1;
        t.side = TickSide::BID;
        t.exchange_timestamp_us = static_cast<uint64_t>(1000000 + i * 1000);
        t.receive_timestamp_us = static_cast<uint64_t>(2000000 + i * 1000);
        writer.writeTick(t);
    }
    std::printf("Generated 1000 ticks\n");

    for (int i = 0; i < 50; i++) {
        OrderRequest o;
        o.order_id = 1000 + i;
        o.price = toDecimal(100.0 + (i % 20) * 0.5);
        o.quantity = toDecimal(0.1);
        o.symbol_id = 1;
        o.side = (i % 2 == 0) ? OrderSide::BUY : OrderSide::SELL;
        o.type = OrderType::LIMIT;
        writer.writeOrder(o);
    }
    std::printf("Generated 50 orders\n");

    for (int i = 0; i < 30; i++) {
        Fill f;
        f.order_id = 1000 + i;
        f.fill_price = toDecimal(100.0 + (i % 20) * 0.5);
        f.fill_quantity = toDecimal(0.1);
        f.symbol_id = 1;
        writer.writeFill(f);
    }
    std::printf("Generated 30 fills\n");

    writer.flush();
    writer.stop();
    std::printf("Done. Logs in %s\n", cfg.log_dir.c_str());
    return 0;
}
