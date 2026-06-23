#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include "AudioGeneratorMP3.h"
#include "AudioFileSourceSD.h"
#include "AudioOutputI2S.h"

// ---- WiFi 配置 ----
static const char *WIFI_SSID     = "xxx";
static const char *WIFI_PASSWORD = "xxxx";

// ---- 音量默认值 ----
#define VOLUME_GAIN 0.4          // 0.0 ~ 1.0（越大越响）

// ---- WebServer（监听 80 端口）----
static WebServer g_webServer(80);

// 音量设置：0.0 ~ 1.0（越大越响）
// 声明为全局变量，方便网页动态修改
static float g_volumeGain = VOLUME_GAIN;

// ---- 音频相关全局变量（提前声明，供 Web 处理函数使用）----
class AudioOutputI2S;       // forward declaration
extern AudioGeneratorMP3  *mp3;
extern AudioFileSourceSD *file;
extern AudioOutputI2S    *out;

// ---- 文件上传相关全局变量
static File g_uploadFile;
static String g_uploadFileName;
static bool   g_uploadOK   = false;
static size_t g_uploadSize = 0;
static String g_uploadMsg;  // 上传结果消息

// 构造并返回主页 HTML
// 可选参数 message / curGain 用于回显提交结果
// curGainPercent: 百分比数值 (0 ~ 100)
// HTML 转义（避免文件名含特殊字符破坏页面）
static String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '"': out += "&quot;"; break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      default:   out += c;        break;
    }
  }
  return out;
}

// 扫描 SD 卡根目录，返回用于网页展示的 HTML 片段（每行一个文件 + 播放按钮）
static String buildFileListHTML() {
  String section;
  section += "  <div class=\"card\" style=\"margin-top:1.5rem;\">\n"
             "    <h2 style=\"margin-top:0;color:#333;\">SD 卡文件</h2>\n";

  File root = SD.open("/");
  if (!root) {
    section += "    <div class=\"msg\" style=\"background:#fde2e2;color:#a33;\">";
    section += "无法打开 SD 卡根目录";
    section += "</div>\n  </div>\n";
    return section;
  }
  if (!root.isDirectory()) {
    section += "    <div class=\"msg\" style=\"background:#fde2e2;color:#a33;\">";
    section += "根目录不是文件夹";
    section += "</div>\n  </div>\n";
    root.close();
    return section;
  }

  int count = 0;
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      ++count;
      String name = entry.name();
      String safeName = htmlEscape(name);
      // 路径以 / 开头传给播放逻辑（AudioFileSourceSD 通常要求完整路径）
      String path = name.startsWith("/") ? name : "/" + name;
      section += "    <div style=\"display:flex;align-items:center;";
      section += "justify-content:space-between;padding:.45rem 0;";
      section += "border-bottom:1px solid #f0f0f0;\">\n";
      section += "      <span style=\"font-family:monospace;font-size:.95rem;";
      section += "word-break:break-all;color:#333;\">" + safeName + "</span>\n";
      section += "      <div style=\"display:flex;gap:.5rem;\">\n";
      section += "        <a href='/play?f=" + safeName + "' "
                 "style='text-decoration:none;padding:.45rem 1rem;"
                 "background:#2d7ef7;color:#fff;border-radius:6px;"
                 "font-size:.9rem;'>播放</a>\n";
      section += "        <a href='/delete?f=" + safeName + "' "
                 "onclick=\"return confirm('确定删除 \\u201c" + safeName +
                 "\” ？');\" "
                 "style='text-decoration:none;padding:.45rem 1rem;"
                 "background:#dc3545;color:#fff;border-radius:6px;"
                 "font-size:.9rem;'>删除</a>\n";
      section += "      </div>\n";
      section += "    </div>\n";
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();

  if (count == 0) {
    section += "    <div class=\"msg\">根目录下没有文件</div>\n";
  }
  section += "  </div>\n";
  return section;
}

