# 门锁主板 rev A：KiCad 8 工程

按 [docs/hardware/board-spec.md](../../docs/hardware/board-spec.md) 生成的门锁后面板主板 KiCad 工程。原理图连接与规格第 3 节引脚表、[`firmware/lock/port/efr32/board.h`](../../firmware/lock/port/efr32/board.h) 一一对应，板子照此做出来就能直接跑仓库里的固件。macOS 上安装 KiCad、导出生产文件、在嘉立创下单的步骤见 [docs/hardware/kicad-macos.md](../../docs/hardware/kicad-macos.md)。

## 目录

| 文件 | 内容 |
| --- | --- |
| `smart-lock-mainboard.kicad_pro` | 工程文件，用 KiCad 打开它。已设好网络类（Power 0.5 mm、RF、Default 0.15 mm）和嘉立创工艺下的最小线宽线距 |
| `smart-lock-mainboard.kicad_sch` | 顶层原理图，下挂 5 张分页图 |
| `power.kicad_sch` | 电源：电池/USB-C、防反接、3V3 降压、负载开关、5V（人脸版）、电池采样 |
| `mcu.kicad_sch` | 主控：EFR32FG28、去耦、DC-DC 电感、39 MHz / 32.768 kHz 晶振、复位、调试口、测试点 |
| `rf.kicad_sch` | 射频：433 MHz 匹配/低通/π 型微调/天线选择，2.4 GHz 匹配 + PCB 天线 |
| `motor.kicad_sch` | 电机：DRV8837、0.22 Ω 电流采样 |
| `io.kicad_sch` | 门磁、开关、蜂鸣器、RGB 灯、前面板 FPC、指纹/人脸接口 |
| `smart-lock-mainboard.kicad_pcb` | 4 层空板：所有封装已导入并带网络，停放在板框右侧，按分页分组。布局布线由你来做 |
| `lib/` | 工程库：EFR32FG28、TPS629210、DRV5032、各电源符号；433 弹簧天线焊盘封装 |
| `production/bom_jlcpcb.csv` | 嘉立创 SMT 格式 BOM（Comment, Designator, Footprint, LCSC Part #），不含 DNP 件 |
| `production/bom_full.csv` | 完整 BOM：型号、LCSC、封装、是否贴、所在页、备注（含 DNP 件） |
| `production/schematic.pdf` | 原理图 PDF，方便不开 KiCad 时审阅 |
| `production/erc.rpt` | ERC 报告 |
| `tools/` | 生成和校验脚本（见文末） |

前面板小板（FM17550 读卡、触摸键盘控制器）不在本工程内，经 J4（16 针 FPC）连接，脚位按规格 6.10 节。

## 已验证

- **ERC**：KiCad 8.0.9 `kicad-cli sch erc --severity-all`，0 个违规（含警告）。
- **引脚表**：从网表反查 U1 每个焊盘的网络，`board.h` 里 37 个信号 + SWD/LFXO 固定脚全部落在正确焊盘上（`tools/check_netlist.py`）。
- **QFN68 焊盘编号**：取自 Silicon Labs 官方 pin tool 数据 `EFR32FG28B322F1024IM68.device`（sisdk-release 仓库）。注意 B322（带 2.4 GHz）和 B320 的 22、24 脚不同：B322 是 RF2G4_I0 / RF2G4_O0，433 MHz 走 SUBG_I1（21）/ SUBG_O1（23）。
- **PCB 与原理图一致**：`kicad-cli pcb drc --schematic-parity`，0 个差异。之后改原理图，在 PCB 里按 F8（从原理图更新 PCB）即可同步。
- **TPS629210 引脚**：两份独立的厂商符号库交叉核对一致。DRV5032 引脚同样核对过。

## 打板前必须补的地方

生成环境里拿不到 Silicon Labs 和 TI 的设计文件，下面这些在原理图上都有文字标注，请对照官方资料补齐：

1. **射频匹配取值（rf 页所有 TBD 件）**：结构和取值要照抄 xG28 射频板 BRD4401C 原理图和 433 MHz 匹配应用笔记。现在的拓扑（TX 三阶 LC 低通、RX L 型匹配、天线端 π 型、2.4 GHz 两路合一）只是占位，拿到参考设计后按它增删位置、填值，再在符号属性 `LCSC` 里填编号。没填完不要下单，BOM 里这些行的 LCSC 是空的。
2. **主控去耦和 DC-DC 电感**：mcu 页用的是 Series 2 常见取值（VREGVDD 10 µF + 100 nF，DVDD 4.7 µF，DECOUPLE 1 µF，各电源脚 100 nF，L3 2.2 µH），按 BRD4401C 核对。
3. **QFN68 散热焊盘尺寸**：封装选了 KiCad 库的 `QFN-68-1EP_8x8mm_P0.4mm_EP5.2x5.2mm_ThermalVias`，请按数据手册封装图核对 EP 尺寸（库里另有 EP 6.4 mm 版本）。
4. **U2 TPS629210 配置**：R6（MODE/S-CONF）和 FB/VSET 的接法要按数据手册的配置表确认（目标：外部分压反馈、自动 PFM）。R4/R5 = 1 MΩ / 221 kΩ 对应 Vref 0.6 V 时 3.31 V。
5. **U8 TPS62933（人脸版，默认不贴）**：RT 脚悬空的默认频率、关断电流是否满足规格 ≤ 1 µA。
6. **39 MHz 晶振**：Y1 选了 TXC 8Y39072005（2016 封装），频率、负载电容和精度按参考设计核对。
7. **C1 漏电流**：VBAT_P 上 220 µF 电解电容的漏电可能有几 µA 到几十 µA，会吃掉睡眠功耗预算。打样后实测睡眠电流，必要时换成陶瓷电容组或低漏电聚合物电容。

