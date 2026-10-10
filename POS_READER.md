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

## 采样器怎么工作

1. `il2cpp_domain_get()` → 遍历所有 assembly/image，用
   `il2cpp_class_from_name(image, 命名空间, 类名)` 解析目标类（结果缓存）。
2. 每 `POS_INTERVAL_MS`（默认 1000ms）用 **`il2cpp_gc_foreach_heap()`** 扫一遍托管堆，
   按"对象头 klass 指针"挑出目标类的实例（回调里不做任何分配/IO，只记录指针）。
3. 回调结束后再打印字段：
   - **紧凑行**（所有实例）：`runtimeId_` / `configId_` / `camp_` / `hpRatio_`，
     以及坐标：优先 `optLocation_`（protobuf oneof），退回 `location_`；
     若指向的是 `VInt3` 实例，则按名字读 `x_`(0x10) / `y_`(0x14) / `z_`(0x18)。
     还会尝试 `optLastVisibleLocation_` / `lastVisibleFrameNo_` / `frameNo_`。
   - **完整行**（每类前 `POS_FULL_OBJ`=2 个实例）：把所有标量字段按 `名字=值` 打印，
     引用字段打指针；指向 `VInt3` 的字段会额外解码成 `(x,y,z)`。
4. 输出追加到 `<game_data_dir>/files/pos.log`，超过 64MB 自动重开。

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
| `POS_INTERVAL_MS` | 1000 | 采样间隔 |
| `POS_MAX_OBJ` | 128 | 每类每轮最多记录实例数 |
| `POS_FULL_OBJ` | 2 | 每类每轮打印几个"全字段"实例 |
| `POS_LOG_MAX` | 64MB | 日志上限，超过就重开 |
| `ENABLE_DUMP`（hack.cpp） | 1 | 0 = 跳过 76MB 的 dump.cs，只采样 |
