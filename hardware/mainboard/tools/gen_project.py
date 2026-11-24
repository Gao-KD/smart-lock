#!/usr/bin/env python3
"""Generate the KiCad 8 project of the lock mainboard rev A.

Source of truth: docs/hardware/board-spec.md (pin table section 3 matches
firmware/lock/port/efr32/board.h line by line). Run inside the KiCad 8
docker image (or any machine with the KiCad 8 symbol library):

    python3 tools/gen_project.py /usr/share/kicad/symbols

It rewrites the schematic sheets, the project symbol library and the BOMs.
The PCB is created separately by tools/make_pcb.py (needs pcbnew).
"""
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from kisch import Project, StdLib, box_symbol  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = "smart-lock-mainboard"

symdir = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/kicad/symbols"
P = Project(NAME, StdLib(symdir))

# ------------------------------------------------------------------ nets
for n in ["GND", "BAT", "USB_VBUS", "VBAT_P", "3V3", "3V3_NFC", "3V3_FP", "5V_FACE", "DVDD"]:
    P.add_power(n)

# ------------------------------------------------------------------ parts tables
FP_R0402 = "Resistor_SMD:R_0402_1005Metric"
FP_C0402 = "Capacitor_SMD:C_0402_1005Metric"
FP_C0603 = "Capacitor_SMD:C_0603_1608Metric"
FP_C0805 = "Capacitor_SMD:C_0805_2012Metric"
FP_L0402 = "Inductor_SMD:L_0402_1005Metric"
FP_TP = "TestPoint:TestPoint_Pad_D1.0mm"

R0402 = {  # value: (LCSC, MPN) -- UniOhm 0402 1 %, JLC basic parts
    "0": ("C17168", "0402WGF0000TCE"), "1k": ("C11702", "0402WGF1001TCE"),
    "4.7k": ("C25900", "0402WGF4701TCE"), "5.1k": ("C25905", "0402WGF5101TCE"),
    "10k": ("C25744", "0402WGF1002TCE"), "19.1k": ("C52036", "0402WGF1912TCE"),
    "100k": ("C25741", "0402WGF1003TCE"), "221k": ("C26998", "0402WGF2213TCE"),
    "330k": ("C25778", "0402WGF3303TCE"), "1M": ("C26083", "0402WGF1004TCE"),
    "10M": ("C26082", "0402WGF1005TCE"),
}
CAPS = {  # value: (footprint, LCSC, MPN)
    "1nF": (FP_C0402, "C14442", "CL05B102KB5NNNC"),
    "10nF": (FP_C0402, "C15195", "CL05B103KB5NNNC"),
    "100nF": (FP_C0402, "C1525", "CL05B104KO5NNNC"),
    "1uF": (FP_C0402, "C52923", "CL05A105KA5NQNC"),
    "4.7uF": (FP_C0603, "C19666", "CL10A475KO8NNNC"),
    "10uF": (FP_C0603, "C19702", "CL10A106KP8NNNC"),
    "10uF/25V": (FP_C0805, "C15850", "CL21A106KAYNNNE"),
    "22uF/25V": (FP_C0805, "C45783", "CL21A226MAQNNNE"),
}
counters = {}


def R(sh, ref, val, a, b, dnp=False, note=""):
    lc, mpn = R0402[val]
    return P.part(sh, ref, "Device:R", val if val != "0" else "0R", FP_R0402, {"1": a, "2": b},
                  lc, mpn, dnp=dnp, note=note)


def C(sh, ref, val, a, b="GND", dnp=False, note=""):
    fp, lc, mpn = CAPS[val]
    return P.part(sh, ref, "Device:C", val, fp, {"1": a, "2": b}, lc, mpn, dnp=dnp, note=note)


def TBD(sh, ref, kind, a, b, dnp=False, note="RF 匹配：取值照抄 xG28 参考设计（BRD4401C / AN），填值后再选料"):
    if kind == "L":
        return P.part(sh, ref, "Device:L", "TBD", FP_L0402, {"1": a, "2": b}, "", "Murata LQW15AN 系列（值待定）",
                      dnp=dnp, note=note)
    return P.part(sh, ref, "Device:C", "TBD", FP_C0402, {"1": a, "2": b}, "", "0402 C0G（值待定）",
                  dnp=dnp, note=note)


def TP(sh, ref, net):
    return P.part(sh, ref, "Connector:TestPoint", net, FP_TP, {"1": net}, bom=False)


