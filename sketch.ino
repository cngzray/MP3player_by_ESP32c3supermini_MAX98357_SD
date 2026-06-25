#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "AudioGeneratorMP3.h"
#include "AudioFileSourceSD.h"
#include "AudioOutputI2S.h"

// ---- WiFi 配置 ----
static const char *WIFI_SSID     = "xxxx";
static const char *WIFI_PASSWORD = "xxxxxx";

// ---- 音量默认值 ----
#define VOLUME_GAIN 0.1          // 0.0 ~ 1.0（越大越响）

// ---- AsyncWebServer（监听 80 端口，非阻塞，适合大文件上传）----
static AsyncWebServer g_webServer(80);

// ---- 文件上传进度（多任务访问，用 volatile 保证可见性）----
static volatile bool   g_uploading  = false;
static volatile size_t g_uploaded   = 0;
static volatile size_t g_uploadTotal = 0; // 来自 Content-Length（0 表示未知）
static volatile bool   g_uploadOK   = true;

// 音量设置：0.0 ~ 1.0（越大越响）
// 声明为全局变量，方便网页动态修改
static float g_volumeGain = VOLUME_GAIN;

// ---- 音频相关全局变量（提前声明，供 Web 处理函数使用）----
class AudioOutputI2S;       // forward declaration
extern AudioGeneratorMP3  *mp3;
extern AudioFileSourceSD *file;
extern AudioOutputI2S    *out;

// ---- 文件上传相关全局变量（AsyncWebServer 调用的回调在另一任务中执行）
static File   g_uploadFile;
static String g_uploadFileName;
static String g_uploadMsg;   // 上传结果消息（供首页展示）
static portMUX_TYPE g_uploadMux = portMUX_INITIALIZER_UNLOCKED;

