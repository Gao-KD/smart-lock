# EFR32FG28 门锁主板移植层

本目录是门锁主板（EFR32FG28B322F1024IM68）的板级代码。上层的门锁逻辑、凭证、协议、MAC（`firmware/lock/core|credential|app|mac`、`common/slp`）和主机测试、模拟平台跑的是同一份代码；这里只负责把它们接到真实硬件上。

| 文件 | 内容 | 状态 |
| --- | --- | --- |
| `board.h` | **引脚表（唯一来源）**、外设实例、分压比、电机堵转阈值、PSA 密钥 ID | 定稿 rev A。按 Silicon Labs 官方器件头文件核对过引脚存在性、调试脚、晶振脚、EM4 唤醒脚；`tests/test_board.c` 每次编译自动检查 |
| `hal_efr32.c/.h` | `hal.h` 的实现：时间、电机驱动 + 堵转检测、电池电压、LED/蜂鸣器、NVM3、唤醒输入、电源轨开关 | 已按 SDK API 写好，**还没在板子上编译过** |
| `crypto_psa.c` | `slp/crypto_port.h` 的实现：AES-128-CCM 走 PSA Crypto，密钥在 Secure Vault | 同上；KDF 留到绑定流程实现 |
| `bringup_main.c` | 新板自检程序：LED、蜂鸣器、电池电压、电机正反转、全部开关、各电源轨、睡眠电流 | 同上，第一块板到手先跑它 |

还要写的（软件这边负责，不影响画板）：433 MHz RAIL 驱动（前导码嗅探、同步唤醒、长唤醒，见 [radio-plan.md](../../../../docs/radio-plan.md)）、BLE（蓝牙钥匙、安装绑定）、FM17550 读卡驱动、触摸键盘驱动、指纹/人脸模组协议（等模组选定）、绑定状态机、主循环、OTA。

## 编译与烧录

1. 安装 Simplicity Studio 5 和 Simplicity SDK（建议 2024.12 或更新，与 `board.h` 核对用的版本一致）。
2. 新建工程：File → New → Silicon Labs Project Wizard → 目标器件 `EFR32FG28B322F1024IM68` → `Empty C Project`。
3. 在 `.slcp` 的 Software Components 里添加：`emlib_gpio`、`emlib_iadc`、`gpiointerrupt`、`sleeptimer`、`udelay`、`nvm3_default`、`psa_crypto`（含 CCM）、`iostream_eusart`（实例 EUSART0，TX=PA8，RX=PA9，115200）、`power_manager`、`device_init`（HFXO 39 MHz、LFXO 32.768 kHz）。
4. 把本目录的 `board.h`、`hal_efr32.c`、`hal_efr32.h`、`crypto_psa.c`、`bringup_main.c` 加进工程，并把 `firmware/lock/hal`、`common/slp/include` 加到 include 路径，`common/slp/src/*.c` 加进源文件。
5. 编译，用 Pro Kit 主板（或 J-Link）经 2×5 1.27 mm 调试口（SWCLK PA1、SWDIO PA2、RESETn、VTARGET、GND）烧录；或命令行：

   ```sh
   commander flash build/release/bringup.s37 --device EFR32FG28B322F1024IM68
   commander device info --device EFR32FG28B322F1024IM68   # 读芯片和 EUI，确认连通
   ```

6. 串口工具连 VCOM（115200 8N1），复位后按屏幕提示逐项检查。

量产时 Bootloader（Gecko Bootloader，签名 GBL）+ 应用一起烧录，再由产测脚本注入 `K_dev` 并锁调试口（见 [provisioning.md](../../../../docs/provisioning.md) 第 2 节）。

## 修改引脚

硬件如果必须改某个信号的引脚：只改 `board.h`（同时改 [board-spec.md](../../../../docs/hardware/board-spec.md) 的表），然后在仓库根目录跑 `cmake --build build && ctest --test-dir build`。`board_tests` 会检查：引脚存在、不重复、不占调试脚和晶振脚、需要唤醒的输入在 Port A/B、复位键和防撬在 EM4 唤醒脚、外部中断号符合 Series 2 分组规则、分压比不超 ADC 量程。