# ------------------------------------------------------------------ custom symbols
# EFR32FG28B322F1024IM68: pad numbers from Silicon Labs pin tool data
# (sisdk-release platform_core/platform/hwconf_data/pin_tool/efr32fg28/
#  efr32fg28b322f1024im68/EFR32FG28B322F1024IM68.device), pad 69 = exposed pad.
EFR_LEFT = [
    ("50", "VREGVDD", "power_in"), ("49", "VREGSW", "passive"), ("51", "VREGVSS", "power_in"),
    ("52", "DVDD", "power_in"), ("48", "DECOUPLE", "power_out"), ("53", "AVDD", "power_in"),
    ("6", "IOVDD", "power_in"), ("33", "IOVDD", "power_in"), ("54", "IOVDD", "power_in"),
    ("19", "RFVDD", "power_in"), ("25", "PAVDD", "power_in"), ("20", "RFVSS", "power_in"),
    ("69", "VSS(EP)", "power_in"), None,
    ("18", "RESETn", "input"), ("16", "HFXTAL_I", "passive"), ("17", "HFXTAL_O", "passive"), None,
    ("23", "SUBG_O1", "passive"), ("21", "SUBG_I1", "passive"),
    ("24", "RF2G4_O0", "passive"), ("22", "RF2G4_I0", "passive"), None,
    ("2", "PD00/LFXTAL_O", "bidirectional"), ("1", "PD01/LFXTAL_I", "bidirectional"),
]
PAD = {  # GPIO -> pad
    "PA00": "32", "PA01": "34", "PA02": "35", "PA03": "36", "PA04": "37", "PA05": "38", "PA06": "39",
    "PA07": "40", "PA08": "41", "PA09": "42", "PA10": "43", "PA11": "44", "PA12": "45", "PA13": "46",
    "PA14": "47", "PB00": "31", "PB01": "30", "PB02": "29", "PB03": "28", "PB04": "27", "PB05": "26",
    "PC00": "3", "PC01": "4", "PC02": "5", "PC03": "7", "PC04": "8", "PC05": "9", "PC06": "10",
    "PC07": "11", "PC08": "12", "PC09": "13", "PC10": "14", "PC11": "15",
    "PD02": "68", "PD03": "67", "PD04": "66", "PD05": "65", "PD06": "64", "PD07": "63", "PD08": "62",
    "PD09": "61", "PD10": "60", "PD11": "59", "PD12": "58", "PD13": "57", "PD14": "56", "PD15": "55",
}
EFR_RIGHT = []
for port, rng in (("A", range(15)), ("B", range(6)), ("C", range(12)), ("D", range(2, 16))):
    for i in rng:
        g = "P%s%02d" % (port, i)
        EFR_RIGHT.append((PAD[g], g, "bidirectional"))
    EFR_RIGHT.append(None)
EFR_RIGHT.pop()
FP_EFR = "Package_DFN_QFN:QFN-68-1EP_8x8mm_P0.4mm_EP5.2x5.2mm_ThermalVias"
efr_sym = box_symbol("smartlock:EFR32FG28B322F1024IM68", "U", "EFR32FG28B322F1024IM68-A",
                     left=EFR_LEFT, right=EFR_RIGHT, fp=FP_EFR, min_w=25.4,
                     desc="Sub-GHz + BLE SoC, Secure Vault High, 1 MB flash, QFN68 8x8 mm")

tps629210 = box_symbol("smartlock:TPS629210DRLR", "U", "TPS629210DRLR",
                       left=[("6", "VIN", "power_in"), ("7", "EN", "input"),
                             ("8", "MODE/S-CONF", "input"), None, ("5", "GND", "power_in")],
                       right=[("4", "SW", "output"), ("3", "VOS", "power_in"),
                              ("1", "FB/VSET", "input"), ("2", "PG", "open_collector")],
                       fp="Package_TO_SOT_SMD:SOT-583-8",
                       desc="3-17 V, 1 A buck, 4 uA Iq, SOT-583 (pinout cross-checked with two vendor libraries)")
drv5032 = box_symbol("smartlock:DRV5032FBDBZR", "U", "DRV5032FBDBZR",
                     left=[("1", "VCC", "power_in"), None, ("3", "GND", "power_in")],
                     right=[("2", "OUT", "output")], fp="Package_TO_SOT_SMD:SOT-23",
                     desc="Ultra-low-power omnipolar Hall switch, 20 Hz, push-pull, SOT-23")

# ================================================================== sheet 1: power
sp = P.sheet("电源", "power.kicad_sch", "电源：电池/USB-C 输入、防反接、3V3、负载开关、5V（人脸版）、电池采样")
sp.text("电源树按 board-spec 第 2 节。所有 *_EN 默认 100k 下拉（复位期间支路关断）。\n"
        "U8 及其外围、J6 为人脸版器件，基础版 DNP。\n"
        "U2 的 MODE/S-CONF（R6）和 FB/VSET 接法、U8 的 RT 悬空频率请按 TI 数据手册核实。\n"
        "C1 选低漏电型号：电解电容漏电可能达数 µA~数十 µA，打样后实测睡眠电流，必要时换陶瓷电容组。", 15, 235)

P.part(sp, "J1", "Connector_Generic_MountingPin:Conn_01x02_MountingPin", "BAT 4xAA/2S",
       "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal",
       {"1": "BAT", "2": "GND", "MP": None}, "C295747", "S2B-PH-SM4-TB(LF)(SN)", note="电池输入，1=正 2=负")
P.part(sp, "Q1", "Transistor_FET:AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23",
       {"1": "REVPOL_G", "2": "BAT_SRC", "3": "BAT"}, "C15127", "AO3401A",
       note="电池支路背靠背 P-MOS（电池侧）：防反接")
P.part(sp, "Q6", "Transistor_FET:AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23",
       {"1": "REVPOL_G", "2": "BAT_SRC", "3": "VBAT_P"}, "C15127", "AO3401A",
       note="电池支路背靠背 P-MOS（负载侧）：插 USB 时关断，阻止反灌电池")
R(sp, "R1", "10M", "REVPOL_G", "BAT_SRC", note="默认关断上拉；10M 避免 Q7 导通时常耗电")
P.part(sp, "D1", "Diode:BZT52Bxx", "BZT52C10", "Diode_SMD:D_SOD-123",
       {"1": "BAT_SRC", "2": "REVPOL_G"}, "C155227", "BZT52C10-7-F", note="Vgs 齐纳保护")
P.part(sp, "Q7", "Transistor_FET:2N7002", "2N7002", "Package_TO_SOT_SMD:SOT-23",
       {"1": "BAT_ON", "2": "GND", "3": "REVPOL_G"}, "C8545", "2N7002",
       note="电池正接且无 USB 时导通，拉低 Q1/Q6 栅极；电池装反时关断")
