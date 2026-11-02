# Seastar VirtualBox Ubuntu 远程调试检查清单

## 1. 目标

本清单用于在 Windows 11 + VirtualBox 7.2.6 + Ubuntu 环境下，建立一套可被 VS Code Remote-SSH 直接使用的本地 Linux 调试链路。

目标包括：

- 在 Ubuntu 虚拟机内完成 Seastar 的配置、编译与增量构建
- 在同一虚拟机中运行单测或指定测试
- 使用 gdb 与 VS Code Remote-SSH 调试 Seastar 示例、应用或测试目标

## 2. 结论先行

可以。

只要满足以下前提，Seastar 可以和 X-Trader 共用同一台 Ubuntu 虚拟机作为调试环境：

1. Windows 主机能够通过 SSH 连到虚拟机中的 Ubuntu。
2. VS Code 能通过 Remote-SSH 打开虚拟机中的 seastar 仓库目录。
3. Ubuntu 内已安装编译 Seastar 所需依赖、Python3、CMake、ninja、gdb。
4. 仓库已完成子模块初始化，或按需使用 configure.py / cooking.sh 准备依赖。

## 3. 虚拟机基础建议

推荐配置：

- VirtualBox 版本: 7.2.6
- Ubuntu 版本: 22.04 LTS 或 24.04 LTS
- CPU: 至少 4 vCPU
- 内存: 至少 8 GB
- 磁盘: 至少 100 GB

Seastar 编译和部分测试对 CPU、内存更敏感。如果你还要在同一台虚拟机里同时保留 X-Trader 的构建环境，建议把磁盘和内存预留得更宽松。

## 4. 网络与 SSH 方案

### 4.1 推荐优先级

推荐优先使用以下两种方式之一：

1. 桥接网卡: 虚拟机直接拿到局域网 IP，Windows 可直接 ssh 到该 IP。
2. NAT + 端口转发: 将主机端口转发到虚拟机的 22 端口。

如果你只是本机开发调试，NAT + 端口转发通常已经够用。

### 4.2 NAT + 端口转发示例

可在 VirtualBox 中为该虚拟机配置一条端口转发规则：

- 主机 IP: 127.0.0.1
- 主机端口: 2222
- 客体 IP: 留空或填虚拟机 IP
- 客体端口: 22

这样在 Windows 上可以通过以下命令连接：

```powershell
ssh -p 2222 ubuntu@127.0.0.1
```

### 4.3 Ubuntu 内安装 SSH 服务

在虚拟机内执行：

```bash
sudo apt update
sudo apt install -y openssh-server
sudo systemctl enable ssh
sudo systemctl start ssh
sudo systemctl status ssh
```

## 5. Windows 侧 SSH 配置建议

建议在 Windows 的 SSH config 中加入一个别名，例如：

```sshconfig
Host dev-vbox
    HostName 127.0.0.1
    Port 2222
    User ubuntu
```

之后可直接使用：

```powershell
ssh dev-vbox
```

这个别名既可以给 X-Trader 用，也可以给 seastar 用，区别只在 VS Code Remote-SSH 连接后打开的远程目录不同。

## 6. 仓库放置方式

优先把 seastar 仓库放在 Ubuntu 虚拟机自己的 Linux 文件系统中，例如：

```bash
~/workspace/seastar
```

不建议长期把代码直接放在 VirtualBox 共享目录里做高频编译，原因包括：

- 文件 I/O 容易变慢
- ninja 增量构建体验可能变差
- 调试器和文件监听的边缘问题更多

## 7. Ubuntu 依赖安装

在虚拟机内执行：

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  ninja-build \
  cmake \
  gdb \
  git \
  pkg-config \
  python3 \
  python3-pip \
  python3-venv \
  python3-dev \
  libboost-dev \
  libhwloc-dev \
  libnuma-dev \
  libaio-dev \
  liburing-dev \
  libcryptopp-dev \
  libc-ares-dev \
  ragel \
  openssl \
  libssl-dev \
  xsltproc \
  libxml2-utils \
  openssh-server
