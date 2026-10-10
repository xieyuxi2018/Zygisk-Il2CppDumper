# pos_reader —— 每秒采样战场坐标对象（本仓库新增）

> 目标：不 hook、不改游戏，只调用 il2cpp 官方 API，把**王者荣耀**
> （`com.tencent.tmgp.sgame`）里 `AivoiceV2` / `Aivoice` 命名空间的
> "按帧下发的战场坐标快照"对象的字段，每秒写一次到
> `<app_data_dir>/files/pos.log`，用于离线分析实时坐标。

## 改了什么

| 文件 | 变化 |
|---|---|
| `module/src/main/cpp/pos_reader.cpp` / `.h` | **新增**：采样器本体 |
| `module/src/main/cpp/hack.cpp` | il2cpp 初始化后先起采样线程；新增 `ENABLE_DUMP` 开关（0 = 不再生成 76MB 的 dump.cs） |
| `module/src/main/cpp/CMakeLists.txt` | 把 `pos_reader.cpp` 加进构建 |
| `module/src/main/cpp/game.h` | `GamePackageName` 设为 `com.tencent.tmgp.sgame` |

## 采样器怎么工作（v2：只读内存扫描）

> ⚠️ v1 用 `il2cpp_gc_foreach_heap()` 扫堆，实测会让游戏进程 **SIGSEGV**
> （crash 回溯：`#00 libil2cpp.so 0x3b7e7c8` ← `#01 il2cpp_gc_foreach_heap+24`
> ← `#02/#03 /memfd:jit-cache`（本模块线程））。所以 v2 不再碰 GC：

1. `il2cpp_domain_get()` → 遍历 assembly/image，用
   `il2cpp_class_from_name(image, 命名空间, 类名)` 解析目标类，拿到 `Il2CppClass*`（纯查询）。
2. 每 `POS_INTERVAL_MS`（默认 2000ms）：
   - 读 `/proc/self/maps`，挑出**匿名、可读写、1MB~420MB、非 dalvik/非文件映射**的区域；
   - 用 `pread(/proc/self/mem)` 按 8 字节对齐扫**指向该 klass 的指针**
     —— 托管对象头 8 字节就是 klass 指针，命中处即对象地址；
   - 再用 `il2cpp_field_get_value()` **按字段名**读值（不写死偏移）。
3. 打印格式与 v1 相同（见下），日志仍在 `<app_data_dir>/files/pos.log`。

这条路径只读内存、不调用 GC 内部函数，不会再打崩游戏。

### 目标类（`TARGETS[]`，可自行增删）

```
AivoiceV2: GameInfoDynamic / HeroUpdate / Hero / MonsterUpdate / Monster /
           OrganUpdate / SoldierUpdate / CakeUpdate / TargetInfo / VInt3
Aivoice  : VInt3
```

## 输出格式

```
S t=123456 cls=AivoiceV2.HeroUpdate total=12 shown=12
OBJ HeroUpdate ptr=0x7b1a28d680
  runtimeId_=10086
  ...
  optLocation_=0x7b2c40a100 -> optLocation_=(112345,900,183420)
C HeroUpdate ptr=0x7b2c40a100 rid=10086 cfg=172 camp=1 hp=100 -> loc=(112345,900,183420)
C HeroUpdate ptr=0x... rid=10087 cfg=... -> loc=(...)
```

- `S` = 一次采样/一个类的汇总；`OBJ` = 完整字段；`C` = 紧凑坐标行。
- 拿坐标只需要 grep `^C `，`loc=(x,y,z)` 就是 **VInt3 世界坐标（整数）**。

## 编译与使用

```bash
# 1) 编译（和原来一样；需要 NDK）
./gradlew :module:assembleRelease

# 2) 安装模块（Magisk 管理器刷入 zip），然后**重启**
# 3) 让 Zygisk 能注入：把游戏从 denylist 移除（否则模块不会被注入！）
adb shell su -c 'magisk --denylist rm com.tencent.tmgp.sgame'
adb reboot

# 4) 启动游戏，进对局；采样每秒一次，自动写日志
adb shell monkey -p com.tencent.tmgp.sgame -c android.intent.category.LAUNCHER 1
adb shell "logcat -s Perfare"      # 会看到 pos_reader: t=... samples=...

# 5) 拉日志
adb shell su -c 'cp /data/data/com.tencent.tmgp.sgame/files/pos.log /data/local/tmp/pos.log; chmod 644 /data/local/tmp/pos.log'
adb pull /data/local/tmp/pos.log .
```

> 用完后建议把 denylist 加回去（隐藏 root，降低 ACE 检测风险）：
> `adb shell su -c 'magisk --denylist add com.tencent.tmgp.sgame com.tencent.tmgp.sgame'`

## 可调参数（`pos_reader.cpp` 顶部）

| 宏 | 默认 | 说明 |
|---|---|---|
| `POS_INTERVAL_MS` | 2000 | 采样间隔 |
| `POS_MAX_OBJ` | 128 | 每类每轮最多记录实例数 |
| `POS_FULL_OBJ` | 2 | 每类每轮打印几个"全字段"实例 |
| `POS_LOG_MAX` | 64MB | 日志上限，超过就重开 |
| `POS_REGION_MIN/MAX` | 1MB / 420MB | 参与扫描的匿名 rw 区域大小范围 |
| `ENABLE_DUMP`（hack.cpp） | 0 | 0 = 跳过 76MB 的 dump.cs，只采样（更快、更少干扰） |

## 排错记录

采样器现在会把**进度写进 `<app_data_dir>/files/pos_debug.log`**（不依赖 logcat，
用 `adb shell su -c 'cat /data/data/com.tencent.tmgp.sgame/files/pos_debug.log'` 看）：

```
hack_start: enter tid=9166 dir=/data/data/...
libil2cpp.so found after 2s handle=0x...
il2cpp_api_init returned
pos_reader_start called
reader_main: enter ...
class OK: AivoiceV2.HeroUpdate -> 0x...
resolved=11/11 regions=42
idle pass=11 t=... (no objects found)
t=... samples=5 pass=23          <- 抓到对象了
```

| 现象 | 原因 | 处理 |
|---|---|---|
| 模块装了但完全没有日志 | 游戏在 Magisk **denylist** 里 → Zygisk 不注入 | `magisk --denylist rm com.tencent.tmgp.sgame` 后重启 |
| 只有 `detect game:`、之后什么都没有 | 旧代码只等 10s libil2cpp，王者冷启动/进对局会重启进程，等不到 | 已改成等 **180s**，并把每步写进 `pos_debug.log` |
| **启动游戏立刻闪退**，crash 回溯里有 `il2cpp_domain_get+4` + `/memfd:jit-cache` | 在 VM 就绪**之前**调用 `il2cpp_domain_get()` 去"试探" → SIGSEGV | 已**删掉**这个探测；只保留上游的 `il2cpp_is_vm_thread(NULL)` 判据，并加 240s 上限（超时则放弃采样，不冒险调用） |
| 卡在 `Waiting for il2cpp_init...` 永不结束 | 上游原写法**无超时**；某些进程（预热的空壳进程、没跑 Unity）永远不就绪 | 现在最多等 240s，超时就放弃并写进 `pos_debug.log` |
| 进对局时游戏闪退，crash 里有 `il2cpp_gc_foreach_heap+24` | v1 的 GC 扫堆把游戏打崩 | 已改成 v2 只读内存扫描 |
| `pos.log` 只有 `# pos_reader start ...` 没有 `C` 行 | 采样器活着，但当前场景没有目标对象（比如还在大厅） | 进对局后自然会有；`resolved=0/11` 才是类名解析失败 |
