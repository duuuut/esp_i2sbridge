# i2s_bridge — USB 声卡 ↔ FPGA

WT9932P4-TINY 做成一个 USB 音频设备：**电脑上出现一个扬声器和一个麦克风**，
声音从 USB 流进 FPGA 的 I2S，处理完再流回电脑。不需要 codec。

```
PC ──USB──► output_cb ──► spk_stream ──► I2S TX ──► FPGA i2s_sd_in
PC ◄──USB── input_cb  ◄── mic_stream ◄── I2S RX ◄── FPGA i2s_sd_out
```

## ⚠️ 延迟 132 ms

核是 6347 抽样感受野的 WaveNet，输出比输入晚 **6346 个样本 = 132 ms**。

- 听成品、处理整段音频、对比效果：**可以**。
- 一边弹一边监听：**不行**，慢半拍还多。

这是算法本身的结构性延迟，不是 I2S 或 USB 引入的，改桥接代码改不掉。

## USB：全速（FS）

`sdkconfig.defaults` 里 `CONFIG_TINYUSB_RHPORT_FS=y`。P4 有两个独立的 OTG：

| 模式 | 控制器 | PHY | 引脚 |
|---|---|---|---|
| **FS（当前）** | OTG 1.1 | FSLS 内部 PHY | **GPIO26 = D-，GPIO27 = D+** |
| HS | OTG 2.0 | UTMI | 专用封装脚，不映射 GPIO |

选 FS 是因为连接器接在 FS 那组线上。**GPIO26/27 别再拿去驱动别的东西**
（BSP 的板级初始化把这两个脚当 LCD 背光和复位——如果那块屏在用，USB 会不工作）。
FS 带宽 12 Mbit/s，单声道 48 kHz 只占 96 KB/s，绰绰有余。

## 接线

| P4 | 方向 | FPGA | 说明 |
|---|---|---|---|
| GPIO20 | → | `i2s_sck`（D17） | BCK |
| GPIO21 | → | `i2s_ws`（E17） | LRCK |
| GPIO22 | → | `i2s_sd_in`（F18） | 送进核的音频 |
| GPIO23 | ← | `i2s_sd_out`（F21） | 核的输出 |
| GND | — | GND | 必须共地 |

**GPIO 20–23 是占位的**，按你板上引出的排针改 `main/i2s_bridge.c` 顶部四个
`#define`。板上已占用：0/7/8/26/27/35/39–44/51（摄像头、触摸、SD、背光、按键、
RGB 灯）。

## 格式

| 侧 | 格式 |
|---|---|
| USB | 48 kHz、16 位、**单声道**（描述符里就是这个，核本身也是单声道） |
| I2S | Philips，32 位时隙，音频在**高 24 位**，BCK = 3.072 MHz |
| 转换 | `s16 << 15` 进，`>> 15` 出 |

用 32 位时隙而不是 24 位：FPGA 只读延迟位之后的 24 个 MSB，剩下的填充位无所谓，
这样两边都不用管“24 位怎么塞进 32 位”的约定。

## 构建与烧写

```bash
. ~/.espressif/v6.1/esp-idf/export.sh
idf.py set-target esp32p4
idf.py build flash monitor
```

已用 ESP-IDF v6.1 编译通过，零警告，镜像 268 KB（分区余 74%）。
依赖 `espressif/usb_device_uac`，写在 `main/idf_component.yml`，首次构建自动拉取。

## 用起来

烧进去后 USB 口应该出现一个新的音频设备：

```bash
aplay -l ; arecord -l                 # 确认枚举
aplay   -D plughw:CARD=xxx,DEV=0 input.wav
arecord -D plughw:CARD=xxx,DEV=0 -f S16_LE -r 48000 -c 1 -d 10 out.wav
```

运行中每 5 秒打印一行：

```
uac_i2s: spk starved 0  mic short 0  mic dropped 0  buffered 0/0
```

| 字段 | 含义 |
|---|---|
| `spk starved` | 主机送来的数据填不满 I2S。有反馈端点反压主机，正常应为 0 |
| `mic short` | 给主机的是补零，通常是刚上电那 132 ms |
| `mic dropped` | 缓冲满、主机读得比核生产得慢。**持续不为 0 就是真丢数据** |
| `buffered` | 两个流缓冲各有多少字节，正常在 0 附近晃说明速率匹配 |

## 时钟从哪来

两个振荡器：USB 主机一个，P4 的 APLL 一个，差几 ppm。

- **播放方向**有反馈端点（`CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP`，按 FIFO 水位反压
  主机），主机跟着我们走，不漂。
- **录音方向**没有反馈，主机按自己的节奏读。这就是 `mic dropped` 存在的原因，
  2048 样本（43 ms）的缓冲用来吸收它。

## 已知的坑

- **Windows / Linux 的 UAC 描述符不完全兼容**，组件文档明说了；macOS 要开
  `UAC_SUPPORT_MACOS`。默认面向 Linux/Windows，先在 Linux 上验证。
- **控制台在 USB Serial/JTAG 上**（`sdkconfig.defaults`）。它是第三个 USB 设备，
  和 OTG 不是一回事；如果板上只有一个 Type-C 接到了 OTG，日志要挪到 UART。
- **UAC 是 2.0**（`bcdADC = 0x0200`，组件没有 1.0 选项）。Win10+/Linux 免驱；
  老主机不认才需要自己写 UAC1 描述符。
- **`spk_starved` 一直涨**：主机没在播放，核在吃静音，不是错误。