static String buildHomePage(const String &message, float curGainPercent) {
  char gainBuf[16];
  dtostrf(curGainPercent, 1, 1, gainBuf);

  String html = "<!DOCTYPE html>\n"
                "<html lang=\"zh-CN\">\n"
                "<head>\n"
                "  <meta charset=\"UTF-8\">\n"
                "  <meta name=\"viewport\" "
                "content=\"width=device-width, initial-scale=1\">\n"
                "  <title>ESP32-C3 播放器</title>\n"
                "  <style>\n"
                "    body { font-family: sans-serif; background: #f5f5f5; "
                "padding: 2rem; max-width: 520px; margin: 0 auto; }\n"
                "    h1 { color: #333; }\n"
                "    .card { background: #fff; border-radius: 12px; "
                "padding: 1.5rem 2rem; box-shadow: 0 2px 8px rgba(0,0,0,.08); }\n"
                "    label { display: block; margin: 1rem 0 .4rem; color: #555; }\n"
                "    input[type=number], input[type=file] { width: 100%; "
                "padding: .6rem .8rem; font-size: 1rem; border: 1px solid #ddd; "
                "border-radius: 6px; box-sizing: border-box; }\n"
                "    .btn { margin-top: 1rem; padding: .7rem 1.2rem; "
                "font-size: 1rem; color: #fff; border: 0; border-radius: 6px; "
                "cursor: pointer; text-decoration: none; display: inline-block; }\n"
                "    .btn-blue  { background: #2d7ef7; }\n"
                "    .btn-blue:hover  { background: #1e63cc; }\n"
                "    .btn-green { background: #28a745; }\n"
                "    .btn-green:hover { background: #1e7e34; }\n"
                "    .btn-red   { background: #dc3545; }\n"
                "    .btn-red:hover   { background: #a71d2a; }\n"
                "    .btn-row { display: flex; gap: .75rem; flex-wrap: wrap; "
                "margin-top: 1rem; }\n"
                "    .hint { color: #888; font-size: .9rem; margin-top: .4rem; }\n"
                "    .msg  { margin-top: 1rem; padding: .6rem .8rem; "
                "border-radius: 6px; background: #e8f4ff; color: #1a5a9a; }\n"
                "  </style>\n"
                "</head>\n"
                "<body>\n"
                "  <div class=\"card\">\n"
                "    <form method=\"POST\" action=\"/volume\">\n"
                "      <label for=\"gain\">音量 (0 ~ 100 %)</label>\n"
                "      <input type=\"number\" id=\"gain\" name=\"gain\" "
                "step=\"1\" min=\"0\" max=\"100\" value=\"" +
                String(gainBuf) + "\">\n"
                "      <div class=\"hint\">推荐范围 10 ~ 80；过大可能导致失真。</div>\n"
                "      <button type=\"submit\" class=\"btn btn-blue\">设置音量</button>\n"
                "    </form>\n"
                "    <div class=\"btn-row\">\n"
                "      <a href=\"/pause\" class=\"btn btn-green\">暂停 / 恢复</a>\n"
                "      <a href=\"/stop\" class=\"btn btn-red\">停止</a>\n"
                "    </div>\n";
  if (message.length() > 0) {
    html += "    <div class=\"msg\">" + message + "</div>\n";
  }
  html += "  </div>\n";

  // ---- 文件上传卡片 ----
  html += "  <div class=\"card\" style=\"margin-top:1.5rem;\">\n"
           "    <h2 style=\"margin-top:0;color:#333;\">上传文件到 SD 卡</h2>\n"
           "    <form method=\"POST\" action=\"/upload\" "
           "enctype=\"multipart/form-data\">\n"
           "      <label for=\"file\">选择文件 (仅保存到 SD 卡根目录)</label>\n"
           "      <input type=\"file\" id=\"file\" name=\"file\">\n"
           "      <div class=\"hint\">推荐上传 MP3/WAV 等音频文件；\n"
           "注意 ESP32 内存有限，单文件不要过大。</div>\n"
           "      <button type=\"submit\" class=\"btn btn-blue\">上传</button>\n"
           "    </form>\n"
           "  </div>\n";

  html += buildFileListHTML();
  html += "</body>\n"
          "</html>";
  return html;
}

// 暂停标志：true 时 loop 里不调用 mp3->loop()，从而暂停播放
static bool g_paused = false;
// 静音填充标志：进入暂停状态后只向 I2S 写一次静音样本填满 DMA 缓冲区
static bool g_muted = false;

// 处理根路径 GET
// 停止当前播放并释放 file 资源（mp3 本身可复用）
// 停止当前播放并释放 file 资源（mp3 本身可复用）
// 优化：检测当前状态，避免重复执行；统一清理 g_paused 标志
static void stopPlayback() {
  const bool running = (mp3 != nullptr) && mp3->isRunning();
  const bool fileOpen = (file != nullptr);

  // 短路径：没有在播放、也没有打开的文件句柄 → 直接返回
  if (!running && !fileOpen && !g_paused) {
    return;
  }

  if (running) {
    mp3->stop();
    delete mp3;
    mp3 = nullptr;
    Serial.println(F("[stopPlayback] decoder stopped"));
  }
  if (fileOpen) {
    file->close();
    delete file;
    file = nullptr;
    Serial.println(F("[stopPlayback] file handle released"));
  }

  // 统一复位暂停标志，避免后续新建播放时被遗留的 g_paused 卡住
  if (g_paused) {
    g_paused = false;
  }
}

