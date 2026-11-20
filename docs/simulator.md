# 门锁模拟演示平台

在一台电脑上跑起整套系统：**数百把虚拟门锁 + 模拟 433MHz 空口 + 楼宇网关 + 管理平台 + Web 控制台**，并且每个功能都有一个可以一键运行、自动检查结果的演示。

## 先回答：有没有在线画主板、还能模拟测试的工具？

结论：**没有一个在线工具能同时“画 EFR32 主板”又“仿真 EFR32 + 433MHz 射频”**。实际做法是分三层，各用最合适的工具：

| 要做的事 | 推荐工具 | 能做什么 | 做不了什么 |
| --- | --- | --- | --- |
| 画原理图和 PCB | **嘉立创 EDA**（网页版，免费）；或 KiCad（离线，开源） | 原理图、PCB、元件库、直接下单打样/贴片 | 不能跑 EFR32 固件 |
| 仿真模拟电路（电池分压、电机 H 桥、LED/蜂鸣器驱动） | 嘉立创 EDA 标准版的仿真模式、KiCad 内置 ngspice、Falstad CircuitJS（网页） | 看电压、电流、低电阈值、电机堵转电流 | 不能跑 MCU 程序 |
| 仿真单片机外围逻辑 | Wokwi（网页） | ESP32 / STM32 / RP2040 / Arduino 外设仿真 | **不支持 EFR32，也没有 433MHz 射频** |
| 跑真实 EFR32 固件 | Renode（开源，离线） | 在 PC 上运行编译好的固件镜像 | 自带的是 EFR32 Series 1 平台（如 EFR32MG12）；FG28 属于 Series 2，外设模型要自己补；不模拟真实射频物理特性（以 Renode 官方支持列表为准） |
| **测协议、门锁逻辑、网关调度、平台、批量下发** | **本仓库的模拟平台** | 每把虚拟锁运行的就是门锁固件的真实 C 代码，空口按时隙模拟，网关和平台是真实程序 | 不验证射频距离、穿墙、功耗——这些需要开发板实测 |

建议路线：**嘉立创 EDA 画板 → 本模拟平台验证协议和业务 → 买 2~3 块 EFR32FG28 开发板做射频和功耗实测 → 打样主板**。

## 模拟平台里有什么

```
 浏览器 http://localhost:8080
        │  (Web 控制台 / HTTP API / 演示脚本)
 ┌──────┴───────────────────────────┐
 │ 平台 platform（Go）              │  指令下发、三段确认、离线补发、上行解密、
 │  + 内置 MQTT broker              │  门锁状态/告警/开门记录
 └──────┬───────────────────────────┘
        │ MQTT  sl/v1/{学校}/{网关}/down/cmd | up/ack | up/result | up/event | up/status
 ┌──────┴──────┐  ┌─────────────┐
 │ 网关 gw-1-1 │  │ 网关 gw-1-2 │   每 1 秒一个侦听周期：信标 + 按 (信道, 时隙) 调度、
 │ (1~6 层)    │  │ (7~12 层)   │   P0~P3 优先级、重发、链路层加密与防重放
 └──────┬──────┘  └──────┬──────┘
        │ UDP（代替网关主机与射频协处理器之间的串口）
 ┌──────┴─────────────────┴─────────────────────────────┐
 │ locksim（C）：射频协处理器 + 433MHz 空口 + 360 把虚拟锁 │
 │  每把锁 = 门锁固件真实代码（lock_app / lock_core / 凭证库 / │
 │  lock_mac / SLP 帧 / AES-CCM 两层加密 / 防重放）+ 模拟硬件  │
 └──────────────────────────────────────────────────────┘
```

默认一栋楼 12 层 × 30 间 = 360 把锁，每 6 层一台网关（每台 180 把锁，3 个信道 × 50 个时隙）。

**哪些是真代码，哪些是模拟的：**

