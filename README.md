# smart-lock

面向高校宿舍、公租房等批量管理场景的智能门锁系统：门锁主板固件、楼宇网关、每校私有化部署的管理平台。

- 门锁：单芯片 EFR32FG28（Sub-GHz + BLE），标准 24×240 锁体，4 节 AA 供电
- 空口：433 MHz 私有协议，相位同步的周期侦听（每 1 s 一次），AES-128-CCM* 两层加密
- 网关：每 6–7 层一台，Linux 主机 + 3–4 路射频协处理器，以太网或 4G 回传
- 平台：MQTT 5（TLS）+ 指令下发服务，每所学校独立私有化部署
- 时延目标：单条指令 P99 ≤ 1.5 s 到锁；单网关约 270 把锁约 2 s 全部送达；广播 1 s 内全楼送达

## 目录

| 目录 | 内容 | 语言 |
| --- | --- | --- |
| `common/slp` | 空口协议库：帧编解码、CRC、CCM nonce、防重放、发送计数器、消息去重、应用 TLV、时隙计算 | C99 |
| `firmware/lock` | 门锁固件：锁状态机、凭证库（校园卡只读卡号）、命令处理、锁端 MAC、硬件抽象 | C99 |
| `firmware/lock/port/efr32` | EFR32FG28 移植层（待实现） | C |
| `firmware/rcp` | 网关射频协处理器固件（待实现） | C |
| `gateway` | 网关主机程序：锁表与信道/时隙分配、按周期的优先级调度、MQTT 主题、射频协处理器接口 | Go |
| `platform` | 私有化平台：指令下发服务（HTTP 开放接口、按网关分批、三段确认）、docker-compose 部署 | Go |
| `proto/gateway/v1` | 网关与平台之间的消息定义 | Protobuf |

## 构建与测试

```sh
# 协议库 + 门锁固件逻辑（主机上编译，用模拟硬件跑单元测试）
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
./build/firmware/lock_host_demo

# 网关：单元测试 + 一栋楼单台网关的下发模拟
cd gateway && go test ./... && go run ./cmd/gateway -locks 270 -channels 3

# 平台：单元测试 + 本地启动下发服务
cd platform && go test ./... && go run ./cmd/dispatch -addr :8080
```

## 现状

这是骨架：协议格式、锁状态机、凭证校验、网关调度和平台分批逻辑已实现并有测试。以下尚未实现：

- EFR32 移植（RAIL 收发、PSA Crypto/Secure Vault、NVM3、BLE）
- AES-CCM 加解密接入（接口见 `common/slp/include/slp/crypto_port.h`）
- 网关 MQTT 客户端、射频协处理器串口驱动、持久化队列
- 平台数据库、密钥服务（KMS）、MQTT 收发、鉴权
- 指纹/人脸模组驱动、OTA、日志拉取
