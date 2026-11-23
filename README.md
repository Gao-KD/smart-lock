# smart-lock

面向高校宿舍、公租房等批量管理场景的智能门锁系统：门锁主板固件、楼宇网关、每校私有化部署的管理平台。

- 门锁：单芯片 EFR32FG28（Sub-GHz + BLE），标准 24×240 锁体，4 节 AA 供电
- 空口：433 MHz 私有协议，“同步唤醒”：网关不发信标、只在有数据时发射，门锁每秒在自己的时隙嗅探约 1 ms 前导码，靠加密 ACK 对齐和校时，带防假前导码耗电保护；AES-128-CCM* 两层加密——见 **[docs/radio-plan.md](docs/radio-plan.md)**
- 网关：每 6–7 层一台，Linux 主机 + 3–4 路射频协处理器，以太网或 4G 回传
- 平台：MQTT 5（TLS）+ 指令下发服务，每所学校独立私有化部署
- 时延目标：单条指令 P99 ≤ 1.5 s 到锁；单网关约 270 把锁约 2 s 全部送达；广播 1 s 内全楼送达
- 门锁功能：校园卡、密码（虚位/一次性/周期）、离线密码、手机蓝牙钥匙、成员冻结与退宿、试错冻结/胁迫/防撬/门未关等告警、室内反锁与门磁、访客呼叫确认后远程开门、参数配置、开门记录补传——见 **[docs/lock-features.md](docs/lock-features.md)**
- 出厂与绑定：未绑定时体验模式（123456/任意卡/指纹/人脸）、网关批量绑定与安装师傅蓝牙绑定、两阶段提交 + A/B 密钥槽保证断电断连不变砖、授权恢复出厂——见 **[docs/provisioning.md](docs/provisioning.md)**
- 连接策略：同步唤醒（一直可达）、事件即时上报 + ACK 重发、30 分钟心跳上报电量与状态（平台不轮询）、断线探测与重连、入网退避、ACK 校时、断网本地开门——见 **[docs/connectivity.md](docs/connectivity.md)**

## 模拟演示平台（在电脑上跑整套系统）

360 把虚拟门锁（运行门锁固件的真实 C 代码）+ 模拟 433MHz 空口 + 2 台网关 + 平台 + Web 控制台，20 个功能各有一键演示：

```sh
docker compose -f deploy/sim/docker-compose.yml up --build   # 或者本机: scripts/sim.sh
# 打开 http://localhost:8080 ，另开终端:
scripts/demo.sh all                                          # Docker: docker compose -f deploy/sim/docker-compose.yml exec sim demo all
```

详细说明、功能演示清单、在线画板/仿真工具对比见 **[docs/simulator.md](docs/simulator.md)**。

## 目录

| 目录 | 内容 | 语言 |
| --- | --- | --- |
| `common/slp` | 空口协议库：帧编解码、CRC、CCM nonce、防重放、发送计数器、消息去重、应用 TLV、时隙计算、两层加密封装；`port/host` 为 PC 用的 AES-128-CCM* 实现 | C99 |
| `common/slpgo` | 同一协议的 Go 实现（网关、平台使用，与 C 版互通测试）+ 模拟平台约定 | Go |
| `firmware/lock` | 门锁固件：锁状态机（含反锁、门磁）、凭证库（校园卡只读卡号、密码、周期/次数/冻结）、离线密码、蓝牙令牌、告警与记录日志、命令处理、锁端 MAC（ACK 同步、假唤醒防护、失联重连、上行确认重发）、硬件抽象 | C99 |
| `firmware/lock/port/efr32` | EFR32FG28 移植层：定稿引脚表 `board.h`、板级驱动、PSA 加密、板级自检程序（射频/蓝牙/模组驱动待实现，见该目录 README） | C |
| `firmware/lock/port/sim` | 模拟平台：locksim（数百把虚拟锁 + 模拟空口 + 模拟射频协处理器） | C |
| `firmware/rcp` | 网关射频协处理器固件（待实现） | C |
| `gateway` | 网关主机程序：入网、锁表与信道/时隙分配、按周期的优先级调度、周期内插入紧急指令、链路层加密与防重放、上行 ACK、MQTT 桥接 | Go |
| `platform` | 私有化平台：指令下发服务（HTTP 开放接口、按网关分批、三段确认、离线补发）、上行解密与去重、记录缺口补传、离线密码/蓝牙钥匙签发、Web 控制台、功能演示程序、docker-compose 部署 | Go |
| `deploy/sim`、`scripts` | 模拟平台一键启动（Docker / 本机脚本）与功能演示 | |
| `proto/gateway/v1` | 网关与平台之间的消息定义 | Protobuf |

## 硬件

- **[主板设计规格 rev A](docs/hardware/board-spec.md)**：可直接画板的电源树、定稿引脚表、各模块参考电路、FPC 定义、BOM、检查清单
- **[硬件交付说明](docs/hardware/handoff.md)**：给硬件工程师的平台选型、BOM 与频段采购、接口约束、软硬件分工、验收标准（含频段合规风险）
- [EFR32 移植层说明](firmware/lock/port/efr32/README.md)：固件怎么编译、烧录，哪些已完成
- [测试购买清单](docs/hardware/purchase-list.md)
- [嘉立创 EDA 原理图与 PCB 指南](docs/hardware/pcb-guide.md)

## 构建与测试

```sh
# 协议库 + 门锁固件逻辑（主机上编译，用模拟硬件跑单元测试）
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
./build/firmware/lock_host_demo

# Go：协议库、网关、平台的单元测试
(cd common/slpgo && go test ./...) && (cd gateway && go test ./...) && (cd platform && go test ./...)

# 只看网关调度（不需要 MQTT 和门锁）
cd gateway && go run ./cmd/gateway -dry-run -locks 270 -channels 3

# 整套系统：见上面“模拟演示平台”
```

## 现状

协议格式、锁状态机、全部门锁功能（见 [lock-features.md](docs/lock-features.md)）、连接策略（见 [connectivity.md](docs/connectivity.md)）、两层加密、网关调度与 MQTT 桥接、平台下发与上行处理已实现并有测试，并能在模拟平台上端到端运行。以下尚未实现：

- EFR32 移植：板级驱动（GPIO、ADC、电机、NVM3、开关输入）和 PSA 加密已写好但只过了语法检查，未在 Simplicity SDK 中编译、未上板；RAIL 收发、BLE 协议栈与蓝牙钥匙挑战应答、FM17550/键盘/指纹/人脸驱动、主循环、OTA 待实现
- 网关射频协处理器串口驱动、持久化队列、入网 K_dev 校验（模拟平台里用 UDP 代替串口）
- 平台数据库、密钥服务（KMS）、鉴权（模拟平台里用内存和模拟密钥代替）
- 指纹/人脸模组驱动、OTA、日志拉取
