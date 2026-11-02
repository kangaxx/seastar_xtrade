# Seastar 云服务器K 低内存分步编译说明

适用场景：2G 内存、网络偶发抖动、希望按步骤定位卡点。

## 1. 上传脚本到云服务器K
在 Windows 本机执行：

```powershell
scp -i "C:\Users\lion_\.ssh\huadong.pem" -r "e:\kangaxx_github\seastar\ops\lowmem" root@47.100.218.196:/root/seastar/ops/
```

## 2. 登录服务器并赋权

```bash
ssh -i "C:\Users\lion_\.ssh\huadong.pem" root@47.100.218.196
cd /root/seastar/ops/lowmem
chmod +x *.sh
```

## 3. 按顺序执行（推荐）

```bash
./01_probe_env.sh
./02_install_base_deps.sh
./03_verify_boost_fmt.sh
./04_configure_debug.sh
JOBS=1 ./05_build_debug_lowmem.sh
./06_smoke_run_cli.sh
```

## 4. 每一步的“操作情况说明”在哪里看
- 每一步都会在终端输出 `[STEP]`、`[INFO]`、`[OK]`。
- 每一步会保存独立日志到：

```bash
/root/seastar/ops/lowmem/logs/
```

可用如下命令查看最近日志：

```bash
ls -lt /root/seastar/ops/lowmem/logs | head
```

## 5. 失败处理建议
- `01` 失败：先修复 SSH/系统基础连通。
- `02` 失败：通常是 apt 源或网络问题，重试该步。
- `03` 失败：说明 Boost/FMT 还未就绪，先补齐再继续。
- `04` 失败：根据 CMake 缺失项补依赖后重跑 `04`。
- `05` 失败：保持 `JOBS=1`，重复执行通常可增量推进。
- `06` 失败：说明运行期或 Redis 配置还有问题，先保留日志给我分析。