R(sp, "R16", "100k", "BAT", "BAT_ON", note="Q7 栅极由电池电压驱动（无直流耗电）")
P.part(sp, "Q8", "Transistor_FET:2N7002", "2N7002", "Package_TO_SOT_SMD:SOT-23",
       {"1": "USB_VBUS", "2": "GND", "3": "BAT_ON"}, "C8545", "2N7002",
       note="USB 插入时导通，关断 Q7，从而关断电池支路")
R(sp, "R17", "100k", "USB_VBUS", "GND", note="USB 未插时 Q8 栅极下拉")
P.part(sp, "J2", "Connector:USB_C_Receptacle_PowerOnly_6P", "USB-C 供电",
       "Connector_USB:USB_C_Receptacle_GCT_USB4125-xx-x_6P_TopMnt_Horizontal",
       {"A9": "USB_VBUS", "B9": "USB_VBUS", "A12": "GND", "B12": "GND", "S1": "GND",
        "A5": "CC1", "B5": "CC2"}, "C3151650", "USB4125-GF-A", note="只供电：VBUS/GND/CC", newrow=False)
R(sp, "R2", "5.1k", "CC1", "GND")
R(sp, "R3", "5.1k", "CC2", "GND")
P.part(sp, "D2", "Diode:SMF5V0A", "SMF5.0A", "Diode_SMD:D_SMF", {"1": "USB_VBUS", "2": "GND"},
       "C669056", "SMF5.0A-TP", note="VBUS TVS")
P.part(sp, "D3", "Device:D_Schottky", "B5819W", "Diode_SMD:D_SOD-123", {"1": "VBAT_P", "2": "USB_VBUS"},
       "C779676", "B5819W-SL", note="USB 5V 经肖特基并入 VBAT_P")
P.part(sp, "C1", "Device:C_Polarized", "220uF/16V", "Capacitor_SMD:CP_Elec_6.3x7.7",
       {"1": "VBAT_P", "2": "GND"}, "C3342", "RVT1C221M0607", note="VBAT_P 储能 >=220uF，注意漏电流")
C(sp, "C2", "10uF/25V", "VBAT_P")
C(sp, "C3", "100nF", "VBAT_P")
# 3V3 buck
P.part(sp, "U2", "smartlock:TPS629210DRLR", "TPS629210DRLR", "Package_TO_SOT_SMD:SOT-583-8",
       {"6": "VBAT_P", "7": "VBAT_P", "8": "SCONF_3V3", "5": "GND", "4": "SW_3V3", "3": "3V3",
        "1": "FB_3V3", "2": None}, "C5219301", "TPS629210DRLR", custom=tps629210,
       note="3V3 常电降压，输入 3-17 V，Iq 4 uA", newrow=True)
C(sp, "C4", "10uF/25V", "VBAT_P")
P.part(sp, "L1", "Device:L", "2.2uH", "Inductor_SMD:L_Sunlord_SWPA4020S", {"1": "SW_3V3", "2": "3V3"},
       "C83423", "SWPA4020S2R2MT")
R(sp, "R4", "1M", "3V3", "FB_3V3", note="FB 分压：Vout = 0.6 V x (1 + R4/R5) = 3.31 V（按手册核实 Vref）")
R(sp, "R5", "221k", "FB_3V3", "GND")
R(sp, "R6", "0", "SCONF_3V3", "GND", note="S-CONF 配置电阻：按 TPS629210 数据手册表格选值（外部 FB、自动 PFM）")
C(sp, "C5", "22uF/25V", "3V3")
C(sp, "C6", "22uF/25V", "3V3")
P.part(sp, "C7", "Device:C_Polarized", "47uF/10V", "Capacitor_Tantalum_SMD:CP_EIA-3528-21_Kemet-B",
       {"1": "3V3", "2": "GND"}, "C22036", "TAJB476K010RNJ", note="3V3 储能，陶瓷+钽合计 >=47 uF")
# load switches
P.part(sp, "U6", "Power_Management:TPS22917DBV", "TPS22917DBVR", "Package_TO_SOT_SMD:SOT-23-6",
       {"1": "3V3", "2": "GND", "3": "NFC_PWR_EN", "4": "CT_NFC", "5": None, "6": "3V3_NFC"},
       "C2681320", "TPS22917DBVR", note="3V3_NFC 负载开关", newrow=True)
C(sp, "C8", "1nF", "CT_NFC", note="CT：控制上升沿")
C(sp, "C9", "1uF", "3V3_NFC")
R(sp, "R7", "100k", "NFC_PWR_EN", "GND", note="上电默认关断")
P.part(sp, "U7", "Power_Management:TPS22917DBV", "TPS22917DBVR", "Package_TO_SOT_SMD:SOT-23-6",
       {"1": "3V3", "2": "GND", "3": "FP_PWR_EN", "4": "CT_FP", "5": None, "6": "3V3_FP"},
       "C2681320", "TPS22917DBVR", note="3V3_FP 负载开关")
C(sp, "C10", "1nF", "CT_FP")
C(sp, "C11", "1uF", "3V3_FP")
R(sp, "R8", "100k", "FP_PWR_EN", "GND", note="上电默认关断")
# 5V face
P.part(sp, "U8", "Regulator_Switching:TPS62933", "TPS62933DRLR", "Package_TO_SOT_SMD:SOT-583-8",
       {"1": None, "2": "FACE_PWR_EN", "3": "VBAT_P", "4": "GND", "5": "SW_5V", "6": "BST_5V",
        "7": "SS_5V", "8": "FB_5V"}, "C3200405", "TPS62933DRLR", dnp=True,
       note="人脸版 5V 降压（基础版不贴）；关断电流请按手册核实 <=1 uA", newrow=True)