```

然后确认关键命令可用：

```bash
gcc --version
g++ --version
cmake --version
ninja --version
gdb --version
python3 --version
```

如果依赖缺失，可优先参考 README.md 和 HACKING.md 中的说明，并按需运行：

```bash
sudo ./install-dependencies.sh
git submodule update --init --recursive
```

## 8. 配置与编译检查

### 8.1 检查构建目录

优先检查是否已有构建目录：

```bash
ls build
```

如果已有目标模式目录，例如 build/dev、build/debug、build/release，优先复用现有目录。

### 8.2 配置命令

若尚未配置，可在仓库根目录执行：

```bash
./configure.py --mode=dev
```

如果要做 gdb 调试，建议补一个 debug 构建：

```bash
./configure.py --mode=debug
```

### 8.3 编译命令

在仓库根目录执行：

```bash
ninja -C build/dev apps/io_tester/io_tester
```

或根据目标改成：

```bash
ninja -C build/debug <target>
```

如果只想先验证构建链路，可执行：

```bash
ninja -C build/dev -t targets all
```

## 9. 测试检查

推荐优先使用 test.py 运行单测，而不是一次跑全量测试：

```bash
./test.py --mode dev --name circular_buffer
```

也可以直接使用 ctest：

```bash
ctest --test-dir build/dev -R circular_buffer --output-on-failure
```

如果只想列出当前可用测试：

```bash
ctest --test-dir build/dev -N
```

## 10. gdb 调试检查

对单个二进制或测试目标进行调试时，进入仓库根目录后执行：

```bash
gdb ./build/debug/tests/unit/circular_buffer_test
```

或：

```bash
gdb ./build/dev/apps/io_tester/io_tester
```

常用命令：

```gdb
run
bt
thread apply all bt
```

如果要分析崩溃，先执行：

```bash
ulimit -c unlimited
```

## 11. VS Code 远程调试检查

确认以下条件满足：

1. Windows 可以通过 ssh dev-vbox 或等价命令连入虚拟机。
2. VS Code 已安装 Remote Development 扩展。
3. VS Code 已通过 Remote-SSH 打开虚拟机中的 seastar 仓库。
4. 远程环境已安装 C/C++ 扩展与 Python 扩展。
5. 可以在 VS Code 终端中成功执行 ./configure.py --mode=dev。
6. 可以在 VS Code 终端中成功执行 ninja -C build/dev <target>。
7. 可以在 VS Code 终端中成功执行 ./test.py --mode dev --name <pattern>。

说明：

- Seastar 的构建目录必须使用 ninja -C build/<mode> 调用，不要在仓库根目录直接执行 ninja。
- 如果你与 X-Trader 共用同一台虚拟机，建议为每个项目分别放在 ~/workspace 下独立目录，避免 build 产物和依赖脚本混用。

## 12. 建议的通过标准

如果以下 6 项全部通过，可以认为 VirtualBox Ubuntu 中的 Seastar 调试环境已经可用：

1. SSH 可以稳定连入虚拟机。
2. VS Code Remote-SSH 可以打开 seastar 仓库。
3. ./configure.py --mode=dev 成功。
4. ninja -C build/dev <target> 成功。
5. ./test.py --mode dev --name <pattern> 成功执行一个相关测试。
6. gdb 可以正常拉起一个可执行目标或测试目标。

## 13. 与 X-Trader 共用虚拟机的建议

如果后续 X-Trader 和 seastar 都在同一台虚拟机里调试，建议按以下方式组织：

1. 统一 SSH 入口和 VS Code Remote-SSH 连接别名。
2. 每个项目各自独立仓库目录与 build 目录。
3. 把通用系统依赖一次装全，项目特定依赖按仓库 README/HACKING 增补。
4. 调试时优先在虚拟机本地 Linux 文件系统中构建，避免共享目录拖慢迭代速度。