// 原子地读取/更新上传进度（避免多任务竞态）
static void uploadProgressSet(size_t uploaded, size_t total, bool ok) {
  portENTER_CRITICAL(&g_uploadMux);
  g_uploaded    = uploaded;
  g_uploadTotal = total;
  g_uploadOK    = ok;
  portEXIT_CRITICAL(&g_uploadMux);
}
static void uploadProgressGet(size_t &uploaded, size_t &total, bool &ok, bool &uploading) {
  portENTER_CRITICAL(&g_uploadMux);
  uploaded  = g_uploaded;
  total     = g_uploadTotal;
  ok        = g_uploadOK;
  uploading = g_uploading;
  portEXIT_CRITICAL(&g_uploadMux);
}

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
// ⚠️ 关键修复：ESP32 最新 SD 库 (V2+) 中，当遍历结束或失败时 openNextFile
//   会返回一个"空"File 对象，此时：
//     - `while (entry)`   -> 在某些版本上 File::operator bool 不会返回 false
//     - `entry.name()`    -> 可能返回 NULL / 空指针
//     - `entry.size()`    -> 可能返回未定义值
//   正确方式是用 `entry.available()` 判断是否还有文件，并使用 if (name) 防护。
//   另外在循环中调用 yield() 避免 Task WDT 超时。
static String buildFileListHTML() {
  String section;
  section += "  <div class=\"card\">\n"
             "    <h2>SD 卡文件</h2>\n";

  File root = SD.open("/");
  if (!root) {
    section += "    <div class=\"msg msg-err\">无法打开 SD 卡根目录</div>\n  </div>\n";
    return section;
  }
  if (!root.isDirectory()) {
    section += "    <div class=\"msg msg-err\">根目录不是文件夹</div>\n  </div>\n";
    root.close();
    return section;
  }

  int count = 0;
  int guard = 0;            // 防止 openNextFile 在某些库上意外死循环
  File entry;
  // ⚠️ 不要依赖 root.available()：ESP32 SD 库中目录对象的 available() 常返回 0/false。
  //   直接用 openNextFile + if (!entry) break 作为终止条件，
  //   同时用 guard 上限 20000 做安全兜底。
  for (;;) {
    if (++guard > 20000) {
      Serial.println(F("[buildFileListHTML] guard triggered, possible openNextFile loop"));
      break;
    }
    entry = root.openNextFile();
    if (!entry) break;       // 空 File：遍历正常结束

    const char *name = entry.name();
    // 空指针 / 空字符串 防护：某些库在异常条目上返回 NULL
    if (name == nullptr || name[0] == '\0') {
      entry.close();
      yield();
      continue;
    }

    if (!entry.isDirectory()) {
      ++count;
      String safeName = htmlEscape(String(name));
      unsigned long sz = entry.size();
      String sizeStr;
      if (sz < 1024) sizeStr = String(sz) + " B";
      else if (sz < 1024 * 1024) sizeStr = String(sz / 1024) + " KB";
      else sizeStr = String(sz / (1024 * 1024)) + " MB";

      section += "    <div class=\"file-row\">\n";
      section += "      <span class=\"file-name\">" + safeName +
                 " <span class=\"hint\">" + sizeStr + "</span></span>\n";
      section += "      <div class=\"file-actions\">\n";
      section += "        <button type=\"button\" class=\"btn btn-sm btn-blue js-play\" data-file=\"" + safeName + "\">播放</button>\n";
      section += "        <button type=\"button\" class=\"btn btn-sm btn-red js-del\" data-file=\"" + safeName + "\">删除</button>\n";
      section += "      </div>\n";
      section += "    </div>\n";
    }
    entry.close();
    yield();   // 在 AsyncWebServer 回调内遍历 SD 时让出 CPU，避免 WDT
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

  // ---- 头部 + CSS ----
  String html = "<!DOCTYPE html>\n"
                "<html lang=\"zh-CN\">\n"
                "<head>\n"
                "  <meta charset=\"UTF-8\">\n"
                "  <meta name=\"viewport\" "
                "content=\"width=device-width, initial-scale=1\">\n"
                "  <title>ESP32-C3 播放器</title>\n"
                "  <script src=\"https://cdn.jsdelivr.net/npm/jquery@3.7.1/dist/jquery.min.js\"></script>\n"
                "  <style>\n"
                "    body { font-family: sans-serif; background: #f5f5f5; "
                "padding: 2rem; max-width: 520px; margin: 0 auto; }\n"
                "    h1 { color: #333; font-size: 1.4rem; margin: 0 0 .3rem; }\n"
                "    h2 { color: #333; font-size: 1.1rem; margin: 0 0 .5rem; }\n"
                "    .card { background: #fff; border-radius: 12px; "
                "padding: 1.5rem 2rem; box-shadow: 0 2px 8px rgba(0,0,0,.08); }\n"
                "    label { display: block; margin: 1rem 0 .4rem; color: #555; font-size: .95rem; }\n"
                "    input[type=number], input[type=file] { width: 100%; "
                "padding: .6rem .8rem; font-size: 1rem; border: 1px solid #ddd; "
                "border-radius: 6px; box-sizing: border-box; background: #fafafa; }\n"
                "    .btn { margin-top: 1rem; padding: .55rem 1.1rem; "
                "font-size: .95rem; color: #fff; border: 0; border-radius: 6px; "
                "cursor: pointer; text-decoration: none; display: inline-block; transition: background .2s; }\n"
                "    .btn:disabled { background: #aaa !important; cursor: not-allowed; }\n"
                "    .btn-sm { margin-top: 0; padding: .45rem .9rem; font-size: .85rem; }\n"
                "    .btn-blue  { background: #2d7ef7; }\n"
                "    .btn-blue:hover:not(:disabled) { background: #1e63cc; }\n"
                "    .btn-green { background: #28a745; }\n"
                "    .btn-green:hover:not(:disabled) { background: #1e7e34; }\n"
                "    .btn-red   { background: #dc3545; }\n"
                "    .btn-red:hover:not(:disabled) { background: #a71d2a; }\n"
                "    .btn-row { display: flex; gap: .6rem; flex-wrap: wrap; margin-top: 1rem; }\n"
                "    .hint { color: #888; font-size: .85rem; margin-top: .3rem; }\n"
                "    .msg { margin-top: 1rem; padding: .6rem .8rem; "
                "border-radius: 6px; background: #e8f4ff; color: #1a5a9a; font-size: .9rem; }\n"
                "    .msg-ok  { background:#e6f7ec; color:#1e7e34; }\n"
                "    .msg-err { background:#fde2e2; color:#a33; }\n"
                "    .file-row { display:flex; align-items:center; justify-content:space-between; "
                "padding:.45rem 0; border-bottom:1px solid #f0f0f0; }\n"
                "    .file-name { font-family: monospace; font-size: .9rem; word-break: break-all; color:#333; }\n"
                "    .file-actions { display:flex; gap:.4rem; }\n"
                "    .playing { color:#28a745; font-weight:bold; }\n"
                "  </style>\n"
                "</head>\n"
                "<body>\n";

  // ---- 音量 + 播放控制卡片 ----
  html += "  <div class=\"card\">\n"
          "    <h1>ESP32-C3 播放器</h1>\n"
          "    <label for=\"gain\">音量 (0 ~ 100 %)</label>\n"
          "    <input type=\"number\" id=\"gain\" step=\"1\" min=\"0\" max=\"100\" value=\"" +
          String(gainBuf) + "\">\n"
          "    <div class=\"hint\">推荐范围 10 ~ 80；过大可能导致失真。</div>\n"
          "    <div class=\"btn-row\">\n"
          "      <button id=\"btnVolume\" class=\"btn btn-blue\">设置音量</button>\n"
          "      <button id=\"btnPause\"  class=\"btn btn-green\">暂停 / 恢复</button>\n"
          "      <button id=\"btnStop\"   class=\"btn btn-red\">停止</button>\n"
          "    </div>\n"
          "    <div id=\"status\" class=\"msg\" style=\"display:none;\"></div>\n"
          "  </div>\n";

  // ---- 文件列表容器（由 jQuery 填充/刷新）----
  html += "  <div id=\"fileListWrap\" style=\"margin-top:1.5rem;\">\n"
          "    <div class=\"msg\">正在加载文件列表...</div>\n"
          "  </div>\n";

  // ---- 文件上传卡片 ----
  html += "  <div class=\"card\" style=\"margin-top:1.5rem;\">\n"
          "    <h2>上传文件到 SD 卡</h2>\n"
          "    <label for=\"file\">选择文件</label>\n"
          "    <input type=\"file\" id=\"file\">\n"
          "    <div class=\"hint\">推荐 MP3/WAV；支持 10MB 级别大文件，上传中可实时看到进度。</div>\n"
          "    <div class=\"btn-row\">\n"
          "      <button id=\"upBtn\" class=\"btn btn-blue\">开始上传</button>\n"
          "      <button id=\"upRefresh\" class=\"btn btn-green\">刷新文件列表</button>\n"
          "    </div>\n"
          "    <div id=\"upBarWrap\" style=\"display:none;margin-top:1rem;height:14px;background:#eee;border-radius:8px;overflow:hidden;\">\n"
          "      <div id=\"upBar\" style=\"height:100%;width:0%;background:#2d7ef7;transition:width .2s;\"></div>\n"
          "    </div>\n"
          "    <div id=\"upText\" style=\"font-size:.85rem;color:#555;margin-top:.4rem;\"></div>\n"
          "  </div>\n";

  // ---- jQuery 脚本 ----
  html += "  <script>\n"
           "  (function(){\n"
           "    var $fileList = $('#fileListWrap');\n"
           "    var $status   = $('#status');\n"
           "    var $gain     = $('#gain');\n"
           "    var $upBtn    = $('#upBtn');\n"
           "    var $upBarWrap= $('#upBarWrap');\n"
           "    var $upBar    = $('#upBar');\n"
           "    var $upText   = $('#upText');\n"
           "    var $file     = $('#file');\n"
           "    var currentPlaying = '';\n"

           "    function showStatus(msg, ok){\n"
           "      $status.removeClass('msg-ok msg-err').text(msg).show();\n"
           "      $status.addClass(ok===false ? 'msg-err' : (ok===true ? 'msg-ok' : ''));\n"
           "    }\n"
           "    function fmt(n){\n"
           "      if(n<1024) return n+' B';\n"
           "      if(n<1024*1024) return (n/1024).toFixed(1)+' KB';\n"
           "      return (n/1024/1024).toFixed(2)+' MB';\n"
           "    }\n"
           "    function refreshFiles(){\n"
           "      $.get('/api/files').done(function(html){\n"
           "        $fileList.html(html);\n"
           "        bindFileActions();\n"
           "      }).fail(function(){\n"
           "        $fileList.html('<div class=\"msg msg-err\">文件列表加载失败</div>');\n"
           "      });\n"
           "    }\n"
           "    function bindFileActions(){\n"
           "      $fileList.off('click', '.js-play').on('click', '.js-play', function(ev){\n"
           "        ev.preventDefault();\n"
           "        var fn = $(this).data('file');\n"
           "        var $b = $(this).prop('disabled', true);\n"
           "        $.getJSON('/play', {f: fn}).done(function(r){\n"
           "          currentPlaying = fn;\n"
           "          showStatus(r.msg, r.ok);\n"
           "          refreshFiles();\n"
           "        }).fail(function(x){\n"
           "          var m = x.responseJSON && x.responseJSON.msg ? x.responseJSON.msg : '播放失败';\n"
           "          showStatus(m, false);\n"
           "        }).always(function(){ $b.prop('disabled', false); });\n"
           "      });\n"
           "      $fileList.off('click', '.js-del').on('click', '.js-del', function(ev){\n"
           "        ev.preventDefault();\n"
           "        var fn = $(this).data('file');\n"
           "        if(!confirm('确定删除 \"'+fn+'\" ？')) return;\n"
           "        var $b = $(this).prop('disabled', true);\n"
           "        $.getJSON('/delete', {f: fn}).done(function(r){\n"
           "          showStatus(r.msg, r.ok);\n"
           "          refreshFiles();\n"
           "        }).fail(function(x){\n"
           "          var m = x.responseJSON && x.responseJSON.msg ? x.responseJSON.msg : '删除失败';\n"
           "          showStatus(m, false);\n"
           "          $b.prop('disabled', false);\n"
           "        });\n"
           "      });\n"
           "    }\n"

           "    $('#btnVolume').on('click', function(){\n"
           "      var v = parseFloat($gain.val());\n"
           "      if(isNaN(v) || v < 0 || v > 100){ showStatus('必须是 0 ~ 100 的数字', false); return; }\n"
           "      var $b=$(this).prop('disabled', true);\n"
           "      $.post('/volume', {gain: v}).done(function(r){\n"
           "        showStatus(r.msg, r.ok);\n"
           "      }).fail(function(x){\n"
           "        var m = x.responseJSON && x.responseJSON.msg ? x.responseJSON.msg : '设置失败';\n"
           "        showStatus(m, false);\n"
           "      }).always(function(){ $b.prop('disabled', false); });\n"
           "    });\n"
           "    $('#btnPause').on('click', function(){\n"
           "      var $b=$(this).prop('disabled', true);\n"
           "      $.getJSON('/pause').done(function(r){ showStatus(r.msg, r.ok); })\n"
           "        .fail(function(x){ var m = x.responseJSON && x.responseJSON.msg ? x.responseJSON.msg : '操作失败'; showStatus(m, false); })\n"
           "        .always(function(){ $b.prop('disabled', false); });\n"
           "    });\n"
           "    $('#btnStop').on('click', function(){\n"
           "      var $b=$(this).prop('disabled', true);\n"
           "      $.getJSON('/stop').done(function(r){ showStatus(r.msg, r.ok); })\n"
           "        .fail(function(x){ var m = x.responseJSON && x.responseJSON.msg ? x.responseJSON.msg : '操作失败'; showStatus(m, false); })\n"
           "        .always(function(){ $b.prop('disabled', false); });\n"
           "    });\n"
           "    $('#upRefresh').on('click', function(){\n"
           "      refreshFiles();\n"
           "      showStatus('文件列表已刷新', true);\n"
           "    });\n"
           "    $upBtn.on('click', function(){\n"
           "      var f = $file[0].files[0];\n"
           "      if(!f){ showStatus('请先选择文件', false); return; }\n"
           "      var fd = new FormData(); fd.append('file', f, f.name);\n"
           "      $upBtn.prop('disabled', true);\n"
           "      $upBarWrap.show(); $upBar.css('width','0%');\n"
           "      $upText.text('准备上传：'+f.name+' ('+fmt(f.size)+')');\n"
           "      $.ajax({\n"
           "        url:'/upload', type:'POST', data:fd, processData:false, contentType:false,\n"
           "        xhr:function(){\n"
           "          var xhr=new window.XMLHttpRequest();\n"
           "          xhr.upload.addEventListener('progress', function(e){\n"
           "            if(e.lengthComputable){\n"
           "              var pct=(e.loaded/e.total*100);\n"
           "              $upBar.css('width', pct.toFixed(1)+'%');\n"
           "              $upText.text('上传中 '+fmt(e.loaded)+' / '+fmt(e.total)+' ('+pct.toFixed(1)+'%)');\n"
           "            }\n"
           "          }, false);\n"
           "          return xhr;\n"
           "        },\n"
           "        success:function(data){\n"
           "          $upBar.css('width','100%');\n"
           "          $upText.text('上传完成：'+data);\n"
           "          showStatus('上传成功：'+f.name, true);\n"
           "          $file.val('');\n"
           "          refreshFiles();\n"
           "        },\n"
           "        error:function(x){\n"
           "          $upText.text('上传失败：HTTP '+x.status);\n"
           "          showStatus('上传失败', false);\n"
           "        },\n"
           "        complete:function(){ $upBtn.prop('disabled', false); }\n"
           "      });\n"
           "    });\n"
           "    // 页面加载完成后刷新文件列表\n"
           "    $(function(){ refreshFiles(); });\n"
           "  })();\n"
           "  </script>\n";

  html += "</body>\n"
          "</html>";
  return html;
}

// ═════════════════════════════════════════════════════════════
// 播放控制方案：原子标志位 + 主任务执行
//
// 问题根源：AsyncWebServer 在独立任务中运行 HTTP 回调，与 Arduino loop()
// 并发执行。之前用 portENTER_CRITICAL 禁用中断来保护 mp3/file，但临界区
// 内不能执行 SD 卡读写、堆分配、I2S 初始化等耗时操作，否则会导致
// Interrupt WDT 超时或外设通信失败 → 立即崩溃重启。
//
// 新方案：
//   g_cmdStop   - Web 任务设置 true，loop 检测到后停止并清理
//   g_cmdPause  - Web 任务设置 true，loop 检测到后切换暂停状态
//   g_cmdPlayPath - 非空时表示要播放的文件路径，由 loop 实际执行播放
//   g_playBusy  - 原子标志，loop 正在操作音频资源时设为 true
//
// 规则：
//   * 所有 mp3/file 的 new/delete/begin/stop/loop 都只在 loop() 中执行
//   * Web 任务只做两件事：1) 等待 g_playBusy=false  2) 设置标志位
//   * g_paused 的读写也只在 loop() 中进行
//
// 这样彻底避免了竞态条件，且所有耗时操作都在正常调度环境中执行。
// ═════════════════════════════════════════════════════════════

static volatile bool  g_cmdStop     = false;  // 请求停止
static volatile bool  g_cmdPause    = false;  // 请求切换暂停
static volatile bool  g_playBusy    = false;  // loop 正在操作音频资源
static String g_cmdPlayPath;                  // 待播放文件路径（空表示无请求）
static bool   g_paused = false;               // 暂停标志（只在 loop 中读写）

// 停止当前播放并释放资源 —— ⚠️ 只允许在 loop() 主任务中调用！
static void stopPlaybackInLoop() {
  const bool hasDecoder = (mp3 != nullptr);
  const bool fileOpen = (file != nullptr);

  if (!hasDecoder && !fileOpen && !g_paused) {
    return;
  }

  if (hasDecoder) {
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

  g_paused = false;
}

// Web 任务请求停止：设置标志位，并等待 loop 实际完成清理（超时兜底）
// ⚠️ 调用此函数后，mp3/file 保证为 nullptr，可以安全地进行 SD 卡操作
static void stopPlayback() {
  g_cmdStop = true;

  // 等待 loop 检测到 g_cmdStop 并完成清理。
  // loop 处理完 g_cmdStop 后会把它设回 false。
  // 用"g_cmdStop 变为 false"作为完成信号最可靠，避免间隙竞态。
  uint32_t start = millis();
  while (g_cmdStop) {
    if (millis() - start > 2000) {
      Serial.println(F("[stopPlayback] WARNING: timeout, forced cleanup"));
      // 兜底：超过 2 秒 loop 未完成清理，直接清理
      if (mp3)  { mp3->stop();  delete mp3;  mp3  = nullptr; }
      if (file) { file->close(); delete file; file = nullptr; }
      g_paused = false;
      g_playBusy = false;
      g_cmdStop = false;
      break;
    }
    delay(1);
  }
}

// ========= Web 任务只设置标志位，实际播放/停止/暂停由 loop() 执行 =========

// 辅助：等待 loop 结束 busy 状态，最多 1 秒
static bool waitLoopIdle(uint32_t timeoutMs = 1000) {
  uint32_t start = millis();
  while (g_playBusy) {
    if (millis() - start > timeoutMs) return false;
    delay(1);
  }
  return true;
}

// 处理 /play?f=文件名（AJAX：返回 JSON）
static void handlePlay(AsyncWebServerRequest *request) {
  String fileName = request->arg("f");
  if (fileName.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"缺少参数 f\"}");
    return;
  }
  if (fileName.indexOf('/') >= 0 || fileName.indexOf('\\') >= 0
      || fileName.indexOf("..") >= 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"非法文件名\"}");
    return;
  }
  if (out == nullptr) {
    request->send(500, "application/json",
                   "{\"ok\":false,\"msg\":\"I2S 未就绪\"}");
    return;
  }

  String path = fileName.startsWith("/") ? fileName : "/" + fileName;

  if (!SD.exists(path)) {
    request->send(404, "application/json",
                   "{\"ok\":false,\"msg\":\"文件不存在\"}");
    return;
  }

  // 等待 loop 空闲，然后同时设置停止+播放请求（loop 会先停止再播放）
  if (!waitLoopIdle()) {
    request->send(500, "application/json",
                   "{\"ok\":false,\"msg\":\"系统繁忙，请稍后再试\"}");
    return;
  }

  g_cmdStop = true;                 // 先请求停止当前播放
  g_cmdPlayPath = path;              // 设置要播放的文件路径
  Serial.printf("[Web] Request play: %s\n", path.c_str());

  request->send(200, "application/json",
                "{\"ok\":true,\"msg\":\"正在播放：" + htmlEscape(path) + "\"}");
}

