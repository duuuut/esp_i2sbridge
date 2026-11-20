# wt_p4_i2s_bridge

WT9932P4-TINY 当作 I2S 主机，给 NAM3 板子当信号源和录音机。**不需要 codec**。

```
ESP32-P4  ──BCK/WS/SD──►  FPGA i2s_sck / i2s_ws / i2s_sd_in
          ◄──SD──────────  FPGA i2s_sd_out
```

FPGA 是从机（`nam_i2s_if.v` 把 BCK/WS 当慢速输入采样），所以时钟由这边产生。
一个控制器跑**全双工**：同一组 BCK/WS 同时驱动收发，两个方向没有时钟漂移。

## 为什么不用 wt_bsp

BSP 管的是 RGB 灯、按键、摄像头、USB CDC/UVC，**没有 I2S**。I2S 用的是
ESP-IDF 自带的 `i2s_std` 驱动，加 BSP 只是多一层依赖。以后想要个状态灯再引。

## 接线

| P4 | 方向 | FPGA | 说明 |
|---|---|---|---|
| GPIO20 | → | `i2s_sck`（D17） | BCK |
| GPIO21 | → | `i2s_ws`（E17） | LRCK |
| GPIO22 | → | `i2s_sd_in`（F18） | 送进去的正弦 |
| GPIO23 | ← | `i2s_sd_out`（F21） | 模型输出 |
| GND | — | GND | 必须共地 |

**GPIO 20–23 是占位的**，按你手上的 TINY 排针改 `i2s_bridge.c` 顶部那四个
`#define`。板子上已被占用的有 0/7/8/26/27/35/39–44/51（摄像头、触摸、SD、
背光、按键、RGB 灯），避开这些即可。

## 格式

| 项 | 值 |
|---|---|
| 标准 | Philips I2S，MSB 先出，WS 跳变后 1 个 BCK 的延迟 |
| 帧 | 32 位时隙 × 2 声道，BCK = 48 kHz × 64 = **3.072 MHz** |
| 有效位 | 24 位，放在 32 位时隙的**高 24 位**（低 8 位是填充） |
| 声道 | FPGA 只取左声道；发送时两边都送同一个样本 |
| 电平 | 3.3 V LVCMOS |

36 倍过采样：FPGA 核心 100 MHz / BCK 3.072 MHz = 32.5，要求是 ≥ 4。

## 构建

```bash
. $IDF_PATH/export.sh          # 需要 ESP-IDF v5.x 或 v6.x
cd mcu/wt_p4_i2s_bridge
idf.py set-target esp32p4
idf.py build flash monitor
```

## 输出

每秒一行：

```
i2s_bridge: sent 47936  got 47936  rx peak 3158291 (37.6% FS)  rms 2231000
```

怎么看：

| 现象 | 含义 |
|---|---|
| `got` 和 `sent` 同步增长 | I2S 链路和 FPGA 都在跑 |
| `rx peak` 在几十 % FS，随正弦有起伏 | 核在正常处理 |
| `rx peak` 恒为 0 | 核没有输出：权重没加载 / KEY2 被按住 / I2S 方向接反 |
| `rx peak` 顶在 8388607（100% FS） | 核在削顶，`head_scale` 太大或输入过载 |
| `got` 为 0、`sent` 在涨 | 只发了没收到：`i2s_sd_out` 没接好 |

板上电即用：bitstream 里已经带了一份编译时的模型（`wrom.hex`），不需要先传权重。
要换模型再用 `scripts/upload_weights.py` 从 PC 传。

## 两个已知的坑

- **这份代码没有编译过**：写它的机器上没装 ESP-IDF。API 是按 IDF 5.x/6.x 的
  `i2s_std` 写的；如果全双工那两行报错，说明你的 IDF 版本要求 RX 通道单独
  配置，把第二个 `i2s_channel_init_std_mode` 换成它建议的形式即可。
- **正弦是实时算的**（`sin()` 每样本一次，96k 次/秒）。P4 有 FPU，这个开销可以
  忽略；如果要更省，换成 1024 点查表。