C(sp, "C12", "100nF", "BST_5V", "SW_5V", dnp=True)
P.part(sp, "L2", "Device:L", "4.7uH", "Inductor_SMD:L_Sunlord_SWPA6045S", {"1": "SW_5V", "2": "5V_FACE"},
       "C78804", "SWPA6045S4R7MT", dnp=True)
R(sp, "R9", "100k", "5V_FACE", "FB_5V", dnp=True, note="Vout = 0.8 V x (1 + R9/R10) = 4.99 V")
R(sp, "R10", "19.1k", "FB_5V", "GND", dnp=True)
C(sp, "C13", "22uF/25V", "5V_FACE", dnp=True)
C(sp, "C14", "22uF/25V", "5V_FACE", dnp=True)
C(sp, "C15", "10uF/25V", "VBAT_P", dnp=True)
C(sp, "C16", "10nF", "SS_5V", dnp=True)
R(sp, "R11", "100k", "FACE_PWR_EN", "GND", note="上电默认关断")
# battery divider
P.part(sp, "Q2", "Transistor_FET:AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23",
       {"1": "VDIV_G", "2": "VBAT_P", "3": "VDIV_HI"}, "C15127", "AO3401A", note="分压高边开关", newrow=True)
R(sp, "R12", "100k", "VBAT_P", "VDIV_G")
P.part(sp, "Q3", "Transistor_FET:2N7002", "2N7002", "Package_TO_SOT_SMD:SOT-23",
       {"1": "VBAT_EN", "2": "GND", "3": "VDIV_G"}, "C8545", "2N7002")
R(sp, "R13", "100k", "VBAT_EN", "GND", note="上电默认关断")
R(sp, "R14", "1M", "VDIV_HI", "VBAT_ADC", note="VBAT = V(PB2) x 1330/330")
R(sp, "R15", "330k", "VBAT_ADC", "GND")
C(sp, "C17", "100nF", "VBAT_ADC")
for i, n in enumerate(["BAT", "USB_VBUS", "VBAT_P", "3V3", "5V_FACE", "GND"]):
    P.part(sp, "#FLG%02d" % (i + 1), "power:PWR_FLAG", "PWR_FLAG", "", {"1": n}, bom=False,
           newrow=(i == 0))
TP(sp, "TP1", "3V3")
TP(sp, "TP2", "VBAT_P")
TP(sp, "TP3", "GND")

# ================================================================== sheet 2: MCU
sm = P.sheet("主控", "mcu.kicad_sch", "主控：EFR32FG28、去耦、39 MHz / 32.768 kHz 晶振、复位、调试口")
sm.text("去耦、DC-DC 电感、晶振照抄 Silicon Labs xG28 射频板参考设计（BRD4401C）；此处取值为常用值，以参考设计为准。\n"
        "HFXO/LFXO 使用芯片内部负载电容（Series 2 HFXO/LFXO 内置可调电容），无需外部负载电容。\n"
        "QFN68 封装选 EP 5.2x5.2 mm 版本，请按数据手册封装图核对散热焊盘尺寸。\n"
        "J3 引脚按 Mini Simplicity 10 针（AN958）定义：1 VTARGET 2 GND 3 RST 4 VCOM_RX 5 VCOM_TX 6 SWO 7 SWDIO 8 SWCLK 9/10 PTI 不用。", 15, 235)
G = {  # GPIO -> signal, exactly board-spec section 3 / board.h
    "PA00": "KEY_INT", "PA01": "SWCLK", "PA02": "SWDIO", "PA03": "SWV", "PA04": "NFC_IRQ",
    "PA05": "SETUP_KEY", "PA06": "FP_TOUCH", "PA07": "DOOR_HALL", "PA08": "VCOM_TX", "PA09": "VCOM_RX",
    "PA10": "PROX_INT", "PA11": "MECH_KEY", "PA12": "KEY_SCL", "PA13": "KEY_SDA", "PA14": "BUZZER",
    "PB00": "DBL_LOCK", "PB01": "TAMPER", "PB02": "VBAT_ADC", "PB03": "SPARE_WAKE",
    "PB04": "MOT_ISENSE", "PB05": "LED_R",
    "PC00": "NFC_SCK", "PC01": "NFC_MOSI", "PC02": "NFC_MISO", "PC03": "NFC_CS", "PC04": "NFC_NRST",
    "PC05": "NFC_PWR_EN", "PC06": "MOT_IN1", "PC07": "MOT_IN2", "PC08": "MOT_NSLEEP",
    "PC09": "VBAT_EN", "PC10": "LED_G", "PC11": "LED_B",
    "PD00": "LFXTAL_O", "PD01": "LFXTAL_I", "PD02": "FP_TX", "PD03": "FP_RX", "PD04": "FP_PWR_EN",
    "PD05": "FACE_PWR_EN", "PD06": "FACE_TX", "PD07": "FACE_RX", "PD08": "KEY_BL_EN",
}
for i in range(9, 16):
    G["PD%02d" % i] = "TP_PD%d" % i
upins = {"50": "3V3", "49": "VREGSW", "51": "GND", "52": "DVDD", "48": "DECOUPLE", "53": "3V3",
         "6": "3V3", "33": "3V3", "54": "3V3", "19": "3V3", "25": "3V3", "20": "GND", "69": "GND",
         "18": "RESETn", "16": "HFXI", "17": "HFXO", "23": "SUBG_O1", "21": "SUBG_I1",
         "24": "RF2G4_O0", "22": "RF2G4_I0", "2": "LFXTAL_O", "1": "LFXTAL_I"}
