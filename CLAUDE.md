# StickS3 桌面系统 — 开发备忘

M5Stack StickS3（ESP32-S3-PICO-1-N8R8，8MB Flash + 8MB PSRAM，1.14" LCD 135x240）上的多应用桌面系统。用 PlatformIO + Arduino 框架开发。

## 工程结构

```
G:\M5Stack StickS3\
├── platformio.ini                    # 板子和库依赖 + OTA 配置
├── partitions_8MB_huge.csv           # 双 OTA 分区（app0/app1 各 3.5MB + spiffs 0.95MB）
├── .claude/settings.json             # Claude Code 钩子（Pre/Post/Stop/UserPrompt → hook_notify.py）
├── src/
│   ├── app.h                         # 共享常量 + 函数原型
│   ├── main.cpp                      # setup()/loop()、draw_title/status、Claude 标志、屏保、stick_log
│   ├── menu.cpp                      # 主菜单（当前 7 项，顺序见下）
│   ├── wifi_mgr.cpp                  # WiFiManager 配网 + NTP + 自动重连事件处理
│   ├── app_clock.cpp                 # 桌面：NTP 时钟 + 杭州天气 + B 站粉丝(UID 8466490)
│   ├── app_ir.cpp                    # 红外学习/回放（4 个槽位，NVS 存储）
│   ├── app_sound.cpp                 # 麦克风：dB 计 + FFT 频谱
│   ├── app_radio.cpp                 # 网络电台（25 台，ESP32-audioI2S）
│   ├── app_voicekb.cpp               # Claude 语音助手（讯飞 STT + PC 助手 + 活动日志）
│   ├── app_ota.cpp                   # OTA 升级模式（停音频/关麦克风/ArduinoOTA）
│   └── app_settings.cpp              # 音量/亮度/自动旋转/屏幕超时，NVS 持久化
├── helper/
│   ├── type_server.py                # Windows 托盘助手（粘贴 + /status + /log + UDP 发现）
│   ├── hook_notify.py                # Claude Code 钩子脚本 → POST /status_event
│   ├── stick_log.txt                 # 板子和助手的事件日志（调试用）
│   └── dist/StickHelper.exe          # PyInstaller 打包的托盘 exe（可选）
└── downloads/                        # PlatformIO 工具链离线包（可忽略）
```

## 主菜单（顺序固定）

1. **桌面** — 时钟 / 天气 / B 站粉丝
2. **红外遥控** — 学习 / 回放
3. **声音** — 分贝 / 频谱
4. **网络电台** — 在线收音机
5. **Claude 语音助手** — 语音输入到 PC + 实时显示 Claude 活动
6. **OTA 升级** — 无线烧录固件
7. **设置** — 音量 / 亮度 / 屏幕超时 / 自动旋转（固定最后一项，用户要求）

## 构建和烧录

PlatformIO 通过 `python -m platformio` 调用（没装 `pio` 快捷命令）。**不要设 `HTTPS_PROXY`**，用户 TUN 模式已经全局代理，再设反而变慢。

```bash
# 只编译
python -m platformio run -d "G:/M5Stack StickS3"

# 编译 + 烧录
python -m platformio run -d "G:/M5Stack StickS3" -t upload --upload-port COM6

# 串口监视
python -m platformio device monitor -d "G:/M5Stack StickS3"
```

**COM 端口坑**：板子每次烧录后重启会把 USB 设备重新枚举，接下来几次烧录很可能找不到 COM6。**失败后让用户拔 USB 重插**，再重试。用 `powershell -Command "[System.IO.Ports.SerialPort]::GetPortNames()"` 查当前可用端口。**OTA 是绕开 COM 端口坑的正道**，板子开机已经有固件的情况下优先用 OTA。

## OTA 无线烧录（默认方式）

分区表 `partitions_8MB_huge.csv` 是双槽布局：`ota_0` + `ota_1` 各 3.5MB，升级时写入备用槽、切 `otadata`，下次启动用新固件。首次必须串口烧录一次才能生效。

```bash
# 板子进 "OTA 升级" App（屏幕会显示 IP，如 192.168.31.x）
# 在 PC 上：
python -m platformio run -d "G:/M5Stack StickS3" -t upload --upload-port 192.168.31.x
# 端口改成 IP 会自动走 espota (UDP)，~65 秒跑完，板子自动重启
```

`platformio.ini` 里配置 `upload_protocol = espota` + `upload_port = sticks3.local`（mDNS 关了，直接走 IP 更稳）。OTA 模式会 `M5.Speaker.end() + M5.Mic.end()`，避免 I2S/Flash 抢资源导致 PANIC。曾经踩过的坑：分区表缺 `ota_1` → OTA 启动直接 panic；OTA 回调里调用 `stick_log`（HTTP）→ 升级中途 WiFi 栈卡死。

## 关键依赖版本

- `espressif32@6.12.0`（平台）
- `ESP32-audioI2S@3.0.0`（pinned — 新版用 C++20 `<span>`，GCC 8.4 编译不过）
- `WiFiManager ^2.0.17`，`ArduinoJson ^7.0.4`，`IRremoteESP8266 ^2.8.6`，`arduinoFFT ^2.0.2`
- `WebSockets ^2.4.1`（讯飞 STT 用）

## 已解决的硬件/软件坑