- 真代码：门锁固件逻辑（和将来烧进 EFR32 的是同一份 C 代码，只换了硬件抽象层）、空口帧格式、AES-128-CCM* 加解密（RFC 3610 测试向量校验过，C 和 Go 两边互通）、防重放、指令去重、网关调度器、平台下发服务、离线密码和蓝牙令牌算法、上行 ACK 重发、记录补传、信标校时。
- 门口的输入（刷卡、键盘、门磁、反锁旋钮、防撬开关、呼叫键、机械钥匙、手机蓝牙）用控制命令模拟，进入固件的是同一个函数（`lock_app_access` / `lock_app_keypad` / `lock_app_door` …）。蓝牙只模拟“出示令牌”，不模拟 BLE 连接和挑战应答。
- 空口丢包率同时作用于下行、上行、信标和 ACK。
- 模拟的：电机、读卡器、电池（`firmware/lock/port/sim/hal_sim.c`）；射频（帧在锁的时隙被“听到”，可设丢包率，不模拟碰撞和距离）；密钥（用固定的模拟主密钥派生，**仅限模拟**，真实设备由出厂 K_dev + 平台 KMS 派生）；入网（没做 K_dev 签名校验）；平台数据库（内存）。

## 快速开始

### 方式一：Docker（Windows / macOS / Linux 都可以，推荐）

需要 Docker Desktop。在仓库根目录：

```sh
docker compose -f deploy/sim/docker-compose.yml up --build
```

看到 `模拟平台已启动` 后，浏览器打开 <http://localhost:8080>。

另开一个终端跑功能演示：

```sh
docker compose -f deploy/sim/docker-compose.yml exec sim demo all
```

### 方式二：本机直接运行（Linux / macOS / Windows 的 WSL2）

需要 Go 1.24+、CMake 3.16+、C 编译器（gcc 或 clang）。

```sh
scripts/sim.sh                    # 编译并启动全部进程，Ctrl+C 停止
# 另开一个终端
scripts/demo.sh                   # 列出所有演示
scripts/demo.sh all               # 依次运行全部演示
scripts/demo.sh bulk              # 只跑某一个
```

楼的规模可以改：

```sh
FLOORS=20 ROOMS=40 FLOORS_PER_GW=7 scripts/sim.sh    # 800 把锁、3 台网关
```

Docker 方式改 `deploy/sim/docker-compose.yml` 里的 `FLOORS` / `ROOMS` / `FLOORS_PER_GW`。

> 演示脚本里的房间号（101、203、1130 ……）按默认 12 层 × 30 间写的，改了规模后请保证这些房间存在。

## 功能演示清单

每个演示都会打印每一步在做什么，并同时检查两边：平台记录到的结果、虚拟门锁（固件）真实的状态。

