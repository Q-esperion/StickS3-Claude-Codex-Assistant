#include "app.h"
#include <Preferences.h>

static int  s_volume = 6;           // 0..10
static int  s_brightness = 7;       // 1..10
static bool s_auto_rotate = false;
static int  s_screen_timeout = 60;  // seconds, 0 = never

int  get_volume()             { return s_volume; }
int  get_brightness()         { return s_brightness; }
bool get_auto_rotate()        { return s_auto_rotate; }
int  get_screen_timeout_sec() { return s_screen_timeout; }

void apply_volume()     { if (!g_radio_owns_i2s) M5.Speaker.setVolume(s_volume * 25); }
void apply_brightness() {
  // Drive our own LEDC channel (set up in main.cpp at 20 kHz) instead of
  // M5.Display.setBrightness so backlight frequency stays anti-flicker.
  ledcWrite(3, s_brightness * 25);
}

void load_settings() {
  Preferences p;
  p.begin("sys", true);
  s_volume         = p.getInt("vol", 6);
  s_brightness     = p.getInt("bri", 7);
  s_auto_rotate    = p.getBool("rot", false);
  s_screen_timeout = p.getInt("sto", 60);
  p.end();
  if (s_volume < 0) s_volume = 0;
  if (s_volume > 10) s_volume = 10;
  if (s_brightness < 1) s_brightness = 1;
  if (s_brightness > 10) s_brightness = 10;
  if (s_screen_timeout < 0) s_screen_timeout = 0;
  apply_volume();
  apply_brightness();
}

static void save_settings() {
  Preferences p;
  p.begin("sys", false);
  p.putInt ("vol", s_volume);
  p.putInt ("bri", s_brightness);
  p.putBool("rot", s_auto_rotate);
  p.putInt ("sto", s_screen_timeout);
  p.end();
}

// Auto-rotate: flip between rotation 3 (default landscape) and rotation 1
// (180° flipped landscape) based on gravity along accel X axis.
void maybe_auto_rotate() {
  if (!s_auto_rotate) return;
  static uint32_t last = 0;
  static uint8_t  cur_rot = 3;
  if (millis() - last < 250) return;
  last = millis();

  M5.Imu.update();
  auto d = M5.Imu.getImuData();
  float ax = d.accel.x;
  float ay = d.accel.y;

  // Pick rotation based on which axis gravity mostly pulls along.
  // Threshold 0.35g — forgiving so a modest tilt triggers the flip.
  uint8_t wanted = cur_rot;
  if (fabsf(ax) > fabsf(ay)) {
    if (ax >  0.35f) wanted = 1;
    else if (ax < -0.35f) wanted = 3;
  } else {
    if (ay >  0.35f) wanted = 1;
    else if (ay < -0.35f) wanted = 3;
  }

  if (wanted != cur_rot) {
    // No stick_log here — it POSTs synchronously and the 600ms timeout
    // starved the radio I2S buffer, producing an audible click/skip when
    // the user tilted the device while music was playing.
    cur_rot = wanted;
    M5.Display.setRotation(cur_rot);
  }
}

static int s_cursor = 0;  // 0=volume 1=brightness 2=rotate 3=screen-timeout 4=wifi-portal
static const int N_ROWS = 5;

static const int TIMEOUT_STEPS[] = { 0, 15, 30, 60, 120, 300 };
static const int N_TIMEOUT_STEPS = sizeof(TIMEOUT_STEPS) / sizeof(TIMEOUT_STEPS[0]);

static int timeoutStepIndex() {
  for (int i = 0; i < N_TIMEOUT_STEPS; i++) if (TIMEOUT_STEPS[i] == s_screen_timeout) return i;
  return 3;  // default to 60s
}