### 一个值得在规格层面讨论的问题

插 USB-C 应急供电时，VBAT_P 约 4.6 V；Q1 的栅极经 R1 接地，Vgs ≈ −4.6 V，Q1 会导通，电流会从 VBAT_P **反灌进电池**（电池没电或电压更低时）。这是“P-MOS 防反接 + 肖特基并入 USB”这个结构本身的特点，规格里就是这么写的，所以原理图按规格画了。碱性电池被反充有漏液风险。可选的改法：Q1 换理想二极管控制器（如 LM66100 一类），或者在电池支路串一个背靠背 MOS。建议改规格后再动原理图。

## 默认不贴（DNP）的器件

- 人脸版：U8、L2、C12–C16、R9、R10、J6、R60、D60
- 天线二选一：R102、J8（默认贴 R101 接弹簧天线 ANT1；改用 IPEX 时贴 R102 + J8、拆 R101）
- π 型微调位 C106、C107，2.4 GHz 微调位 C203
- 备用开关 SW5

改成人脸版：在原理图里选中这些符号，属性里取消“不贴装（DNP）”，再重新导出 BOM。

ANT1（433 MHz 弹簧天线）没有嘉立创编号，需要手焊，不在 `bom_jlcpcb.csv` 里。U1 嘉立创可能没货，可选“客供料”或先在嘉立创元件库下单备货。

## PCB 布局要点

**叠层**：4 层 1.6 mm，L1 信号 + 射频、L2 完整地（不要切割）、L3 电源（3V3 / VBAT_P 铺铜）、L4 信号。嘉立创下单选“阻抗控制”，叠层用常用的 JLC04161H-7628。50 Ω 共面波导的线宽和缝隙用嘉立创阻抗计算器按这个叠层算，结果填到 `RF` 网络类里。

**先放射频和主控**：

- U1 放在板子上半部，射频脚（21–24）朝板子上沿。
- 433 MHz 匹配元件按原理图顺序排成一条直线，紧贴 SUBG_O1 / SUBG_I1，走线尽量短，不拐直角，两侧打一排地过孔。π 型（C106/L106/C107）放在天线馈点旁边。
- **天线净空**：ANT1 弹簧天线放在板子上沿，离电池仓和电机至少 15 mm，周围和下方的净空按参考设计。2.4 GHz PCB 天线（ANT2，倒 F 形状）放在板角，天线区域所有层都不铺铜、不走线，形状最好照抄 BRD4401C。
- Y1（39 MHz）紧贴 HFXTAL_I/O（16、17 脚），下方和周围不走其他信号线，周边铺地；Y2 紧贴 PD0/PD1。
- U1 散热焊盘的封装已带 25 个地过孔（规格要求 9–16 个，可按工艺删减）；去耦电容按“每个电源脚一个 100 nF、离脚最近”放。DC-DC 电感 L3 和 C22 紧贴 VREGSW/DVDD。

**电源走线**：

- J1 → Q1 → VBAT_P 主干、电机回路（J7、U3、R30）走 0.5 mm 以上（`Power` 网络类已设好），能铺铜就铺铜。
- U2 降压的高频回路要小：C4 紧贴 VIN/GND，L1 紧贴 SW，SW 节点铜皮尽量小；R4/R5 是 1 MΩ 级高阻节点，FB 走线要短，远离 SW 和电感。
- **电机地单点接地**：电机电流经 R30 回到 GND，R30 的 GND 端直接接到 J1 负极附近，不要让电机电流穿过射频和主控下方的地。MOT_ISENSE 从 R30 的 MOT_GND 端开尔文引出，经 R31/C40 送到 U1。
- 电机驱动和大电流走线远离射频、晶振和天线。
- 空白处铺地，各层地多打缝合过孔，射频区域加密。

**接口和机械**：

- J4（FPC）放在朝前面板穿线孔的一边；J5/J6 靠近指纹、人脸模组走线方向。
- SW4（设置键）必须在门内侧电池盒里能按到；SW2（防撬）放在面板被拆时会动作的位置；SW1/SW3 按锁体结构定，也可以改成线接。
- J3 调试口和 TP1–TP16 测试点放在板边，方便产测顶针。
- 板框现在是 70 × 50 mm 占位矩形，按拆机量好的后面板内腔尺寸替换，标好螺丝孔、电池仓、电机线、把手方轴位置。最后看 3D 预览，检查元件高度。

## 重新生成

改连接请改 `tools/gen_project.py`（同时改规格和 `board.h`），然后运行：

```bash
cd hardware/mainboard
docker run --rm -v "$PWD/../..":/repo -w /repo/hardware/mainboard kicad/kicad:8.0 tools/build.sh
```

脚本依次生成原理图、跑 ERC、核对网表和引脚表、生成 PCB、跑原理图/PCB 一致性检查、导出 PDF。**注意**：重新生成会覆盖原理图和 PCB，开始手工布局之后就不要再跑它，直接在 KiCad 里改。