| 演示名 | 功能 | 演示内容与预期结果 | 在网页上手动操作 |
| --- | --- | --- | --- |
| `card-unlock` | 校园卡开门 | 平台给 101 下发校园卡（只认 UID）→ 刷卡开门 → 5 秒后自动落锁 → 陌生卡被拒绝，开门记录上报平台 | 选中房间 → “添加此卡” → “刷卡”；“刷一张陌生卡” |
| `remote-unlock` | 远程开门 | 三个房间各开一次，打印 平台→网关、空口 ACK、门锁执行结果 各段耗时（通常 0.1~1 秒，取决于等到该锁时隙的时间） | “远程开门” |
| `card-manage` | 加卡 / 删卡 / 有效期 | 加一张 7 天有效期的卡能开门；删除后同一张卡立即被拒绝 | “添加此卡” / “删除此卡” |
| `bulk` | 整栋楼批量秒级下发 | 360 把锁每间发一张卡，约 2~3 秒全部执行完，打印进度和单锁耗时；中途插入的远程开门几百毫秒完成（P1 优先于 P2）；再批量删卡 | “批量发卡（每间一张）”，看楼栋图逐层亮起白点，下方进度条显示总耗时 |
| `broadcast` | 全楼广播 | 消防应急常开：每台网关只发一帧，约 1~2 秒全楼变橙色；恢复；全楼封锁时合法卡也打不开、宿管远程开门仍有效 | “应急常开（广播）” / “全楼封锁” / “恢复正常” |
| `anti-replay` | 防重放 / 防篡改 / 去重 | 攻击者原样重放截获的开门帧 → 门锁按帧计数器拒绝；篡改后重发 → MIC 校验失败；ACK 丢失导致网关重发 → 门锁按指令号去重，回复 DUPLICATE，只开一次门 | “原样重放” / “篡改后重发” / “丢掉下一个 ACK” 后再“远程开门”，看事件日志的“安全”分类 |
| `offline-retry` | 离线与重试、离线补发 | 射频遮挡后远程开门，网关每个周期重发，恢复后成功；一直离线时网关重发 5 次后报失败，加卡指令被挂起，锁重新上线后自动补发并执行；远程开门不补发 | “射频遮挡（离线）” → 下发指令 → “恢复在线”，看指令追踪表的“发送次数”和“补发” |
| `status` | 状态上报与查询 | 心跳（默认 30 秒）判断在线；主动查询返回状态、电量、卡数；全楼巡检约 2 秒 | “状态查询” / “全楼状态巡检” |
| `low-battery` | 低电量告警 | 电压降到 4300 mV → 平台收到低电告警、房间标橙点；刷卡照常开门但给出低电提示；3900 mV 严重低电；换电池后恢复 | 拖动“电池”滑块 → “设置电压” |
| `fault` | 电机故障 | 电机卡死时远程开门返回 HW_FAULT，门锁进入故障状态（紫色）；排除后恢复 | “电机卡死” → “远程开门” |
| `password` | 密码开门 | 固定密码；前后加任意数字的虚位密码；访客一次性密码用一次就失效；保洁周期密码只在指定星期和时段有效 | “添加密码” / “一次性访客密码” → 在“键盘输入”里输入 → “输入密码 #” |
| `offline-password` | 离线密码 | 锁完全离线时，平台算出 10 位离线密码：一次性的只能用一次；限时 8 小时的可反复用；输入清除码后全部作废；恢复在线后记录补传 | “射频遮挡” → “离线密码 一次性/限时/清除码”（密码自动填进键盘输入框）→ “输入密码 #” |
| `member` | 成员管理 | 同一用户的卡和密码一起冻结（欠费）返回 FROZEN、解冻；退宿一条指令删光该用户全部凭证 | “成员”框填用户号 → “冻结 / 解冻 / 退宿” |
| `alarms` | 告警 | 连续 5 张陌生卡 → 试错告警，3 分钟内合法卡也刷不开但远程开门有效；胁迫密码照常开门并静默告警；撬面板、机械钥匙、门未关好（演示里设为 3 秒）都上报 | “刷一张陌生卡”×5、“胁迫密码”、“撬面板”、“机械钥匙开门”、“推开门”，看事件日志“告警” |
| `double-lock` | 室内反锁与门磁 | 反锁后门外刷卡、远程开门都返回 DOUBLE_LOCKED；刷卡进门后关门立即落锁 | “室内反锁” → “刷卡”/“远程开门”；“推开门” → “关上门”（格子左上角黑点 = 反锁，虚线框 = 门开着）|
| `unlock-request` | 访客呼叫与确认开门 | 关闭“允许远程开门”后平台直接开门被拒（NEED_REQUEST）；访客按呼叫键，批准这一次请求后开门 | “禁止直接远程开门” → “访客按呼叫键”（格子闪蓝点）→ “批准访客开门请求” |
| `config` | 整栋楼统一配置 | 批量给 360 把锁下发自动落锁时间等参数，约 3 秒全部生效 | “全楼统一自动落锁时间” |
| `ble-fallback` | 手机蓝牙本地开门 | 锁封锁后断网：学生蓝牙钥匙被拒，宿管蓝牙钥匙能开，篡改的钥匙被拒；恢复后记录补传 | “射频遮挡” → “学生/宿管手机蓝牙开门” |
| `offline-records` | 断网记录不丢 / 上行重发 | 离线刷卡 25 次（超出锁内 16 条队列），恢复后平台发现序号缺口自动补传，25 条全部到达；空口丢包 30% 时上行靠 ACK 重发不丢 | “射频遮挡” → 多次“刷卡” → “恢复在线”，看右侧“开门记录”一行 |
| `time-sync` | 信标校时 | 门锁时钟慢 5 分钟，下一个信标就校正 | “时钟慢 5 分钟” |

## Web 控制台怎么看

