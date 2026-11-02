# Seastar 环境配置、编译与调试指南

## 1. 适用范围

本文档用于当前已验证的 VirtualBox Ubuntu 调试环境，覆盖以下目标：

- 环境准备
- 依赖升级
- Debug 配置
- 最小目标编译与运行
- GDB 调试
- 常见问题排查

当前已验证路径：

- Seastar 仓库: /home/vboxuser/workspace/seastar
- SSH 别名: xtrader-vbox
- 端口转发: 127.0.0.1:2222

## 2. 基础连通性检查

在 Windows 终端执行：

```powershell
ssh xtrader-vbox "echo VM_OK; uname -a"
```

若失败，请先检查 VirtualBox 虚拟机是否启动、NAT 端口转发是否仍为 2222 -> 22。

## 3. 关键依赖版本（本机已验证）

由于 Ubuntu 22.04 系统包默认版本偏旧，需要系统级升级以下依赖：

- Boost: 1.83.x（/usr/local）
- fmt: 11.0.2（/usr/local）

版本检查命令：

```bash
grep -n "BOOST_LIB_VERSION" /usr/local/include/boost/version.hpp | head -n 2
grep -n "FMT_VERSION" /usr/local/include/fmt/core.h | head -n 2
```

## 4. 一次性升级命令

### 4.1 升级 Boost 1.83

```bash
cd /tmp
wget https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.bz2 -O boost_1_83_0.tar.bz2
rm -rf /tmp/boost_build_183
mkdir -p /tmp/boost_build_183
cd /tmp/boost_build_183
tar -xjf /tmp/boost_1_83_0.tar.bz2
cd boost_1_83_0
./bootstrap.sh --prefix=/usr/local --with-libraries=filesystem,program_options,thread,chrono,date_time,atomic,test
sudo ./b2 -j4 install
sudo ldconfig
```

### 4.2 升级 fmt 11.0.2

```bash
cd /tmp
wget https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.tar.gz -O 11.0.2.tar.gz
rm -rf /tmp/fmt-11.0.2
tar -xzf 11.0.2.tar.gz
cd fmt-11.0.2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DFMT_TEST=OFF
sudo cmake --build build -j4 --target install
sudo ldconfig
```

## 5. Debug 配置与编译

进入 Seastar 根目录：

```bash
cd /home/vboxuser/workspace/seastar
```

建议每次环境大变更后清理并重建 debug 目录：

```bash
rm -rf build/debug
BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local python3 configure.py --mode=debug --disable-dpdk
```

编译最小可执行目标（已验证）：

```bash
ninja -C build/debug apps/io_tester/ioinfo -j2
```

## 6. 运行验证

```bash
./build/debug/apps/io_tester/ioinfo --help
```

期望结果：输出 `App options` 等帮助信息，退出码为 0。

## 7. GDB 调试示例

```bash
gdb ./build/debug/apps/io_tester/ioinfo
```

GDB 内常用命令：

```gdb
set args --help
run
bt
quit
```

## 8. 常见问题

### 8.1 报 Boost 版本过低（1.74）

现象：配置阶段提示 `required is at least 1.79.0`。

处理：确认 `/usr/local/include/boost/version.hpp` 为 1.83，并在配置时显式带上：

```bash
BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local python3 configure.py --mode=debug --disable-dpdk
```

### 8.2 报 fmt::ostream_formatter 相关编译错误

现象：编译阶段出现 `expected class-name before '{' token`，位置在 `fmt::ostream_formatter`。

处理：升级 fmt 到 11.0.2，并重新 configure + build。

### 8.3 SSH 突然 Connection refused

处理顺序：

1. 确认虚拟机是否仍在运行。
2. 检查 VirtualBox 端口转发规则是否仍是 127.0.0.1:2222 -> 22。
3. 虚拟机内确认 SSH 服务状态：

```bash
sudo systemctl status ssh
```

## 9. 推荐日常流程

```bash
cd /home/vboxuser/workspace/seastar
BOOST_ROOT=/usr/local CMAKE_PREFIX_PATH=/usr/local python3 configure.py --mode=debug --disable-dpdk
ninja -C build/debug apps/io_tester/ioinfo -j2
./build/debug/apps/io_tester/ioinfo --help
```

如果仅代码变更、依赖未变更，可直接从 `ninja` 开始增量编译。