static void handleStop(AsyncWebServerRequest *request) {
  if (!waitLoopIdle()) {
    request->send(500, "application/json",
                   "{\"ok\":false,\"msg\":\"系统繁忙\"}");
    return;
  }
  g_cmdStop = true;
  Serial.println("[Web] Playback stopped by user");
  request->send(200, "application/json",
               "{\"ok\":true,\"msg\":\"已停止播放\"}");
}

// 处理 /pause（AJAX：返回 JSON）
static void handlePause(AsyncWebServerRequest *request) {
  if (mp3 == nullptr) {
    request->send(200, "application/json",
                   "{\"ok\":false,\"msg\":\"当前没有正在播放的文件\"}");
    return;
  }

  if (!waitLoopIdle()) {
    request->send(500, "application/json",
                   "{\"ok\":false,\"msg\":\"系统繁忙\"}");
    return;
  }
  g_cmdPause = true;
  Serial.printf("[Web] Pause toggle requested\n");
  request->send(200, "application/json",
               "{\"ok\":true,\"msg\":\"切换暂停/恢复\"}");
}

// 处理 /delete?f=文件名（AJAX：返回 JSON）
static void handleDelete(AsyncWebServerRequest *request) {
  String fileName = request->arg("f");
  if (fileName.length() == 0) {
    request->send(400, "application/json",
                   "{\"ok\":false,\"msg\":\"缺少参数 f\"}");
    return;
  }
  if (fileName.indexOf('/') >= 0 || fileName.indexOf('\\') >= 0
      || fileName.indexOf("..") >= 0) {
    request->send(400, "application/json",
                   "{\"ok\":false,\"msg\":\"非法文件名\"}");
    return;
  }

  String path = fileName.startsWith("/") ? fileName : "/" + fileName;

  // 删除前统一停止播放（不论是否在播放），避免 SD 句柄冲突
  stopPlayback();

  if (!SD.exists(path)) {
    request->send(404, "application/json",
                   "{\"ok\":false,\"msg\":\"文件不存在\"}");
    return;
  }
  if (SD.remove(path)) {
    Serial.printf("[Web] Deleted: %s\n", path.c_str());
    request->send(200, "application/json",
                   "{\"ok\":true,\"msg\":\"已删除：" + htmlEscape(path) + "\"}");
  } else {
    Serial.printf("[Web] Failed to delete: %s\n", path.c_str());
    request->send(500, "application/json",
                   "{\"ok\":false,\"msg\":\"删除失败\"}");
  }
}