- **楼栋视图**：每个格子是一把锁，颜色是锁的真实状态（灰蓝上锁、绿开门、橙应急、红封锁、紫故障、斜纹射频离线），右上橙点是低电量，左下白点表示锁里存了卡。点击格子选中。
- **右侧主板**：选中门锁的虚拟主板——EFR32FG28、433MHz 天线（显示信道和时隙）、NFC 读卡、电机驱动/离合（啮合或分离）、指示灯、蜂鸣器、4 节 AA 电池电压。刷卡、开门时指示灯和蜂鸣器会动。
- **楼栋格子的标记**：左上黑点 = 室内反锁，橙色虚线框 = 门开着，右下闪烁蓝点 = 访客正在请求开门。
- **操作区**分五组：门口操作（刷卡、键盘输入、手机蓝牙、推门/关门、反锁、呼叫键、机械钥匙、撬面板）、平台下发（真正走 MQTT → 网关 → 433MHz）、密码与成员（密码、一次性/胁迫密码、离线密码、冻结/解冻/退宿、参数）、故障与环境（电池、射频遮挡、ACK 丢失、电机卡死、时钟漂移）、攻击者（重放、篡改）。顶部还可以调整整个空口的丢包率。
- **右侧信息**新增：门/反锁/试错冻结、开门记录（锁内序号、平台收到多少、缺多少、补传次数）、连接（待上行条数、重连次数、时钟偏差）、参数、告警、访客请求。
- **指令追踪**：每条指令的 排队 → 网关已收 → 已送达（空口 ACK）→ 已执行（门锁端到端结果）、发送次数、耗时和说明。
- **事件日志**：开门记录、告警、安全事件、平台指令、门锁固件日志，可按类别筛选。

## 秒级下发是怎么做到的（在模拟里能直接看到）

- 每把锁每 1 秒只在自己的时隙醒来一次（省电），网关知道每把锁的信道和时隙，在那个时刻发给它。
- 一台网关有 3 个信道 × 50 个时隙 = 150 个“单元”，180 把锁约 2 个周期就能每把各收到一条指令。所以整栋楼批量发卡约 2~3 秒，和楼的层数无关（楼越高，网关越多，并行进行）。
- 远程开门（P1）到达时，如果目标锁本周期的时隙还没过，网关当场插入发送；否则下个周期发。管理类指令（P2）最多占每个信道 70% 的时隙，不会挤占远程开门。
- 应急常开等广播：每台网关只构造一帧，在所有单元上各发一次，每把锁在自己的时隙听到，一个周期覆盖全楼。

## 端口与日志

| 端口 | 用途 |
| --- | --- |
| 8080 | Web 控制台、HTTP API |
| 1883 | 内置 MQTT（可以用 MQTTX 等客户端订阅 `sl/v1/#` 看网关和平台之间的消息） |
| UDP 9100、9101 … | 网关 ↔ 模拟射频协处理器（每台网关一个） |
| UDP 9200 | locksim 控制口（刷卡、电池、射频遮挡、攻击等） |

本机运行时日志在 `.sim/logs/`：`locksim.log` 是所有门锁固件的动作记录，`gateway-*.log`、`platform.log` 分别是网关和平台。

## HTTP API（演示脚本用的就是这些）