// 处理 /play?f=文件名
static void handlePlay() {
  String msg;
  String fileName = g_webServer.arg("f");
  if (fileName.length() == 0) {
    g_webServer.send(400, "text/plain", "缺少参数 f");
    return;
  }

  // 禁止路径穿越
  if (fileName.indexOf('/') >= 0 || fileName.indexOf('\\') >= 0
      || fileName.indexOf("..") >= 0) {
    g_webServer.send(400, "text/plain", "非法文件名");
    return;
  }

  if (out == nullptr) {
    g_webServer.send(500, "text/plain",
                     "I2S 输出未就绪，请检查硬件接线 / setup 流程");
    return;
  }

  // 路径统一以 / 开头
  String path = fileName.startsWith("/") ? fileName : "/" + fileName;

  if (!SD.exists(path)) {
    msg = "文件不存在：" + path;
    g_webServer.send(200, "text/html; charset=utf-8",
                     buildHomePage(msg, g_volumeGain * 100.0f));
    return;
  }

  // 停止当前正在播放的文件，释放旧资源
  stopPlayback();

  // 创建/重置 mp3 解码器
  if (mp3 == nullptr) {
    mp3 = new AudioGeneratorMP3();
  } else {
    mp3->stop();
  }

  file = new AudioFileSourceSD(path.c_str());
  if (!file) {
    msg = "无法打开文件：" + path;
    g_webServer.send(200, "text/html; charset=utf-8",
                     buildHomePage(msg, g_volumeGain * 100.0f));
    return;
  }

  // 确保音量使用最新设定
  out->SetGain(g_volumeGain);

  if (!mp3->begin(file, out)) {
    msg = "开始播放失败：" + path + "（格式 / 库？）";
    Serial.printf("[Web] mp3->begin() failed for %s\n", path.c_str());
    g_webServer.send(200, "text/html; charset=utf-8",
                     buildHomePage(msg, g_volumeGain * 100.0f));
    return;
  }

  Serial.printf("[Web] Now playing: %s (gain=%.3f)\n",
                path.c_str(), g_volumeGain);
  msg = "正在播放：" + path;
  g_webServer.send(200, "text/html; charset=utf-8",
                   buildHomePage(msg, g_volumeGain * 100.0f));
}

static void handleStop() {
  stopPlayback();
  g_paused = false;
  Serial.println("[Web] Playback stopped by user");
  g_webServer.send(200, "text/html; charset=utf-8",
                   buildHomePage("已停止播放", g_volumeGain * 100.0f));
}

// 处理 /delete?f=文件名：从 SD 卡根目录删除指定文件
static void handleDelete() {
  String msg;
  String fileName = g_webServer.arg("f");
  if (fileName.length() == 0) {
    g_webServer.send(200, "text/html; charset=utf-8",
                     buildHomePage("缺少参数 f", g_volumeGain * 100.0f));
    return;
  }

  // 禁止路径穿越
  if (fileName.indexOf('/') >= 0 || fileName.indexOf('\\') >= 0
      || fileName.indexOf("..") >= 0) {
    g_webServer.send(200, "text/html; charset=utf-8",
                     buildHomePage("非法文件名", g_volumeGain * 100.0f));
    return;
  }

  String path = fileName.startsWith("/") ? fileName : "/" + fileName;

  // 如果当前正在播放该文件，先停止，避免句柄冲突
  if (file != nullptr && g_paused == false && mp3 != nullptr && mp3->isRunning()) {
    // 仅当正在播放的文件与待删除文件同名时停止（简单比较）
    // 这里保守地停止所有播放，保证 SD 写操作安全
    stopPlayback();
  }

  if (!SD.exists(path)) {
    msg = "文件不存在：" + path;
  } else if (SD.remove(path)) {
    msg = "已删除：" + path;
    Serial.printf("[Web] Deleted: %s\n", path.c_str());
  } else {
    msg = "删除失败：" + path;
    Serial.printf("[Web] Failed to delete: %s\n", path.c_str());
  }

  g_webServer.send(200, "text/html; charset=utf-8",
                   buildHomePage(msg, g_volumeGain * 100.0f));
}