static void handleRoot(AsyncWebServerRequest *request) {
  String msg = g_uploadMsg;
  g_uploadMsg = "";
  request->send(200, "text/html; charset=utf-8",
              buildHomePage(msg, g_volumeGain * 100.0f));
}

// ---- AsyncWebServer 上传回调（multipart/form-data）----
// 注意：AsyncWebServer 会在独立任务中调用这些回调，不要和主循环共享非线程
// 安全资源（直接操作 SD 的 File 是线程安全的；其它共享状态使用临界区）
static void handleUploadRequest(AsyncWebServerRequest *request) {
  // 上传结束后（body 处理完毕）被调用：返回简单 JSON/文本
  // 由于实际文件写入在 onFile 回调中完成，这里只返回上传结果文本
  char buf[256];
  size_t uploaded, total; bool ok, up;
  uploadProgressGet(uploaded, total, ok, up);
  if (ok) {
    snprintf(buf, sizeof(buf), "OK %u bytes", (unsigned)uploaded);
    request->send(200, "text/plain", buf);
  } else {
    snprintf(buf, sizeof(buf), "FAIL (uploaded %u bytes)", (unsigned)uploaded);
    request->send(500, "text/plain", buf);
  }
}

// 文件上传 onFile：对 multipart 的每个文件字段被调用
// data==NULL 表示该字段结束；len==当前 chunk 大小；index 用于区分同一字段多次调用
static void handleUploadFile(AsyncWebServerRequest *request,
                             const String &filename,
                             size_t index, uint8_t *data, size_t len, bool final) {
  // 第一次（index==0）：打开目标文件
  if (!index) {
    // 禁止路径穿越：只取文件名
    String name = filename;
    int s1 = name.lastIndexOf('/');
    int s2 = name.lastIndexOf('\\');
    if (s1 >= 0) name = name.substring(s1 + 1);
    if (s2 >= 0) name = name.substring(s2 + 1);
    if (name.length() == 0 || name == "." || name == "..") {
      Serial.println("[Upload] invalid filename");
      uploadProgressSet(0, 0, false);
      g_uploadMsg = "非法文件名";
      return;
    }

    // 先清理上次失败上传残留的 File 句柄，避免状态污染
    if (g_uploadFile) {
      g_uploadFile.close();
      g_uploadFile = File();
    }

    stopPlayback(); // 停止播放，释放可能占用的 SD 句柄

    g_uploadFileName = "/" + name;

    // 若同名文件已存在，先删除（防止 FILE_WRITE 在某些实现上出问题）
    if (SD.exists(g_uploadFileName)) {
      if (!SD.remove(g_uploadFileName)) {
        Serial.printf("[Upload] Failed to remove existing %s\n",
                      g_uploadFileName.c_str());
      }
      yield();
    }

    // 尝试打开文件（最多重试 3 次，给 SD 卡缓冲时间）
    int openRetry = 3;
    while (openRetry > 0 && !g_uploadFile) {
      g_uploadFile = SD.open(g_uploadFileName, FILE_WRITE);
      if (!g_uploadFile) {
        --openRetry;
        Serial.printf("[Upload] SD.open(%s) failed (errno=%d), "
                      "retry remaining=%d\n",
                      g_uploadFileName.c_str(), errno, openRetry);
        yield();
        delay(5);
      }
    }
    if (!g_uploadFile) {
      Serial.printf("[Upload] Finally failed to open %s for writing\n",
                    g_uploadFileName.c_str());
      uploadProgressSet(0, 0, false);
      g_uploadMsg = "无法写入 SD 卡";
      return;
    }

    // 从 HTTP 请求获取 Content-Length 作为总大小
    size_t total = 0;
    if (request->hasHeader("Content-Length")) {
      total = request->header("Content-Length").toInt();
      // multipart 头部与边界约 ~300~500 字节，粗略扣掉以获得更合理上限：
      if (total > 512) total -= 512;
    }
    portENTER_CRITICAL(&g_uploadMux);
    g_uploading   = true;
    g_uploaded    = 0;
    g_uploadTotal = total;
    g_uploadOK    = true;
    portEXIT_CRITICAL(&g_uploadMux);

    Serial.printf("[Upload] Start: %s (Content-Length=%u, free ~%u MB)\n",
                  g_uploadFileName.c_str(), (unsigned)total,
                  (unsigned)(SD.totalBytes() - SD.usedBytes()) / (1024 * 1024));
  }

  // 写入 chunk 到 SD（len 可能为 0，也要判断 file 是否打开成功）
  if (g_uploadFile && data != nullptr && len > 0) {
    // 分块写入（每块最多 4KB）+ 每写一块后 yield，防止 AsyncTCP 任务看门狗超时
    size_t totalWritten = 0;
    const size_t BLOCK = 4096;
    while (totalWritten < len) {
      size_t remain = len - totalWritten;
      size_t toWrite = (remain < BLOCK) ? remain : BLOCK;
      size_t w = g_uploadFile.write(data + totalWritten, toWrite);
      if (w == 0) {
        // 写入 0 字节：很可能是 SD 卡忙/超时，重试若干次
        int retry = 5;
        while (retry > 0 && w == 0) {
          yield();
          delay(1);
          w = g_uploadFile.write(data + totalWritten, toWrite);
          --retry;
        }
        if (w == 0) {
          Serial.printf("[Upload] write() returned 0 at %u bytes, "
                        "errno=%d, giving up\n",
                        (unsigned)(g_uploaded + totalWritten), errno);
          break;
        }
      }
      totalWritten += w;
      yield();   // 关键：让 AsyncTCP 任务可以处理网络事件，避免 WDT
    }

    portENTER_CRITICAL(&g_uploadMux);
    g_uploaded += totalWritten;
    if (totalWritten != len) g_uploadOK = false;
    portEXIT_CRITICAL(&g_uploadMux);

    // 每约 1MB 执行一次 flush，把缓存真正写到卡上
    static size_t lastFlush = 0;
    if (g_uploaded - lastFlush > 1024 * 1024) {
      lastFlush = g_uploaded;
      g_uploadFile.flush();
    }

    // 定期打印进度（每 ~256KB 输出一次，避免串口刷屏）
    static size_t lastReported = 0;
    if (g_uploaded - lastReported > 256 * 1024 || final) {
      lastReported = g_uploaded;
      Serial.printf("[Upload] %u / %u bytes (free ~%u MB)\n",
                    (unsigned)g_uploaded, (unsigned)g_uploadTotal,
                    (unsigned)(SD.totalBytes() - SD.usedBytes()) / (1024 * 1024));
    }
  }

  // 最后一块：关闭文件
  if (final) {
    bool ok = true;
    if (g_uploadFile) {
      g_uploadFile.close();
      g_uploadFile = File();   // 清空，避免残留状态影响下次上传
    }
    // size() 必须在 close() 之后读取，否则 SD 库可能返回未更新的 FAT 缓存值
    size_t upSize = SD.exists(g_uploadFileName)
                    ? SD.open(g_uploadFileName, FILE_READ).size()
                    : 0;
    portENTER_CRITICAL(&g_uploadMux);
    ok = g_uploadOK;
    g_uploading = false;
    portEXIT_CRITICAL(&g_uploadMux);

    char msg[256];
    if (ok) {
      snprintf(msg, sizeof(msg), "上传成功：%s (%u 字节)",
               g_uploadFileName.c_str(), (unsigned)upSize);
      Serial.printf("[Upload] Finished: %s (%u bytes)\n",
                    g_uploadFileName.c_str(), (unsigned)upSize);
    } else {
      snprintf(msg, sizeof(msg), "上传失败：已写入 %u 字节", (unsigned)upSize);
      Serial.printf("[Upload] Write failed after %u bytes\n", (unsigned)upSize);
      // 失败时删除不完整文件，避免下次上传受影响
      if (g_uploadFileName.length() > 0 && SD.exists(g_uploadFileName)) {
        SD.remove(g_uploadFileName);
      }
    }
    g_uploadMsg = msg;
  }
}