for g, pad in PAD.items():
    upins[pad] = G[g]
P.part(sm, "U1", "smartlock:EFR32FG28B322F1024IM68", "EFR32FG28B322F1024IM68-A", FP_EFR, upins,
       "C20471554", "EFR32FG28B322F1024IM68-A", custom=efr_sym,
       note="主控，嘉立创无货时选客供料")
C(sm, "C20", "10uF", "3V3", note="VREGVDD")
C(sm, "C21", "100nF", "3V3", note="VREGVDD")
P.part(sm, "L3", "Device:L", "2.2uH", "Inductor_SMD:L_0805_2012Metric", {"1": "VREGSW", "2": "DVDD"},
       "C341781", "LQM21PN2R2MGHL", note="DC-DC 电感，按参考设计核对")
C(sm, "C22", "4.7uF", "DVDD", note="DVDD")
C(sm, "C23", "1uF", "DECOUPLE", note="DECOUPLE")
C(sm, "C24", "100nF", "3V3", note="IOVDD pad 6")
C(sm, "C25", "100nF", "3V3", note="IOVDD pad 33")
C(sm, "C26", "100nF", "3V3", note="IOVDD pad 54")
C(sm, "C27", "100nF", "3V3", note="AVDD")
C(sm, "C28", "100nF", "3V3", note="RFVDD")
C(sm, "C29", "100nF", "3V3", note="PAVDD")
C(sm, "C30", "10uF", "3V3", note="PAVDD bulk")
P.part(sm, "Y1", "Device:Crystal_GND24", "39.0MHz", "Crystal:Crystal_SMD_2016-4Pin_2.0x1.6mm",
       {"1": "HFXI", "3": "HFXO", "2": "GND", "4": "GND"}, "C20248925", "8Y39072005",
       note="39.0 MHz，负载电容/精度按参考设计核对", newrow=True)
P.part(sm, "Y2", "Device:Crystal", "32.768kHz", "Crystal:Crystal_SMD_2012-2Pin_2.0x1.2mm",
       {"1": "LFXTAL_I", "2": "LFXTAL_O"}, "C32346", "Q13FC13500004", note="32.768 kHz ±20 ppm 2012")
R(sm, "R20", "10k", "RESETn", "3V3")
C(sm, "C31", "100nF", "RESETn")
P.part(sm, "J3", "Connector_Generic:Conn_02x05_Odd_Even", "DEBUG 2x5 1.27",
       "Connector_PinHeader_1.27mm:PinHeader_2x05_P1.27mm_Vertical_SMD",
       {"1": "3V3", "2": "GND", "3": "RESETn", "4": "VCOM_RX", "5": "VCOM_TX", "6": "SWV",
        "7": "SWDIO", "8": "SWCLK", "9": None, "10": None}, "C5199898", "FTSH-105-01-L-DV",
       note="Mini Simplicity 兼容调试口")
for i, n in enumerate(["SWCLK", "SWDIO", "RESETn", "VCOM_TX", "VCOM_RX", "SWV"]):
    TP(sm, "TP%d" % (4 + i), n)
P.part(sm, "#FLG07", "power:PWR_FLAG", "PWR_FLAG", "", {"1": "DVDD"}, bom=False)
for i in range(9, 16):
    TP(sm, "TP%d" % (i + 1), "TP_PD%d" % i)

# ================================================================== sheet 3: RF
sr = P.sheet("射频", "rf.kicad_sch", "射频：433 MHz 匹配/低通/π 型微调/天线选择，2.4 GHz 匹配 + PCB 天线")
sr.text("433 MHz：TX/RX 直连（Direct-Tie）匹配，无射频开关。拓扑和取值由项目方提供，依据 EFR32ZG28 数据手册 5.2.3（Fig 5.7 / Table 5.5）\n"
        "   与 AN923.2 的 433 MHz +14 dBm 匹配网络，生成工程时无法下载原文核对，打样前请对照原文确认一遍。\n"
        "   映射：官方 L1=L101（馈电，接 PAVDD 同一电源 3V3）、L2=L102、C1=C101、C2=C102、L3=L103、C3=C105（原 L106 位）。\n"
        "   +14 dBm 匹配：门锁限 10 mW e.r.p.，传导功率按天线实测设定，不要按 +20 dBm 满功率发射（满功率需另一套匹配）。\n"
        "   单级 LC，谐波余量少，送检前用频谱仪看二、三次谐波。C106/C107 为天线端 π 型微调位，默认不贴。\n"
        "R101/R102 二选一：默认贴 R101 接弹簧天线 ANT1；改用 IPEX（J8）时贴 R102、拆 R101。\n"
        "2.4 GHz 部分（L201/L202/C201/C202）取值仍待照抄 BRD4401C。\n"
        "布线：50 Ω 共面波导（JLC04161H-7628 参考 0.30 mm 线宽 / 0.20 mm 间距，用嘉立创计算器复核），第 2 层完整地，地过孔间距 1.5-2 mm；\n"
        "   ANT1 周围 5 mm 内所有层禁铺铜；天线在板上沿，远离电池仓和电机 >=15 mm。", 15, 225)


def RFP(sh, ref, sym, val, a, b, lcsc, mpn, note):
    fp = FP_L0402 if sym == "Device:L" else FP_C0402
    return P.part(sh, ref, sym, val, fp, {"1": a, "2": b}, lcsc, mpn, note=note)