static void handlePause() {
  if (mp3 == nullptr || !mp3->isRunning()) {
    g_webServer.send(200, "text/html; charset=utf-8",
                     buildHomePage("当前没有正在播放的文件",
                                   g_volumeGain * 100.0f));
    return;
  }
  g_paused = !g_paused;
  // 切换状态后重置静音填充标志，让下一次暂停可以重新填充；恢复播放时把音量改回
  g_muted = false;
  if (out != nullptr) {
    out->SetGain(g_volumeGain);
  }
  String msg = g_paused ? "已暂停" : "已恢复播放";
  Serial.printf("[Web] Playback %s\n", g_paused ? "paused" : "resumed");
  g_webServer.send(200, "text/html; charset=utf-8",
                   buildHomePage(msg, g_volumeGain * 100.0f));
}

static void handleRoot() {
  String msg = g_uploadMsg;
  g_uploadMsg = "";  // 读取后清空，避免下次刷新不重复显示
  g_webServer.send(200, "text/html; charset=utf-8",
                   buildHomePage(msg, g_volumeGain * 100.0f));
}

// 文件上传回调：每次接收到一部分文件数据时被 WebServer 调用
// 由 multipart/form-data 上传流程：
//   1) 每遇到一个表单字段，status=FILE_START → 打开文件
//   2) 每收到一段文件内容，status=FILE_WRITE → 写入文件
//   3) 文件结束，status=FILE_END → 关闭文件
static void handleFileUpload() {
  HTTPUpload &upload = g_webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    g_uploadOK   = true;
    g_uploadSize = 0;

    String filename = upload.filename;
    // 禁止路径穿越：只取最后一段文件名
    int slashIdx = filename.lastIndexOf('/');
    int bslashIdx = filename.lastIndexOf('\\');
    if (slashIdx  >= 0) filename = filename.substring(slashIdx + 1);
    if (bslashIdx >= 0) filename = filename.substring(bslashIdx + 1);
    if (filename.length() == 0 || filename == "." || filename == "..") {
      g_uploadOK = false;
      return;
    }

    // 如果当前正在播放同一个文件，先停止，避免写入冲突
    stopPlayback();

    String path = "/" + filename;
    g_uploadFileName = path;

    // 关闭旧文件
    if (g_uploadFile) {
      g_uploadFile.close();
    }
    g_uploadFile = SD.open(path, FILE_WRITE);
    if (!g_uploadFile) {
      Serial.printf("[Upload] Failed to open %s for writing\n", path.c_str());
      g_uploadOK = false;
    } else {
      Serial.printf("[Upload] Start receiving: %s\n", path.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (g_uploadOK && g_uploadFile) {
      size_t written = g_uploadFile.write(upload.buf, upload.currentSize);
      g_uploadSize += written;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (g_uploadFile) {
      g_uploadFile.close();
    }
    char buf[128];
    if (g_uploadOK) {
      snprintf(buf, sizeof(buf),
               "上传成功：%s ( %u 字节 )",
               g_uploadFileName.c_str(), (unsigned)g_uploadSize);
      Serial.printf("[Upload] Finished: %s (%u bytes)\n",
                     g_uploadFileName.c_str(), (unsigned)g_uploadSize);
    } else {
      snprintf(buf, sizeof(buf), "上传失败：无法写入 SD 卡");
      Serial.println("[Upload] Upload failed");
    }
    g_uploadMsg = buf;
  }
}

// /upload 处理函数（表单提交完成后返回网页
static void handleUpload() {
  // 如果上传过程中 g_uploadOK 为 false，认为失败
  String msg = g_uploadMsg.length() > 0
              ? g_uploadMsg
              : String("上传完成");
  g_uploadMsg = "";
  g_webServer.send(200, "text/html; charset=utf-8",
                   buildHomePage(msg, g_volumeGain * 100.0f));
}

// 处理 /volume POST：解析参数并设置播放音量
static void handleSetVolume() {
  float curPercent = g_volumeGain * 100.0f;
  String msg;

  if (!g_webServer.hasArg("gain")) {
    msg = "缺少参数 gain";
  } else {
    String gainStr = g_webServer.arg("gain");
    char *end = nullptr;
    float percent = strtof(gainStr.c_str(), &end);
    if (end == gainStr.c_str() || percent < 0.0f || percent > 100.0f) {
      msg = "输入无效：" + gainStr + "（必须是 0 ~ 100 的数字）";
    } else {
      g_volumeGain = percent / 100.0f;
      if (out != nullptr) {
        out->SetGain(g_volumeGain);
      }
      char buf[64];
      snprintf(buf, sizeof(buf),
               "音量已设置为 %.1f %% (gain = %.3f)",
               percent, g_volumeGain);
      msg = buf;
      curPercent = percent;
      Serial.printf("[Web] Volume set to: %.1f%% (gain=%.3f)\n",
                    percent, g_volumeGain);
    }
  }

  g_webServer.send(200, "text/html; charset=utf-8",
                   buildHomePage(msg, curPercent));
}

static void handleNotFound() {
  g_webServer.send(404, "text/plain", "404: Not Found");
}

// 连接 WiFi，成功后打印 IP 信息
static void connectWiFi() {
  Serial.printf("\nConnecting to WiFi: %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // 最多等待 30 秒（每 500ms 打印一个点）
  uint32_t timeout_ms = 30000;
  uint32_t start_ms   = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - start_ms > timeout_ms) {
      Serial.println("\nWiFi connect timeout. Check SSID/password.");
      return;
    }
  }

  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("  SSID:     "); Serial.println(WiFi.SSID());
  Serial.print("  RSSI:     "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  Serial.print("  IP:       "); Serial.println(WiFi.localIP());
  Serial.print("  Gateway:  "); Serial.println(WiFi.gatewayIP());
  Serial.print("  Subnet:   "); Serial.println(WiFi.subnetMask());
  Serial.print("  DNS:      "); Serial.println(WiFi.dnsIP());
  Serial.print("  MAC:      "); Serial.println(WiFi.macAddress());

  // WiFi 连接成功后启动 WebServer
  g_webServer.on("/", HTTP_GET, handleRoot);
  g_webServer.on("/volume", HTTP_POST, handleSetVolume);
  g_webServer.on("/play", HTTP_GET, handlePlay);
  g_webServer.on("/stop", HTTP_GET, handleStop);
  g_webServer.on("/pause", HTTP_GET, handlePause);
  g_webServer.on("/delete", HTTP_GET, handleDelete);
  // 文件上传：POST /upload（使用 multipart/form-data）
  g_webServer.on("/upload", HTTP_POST, handleUpload, handleFileUpload);
  g_webServer.onNotFound(handleNotFound);
  g_webServer.begin();
  Serial.printf("\nWebServer started. Open http://%s/ in your browser.\n",
                WiFi.localIP().toString().c_str());
}

// 打印重启原因（帮助定位崩溃源）
static void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  switch (reason) {
    case ESP_RST_UNKNOWN:    Serial.println("UNKNOWN"); break;
    case ESP_RST_POWERON:    Serial.println("POWERON"); break;
    case ESP_RST_EXT:        Serial.println("EXT (EN pin)"); break;
    case ESP_RST_SW:         Serial.println("SW reset"); break;
    case ESP_RST_PANIC:      Serial.println("PANIC (异常/断言)"); break;
    case ESP_RST_INT_WDT:    Serial.println("INT WDT"); break;
    case ESP_RST_TASK_WDT:   Serial.println("TASK WDT"); break;
    case ESP_RST_WDT:        Serial.println("WDT"); break;
    case ESP_RST_DEEPSLEEP:  Serial.println("DEEPSLEEP"); break;
    case ESP_RST_BROWNOUT:   Serial.println("BROWNOUT (电压过低)"); break;
    case ESP_RST_SDIO:       Serial.println("SDIO"); break;
    default:                 Serial.println(reason); break;
  }
}

#define SD_CS    7
#define SD_MOSI  6
#define SD_MISO  4
#define SD_SCK   5

// MAX98357 I2S 引脚（自定义）
// ⚠️ 注意：
//   - GPIO0 是 Strapping 引脚，上电时若被拉低会进下载模式
//   - GPIO3 在很多 ESP32-C3 板上是 USB_D-，接 I2S 会与 USB 冲突
// 如果仍然不断重启，请把 I2S_DOUT 改到 GPIO10/GPIO20/GPIO21 这类普通 GPIO
#define I2S_LRC   21   // LRC / WS
#define I2S_BCLK  20   // BCLK
#define I2S_DOUT  10   // DIN

// 改为更安全的引脚（可选，按你板子实际引出的引脚选）：
// #define I2S_LRC   10
// #define I2S_BCLK  20
// #define I2S_DOUT  21

SPIClass *sdspi = nullptr;

AudioGeneratorMP3  *mp3 = nullptr;
AudioFileSourceSD *file = nullptr;
AudioOutputI2S    *out = nullptr;

void listDir(fs::FS &fs, const char * dirname, uint8_t levels);

void setup() {
  Serial.begin(115200);
  delay(500);

  printResetReason();

  // ---- 连接 WiFi ----
  connectWiFi();

  Serial.println();
  Serial.println("\nInitializing SD card...");

  // 初始化 SPI 总线（SCK, MISO, MOSI；CS 由 SD 库管理）
  sdspi = new SPIClass(SPI);
  sdspi->begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  // 挂载 SD 卡（这一步是关键，之前缺失）
  if (!SD.begin(SD_CS, *sdspi, 4000000)) {
    Serial.println("Card Mount Failed");
    Serial.println("Please check:");
    Serial.println("  - Wiring (CS/MOSI/MISO/SCK)");
    Serial.println("  - SD card formatted as FAT32");
    return;
  }

  Serial.println("SD card initialized successfully.");

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }

  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC)        Serial.println("MMC");
  else if (cardType == CARD_SD)    Serial.println("SDSC");
  else if (cardType == CARD_SDHC)  Serial.println("SDHC");
  else                             Serial.println("UNKNOWN");

  Serial.printf("Total space: %llu MB\n", SD.totalBytes() / (1024 * 1024));
  Serial.printf("Used space:  %llu MB\n", SD.usedBytes() / (1024 * 1024));
  Serial.println();

  // ---- 初始化 I2S（MAX98357）----
  // 用最兼容的两参数构造函数: (port, output_mode)
  //   port=0         → I2S0
  //   output_mode=1  → 外部 I2S 编解码芯片（MAX98357 属于此类）
  // 然后通过 SetPinout 把 BCLK/LRC/DOUT 映射到自定义引脚。
  out = new AudioOutputI2S(0, 1);
  if (!out) {
    Serial.println("ERROR: AudioOutputI2S allocation failed");
    return;
  }
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(g_volumeGain);
  Serial.printf("Volume gain set to: %.2f\n", g_volumeGain);
  Serial.printf("I2S configured: BCLK=%d, LRC=%d, DOUT=%d\n",
                I2S_BCLK, I2S_LRC, I2S_DOUT);
  Serial.flush();
  delay(200);  // 给 I2S 驱动一点时间稳定

  // 上电不再自动播放固定文件；通过网页点击播放按钮触发
  Serial.println("\nSystem ready. Open http://" + WiFi.localIP().toString() +
                 "/ in your browser to select a file.");
}

