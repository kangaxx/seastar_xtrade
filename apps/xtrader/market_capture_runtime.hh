// Shared process plumbing for the interactive and daemon collector entries.
// Source: user daemonization request, 2026-11-25.
#pragma once

#include <filesystem>

namespace seastar::xtrader::cli {

bool stop_requested() noexcept;

// SIGINT/SIGTERM/SIGHUP request a graceful stop; repeating a stop signal forces
// exit. SIGPIPE is ignored; PDEATHSIG guards against orphaned collectors.
void install_signals();

// Owner-only state directory (CTP flow files, collector lock, daemon status).
std::filesystem::path state_directory();

} // namespace seastar::xtrader::cli