// /upload/progress：返回 JSON，方便客户端轮询（可选兜底）
static void handleUploadProgress(AsyncWebServerRequest *request) {
  size_t uploaded = 0, total = 0;
  bool ok = true, up = false;
  uploadProgressGet(uploaded, total, ok, up);
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"uploading\":%s,\"ok\":%s,\"uploaded\":%u,\"total\":%u}",
           up ? "true" : "false",
           ok ? "true" : "false",
           (unsigned)uploaded, (unsigned)total);
  request->send(200, "application/json", buf);
}

// 处理 /volume POST（AJAX：返回 JSON）
static void handleSetVolume(AsyncWebServerRequest *request) {
  if (!request->hasArg("gain")) {
    request->send(400, "application/json",
                   "{\"ok\":false,\"msg\":\"缺少参数 gain\"}");
    return;
  }
  String gainStr = request->arg("gain");
  char *end = nullptr;
  float percent = strtof(gainStr.c_str(), &end);
  if (end == gainStr.c_str() || percent < 0.0f || percent > 100.0f) {
    request->send(400, "application/json",
                   "{\"ok\":false,\"msg\":\"输入无效，必须是 0 ~ 100 的数字\"}");
    return;
  }
  g_volumeGain = percent / 100.0f;
  if (out != nullptr) {
    out->SetGain(g_volumeGain);
  }
  char buf[96];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"msg\":\"音量已设置为 %.1f %\",\"gain\":%.3f}",
           percent, g_volumeGain);
  Serial.printf("[Web] Volume set to: %.1f%% (gain=%.3f)\n",
                percent, g_volumeGain);
  request->send(200, "application/json", buf);
}