void loop() {
  static bool listed = false;
  if (!listed) {
    listDir(SD, "/", 1);
    listed = true;
    Serial.println("\n--- Done listing, starting playback ---\n");
  }

  // 处理 HTTP 请求（必须在 loop 中持续调用）
  if (WiFi.status() == WL_CONNECTED) {
    g_webServer.handleClient();
  }

  if (mp3 && mp3->isRunning()) {
    if (g_paused) {
      // 暂停状态：不驱动解码；向 I2S DMA 缓冲区写入静音样本 (0,0) 填满缓冲
      // DMA 循环播放时就会播放静音而不是最后一段非零音频
      if (!g_muted && out != nullptr) {
        int16_t silent[2] = {0, 0};
        for (int i = 0; i < 2048; ++i) {
          out->ConsumeSample(silent);
        }
        g_muted = true;
      }
    } else {
      // 恢复播放：清除静音填充标志，让下次暂停可以重新填充
      if (g_muted) {
        g_muted = false;
      }
      if (!mp3->loop()) {
        mp3->stop();
        if (file) { file->close(); delete file; file = nullptr; }
        g_paused = false;
        Serial.println("\n>>> Playback finished.");
      }
    }
  } else {
    // 不在播放：重置静音填充标志
    if (g_muted) {
      g_muted = false;
    }
    delay(10);
  }
}

void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
  Serial.printf("Listing directory: %s\n", dirname);
  
  File root = fs.open(dirname);
  if(!root){
    Serial.println("Failed to open directory");
    return;
  }
  if(!root.isDirectory()){
    Serial.println("Not a directory");
    return;
  }
  
  File file = root.openNextFile();
  while(file){
    if(file.isDirectory()){
      Serial.print("  [DIR]  ");
      Serial.println(file.name());
      if(levels > 0){
        listDir(fs, file.path(), levels - 1);
      }
    } else {
      Serial.print("  [FILE] ");
      Serial.print(file.name());
      Serial.print("  |  Size: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}