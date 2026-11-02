# Seastar xtrader 调试说明

本文档用于在 Ubuntu 虚拟机中对 Seastar 的 xtrader 目标进行编译、运行与调试。

## 1. 前提

- 虚拟机系统：Ubuntu 22.04+
- 仓库路径：`/home/vboxuser/workspace/seastar`
- 推荐通过 SSH 别名连接：`xtrader-vbox`

连通性检查：

```bash
ssh xtrader-vbox "echo VM_OK; uname -a; whoami"
```

## 2. 一次性环境准备

安装依赖（首次）：

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; sudo ./install-dependencies.sh'"
```

检查关键工具：

```bash
ssh xtrader-vbox "bash -lc 'python3 --version; cmake --version | head -n 1; ninja --version; gdb --version | head -n 1'"
```

## 3. 配置与编译

### 3.1 配置（首次或依赖变化后）

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local python3 configure.py --mode=debug --disable-dpdk'"
```

### 3.2 编译 xtrader

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ninja -C build/debug app_xtrader_cli app_xtrader_demo -j2'"
```

### 3.3 检查产物

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ls -lh build/debug/apps/xtrader/xtrader_cli build/debug/apps/xtrader/xtrader_demo'"
```

## 4. 运行方法

### 4.1 CLI 入口（推荐先测）

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ./build/debug/apps/xtrader/xtrader_cli --help | head -n 30'"
```

### 4.2 Demo 入口

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ./build/debug/apps/xtrader/xtrader_demo --help | head -n 30'"
```

## 5. GDB 调试

### 5.1 调试 xtrader_cli

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; gdb ./build/debug/apps/xtrader/xtrader_cli'"
```

GDB 常用命令：

```gdb
set args --help
run
bt
info threads
thread apply all bt
quit
```

### 5.2 调试 xtrader_demo

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; gdb ./build/debug/apps/xtrader/xtrader_demo'"
```

## 6. 常见问题与处理

### 6.1 报错：找不到编译器或 cmake/ninja

现象：`configure.py` 报 `FileNotFoundError`。

原因：命令在 Windows 本机执行，而非 Ubuntu VM。

处理：确保命令通过 SSH 在 VM 中执行。

### 6.2 报错：PowerShell 引号/管道被提前解析

现象：远程命令中的 `|`、引号行为异常。

处理建议：
- 尽量将长命令拆成多条短命令；
- 必要时用 `C:\Windows\System32\OpenSSH\ssh.exe` 调用 ssh；
- 或直接登录 VM 后本地执行。

### 6.3 仅一个目标编译成功

排查顺序：
1. 单独编译失败目标：
   ```bash
   ninja -C build/debug app_xtrader_demo -j2
   ```
2. 检查产物是否生成：
   ```bash
   ls -lh build/debug/apps/xtrader/
   ```
3. 必要时清理该目标相关对象后重编。

## 7. 推荐日常流程（最短）

```bash
ssh xtrader-vbox "bash -lc 'cd /home/vboxuser/workspace/seastar; ninja -C build/debug app_xtrader_cli app_xtrader_demo -j2; ./build/debug/apps/xtrader/xtrader_cli --help | head -n 20'"
```

如果依赖或配置发生变化，先补跑 `configure.py --mode=debug --disable-dpdk`。
