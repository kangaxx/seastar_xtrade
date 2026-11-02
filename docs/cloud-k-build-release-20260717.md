# 云服务器K /root/seastar 基础编译处理记录（2026-07-17）

## 1. 任务目标
- 在云服务器K（47.98.20.225, root）上完成 `/root/seastar` 的基础编译。
- 跑通 `build_release.sh`。
- 排查并修复环境缺失问题。

## 2. 连接方式
- 本地环境：Windows + PuTTY `plink.exe`
- 连接命令：

```powershell
"C:\Program Files\PuTTY\plink.exe" -i "E:\myssh\hangzhou.ppk" -batch root@47.98.20.225 "<remote-cmd>"
```

## 3. 初始验证
执行远端预检后确认：
- 系统：Ubuntu 24.04.3 LTS
- 项目目录：`/root/seastar` 存在
- Git 版本：`d4df4ade`

## 4. 首次失败与根因
执行：

```bash
cd /root/seastar
bash ./build_release.sh
```

报错：

```text
CMake Error: CMake was unable to find a build program corresponding to "Ninja".
CMAKE_MAKE_PROGRAM is not set.
```

根因：远端缺少 `ninja`。

## 5. 修复动作
### 5.1 安装 Ninja
执行：

```bash
apt-get update
apt-get install -y ninja-build
```

### 5.2 二次执行脚本
再次执行：

```bash
cd /root/seastar
JOBS=1 bash ./build_release.sh
```

随后出现新缺失依赖：

```text
Could NOT find c-ares (missing: c-ares_LIBRARY c-ares_INCLUDE_DIR)
```

### 5.3 按项目官方脚本补齐依赖
执行：

```bash
cd /root/seastar
bash ./install-dependencies.sh
```

该步骤成功安装 `libc-ares-dev` 等构建依赖集合。

### 5.4 三次执行脚本
再次执行：

```bash
cd /root/seastar
JOBS=1 bash ./build_release.sh
```

日志显示：
- CMake 配置成功（`Build files have been written to: /root/seastar/build/release`）
- `ninja -C build/release -j1 app_xtrader_cli` 已启动并进入编译阶段（输出到 `[27/106]`）

## 6. 当前阻塞状态
在编译进行期间，后续新建 SSH 会话出现握手阶段卡住：
- `plink -v` 日志停在：
  - `Connected to 47.98.20.225 ...`
  - 未收到后续 SSH banner/认证输出
- `ssh -o BatchMode=yes` 出现 `banner exchange timeout`
- `Test-NetConnection 47.98.20.225 -Port 22` 显示 TCP 22 可达

结论：
- 已完成环境缺失修复（Ninja + 依赖集）。
- `build_release.sh` 已成功进入正式编译流程。
- 当前阻塞在服务器 SSH 服务会话层（新会话握手不稳定），导致无法在本次会话内读取最终编译收尾状态。

## 7. 建议的收尾动作（在服务器控制台执行）
如果你有云主机控制台（VNC/网页终端）权限，可直接执行：

```bash
cd /root/seastar
JOBS=1 bash ./build_release.sh
ls -lh /root/seastar/build/release/apps/xtrader/xtrader_cli
ls -lh /root/seastar/build/release/apps/xtrader/run_xtrader_cli.sh
```

若仍有 SSH 会话卡顿，先检查：

```bash
systemctl status ssh --no-pager
journalctl -u ssh --since "30 min ago" --no-pager | tail -n 200
ss -tnlp | grep :22
```

必要时重启 ssh 服务：

```bash
systemctl restart ssh
```

## 8. 已修复问题清单
- [x] 缺少 `ninja`（已安装 `ninja-build`）
- [x] 缺少 `c-ares` 及相关构建依赖（已通过 `install-dependencies.sh` 补齐）
- [x] `build_release.sh` 已可启动并进入 Ninja 编译阶段
- [ ] 最终二进制产物存在性校验（受 SSH 会话层阻塞影响，待服务器侧确认）

## 9. 备注
- `build_release.sh` 默认 `JOBS=1` 对低内存机器更稳妥，保留此配置可降低 OOM 风险。
- 当前记录为现场处理日志，后续可补一条“最终产物校验结果”附录。