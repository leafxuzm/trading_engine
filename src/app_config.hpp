#pragma once

#include <chronos/core/config.hpp>
#include <chronos/core/types.hpp>

#include <gflags/gflags.h>
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

// ── CLI flags (gflags) ────────────────────────────────────────────────

DEFINE_string(replay, "", "Replay historical ticks from binary log directory");
DEFINE_string(date, "", "Date filter YYYYMMDD (file replay mode)");
DEFINE_bool(live, false, "Live market data mode (testnet)");
DEFINE_bool(simulate, false, "Force simulated fills even with API keys");
DEFINE_string(exchange, "binance", "Exchange for live mode (binance, okx)");
DEFINE_string(config, "", "YAML config file path");

// ── AppConfig ─────────────────────────────────────────────────────────

struct AppConfig {
    std::string config_path;
    std::string log_dir;       // input: log directory for file replay
    std::string date;          // date filter YYYYMMDD
    bool live_mode = false;
    bool force_simulate = false;

    // Strategy
    struct StrategyCfg {
        std::string type;
        double grid_low = 95.0, grid_high = 105.0;
        int grid_levels = 5;
        double quantity = 0.1;
        uint32_t symbol_id = 1;
    };
    std::vector<StrategyCfg> strategies;

    // Exchange (for live mode)
    std::string exchange_name = "binance";
    std::string exchange_url;
    std::string rest_url;
    std::string user_stream_url;
    std::string exchange_api_key;
    std::string exchange_api_secret;
    std::string proxy_host;
    int proxy_port = 0;
    std::vector<std::string> exchange_symbols;

    chronos::RiskParameters risk_params;
    double initial_capital = 100000.0;

    struct {
        std::string output_dir = "./logs";
    } log_cfg;

    struct {
        std::string endpoint = "tcp://*:5555";
        int hwm = 10000;
        int linger = 1000;
    } zmq_cfg;
};

// ── CLI parsing (wraps gflags) ────────────────────────────────────────

inline bool parseArgs(int argc, char* argv[], AppConfig& cfg) {
    gflags::SetUsageMessage("Chronos Trading Engine — HFT pipeline demo");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    cfg.log_dir  = FLAGS_replay;
    cfg.date     = FLAGS_date;
    cfg.live_mode = FLAGS_live;
    cfg.force_simulate = FLAGS_simulate;
    cfg.exchange_name  = FLAGS_exchange;

    // Config file: --config flag or first positional arg
    if (!FLAGS_config.empty()) {
        cfg.config_path = FLAGS_config;
    } else if (argc > 1) {
        cfg.config_path = argv[1];
    }

    if (cfg.config_path.empty()) {
        std::fprintf(stderr, "Error: config file required (--config <path>)\n");
        return false;
    }
    return true;
}

// ── YAML config loading ───────────────────────────────────────────────

inline bool loadConfig(const std::string& path, AppConfig& cfg) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        std::fprintf(stderr, "Error: cannot parse '%s': %s\n", path.c_str(), e.what());
        return false;
    }

    // Engine
    if (root["engine"]) {
        auto eng = root["engine"];
        cfg.initial_capital = eng["initial_capital"].as<double>(100000.0);
    }

    // Strategies
    if (root["strategies"] && root["strategies"].IsSequence()) {
        for (auto s : root["strategies"]) {
            if (!s["enabled"].as<bool>(true)) continue;
            AppConfig::StrategyCfg sc;
            sc.type = s["name"].as<std::string>("GridStrategy");
            if (s["parameters"]) {
                auto p = s["parameters"];
                sc.grid_low    = p["grid_low"].as<double>(95.0);
                sc.grid_high   = p["grid_high"].as<double>(105.0);
                sc.grid_levels = p["grid_levels"].as<int>(5);
                sc.quantity    = p["quantity"].as<double>(0.1);
                sc.symbol_id   = p["symbol_ids"][0].as<uint32_t>(1);
            }
            cfg.strategies.push_back(sc);
        }
    }

    // Exchanges
    if (root["exchanges"] && root["exchanges"].IsSequence()
        && root["exchanges"].size() > 0) {
        auto ex = root["exchanges"][0];
        cfg.exchange_name = ex["name"].as<std::string>("binance");
        cfg.exchange_url  = ex["websocket_url"].as<std::string>("");
        cfg.rest_url      = ex["rest_url"].as<std::string>("");
        cfg.user_stream_url = ex["user_stream_url"].as<std::string>("");
        cfg.proxy_host    = ex["proxy_host"].as<std::string>("");
        cfg.proxy_port    = ex["proxy_port"].as<int>(0);

        // API key: config first, then env var fallback
        cfg.exchange_api_key = ex["api_key"].as<std::string>("");
        cfg.exchange_api_secret = ex["api_secret"].as<std::string>("");

        auto resolveEnv = [](const std::string& s) -> std::string {
            if (s.size() > 3 && s[0] == '$' && s[1] == '{' && s.back() == '}') {
                std::string var = s.substr(2, s.size() - 3);
                const char* val = std::getenv(var.c_str());
                return val ? std::string(val) : "";
            }
            return s;
        };
        cfg.exchange_api_key = resolveEnv(cfg.exchange_api_key);
        cfg.exchange_api_secret = resolveEnv(cfg.exchange_api_secret);

        if (cfg.exchange_api_key.empty()) {
            const char* k = std::getenv("BINANCE_API_KEY");
            if (k) cfg.exchange_api_key = k;
        }
        if (cfg.exchange_api_secret.empty()) {
            const char* s = std::getenv("BINANCE_API_SECRET");
            if (s) cfg.exchange_api_secret = s;
        }

        if (ex["symbols"] && ex["symbols"].IsSequence()) {
            for (auto s : ex["symbols"])
                cfg.exchange_symbols.push_back(s.as<std::string>());
        }
    }

    // Risk
    if (root["risk"]) {
        auto r = root["risk"];
        chronos::RiskParameters rp;
        rp.max_order_value       = r["max_order_value"].as<double>(100000.0);
        rp.max_position_value    = r["max_position_value"].as<double>(500000.0);
        rp.max_orders_per_second = r["max_orders_per_second"].as<int>(100);
        rp.max_total_position_value = r["max_total_position_value"].as<double>(1000000.0);
        rp.min_available_capital = r["min_available_capital"].as<double>(10000.0);
        rp.max_drawdown_percent  = r["max_drawdown_percent"].as<double>(20.0);
        cfg.risk_params = rp;
    }

    // Logging
    if (root["log"]) {
        auto l = root["log"];
        cfg.log_cfg.output_dir = l["output_dir"].as<std::string>("./logs");
    }

    // ZMQ
    if (root["zmq"]) {
        auto z = root["zmq"];
        cfg.zmq_cfg.endpoint = z["publish_endpoint"].as<std::string>("tcp://*:5555");
        cfg.zmq_cfg.hwm      = z["high_water_mark"].as<int>(10000);
        cfg.zmq_cfg.linger   = z["linger_ms"].as<int>(1000);
    }

    return true;
}
