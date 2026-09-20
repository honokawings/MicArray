#include "hardwareprofile.h"

#include <array>

namespace MicArray::Array128 {
namespace {
// README 2026-09-20: FPGA package pads for DATA0..63, not connector pin numbers.
constexpr std::array<const char *, 64> DataPins {{
    "AD25", "AC22", "AC23", "AD26", "AD24", "AE20", "AF17", "AF18",
    "AD20", "AC17", "AF20", "AE26", "AB20", "W15", "AC16", "AD17",
    "AD23", "AE23", "AC19", "AF23", "AD21", "AD19", "AF25", "AE21",
    "AF19", "AE18", "AA20", "AE25", "AE17", "W14", "AB16", "AF24",
    "U16", "T18", "V14", "U19", "T17", "W18", "U14", "V18",
    "T15", "Y20", "W26", "W19", "Y23", "W23", "U20", "Y26",
    "V21", "V24", "W24", "V16", "U21", "V19", "T14", "U15",
    "V17", "V23", "T20", "W20", "T19", "W25", "Y22", "V26"
}};
}

QString dataConnector(int channel)
{
    if (channel < 0 || channel >= 128) return {};
    return channel < 64 ? QStringLiteral("J26") : QStringLiteral("J25");
}

QString dataBank(int channel)
{
    if (channel < 0 || channel >= 128) return {};
    return channel < 64 ? QStringLiteral("L7") : QStringLiteral("L6");
}

QString dataPin(int channel)
{
    if (channel < 0 || channel >= 128) return {};
    return QString::fromLatin1(DataPins[size_t(channel / 2)]);
}

QJsonObject hardwareReference()
{
    return {{"source", "260902_MEMS_Array/README.md"}, {"review_date", "2026-09-20"},
        {"reference_only", true}, {"detected_firmware", QJsonValue(QJsonValue::Null)},
        {"recommended_build", "MEMS_Array_uart_clk8_18v_spread3_long"},
        {"microphone", "IM72D128V01"}, {"clock_outputs", 8}, {"pdm_clock_hz", 3072000},
        {"clock_load_mapping", "external wiring; not derivable from channel index"},
        {"mems_io_voltage_v", 1.8}, {"passband_low_hz", 20}, {"passband_high_hz", 16000},
        {"dc_retained", true}, {"udp_payload_bytes", 1208}, {"packet_rate_hz", 16000},
        {"udp_payload_mbps", 154.624}, {"wire_budget_mbps", 163.072},
        {"uart_reference", "SYS3, COM3, 115200 8N1, no flow control"},
        {"uart_supported_by_host", true}};
}

QVector<QStringList> hardwareSpecificationRows()
{
    return {
        {QStringLiteral("板卡 / FPGA"), "MINI-200H / PG2L200H", QStringLiteral("封装 FBB676，速度等级 -6")},
        {QStringLiteral("参考构建"), "MEMS_Array_uart_clk8_18v_spread3_long", QStringLiteral("README 推荐版本；不是当前位流识别结果")},
        {QStringLiteral("麦克风"), "IM72D128V01", QStringLiteral("64 路共享 DATA，128 通道")},
        {QStringLiteral("PDM 时钟"), "3.072 MHz / CLK0–7", QStringLiteral("8 路同源输出；负载分组由接线决定")},
        {QStringLiteral("PCM 输出"), "48 kHz / 24 bit / s24le", QStringLiteral("有符号 Q1.23；通道 CH0–127")},
        {QStringLiteral("有效通带"), "20 Hz–16 kHz", QStringLiteral("DC 保留；16–24 kHz 不作为保证定位带宽")},
        {QStringLiteral("FPGA 前处理"), "CIC5 /8 → FIR63 /4 → FIR127 /2", QStringLiteral("总抽取 64；随后执行增益/偏置校准")},
        {QStringLiteral("默认校准"), QStringLiteral("增益 1 / 偏置 0"), QStringLiteral("板级 ROM 配置，不是主机监听增益")},
        {QStringLiteral("公共群延迟"), "2285.5 PDM cycles / 743.978 μs", QStringLiteral("所有通道一致；故障 bit2=1 时停用时间轴")},
        {QStringLiteral("M128 数据报"), "v1 / 1208 B / 3 frames", QStringLiteral("32 B 大端头；每记录 392 B，PCM 小端")},
        {QStringLiteral("标称包率"), "16,000 packet/s", QStringLiteral("48,000 采样帧/s；到达间隔不是采样时钟")},
        {QStringLiteral("UDP 负载"), "19.328 MB/s / 154.624 Mbit/s", QStringLiteral("不含 Ethernet/IP/UDP 与线路开销")},
        {QStringLiteral("线路预算"), "163.072 Mbit/s", QStringLiteral("含帧头、FCS、前导码和 IFG；非 PC 实测速率")},
        {QStringLiteral("网络"), "192.168.10.2 → 192.168.10.1/24", QStringLiteral("UDP 5000→5000；1000M 全双工，ARP 就绪后采集")},
        {QStringLiteral("MEMS 接口"), "J10 CLK / J25 DATA32–63 / J26 DATA0–31", QStringLiteral("J10/J25 属 L6，J26 属 L7；LVCMOS18")},
        {QStringLiteral("MEMS 电压"), "1.8 V", QStringLiteral("板卡跳线和实测确认；主机不能设定/测量 VCCIO")},
        {QStringLiteral("UART 诊断"), "SYS3 / COM3 / 115200 8N1", QStringLiteral("无流控；UART 3.3 V；在设备诊断页按需连接")},
        {QStringLiteral("视频输入"), QStringLiteral("本机 USB 摄像头"), QStringLiteral("独立采集，无 FPGA 音视频硬件同步")}
    };
}

QString commissioningGuide()
{
    return QStringLiteral(
        "参考：README 2026-09-20，八路 CLK / 1.8 V 完整系统。\n"
        "这些是文档规格，不是对当前下载位流或电气状态的自动检测。\n\n"
        "1  供电与接线\n"
        "断电后按板卡丝印设置 J24：VADJ=1.8 V，J20：L6/L7 由 VADJ 供电。\n"
        "实测 VCCIO 与 MEMS VDD 均为 1.8 V；A3V3/A5V0 仍为固定电压。\n"
        "J10 输出 CLK0–7；J26 为 DATA0–31（L7），J25 为 DATA32–63（L6）。\n"
        "CLK 同源不等于焊盘零偏斜；实际负载分组按阵列接线记录，不按 CH/32 推定。\n\n"
        "2  串口诊断（设备诊断 → SYS3 串口）\n"
        "COM3，115200 8N1，无流控；UART 电平 3.3 V。先识别 SYS3，不能把 CLK2 当作完整系统。\n"
        "s / h：请求状态；r：完整系统复位并回到实采；t：切换内部 PDM 自测；\n"
        "c：仅清除时钟历史故障，不清当前采集故障或 PHY/MAC 计数。\n"
        "命令不需要换行；t 间隔至少 0.2 s。回执只表示已识别，完成以新鲜 SYS3 状态为准。\n"
        "检查 PLL/复位→PHY/MDIO→ARP→UDP；字段 freshness=0 时不得当作当前状态。\n"
        "KEY0/KEY1 仅兼容保留，联调以 UART 为主。PHY 扫描无 ACK 不一定是故障。\n\n"
        "连接时不自动发命令；复位/模式切换在录制中禁用。复位前主机暂停 UDP，恢复后手动重新接收。\n"
        "命令不自动重试；状态超时或域过期时不能当作当前健康数据。\n\n"
        "3  网络接收\n"
        "本机网卡 192.168.10.1/24，设备 192.168.10.2，源/目的 UDP 5000。\n"
        "校准、1000M 全双工链路及 ARP 全部就绪后自动采集；无 TCP、ping、DHCP 或 UDP 控制。\n"
        "目标 16,000 包/s，UDP 负载 19.328 MB/s；含线路开销预算为 163.072 Mbit/s。\n"
        "主机停止接收不停止 FPGA；UdpSent / SelftestTxPass 不能证明电脑已收到。\n\n"
        "4  数字自测\n"
        "通过 UART 确认模式，再按需 t 切换；接收端应看到 flags.bit4=1 和新的 epoch。\n"
        "至少 48,000 帧、全部 128 通道匹配官方参考表且无错误/丢弃/不连续才通过。\n"
        "标称参考 pattern_id=1；改变激励/滤波系数须同步更新参考。UDP 不报告位流身份。\n"
        "自测只验证数字链路，不证明 MEMS 接线、声学 SNR 或板卡电压正常。\n\n"
        "5  实采与记录\n"
        "接入 MEMS、退出自测，先检查直流、交流电平、削顶和相邻重复值。\n"
        "导入真实 128 阵元坐标后再定位、波束处理和视频校准；不猜测螺旋几何。\n"
        "保存诊断报告、UART 日志和录制清单，另记位流校验值及实际电压。\n"
        "硬复位可能重用 epoch，重新启动主机接收；不要合并复位前后的同号会话。\n"
        "相对时间 t=(64×frame_id+63−2285.5)/3072000 s，仅 bit2=0 时有效；非 UTC。"
    );
}

} // namespace MicArray::Array128