static void drawUI() {
  g_canvas.fillScreen(CLR_BG);
  draw_title("设置");

  int row_h = 22;
  int start_y = 24;

  const char* labels[N_ROWS] = {"音量", "亮度", "自动旋转", "自动熄屏", "WiFi 配网"};

  for (int i = 0; i < N_ROWS; i++) {
    int y = start_y + i * row_h;
    bool sel = (i == s_cursor);
    uint32_t label_color = sel ? CLR_ACCENT : CLR_TEXT;

    g_canvas.setFont(&fonts::efontCN_14);
    g_canvas.setTextColor(label_color, CLR_BG);
    g_canvas.setTextDatum(middle_left);
    g_canvas.drawString(labels[i], 8, y + 7);

    if (i < 2) {
      int v = (i == 0) ? s_volume : s_brightness;
      int maxv = 10;
      char vbuf[8]; snprintf(vbuf, sizeof(vbuf), "%d", v);
      g_canvas.setTextDatum(middle_right);
      g_canvas.drawString(vbuf, SCR_W - 8, y + 7);

      int bar_x = 8;
      int bar_y = y + 14;
      int bar_w = SCR_W - 16;
      int bar_h = 4;
      g_canvas.drawRoundRect(bar_x, bar_y, bar_w, bar_h, 2, CLR_DIM);
      int fill_w = (int)((float)v / maxv * (bar_w - 2));
      uint32_t bar_color = sel ? CLR_ACCENT : CLR_GOOD;
      if (fill_w > 0) g_canvas.fillRoundRect(bar_x + 1, bar_y + 1, fill_w, bar_h - 2, 2, bar_color);
    } else if (i == 2) {
      g_canvas.setTextDatum(middle_right);
      const char* v = s_auto_rotate ? "开" : "关";
      g_canvas.setTextColor(s_auto_rotate ? CLR_GOOD : CLR_DIM, CLR_BG);
      g_canvas.drawString(v, SCR_W - 8, y + 7);
    } else if (i == 3) {
      g_canvas.setTextDatum(middle_right);
      char buf[16];
      if (s_screen_timeout == 0) snprintf(buf, sizeof(buf), "关");
      else if (s_screen_timeout < 60) snprintf(buf, sizeof(buf), "%d 秒", s_screen_timeout);
      else snprintf(buf, sizeof(buf), "%d 分", s_screen_timeout / 60);
      g_canvas.setTextColor(s_screen_timeout == 0 ? CLR_DIM : CLR_GOOD, CLR_BG);
      g_canvas.drawString(buf, SCR_W - 8, y + 7);
    } else {
      // WiFi 配网 — action row, B press triggers portal.
      g_canvas.setTextDatum(middle_right);
      g_canvas.setTextColor(sel ? CLR_ACCENT : CLR_DIM, CLR_BG);
      g_canvas.drawString("点 B 开始", SCR_W - 8, y + 7);
    }
  }

  g_canvas.setTextDatum(top_left);
  push_frame();
}

void app_settings_run() {
  drawUI();

  bool longpress_sent = false;
  // The B press that brought us into this app is still held down when we
  // start. If we don't wait for it to be released first, its release event
  // fires on the first row (volume) and bumps the value by one. Prime here.
  bool b_primed = false;
  while (true) {
    M5.update();
    screen_saver_tick();
    maybe_auto_rotate();

    if (!b_primed) {
      if (!M5.BtnB.isPressed()) b_primed = true;
      delay(20);
      continue;
    }

    if (M5.BtnB.pressedFor(LONG_PRESS_MS)) {
      if (!longpress_sent) { beep_ok(); longpress_sent = true; }
    }
    if (longpress_sent && !M5.BtnB.isPressed()) {
      save_settings();
      return;
    }

    if (M5.BtnA.wasPressed() && !longpress_sent) {
      s_cursor = (s_cursor + 1) % N_ROWS;
      beep_ok();
      drawUI();
    }

    if (M5.BtnB.wasReleased() && !longpress_sent) {
      if (s_cursor == 0) {
        s_volume = (s_volume + 1) % 11;
        apply_volume();
      } else if (s_cursor == 1) {
        s_brightness = (s_brightness + 1);
        if (s_brightness > 10) s_brightness = 1;
        apply_brightness();
      } else if (s_cursor == 2) {
        s_auto_rotate = !s_auto_rotate;
        if (!s_auto_rotate) M5.Display.setRotation(3);
      } else if (s_cursor == 3) {
        int idx = (timeoutStepIndex() + 1) % N_TIMEOUT_STEPS;
        s_screen_timeout = TIMEOUT_STEPS[idx];
        screen_saver_kick();   // reset idle timer so change takes effect cleanly
      } else {
        // WiFi 重新配网 — save current settings first, then hand the screen
        // over to WiFiManager. When it returns (connected or timed out) we
        // redraw our UI and continue.
        save_settings();
        beep_ok();
        stick_log("info", "settings: forcing WiFi config portal");
        wifi_setup(true);
        screen_saver_kick();
        drawUI();
        continue;
      }
      save_settings();
      beep_ok();
      drawUI();
    }

    delay(20);
  }
}
