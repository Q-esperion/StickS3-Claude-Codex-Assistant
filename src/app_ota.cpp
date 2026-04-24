#include "app.h"
#include <WiFi.h>
#include <ArduinoOTA.h>

// Dedicated OTA upgrade mode. Before beginning OTA we shut down every heavy
// consumer (audio, mic, speaker) so the upload doesn't collide with other
// tasks writing/reading Flash at the same time — which was the root cause of
// the PANIC crashes when OTA ran as a background task alongside the radio /
// voice-kb / WebSocket stacks.

static volatile int  s_ota_progress = -1;  // 0..100 during upload, -1 idle, -2 done, -3 err
static volatile int  s_ota_err      = 0;
// ArduinoOTA.handle() blocks for the full ~40s upload, so the main loop can't
// refresh the screen during transfer. We draw from inside the onProgress
// callback instead — that's the only code path running mid-transfer. IP is
// stashed at file scope so the callback has access.
static String s_ota_ip;

static void drawOTAScreen(const char* ip) {
  g_canvas.fillScreen(CLR_BG);
  draw_title("OTA 升级");

  // Big claude mascot in the middle for vibe.
  draw_claude_mascot(SCR_W / 2, 48, 14, CLR_ACCENT);

  g_canvas.setFont(&fonts::efontCN_16);
  g_canvas.setTextDatum(middle_center);

  if (s_ota_progress == -3) {
    g_canvas.setTextColor(CLR_BAD, CLR_BG);
    g_canvas.drawString("升级失败", SCR_W / 2, 76);
    g_canvas.setFont(&fonts::efontCN_12);
    char buf[32]; snprintf(buf, sizeof(buf), "错误码 %d", s_ota_err);
    g_canvas.drawString(buf, SCR_W / 2, 94);
  } else if (s_ota_progress == -2) {
    g_canvas.setTextColor(CLR_GOOD, CLR_BG);
    g_canvas.drawString("升级成功，重启中", SCR_W / 2, 76);
  } else if (s_ota_progress >= 0) {
    g_canvas.setTextColor(CLR_WARN, CLR_BG);
    char buf[16]; snprintf(buf, sizeof(buf), "升级中 %d%%", s_ota_progress);
    g_canvas.drawString(buf, SCR_W / 2, 76);
    // Progress bar
    int pbx = 20, pby = 96, pbw = SCR_W - 40, pbh = 8;
    g_canvas.drawRoundRect(pbx - 1, pby - 1, pbw + 2, pbh + 2, 2, CLR_DIM);
    int fill = pbw * s_ota_progress / 100;
    g_canvas.fillRoundRect(pbx, pby, fill, pbh, 2, CLR_ACCENT);
  } else {
    g_canvas.setTextColor(CLR_TEXT, CLR_BG);
    g_canvas.drawString("等待上传", SCR_W / 2, 76);
    g_canvas.setFont(&fonts::efontCN_12);
    g_canvas.setTextColor(CLR_DIM, CLR_BG);
    char buf[48]; snprintf(buf, sizeof(buf), "IP %s", ip);
    g_canvas.drawString(buf, SCR_W / 2, 96);
    g_canvas.drawString("长按 B 退出", SCR_W / 2, SCR_H - 12);
  }
  g_canvas.setTextDatum(top_left);
  push_frame();
}

void app_ota_run() {
  // 1) Quiet down everything that might fight for WiFi / Flash / I2S.
  M5.Speaker.end();
  M5.Mic.end();
  // (Audio-lib streaming lives inside the radio app — not active here.)
  // (BLE lib no longer used after we moved to WiFi helper.)

  if (WiFi.status() != WL_CONNECTED) {
    g_canvas.fillScreen(CLR_BG);
    draw_title("OTA 升级");
    g_canvas.setFont(&fonts::efontCN_16);
    g_canvas.setTextColor(CLR_BAD, CLR_BG);
    g_canvas.setTextDatum(middle_center);
    g_canvas.drawString("WiFi 未连接", SCR_W / 2, SCR_H / 2);
    g_canvas.setTextDatum(top_left);
    push_frame();
    delay(2000);
    M5.Speaker.begin();
    apply_volume();
    return;
  }

  String ip_str = WiFi.localIP().toString();

  // 2) Wire up OTA callbacks — draw from within them because handle()
  //    blocks for the full upload and the main loop can't repaint.
  //    Keep it screen-only: no HTTP/stick_log/etc (those crashed before).
  s_ota_ip = ip_str;
  static bool ota_configured = false;
  if (!ota_configured) {
    ArduinoOTA.setHostname("sticks3");
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.onStart([]() {
      s_ota_progress = 0;
      drawOTAScreen(s_ota_ip.c_str());
    });
    ArduinoOTA.onEnd([]() {
      s_ota_progress = -2;
      drawOTAScreen(s_ota_ip.c_str());
    });
    ArduinoOTA.onProgress([](unsigned done, unsigned total) {
      if (total == 0) return;
      int p = (int)((uint64_t)done * 100 / total);
      if (p == s_ota_progress) return;
      s_ota_progress = p;
      // Rate-limit screen pushes so frequent small chunks don't starve the
      // Flash write path. Force a draw at 100% so the final frame lands.
      static uint32_t s_last_draw_ms = 0;
      uint32_t now = millis();
      if (p < 100 && now - s_last_draw_ms < 150) return;
      s_last_draw_ms = now;
      drawOTAScreen(s_ota_ip.c_str());
    });
    ArduinoOTA.onError([](ota_error_t e) {
      s_ota_err = (int)e;
      s_ota_progress = -3;
      drawOTAScreen(s_ota_ip.c_str());
    });
    ArduinoOTA.begin();
    ota_configured = true;
  } else {
    // Just re-begin in case previous session ended.
    ArduinoOTA.begin();
  }

  s_ota_progress = -1;  // reset state to idle
  drawOTAScreen(ip_str.c_str());

  bool longpress_sent = false;
  uint32_t last_draw = 0;

  while (true) {
    // Tight OTA loop — no other work here; that's the whole point.
    // Keep the screen ON for the whole OTA mode (progress must stay visible).
    screen_saver_kick();
    ArduinoOTA.handle();

    // UI refresh at most every 80ms to avoid starving OTA.
    if (millis() - last_draw > 80) {
      last_draw = millis();
      drawOTAScreen(ip_str.c_str());
    }

    if (s_ota_progress == -2) {   // success — board will reboot itself
      delay(1500);
      ESP.restart();
    }

    // Allow exit unless we're actively receiving.
    if (s_ota_progress < 0 || s_ota_progress == -3) {
      M5.update();
      if (M5.BtnB.pressedFor(LONG_PRESS_MS)) {
        if (!longpress_sent) longpress_sent = true;
      }
      if (longpress_sent && !M5.BtnB.isPressed()) {
        M5.Speaker.begin();
        apply_volume();
        return;
      }
    }
    delay(5);
  }
}