// /api/files：返回文件列表 HTML 片段（供 jQuery AJAX 局部刷新）
static void handleApiFiles(AsyncWebServerRequest *request) {
  request->send(200, "text/html; charset=utf-8", buildFileListHTML());
}

static void handleNotFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "404: Not Found");
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

  // WiFi 连接成功后启动 AsyncWebServer
  g_webServer.on("/", HTTP_GET, handleRoot);
  g_webServer.on("/volume", HTTP_POST, handleSetVolume);
  g_webServer.on("/play", HTTP_GET, handlePlay);
  g_webServer.on("/stop", HTTP_GET, handleStop);
  g_webServer.on("/pause", HTTP_GET, handlePause);
  g_webServer.on("/delete", HTTP_GET, handleDelete);

  // 上传进度查询（GET /upload/progress，返回 JSON）
  g_webServer.on("/upload/progress", HTTP_GET, handleUploadProgress);
  // 文件列表 HTML 片段（供 jQuery 局部刷新）
  g_webServer.on("/api/files", HTTP_GET, handleApiFiles);

  // 文件上传：POST /upload（multipart/form-data，支持 10MB 级大文件）
  g_webServer.on("/upload", HTTP_POST,
                  handleUploadRequest,
                  [](AsyncWebServerRequest *request, const String &filename,
                     size_t index, uint8_t *data, size_t len, bool final) {
                    handleUploadFile(request, filename, index, data, len, final);
                  });

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
  // 10MHz 对绝大多数 SDHC 卡 + 一般接线都很稳；如继续出现写入失败，可降到 4MHz
  if (!SD.begin(SD_CS, *sdspi, 10000000)) {
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
    Serial.println("\n--- Done listing, ready for playback ---\n");
  }

  // AsyncWebServer 在内部任务中处理 HTTP，loop 中无需调用 handleClient

  // ═════════════════════════════════════════════════════════════
  // 音频操作都只在 loop() 中执行，Web 任务只设置标志位
  // 优先级：STOP > PLAY > PAUSE > 正常播放
  //
  // 由于所有 mp3/file 的 new/delete/loop/begin 都只在这里
  // （同一个任务）中执行，所以完全没有竞态条件。
  //
  // g_playBusy=true 时，Web 任务会等待，避免"loop 正在
  // 播放，Web 又设置了新请求，loop 半途中清理"的情况。
  // ═════════════════════════════════════════════════════════════

  // ---- 阶段 1：处理停止请求 ----
  if (g_cmdStop) {
    g_playBusy = true;
    stopPlaybackInLoop();
    g_playBusy = false;
    g_cmdStop = false;
  }

  // ---- 阶段 2：处理播放请求（在此之前 stop 已执行完毕）----
  if (g_cmdPlayPath.length() > 0) {
    g_playBusy = true;
    String path = g_cmdPlayPath;
    g_cmdPlayPath = "";

    Serial.printf("[loop] Starting playback: %s\n", path.c_str());

    mp3 = new AudioGeneratorMP3();
    file = new AudioFileSourceSD(path.c_str());
    if (!mp3 || !file) {
      Serial.println(F("[loop] ERROR: failed to alloc decoder or file source"));
      if (mp3)  { delete mp3;  mp3  = nullptr; }
      if (file) { delete file; file = nullptr; }
      g_playBusy = false;
      return;
    }

    out->SetGain(g_volumeGain);
    if (!mp3->begin(file, out)) {
      Serial.printf("[loop] ERROR: mp3->begin() failed for %s\n", path.c_str());
      delete mp3;  mp3  = nullptr;
      delete file; file = nullptr;
      g_playBusy = false;
      return;
    }

    Serial.printf("[loop] Now playing: %s\n", path.c_str());
    g_playBusy = false;
    // 继续到下一阶段处理播放
  }

  // ---- 阶段 3：处理暂停请求 ----
  if (g_cmdPause) {
    g_cmdPause = false;
    if (mp3 != nullptr && mp3->isRunning()) {
      g_paused = !g_paused;
      Serial.printf("[loop] Pause toggled, now: %s\n", g_paused ? "paused" : "playing");
    }
  }

  // ---- 阶段 4：正常播放 / 暂停填充 ----
  if (mp3 != nullptr && mp3->isRunning()) {
    g_playBusy = true;  // 标记为忙，Web 请求会等待
    if (g_paused) {
      // 暂停状态：持续向 I2S 写入静音样本 (0,0)
      int16_t silent[2] = {0, 0};
      for (int i = 0; i < 128; ++i) {
        out->ConsumeSample(silent);
      }
    } else {
      // 正常播放
      if (!mp3->loop()) {
        Serial.println("\n>>> Playback finished.");
        stopPlaybackInLoop();
      }
    }
    g_playBusy = false;
    yield();
  } else {
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
    root.close();
    return;
  }

  File item;
  int guard = 0;
  // 不依赖 root.available()（对目录常返回 false），
  // 直接用 openNextFile + !item 判断终止。
  for (;;) {
    if (++guard > 20000) break;
    item = root.openNextFile();
    if (!item) break;
    const char *name = item.name();
    if (name == nullptr || name[0] == '\0') {
      item.close();
      yield();
      continue;
    }
    if(item.isDirectory()){
      Serial.print("  [DIR]  ");
      Serial.println(name);
      if(levels > 0){
        listDir(fs, item.path(), levels - 1);
      }
    } else {
      Serial.print("  [FILE] ");
      Serial.print(name);
      Serial.print("  |  Size: ");
      Serial.println(item.size());
    }
    item.close();
    yield();
  }
  root.close();
}