```sh
curl -X POST localhost:8080/api/locks/1203/unlock -d '{"duration_s":5}'      # 远程开门
curl -X POST localhost:8080/api/locks/1203/cards  -d '{"uid":"8A3F115C","user_id":2024001,"valid_days":30}'
curl -X DELETE localhost:8080/api/locks/1203/cards/8A3F115C
curl -X POST localhost:8080/api/locks/1203/status
curl localhost:8080/api/commands/<msg_id>                                     # 指令状态和各段时间
curl -X POST localhost:8080/api/buildings/1/issue-cards                       # 也可以 revoke-cards / unlock-all / status-all
curl -X POST localhost:8080/api/buildings/1/emergency-open                    # 也可以 lockdown / release
curl localhost:8080/api/jobs/<job>                                            # 批量任务进度与耗时
# 模拟“真实世界”
curl -X POST localhost:8080/api/sim/locks/1203/swipe   -d '{"uid":"8A3F115C"}'
curl -X POST localhost:8080/api/sim/locks/1203/battery -d '{"mv":4300}'
curl -X POST localhost:8080/api/sim/locks/1203/rf      -d '{"on":false}'
curl -X POST localhost:8080/api/sim/locks/1203/replay
curl -X POST localhost:8080/api/sim/loss -d '{"pct":10}'

# 密码、成员、参数、离线密码、蓝牙钥匙、访客请求、记录补传
curl -X POST localhost:8080/api/locks/211/passwords -d '{"pin":"246810","user_id":3001}'
curl -X POST localhost:8080/api/locks/211/passwords -d '{"pin":"135790","user_id":9001,"uses":1,"valid_hours":24}'
curl -X POST localhost:8080/api/locks/211/passwords -d '{"pin":"808080","user_id":5001,"days":[1,2,3,4,5],"start":"08:00","end":"10:00"}'
curl -X POST localhost:8080/api/locks/211/cards     -d '{"uid":"A0B0C0D0","user_id":3002,"duress":false,"admin":false}'
curl -X POST localhost:8080/api/locks/416/users/3002/freeze -d '{"freeze":true}'
curl -X DELETE localhost:8080/api/locks/416/users/3002                        # 退宿：删除该用户全部凭证
curl -X POST localhost:8080/api/locks/725/config -d '{"autolock_s":8,"remote_allowed":false}'
curl -X POST localhost:8080/api/buildings/1/config -d '{"autolock_s":5}'      # 也可以 clear-creds
curl -X POST localhost:8080/api/locks/322/offline-password -d '{"kind":"timed","hours":8}'   # once / timed / clear
curl -X POST localhost:8080/api/locks/826/ble-key -d '{"user_id":1,"hours":2,"admin":true}'
curl -X POST localhost:8080/api/locks/725/requests/approve
curl -X POST localhost:8080/api/locks/927/read-log -d '{"from":1,"count":16}'
# 门口的“真实世界”
curl -X POST localhost:8080/api/sim/locks/211/keypad   -d '{"digits":"7724681093"}'
curl -X POST localhost:8080/api/sim/locks/826/ble      -d '{"token":"<ble-key 返回的 token>"}'
curl -X POST localhost:8080/api/sim/locks/624/door     -d '{"open":true}'
curl -X POST localhost:8080/api/sim/locks/624/dlock    -d '{"on":true}'
curl -X POST localhost:8080/api/sim/locks/725/doorbell
curl -X POST localhost:8080/api/sim/locks/528/pry                             # 也可以 key（机械钥匙）
curl -X POST localhost:8080/api/sim/locks/1029/clock   -d '{"delta_s":-300}'
```

房间可以写房间号（`1203`）或 16 位 EUI。

## 以后：用 Renode 跑真正的 EFR32 固件

现在虚拟锁跑的是“固件逻辑 + 模拟硬件层”。等 `firmware/lock/port/efr32` 写好以后，可以再往前一步，在 Renode 里运行编译出来的 EFR32 固件镜像：

1. 用 Simplicity Studio / SLC 编译出 `.elf`。
2. 在 Renode 里加载 EFR32 平台描述（Series 1 有现成的；FG28 需要基于它补 Series 2 的外设：GPIO、USART、定时器、RAIL 射频接口）。
3. 用 Renode 的 Wireless Medium 连接多个锁实例，或者把射频接口桥接到本仓库网关的 UDP 协议（`common/slpgo/simnet` 定义的格式），这样 Renode 里的“真固件”可以直接接到现在的网关和平台上。

这一步工作量主要在补 FG28 外设模型，建议在开发板到手、固件移植基本完成后再评估是否值得做。

## 和真实部署的差别（重要）

- 密钥：模拟用固定主密钥 `smartlock-sim-01` 派生所有密钥，只能用于模拟。
- 入网：模拟里锁发入网请求网关就分配地址；真实设备入网请求要带 K_dev 计算的 MIC，由平台校验并下发加密的 K_link。
- MQTT：内置 broker 没有认证和 TLS；生产用 EMQX + TLS 客户端证书（`platform/deploy`）。
- 平台数据：存在内存里，重启就没了；生产用 PostgreSQL。
- 射频：不模拟碰撞、距离、穿墙和功耗，这些必须用开发板实测。
