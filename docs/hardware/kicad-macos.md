# 在 macOS 上用 KiCad 画门锁主板并在嘉立创打样

工程在 [`hardware/mainboard/`](../../hardware/mainboard/)：原理图、封装、BOM 已经做好，PCB 是导入了网表的空板，布局布线由你来做。工程说明、打板前必须补齐的项目和布局要点见 [hardware/mainboard/README.md](../../hardware/mainboard/README.md)。

> 网站界面和菜单名称会随版本变化，以你看到的为准。

## 1. 安装 KiCad

1. 到 kicad.org 下载页选 macOS，下载 KiCad 安装包（dmg），拖进“应用程序”。也可以用 Homebrew：`brew install --cask kicad`。
2. 工程是 KiCad 8 格式，用 KiCad 8.0.x 最稳。用更新的 9.x 也能打开，保存时会升级文件格式，升级后旧版 KiCad 就打不开了，团队里统一一个版本即可。
3. 第一次打开 KiCad 时，提示配置全局符号库/封装库，选“复制默认的全局库表（推荐）”。官方库随安装包一起装好，不用另外下载。

## 2. 打开工程

1. 把仓库克隆到本地（`git clone`，或在 GitHub 上下载 ZIP 解压）。
2. 双击 `hardware/mainboard/smart-lock-mainboard.kicad_pro`，或在 KiCad 里“文件 → 打开工程”。
3. 工程自带库表（`sym-lib-table`、`fp-lib-table`），会自动加载工程库 `smartlock`（EFR32FG28、TPS629210、DRV5032、电源符号、弹簧天线焊盘），不用手动导入。如果提示找不到 `smartlock` 库：“偏好设置 → 管理符号库 → 工程专用库”里确认路径是 `${KIPRJMOD}/lib/smartlock.kicad_sym`，封装库同理。

## 3. 推荐装两个插件

在 KiCad 主窗口打开“插件和内容管理器”（Plugin and Content Manager），搜索安装：

- **Fabrication Toolkit**：一键导出嘉立创要的 Gerber 压缩包、BOM 和坐标文件。它读取每个器件的 `LCSC` 字段，本工程所有器件都已填好；自动排除 DNP 器件。
- （可选）**easyeda2kicad** 或类似的嘉立创库导入工具：以后换料时，可以按 C 编号把立创商城的符号、封装、3D 模型导入 KiCad。

## 4. 完善原理图

