# xtrader-build

## Description
在 Seastar 仓库中编译和验证 xtrader 应用目标（`app_xtrader_cli`、`app_xtrader_demo`），并输出可执行的运行/排障步骤。

适用场景：
- 用户要求“编译 xtrader”或“确认 xtrader 是否编译成功”。
- 用户需要在 Ubuntu VM/远程主机做增量编译与运行验证。
- 用户需要快速定位 xtrader 目标构建失败的根因。

不适用场景：
- 纯 Seastar 全量编译（无 xtrader 目标诉求）。
- Windows 本机直接构建 Linux 目标（应转到 Ubuntu VM/远程 Linux）。

## Inputs To Confirm
执行前应确认：
1. 仓库路径（默认 `/home/vboxuser/workspace/seastar`）。
2. 构建模式（默认 `debug`）。
3. 是否禁用 DPDK（默认 `--disable-dpdk`）。
4. 是否有 SSH 别名（默认 `xtrader-vbox`）。

## Standard Workflow

### 1) 连通性与工具链检查
```bash
ssh xtrader-vbox "echo VM_OK; uname -a; whoami"
ssh xtrader-vbox "bash -lc 'python3 --version; cmake --version | head -n 1; ninja --version'"
```

### 2) 配置构建目录（首次或环境变化后）
```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local python3 configure.py --mode=debug --disable-dpdk'"
```

### 3) 编译 xtrader 目标
```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ninja -C build/debug app_xtrader_cli app_xtrader_demo -j2'"
```

### 4) 结果验证
```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ls -lh build/debug/apps/xtrader/xtrader_cli build/debug/apps/xtrader/xtrader_demo'"
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ./build/debug/apps/xtrader/xtrader_cli --help | head -n 20'"
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ./build/debug/apps/xtrader/xtrader_demo --help | head -n 20'"
```

## Success Criteria
- `build/debug/apps/xtrader/xtrader_cli` 存在且可执行。
- `build/debug/apps/xtrader/xtrader_demo` 存在且可执行。
- 至少一个目标能输出 `--help`，建议两个都验证。

## Troubleshooting

### A. `configure.py` 报 `FileNotFoundError`（找不到编译器）
原因：在 Windows 本机环境执行了 Linux 编译流程。
处理：切换到 Ubuntu VM 或远程 Linux 执行。

### B. PowerShell 下 SSH 命令被转义/管道干扰
现象：`|`、引号被本地 shell 提前解析。
处理：
- 使用简单命令拆分执行；
- 或使用绝对路径 `C:\Windows\System32\OpenSSH\ssh.exe`；
- 或改为在 VM 里直接登录后执行。

### C. Boost/GnuTLS 警告
- Boost 新版本依赖映射警告通常不阻断 xtrader 编译。
- GnuTLS 版本偏低在本流程中通常不影响 `xtrader_cli`/`xtrader_demo`。

## Output Format (Recommended)
向用户反馈时建议包含：
1. 是否发现 xtrader 代码入口（`apps/xtrader`、`src/xtrader`）。
2. 编译结果（成功/失败、失败阶段）。
3. 可执行文件路径。
4. 运行命令与最小验证输出。
5. 下一步建议（如全量编译、加入 gdb 验证）。

## Related Files
- `apps/xtrader/CMakeLists.txt`
- `src/xtrader/`
- `include/seastar/xtrader/`
- `SEASTAR_ENV_BUILD_DEBUG_GUIDE.md`
- `docs/xtrader_debug_guide.md`