- **屏幕频闪**：M5GFX 默认背光 PWM ~1kHz 有可见频闪。`main.cpp` 里 `ledcSetup(3, 20000, 8) + ledcAttachPin(38, 3)` 强制 20kHz，`apply_brightness()` 直接 `ledcWrite(3, ...)`。
- **全屏刷新闪烁**：改用 `M5Canvas g_canvas`（PSRAM 离屏画布），每帧先画到画布再 `pushSprite(0,0)`。
- **音频库 C++20 依赖**：`schreibfaul1/ESP32-audioI2S` 新版引入 `<span>`，pin 到 `3.0.0` 避开。
- **电台切台后无声**：`Audio.connecttohost` 前强制 `stopSong() + delay(120)` 彻底清缓冲；另外加看门狗 `isRunning()` 检测流中断。
- **语音键盘蓝牙方案 fail**：T-vK/ESP32-BLE-Keyboard + NimBLE 在 S3 上连接不稳；改用 **HTTP POST 到 PC 助手** 方案。
- **讯飞 STT 要 GMT 时间**：签名需要当前时间，WiFi 一连上就 `configTzTime`。
- **PC 助手原先 pyautogui/pyperclip 不稳**：多次迭代后改用 **Win32 剪贴板 API + SendInput Ctrl+V**。记得对所有 API 显式设 `argtypes`（64 位 HANDLE 不能用默认 c_int，会 OverflowError）。

## 语音键盘链路（当前方案）

```
板子麦克风采样 PCM (16kHz 16bit)
  → 讯飞 WebSocket STT（HMAC-SHA256 签名 URL）
  → 识别文字
  → HTTP POST http://<PC IP>:8765/type
  → Python 助手 type_server.py
  → Win32 SetClipboardData (UTF-16LE CF_UNICODETEXT)
  → SendInput Ctrl+V（1 次 4 个按键事件）
  → delay 200ms
  → SendInput VK_RETURN
```

PC 助手是 **Windows 托盘程序**，常驻运行两种启动方式：
- 开发期：`python "G:/M5Stack StickS3/helper/type_server.py"`
- 打包后：双击 `helper/dist/StickHelper.exe`（PyInstaller 打的单文件 exe）

托盘右键菜单可打开配置对话框（tkinter），里面能改端口、开关 UDP 发现、设置开机自启（通过 Startup 文件夹放快捷方式）、编辑纠错表。配置存 `helper/config.json`。

**PC IP 不再硬编码在固件里**。板子通过 UDP 广播做 **自动发现**：
- 板子发 `STICK?` 到 UDP 端口 8766
- 助手回 `STICK! <ip>:<port>`
- 板子把结果存 NVS（`pc.ip` / `pc.port`），下次开机先试缓存再广播
所以 PC IP 变了通常不用重烧，最多进语音助手 App 触发一次重发现。

常见误识别替换表在 `type_server.py` 的 `CORRECTIONS` 里（如 `克拉克 → Claude`，`cloud code → Claude Code`），托盘里改完保存即可生效，不用重烧板子。

## Claude 活动实时显示

板子的"Claude 语音助手" App 左栏滚动显示 Claude Code 正在做什么。链路：

```
Claude Code (钩子 PreToolUse / PostToolUse / Stop / UserPromptSubmit)
  → hook_notify.py 读 stdin JSON
  → POST http://127.0.0.1:8765/status_event  (加 marker \x01U / \x01C 区分用户/回复)
  → type_server.py 维护环形缓冲（每条带单调 seq）
  → 板子 GET /status 轮询，diff seq 拉新事件
  → 动画队列按 500ms 节奏揭示，带 UTF-8 安全截断 + 2 行换行
```

钩子配置在 `.claude/settings.json`。`hook_notify.py` 有几个关键点：
- 用 `sys.stdin.buffer.read().decode("utf-8", errors="replace")` 读 JSON，**绝对不要**用默认文本模式（Windows 下是 GBK，JSON 中文会解码崩）
- `ProxyHandler({})` 强制 localhost 请求不走代理（用户开着 TUN 代理）
- Stop 钩子读 `transcript_path` 的最后一条 assistant 消息，即使解析失败也要发一条空 `\x01C`，否则板子右边的"运行中"标签不会回到"空闲"

板子上不同类型事件用颜色区分：用户输入青色、Claude 回复琥珀色、工具调用橙/灰色。`\x01C`（Claude 回复）揭示时会发两声短促提示音。

## 按键约定（所有 App）

- **BtnA**：主动作（菜单切换 / 录音 / 电台下一台 / 等）
- **BtnB 短按**：确认 / 进入 / 播放停止
- **BtnB 长按 ~0.9 秒**：返回上一层（菜单里叫"返回主菜单"）
- 语音键盘特殊：短按 B 在 2 秒冷静期后是"测试发送"（发 `hello from StickS3`）
- 屏保：默认 60 秒无操作自动熄屏，按任意键唤醒；OTA App 常开屏不熄灭

## 硬件特性（无法软件修复）

- **充电时喇叭吱啦声**：充电 IC 开关噪声耦合到功放电源，换电源头或用电池能改善
- **录音时喇叭电流声**：ES8311 codec ADC 和 DAC 共用参考电压，ADC 活动会通过内部耦合漏到 DAC。语音键盘 App 已经 `M5.Speaker.end()` 试过，改善有限
- **BLE A2DP 不支持**：ESP32-S3 只有 BLE，没蓝牙经典，**连不了蓝牙音箱**

## 内存系统参考

重要上下文在 `C:\Users\zhebin\.claude\projects\G--M5Stack-StickS3\memory\`：
- `user_profile.md` — 用户画像（不写代码，要直接执行；在国内网络）
- `project_state.md` — 工程状态快照
- `device_and_env.md` — COM 端口、PDF 路径、工具环境
- `xfyun_credentials.md` — 讯飞 API key
- `pc_helper.md` — PC 助手地址 192.168.31.124:8765