1. 打开原理图（顶层页双击进入各分页）。每页顶部有蓝色说明文字，写了这一页要核对的事项。
2. 按 [README 的“打板前必须补的地方”](../../hardware/mainboard/README.md#打板前必须补的地方) 补齐射频匹配取值等内容：到 Silicon Labs 官网 xG28 开发套件页面下载 BRD4401C 的原理图和 PCB 设计文件，照抄射频、去耦和晶振部分。
3. 改元件值或料号：选中符号按 E 打开属性，改“值”和 `LCSC` 字段（C 开头的编号）。
4. 每次改完运行“检查 → 电气规则检查（ERC）”，保持 0 错误。

## 5. 画 PCB

1. 在原理图编辑器里点“工具 → 从原理图更新 PCB”（快捷键 F8），确认没有差异，然后打开 PCB 编辑器。所有器件停放在板框右侧，按功能页分组。
2. “文件 → 电路板设置”：
   - 板层叠层：4 层，板厚 1.6 mm，层定义按嘉立创 JLC04161H-7628 叠层填写。
   - 网络类已设好：`Power`（0.5 mm）、`RF`、`Default`（0.15 mm）。用嘉立创阻抗计算器算出 50 Ω 共面波导线宽和缝隙后，填到 `RF` 网络类。
   - 设计规则里的最小线宽/线距（0.1 mm）、最小过孔（0.45/0.2 mm）是嘉立创 4 层板的常规工艺，可以按报价需要放宽。
3. 删掉 70 × 50 mm 的占位板框，在 Edge.Cuts 层按锁壳实测尺寸画板框和安装孔。
4. 按 README 的“PCB 布局要点”布局：先射频和主控，再电源，最后接口；天线净空、电机单点接地、降压回路这几条一定要看。
5. 走线完成后铺铜：L2 整层 GND；L1、L4 空白处铺 GND；L3 按需铺 3V3 / VBAT_P。按 B 重新填充铺铜。
6. “检查 → 设计规则检查（DRC）”，勾选“测试与原理图的一致性”，清到 0 错误。
7. 按 Alt+3（或“视图 → 3D 查看器”）看元件高度和锁壳是否干涉。

## 6. 导出生产文件

### 用 Fabrication Toolkit（推荐）

在 PCB 编辑器工具栏点 Fabrication Toolkit 图标，确认输出即可。它会在工程目录下生成 `production/` 子目录，里面有：

- `*.zip`：Gerber + 钻孔文件，直接上传嘉立创
- `bom.csv`：嘉立创 BOM 格式
- `positions.csv`：嘉立创坐标（CPL）格式

### 手动导出

1. **Gerber**：“文件 → 制造输出 → Gerber”。勾选 F.Cu、In1.Cu、In2.Cu、B.Cu、F.Paste、B.Paste、F.SilkS、B.SilkS、F.Mask、B.Mask、Edge.Cuts；勾选“使用 Protel 文件扩展名”。点“绘制”，再点“生成钻孔文件”，格式选 Excellon、单位 mm。把输出目录里所有文件打成一个 zip。
2. **BOM**：仓库里已有 [`production/bom_jlcpcb.csv`](../../hardware/mainboard/production/bom_jlcpcb.csv)（列为 Comment、Designator、Footprint、LCSC Part #）。改过原理图后需要重新生成：用 Fabrication Toolkit，或者原理图编辑器“工具 → 生成物料清单”，字段选值、位号、封装、LCSC，按值+封装分组，导出 CSV 后把表头改成上面四列。
3. **坐标文件**：“文件 → 制造输出 → 元件放置文件（.pos）”，格式 CSV、单位 mm、单个文件。打开后把表头改成嘉立创要求的 `Designator, Mid X, Mid Y, Layer, Rotation`（Ref→Designator，PosX→Mid X，PosY→Mid Y，Side→Layer，Rot→Rotation）。

## 7. 在嘉立创下单（PCB + SMT 贴片）

1. 登录嘉立创（国内站 jlc.com 或“嘉立创下单助手”；海外站是 JLCPCB），和立创商城是同一个账号。
2. PCB 下单：上传 Gerber 压缩包，确认识别出的尺寸和层数。参数：
   - 层数 4，板厚 1.6 mm，阻抗控制选“需要”，叠层选 JLC04161H-7628（要和你算阻抗时用的一致）
   - 表面处理选沉金（0.4 mm 间距 QFN 焊接更可靠）
   - 过孔盖油；数量 5
3. 选“需要 SMT 贴片”，贴片面选顶层，贴片数量可以先贴 2–5 片。
4. 上传 BOM 和坐标文件。系统会按 LCSC 编号自动匹配：
   - 检查每一行是否匹配成功、有没有缺货。2.4 GHz 匹配的 7 个件是 0201 封装，确认嘉立创接受 0201 贴装。
   - **U1（EFR32FG28B322F1024IM68-A）** 如果没库存：选“客供料”自己寄芯片，或在立创商城/嘉立创元件库提前下单备货到你的账户库存。0.4 mm 间距 QFN 不建议手焊。
   - 扩展库器件每种会收一次换料费，能换成基础库的阻容尽量换。
   - DNP 器件（人脸版、天线二选一、微调位）不要勾选贴装。
5. **检查贴片预览**：重点看 U1 第 1 脚方向、二极管和 LED 极性、晶振、连接器和 USB-C 的朝向。方向不对就在预览里旋转修正（KiCad 封装和嘉立创库的零度方向不一定一致）。
6. ANT1 弹簧天线没有贴片料，收到板子后手焊。
7. 确认工程费、换料费和交期后付款。PCB 加贴片一般一周左右，客供料要算上寄送时间。

## 8. 板子到手后

按 [board-spec.md 第 9 节](board-spec.md#9-板子到手后) 和 [`firmware/lock/port/efr32/README.md`](../../firmware/lock/port/efr32/README.md) 走：不上电量短路 → 限流 100 mA 上电看 3V3 和静态电流 → 烧自检程序逐项检查 → RAIL Test 调 433 MHz 匹配 → 交回软件联调。