RFP(sr, "L101", "Device:L", "10nH", "SUBG_O1", "3V3", "C77103", "LQG15HS10NJ02D", "官方 L1：TX 馈电/匹配，接 PAVDD 电源（3V3）")
C(sr, "C108", "100nF", "3V3", note="L101 电源端就近去耦")
RFP(sr, "C109", "Device:C", "100pF", "3V3", "GND", "C77177", "GRM1555C1H101JA01D", "L101 电源端就近去耦（高频）")
RFP(sr, "L102", "Device:L", "33nH", "SUBG_O1", "RF_SUBG", "C86062", "LQG15HS33NJ02D", "官方 L2：TX 串联匹配")
RFP(sr, "C101", "Device:C", "15pF", "RF_SUBG", "GND", "C76950", "GRM1555C1H150JA01D", "官方 C1：公共节点对地，C0G ±5%")
RFP(sr, "C102", "Device:C", "5.6pF", "SUBG_I1", "RF_SUBG", "C85941", "GRM1555C1H5R6CA01D", "官方 C2：RX 串联，C0G ±0.25 pF")
RFP(sr, "L103", "Device:L", "82nH", "SUBG_I1", "GND", "C98002", "LQG15HS82NJ02D", "官方 L3：RX 对地")
C_PI = "π 型微调位，默认不贴"
P.part(sr, "C106", "Device:C", "DNP", FP_C0402, {"1": "RF_SUBG", "2": "GND"}, "", "", dnp=True, note=C_PI,
       newrow=True)
RFP(sr, "C105", "Device:C", "220pF", "RF_SUBG", "RF_ANT", "C71693", "GRM1555C1H221JA01D",
    "官方 C3：隔直/耦合，占原 L106 位（π 型串联位）")
P.part(sr, "C107", "Device:C", "DNP", FP_C0402, {"1": "RF_ANT", "2": "GND"}, "", "", dnp=True, note=C_PI)
R(sr, "R101", "0", "RF_ANT", "ANT_SPRING", note="选弹簧天线（默认）")
R(sr, "R102", "0", "RF_ANT", "ANT_IPEX", dnp=True, note="选 IPEX（与 R101 二选一）")
P.part(sr, "ANT1", "Device:Antenna", "433MHz spring", "smartlock:Spring_Antenna_433_THT",
       {"1": "ANT_SPRING"}, "", "433 MHz 螺旋弹簧天线（手焊）", note="弹簧天线焊盘，板上沿")
P.part(sr, "J8", "Connector:Conn_Coaxial", "IPEX", "Connector_Coaxial:U.FL_Hirose_U.FL-R-SMT-1_Vertical",
       {"1": "ANT_IPEX", "2": "GND"}, "C496552", "BWIPX-1-001E", dnp=True, note="IPEX/U.FL 座（预留）")
TBD(sr, "L201", "L", "RF2G4_O0", "RF24_N", note="2.4 GHz 匹配，整块照抄 BRD4401C")
TBD(sr, "L202", "L", "RF2G4_I0", "RF24_N", note="2.4 GHz 匹配，整块照抄 BRD4401C")
TBD(sr, "C201", "C", "RF24_N", "GND", note="2.4 GHz 匹配，整块照抄 BRD4401C")
TBD(sr, "C202", "C", "RF24_N", "RF24_ANT", note="2.4 GHz 匹配，整块照抄 BRD4401C")
P.part(sr, "C203", "Device:C", "DNP", FP_C0402, {"1": "RF24_ANT", "2": "GND"}, "", "", dnp=True,
       note="天线微调位，默认不贴")
P.part(sr, "ANT2", "Device:Antenna_Shield", "2.4GHz PCB IFA", "RF_Antenna:Texas_SWRA117D_2.4GHz_Left",
       {"1": "RF24_ANT", "2": "GND"}, "", "", bom=False,
       note="PCB 倒 F 天线（铜皮，不是器件）；形状建议照抄 BRD4401C")

# ================================================================== sheet 4: motor
so = P.sheet("电机", "motor.kicad_sch", "电机：DRV8837 + 0.22 Ω 低边电流采样", paper="A4")
so.text("VM 直接接 VBAT_P；低边 0.22 Ω 采样（MOT_GND），经 1k/10nF 到 PB4。\n"
        "大电流回路（J7、U3、R30、C41）远离射频和晶振，电机地单点回到电池负极。\n"
        "U3 散热焊盘 EP 与 GND 脚同接 MOT_GND。", 15, 150)
P.part(so, "U3", "Driver_Motor:DRV8837", "DRV8837DSGR", "Package_SON:WSON-8-1EP_2x2mm_P0.5mm_EP0.9x1.6mm",
       {"1": "VBAT_P", "2": "MOT_OUT1", "3": "MOT_OUT2", "4": "MOT_GND", "5": "MOT_IN2", "6": "MOT_IN1",
        "7": "MOT_NSLEEP", "8": "3V3", "9": "MOT_GND"}, "C39159", "DRV8837DSGR", note="离合电机 H 桥")
P.part(so, "R30", "Device:R", "0.22R 1%", "Resistor_SMD:R_0603_1608Metric", {"1": "MOT_GND", "2": "GND"},
       "C20160538", "PE0603FRF070R22L", note="电机电流采样（BOARD_MOT_RSENSE_MOHM=220）")
