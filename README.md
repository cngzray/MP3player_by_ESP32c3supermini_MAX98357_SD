使用ESP8266Audio库实现

(!!!因为esp32c3不带PSRAM，不能使用esp32-audioI2S库)

### 一、硬件平台

- **主控芯片**: ESP32-C3（RISC-V 架构）
- **音频解码芯片**: MAX98357（I2S 接口，单声道 D 类功放）
- **存储设备**: Micro SD 卡（SPI 接口，FAT32 文件系统）
- **引脚分配**:
  - SD 卡: CS=GPIO7, MOSI=GPIO6, MISO=GPIO4, SCK=GPIO5
  - I2S: BCLK=GPIO20, LRC=GPIO21, DOUT=GPIO10

<img width="875" height="512" alt="image" src="https://github.com/user-attachments/assets/3b9ffacb-92cb-43c6-816b-b52f68c5cd7d" />

### 二、核心功能

| 功能 | 说明 |
|------|------|
| 🎵 MP3 播放 | 从 SD 卡读取 MP3 文件，通过 I2S 解码播放 |
| 🌐 Web 控制 | 通过浏览器访问 Web 界面进行远程控制 |
| 📁 文件管理 | 网页端可查看、播放、删除 SD 卡中的文件 |
| ⬆️ 大文件上传 | 支持通过网页上传 10MB 级文件到 SD 卡 |
| 🔊 音量控制 | 网页端实时调整播放增益（0~100%） |
| ⏯️ 播放控制 | 播放 / 暂停 / 停止 |
| 📊 实时进度 | 网页端实时显示播放进度条（mm:ss） |
| 📈 上传进度 | 网页端实时显示上传进度条 |

### 三、Web 界面路由表
<img width="391" height="258" alt="image" src="https://github.com/user-attachments/assets/f0f40780-6233-4199-a4b3-689f57a5b87d" />

### 四、关键技术设计

#### 1. **任务隔离与竞态消除** (核心亮点)
- **问题**: AsyncWebServer 在独立任务中运行 HTTP 回调，与 `loop()` 并发执行。之前在 Web 回调中直接操作音频解码器会导致竞态条件和 WDT 超时。
- **解决方案**: 采用"Web 任务只设置标志位，loop 实际执行"的设计模式
- **优先级**: `STOP > PLAY > PAUSE > 正常播放`
- **关键变量**:
  - `g_cmdStop` / `g_cmdPause` — volatile bool 标志位
  - `g_cmdPlayPath` — 待播放文件路径
  - `g_playBusy` — loop 繁忙标记，Web 任务通过 `waitLoopIdle()` 等待

#### 2. **MP3 时长解析**
- 不依赖第三方库，直接解析 MP3 帧头获取比特率和采样率
- 扫描前 64KB 寻找有效帧，对 5 个有效帧取平均
- 通过 `(文件大小 × 8) / 平均比特率` 估算总时长
- 支持 MPEG1 / MPEG2 / MPEG2.5 的 Layer III
- 帧对齐校验（下一帧同步字验证），排除假帧头

#### 3. **大文件上传支持**
- AsyncWebServer multipart/form-data 流式上传
- 每 4KB 分块写入，每次 `yield()` 防止 AsyncTCP WDT 超时
- 每 1MB flush 到 SD 卡，每 256KB 打印进度
- 3 次重试打开文件，给 SD 卡缓冲时间
- **关键**: `handleUploadRequest` 中**不调用 `request->send()`**，响应在 `onFile` 的 `final=true` 阶段发送

#### 4. **实时进度显示**
- Web 端通过 `/api/status` 每 500ms 轮询
- 返回当前文件名、百分比进度、当前时间/总时长 (mm:ss)
- 暂停状态通过 `g_paused` 标志位

#### 5. **暂停机制**
- 暂停时持续向 I2S 写入静音样本，保持 I2S 时钟运行
- 避免切换时的爆音
- `g_paused` 仅在 loop 中读写

### 五、安全性设计

- **路径穿越防护: `handlePlay` / `handleDelete` / `handleUploadFile` 中检查文件名包含 `..` / `/` / `\`
- **HTML 转义**: `htmlEscape()` 防止文件名含特殊字符破坏页面
- **上传失败清理**: 上传失败时删除不完整文件
- **SD 句柄管理**: 删除/上传前统一停止播放，释放 SD 句柄
- **文件名规范化**: 只取文件名部分，忽略路径前缀

### 六、稳定性设计

- **看门狗防护**: 所有耗时操作都插入 `yield()` 避免 Task WDT
- **SD 卡遍历 guard**: 文件遍历使用 guard 上限防止死循环
- **初始化重试**: SD.open 失败自动重试 3 次
- **失败上传重试**: write 返回 0 字节时自动重试 5 次
- **停止超时兜底**: `stopPlayback()` 等待 loop 清理 2 秒后强制清理
- **启动信息**: 打印重启原因，帮助定位崩溃源

### 七、依赖库

| 库 | 用途 |
|------|------|
| `ESPAsyncWebServer` | 异步 Web 服务器 |
| `AsyncTCP` | TCP 底层（ESP32-C3 专用） |
| `ESP8266Audio` | MP3 解码 + I2S 输出 |
| `SD` (SPI) | SD 卡文件系统 |
| `WiFi` | WiFi 连接 |
| `SPI` | SPI 总线 |

### 八、使用方法

1. 修改 `sketch.ino` 顶部的 WiFi 配置:
```cpp
static const char *WIFI_SSID     = "你的WiFi名称";
static const char *WIFI_PASSWORD = "你的WiFi密码";
```

2. 编译并上传到 ESP32-C3

3. 打开串口监视器，获取 IP 地址

4. 浏览器访问 `http://<IP地址>/

5. 选择文件播放、上传、删除或调整音量

<img width="427" height="945" alt="image" src="https://github.com/user-attachments/assets/88ba540e-cc93-4607-b86d-e9da106457b4" />

