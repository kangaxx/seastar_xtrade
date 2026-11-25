// Non-interactive market capture entry: --daemon-tick skips every menu and runs
// until a stop signal or a fatal feed condition. systemd packaging lives in
// deploy/ctp-md/. Source: user daemonization request, 2026-11-25.
#pragma once

namespace seastar::xtrader::cli {

// Exit codes: 0 clean stop, 1 usage, 2 runtime failure, 3 stale feed watchdog,
// 10 pid lock already held (systemd RestartPreventExitStatus=10).
int run_market_capture_daemon(int argc, char** argv);

} // namespace seastar::xtrader::cli