R(so, "R31", "1k", "MOT_GND", "MOT_ISENSE")
C(so, "C40", "10nF", "MOT_ISENSE")
C(so, "C41", "10uF/25V", "VBAT_P", note="VM")
C(so, "C42", "100nF", "VBAT_P", note="VM")
C(so, "C43", "100nF", "3V3", note="VCC")
R(so, "R32", "100k", "MOT_NSLEEP", "GND", note="上电默认休眠")
P.part(so, "J7", "Connector_Generic_MountingPin:Conn_01x02_MountingPin", "MOTOR",
       "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal",
       {"1": "MOT_OUT1", "2": "MOT_OUT2", "MP": None}, "C295747", "S2B-PH-SM4-TB(LF)(SN)")
P.part(so, "#FLG08", "power:PWR_FLAG", "PWR_FLAG", "", {"1": "MOT_GND"}, bom=False)

# ================================================================== sheet 5: IO
si = P.sheet("传感器与接口", "io.kicad_sch", "传感器与接口：门磁、开关、蜂鸣器、RGB 灯、前面板 FPC、指纹/人脸接口")
si.text("开关输入均低有效：芯片内部上拉 + 数字滤波，板上每路 100 nF 到地 + ESD。\n"
        "SW4（设置键）必须装在门内侧电池盒里；SW2（防撬）在面板被拆时动作；SW1/SW3 的安装位置按锁体结构定，也可改为线接。\n"
        "SW5 为备用唤醒（SPARE_WAKE，同时引到 FPC 第 15 脚），默认不贴。\n"
        "J6/R60/D60 为人脸版器件；人脸模组 UART 若为 5 V，FACE_RX 经 R60 串阻 + D60 钳位到 3V3，FACE_TX 3.3 V 直连（按模组手册核实）。", 15, 235)
P.part(si, "U5", "smartlock:DRV5032FBDBZR", "DRV5032FBDBZR", "Package_TO_SOT_SMD:SOT-23",
       {"1": "3V3", "2": "DOOR_HALL", "3": "GND"}, "C2655033", "DRV5032FBDBZR", custom=drv5032,
       note="门磁霍尔，低 = 门关")
C(si, "C50", "100nF", "3V3")
SWS = [("SW1", "DBL_LOCK", "反锁旋钮"), ("SW2", "TAMPER", "防撬"), ("SW3", "MECH_KEY", "机械钥匙"),
       ("SW4", "SETUP_KEY", "设置/复位键（门内侧）"), ("SW5", "SPARE_WAKE", "备用")]
for i, (ref, net, what) in enumerate(SWS):
    P.part(si, ref, "Switch:SW_Push", "PTS810", "Button_Switch_SMD:SW_SPST_PTS810", {"1": net, "2": "GND"},
           "C116501", "PTS810SJM250SMTRLFS", dnp=(ref == "SW5"), note=what, newrow=(i == 0))
    C(si, "C%d" % (51 + i), "100nF", net)
    P.part(si, "D%d" % (51 + i), "Diode:ESD5Zxx", "ESD5Z5.0T1G", "Diode_SMD:D_SOD-523", {"1": net, "2": "GND"},
           "C82044", "ESD5Z5.0T1G")
P.part(si, "BZ1", "Device:Buzzer", "Buzzer", "Buzzer_Beeper:Buzzer_CUI_CPT-9019S-SMT",
       {"1": "3V3", "2": "BUZ_N"}, "C95163", "CPT-9019S-SMT-TR", note="无源蜂鸣器 2-3.2 kHz", newrow=True)
P.part(si, "Q4", "Transistor_BJT:MMBT3904", "MMBT3904", "Package_TO_SOT_SMD:SOT-23",
       {"1": "BUZ_B", "2": "GND", "3": "BUZ_N"}, "C20526", "MMBT3904")
R(si, "R50", "1k", "BUZZER", "BUZ_B")
P.part(si, "D56", "Device:D", "1N4148W", "Diode_SMD:D_SOD-123", {"1": "3V3", "2": "BUZ_N"},
       "C81598", "1N4148W", note="蜂鸣器反向续流")
for ref, rref, net, mpn, lc, col in [("D57", "R52", "LED_R", "KT-0603R", "C2286", "红"),
                                     ("D58", "R53", "LED_G", "19-217/GHC-YR1S2/3T", "C72043", "绿"),
                                     ("D59", "R54", "LED_B", "19-217/BHC-ZL1M2RY/3T", "C72041", "蓝")]:
    R(si, rref, "1k", net, net + "_A")
    P.part(si, ref, "Device:LED", "LED " + col, "LED_SMD:LED_0603_1608Metric", {"1": "GND", "2": net + "_A"},
           lc, mpn, note=col + "灯，高有效")
P.part(si, "Q5", "Transistor_FET:AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23",
       {"1": "KEY_BL_EN", "2": "GND", "3": "KEY_BL"}, "C20917", "AO3400A", note="键盘背光低边", newrow=True)
R(si, "R56", "100k", "KEY_BL_EN", "GND", note="上电默认关断")
R(si, "R57", "4.7k", "KEY_SCL", "3V3")
R(si, "R58", "4.7k", "KEY_SDA", "3V3")
R(si, "R59", "100k", "NFC_NRST", "GND", note="上电默认读卡芯片掉电")
FPC = {"1": "GND", "2": "3V3_NFC", "3": "NFC_SCK", "4": "NFC_MOSI", "5": "NFC_MISO", "6": "NFC_CS",
       "7": "NFC_IRQ", "8": "NFC_NRST", "9": "GND", "10": "3V3", "11": "KEY_SCL", "12": "KEY_SDA",
       "13": "KEY_INT", "14": "KEY_BL", "15": "SPARE_WAKE", "16": "GND", "MP": "GND"}
P.part(si, "J4", "Connector_Generic_MountingPin:Conn_01x16_MountingPin", "FPC 16P 0.5mm",
       "Connector_FFC-FPC:Hirose_FH12-16S-0.5SH_1x16-1MP_P0.50mm_Horizontal", FPC,
       "C596731", "FH12-16S-0.5SH(55)", note="前后板 FPC，脚位按 board-spec 6.10", newrow=True)
P.part(si, "J5", "Connector_Generic_MountingPin:Conn_01x06_MountingPin", "FINGERPRINT",
       "Connector_JST:JST_SH_SM06B-SRSS-TB_1x06-1MP_P1.00mm_Horizontal",
       {"1": "3V3_FP", "2": "GND", "3": "FP_RX", "4": "FP_TX", "5": "FP_TOUCH", "6": "3V3", "MP": None},
       "C160405", "SM06B-SRSS-TB(LF)(SN)",
       note="1 3V3_FP 2 GND 3 模组TX(->PD3) 4 模组RX(<-PD2) 5 TOUCH_OUT 6 TOUCH_VCC；按模组线序调整")
P.part(si, "J6", "Connector_Generic_MountingPin:Conn_01x06_MountingPin", "FACE",
       "Connector_JST:JST_SH_SM06B-SRSS-TB_1x06-1MP_P1.00mm_Horizontal",
       {"1": "5V_FACE", "2": "GND", "3": "FACE_RX_M", "4": "FACE_TX", "5": "PROX_INT", "6": None, "MP": None},
       "C160405", "SM06B-SRSS-TB(LF)(SN)", dnp=True,
       note="1 5V_FACE 2 GND 3 模组TX 4 模组RX 5 PROX 6 备用；人脸版")
R(si, "R60", "1k", "FACE_RX_M", "FACE_RX", dnp=True, note="5 V 模组 TX 串阻")
P.part(si, "D60", "Diode:BAT54S", "BAT54S", "Package_TO_SOT_SMD:SOT-23",
       {"1": "GND", "2": "3V3", "3": "FACE_RX"}, "C47546", "BAT54S,215", dnp=True, note="FACE_RX 钳位")

# ================================================================== write
os.makedirs(os.path.join(HERE, "lib"), exist_ok=True)
for s in P.sheets:
    P.write_sheet(s, os.path.join(HERE, s.filename))
P.write_root(os.path.join(HERE, NAME + ".kicad_sch"), "门锁主板 rev A（smart-lock mainboard）", notes=[
    "门锁后面板主板 rev A。规格：docs/hardware/board-spec.md；固件引脚：firmware/lock/port/efr32/board.h。\n"
    "本工程由 tools/gen_project.py 生成。连线用全局标签，原理图可自行整理排版，改连接请同时改规格和 board.h。\n"
    "前面板小板（FM17550 读卡、触摸键盘控制器）不在本工程内，经 J4 FPC 连接。"])
P.write_symlib(os.path.join(HERE, "lib", "smartlock.kicad_sym"))

# ------------------------------------------------------------------ BOMs
groups = {}
for p in P.parts:
    if p["dnp"] or p["ref"].startswith("#"):
        continue
    if not p["lcsc"] and p["value"] != "TBD":
        continue  # hand-soldered (ANT1 spring antenna), listed in bom_full.csv
    key = (p["value"], p["footprint"], p["lcsc"], p["mpn"])
    groups.setdefault(key, []).append(p["ref"])


def refkey(r):
    import re
    m = re.match(r"([A-Z]+)(\d+)", r)
    return (m.group(1), int(m.group(2))) if m else (r, 0)


rows = sorted(groups.items(), key=lambda kv: refkey(sorted(kv[1], key=refkey)[0]))
os.makedirs(os.path.join(HERE, "production"), exist_ok=True)
with open(os.path.join(HERE, "production", "bom_jlcpcb.csv"), "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
    for (val, fp, lc, mpn), refs in rows:
        passive = refkey(refs[0])[0] in ("R", "C", "L")
        if val == "TBD":
            comment = "TBD-按参考设计取值"
        elif passive:
            comment = val
        else:
            comment = mpn or val
        w.writerow([comment, ",".join(sorted(refs, key=refkey)), fp.split(":", 1)[-1], lc])
with open(os.path.join(HERE, "production", "bom_full.csv"), "w", newline="", encoding="utf-8") as f:
    w = csv.writer(f)
    w.writerow(["Designator", "Qty", "Value", "MPN", "LCSC Part #", "Footprint", "Fitted", "Sheet", "Note"])
    allg = {}
    for p in P.parts:
        if p["ref"].startswith("#"):
            continue
        key = (p["value"], p["footprint"], p["lcsc"], p["mpn"], p["dnp"], p["sheet"].name, p["note"])
        allg.setdefault(key, []).append(p["ref"])
    for (val, fp, lc, mpn, dnp, sh, note), refs in sorted(allg.items(), key=lambda kv: refkey(sorted(kv[1], key=refkey)[0])):
        w.writerow([",".join(sorted(refs, key=refkey)), len(refs), val, mpn, lc, fp, "DNP" if dnp else "yes", sh, note])

# parts list for make_pcb.py
with open(os.path.join(HERE, "tools", "parts.json"), "w", encoding="utf-8") as f:
    json.dump([dict(ref=p["ref"], value=p["value"], footprint=p["footprint"], lcsc=p["lcsc"], mpn=p["mpn"],
                    dnp=p["dnp"], path=p["path"], sheet=p["sheet"].filename, sheetname=p["sheet"].name,
                    pins=p["pins"])
               for s in P.sheets for p in s.parts if p["footprint"]], f, ensure_ascii=False, indent=1)
print("parts:", len([p for p in P.parts]), "sheets:", len(P.sheets))
