#include "app.h"
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <Preferences.h>

static const uint16_t IR_RX_PIN = 42;
static const uint16_t IR_TX_PIN = 46;
static const uint16_t kCaptureBufferSize = 1024;
static const uint8_t  IR_TIMEOUT_MS = 50;
static const int      N_SLOTS = 4;

static IRrecv irrecv(IR_RX_PIN, kCaptureBufferSize, IR_TIMEOUT_MS, true);
static IRsend irsend(IR_TX_PIN);
static Preferences prefs;

struct Slot {
  bool learned = false;
  uint16_t len = 0;
  uint16_t buf[kCaptureBufferSize];
};

static Slot s_slots[N_SLOTS];
static int s_cursor = 0;

static String slotKey(int i) { return String("ir") + i; }

static void loadSlots() {
  prefs.begin("ir", true);
  for (int i = 0; i < N_SLOTS; i++) {
    String k = slotKey(i);
    size_t sz = prefs.getBytesLength(k.c_str());
    if (sz >= sizeof(uint16_t) * 2) {
      uint8_t* tmp = (uint8_t*)malloc(sz);
      prefs.getBytes(k.c_str(), tmp, sz);
      uint16_t len;
      memcpy(&len, tmp, sizeof(uint16_t));
      if (len > 0 && len < kCaptureBufferSize) {
        s_slots[i].len = len;
        size_t bytes = len * sizeof(uint16_t);
        if (bytes + sizeof(uint16_t) <= sz) {
          memcpy(s_slots[i].buf, tmp + sizeof(uint16_t), bytes);
          s_slots[i].learned = true;
        }
      }
      free(tmp);
    }
  }
  prefs.end();
}

static void saveSlot(int i) {
  prefs.begin("ir", false);
  String k = slotKey(i);
  size_t bytes = s_slots[i].len * sizeof(uint16_t);
  size_t total = sizeof(uint16_t) + bytes;
  uint8_t* tmp = (uint8_t*)malloc(total);
  memcpy(tmp, &s_slots[i].len, sizeof(uint16_t));
  memcpy(tmp + sizeof(uint16_t), s_slots[i].buf, bytes);
  prefs.putBytes(k.c_str(), tmp, total);
  prefs.end();
  free(tmp);
}

static void drawList() {
  g_canvas.fillScreen(CLR_BG);
  draw_title("红外遥控");
  int item_h = 20;
  int start_y = 26;
  for (int i = 0; i < N_SLOTS; i++) {
    int y = start_y + i * item_h;
    bool sel = (i == s_cursor);
    if (sel) {
      g_canvas.fillRoundRect(4, y, SCR_W - 8, item_h - 2, 3, CLR_ACCENT);
      g_canvas.setTextColor(CLR_BG, CLR_ACCENT);
    } else {
      g_canvas.setTextColor(CLR_TEXT, CLR_BG);
    }
    g_canvas.setFont(&fonts::efontCN_16);
    g_canvas.setTextDatum(middle_left);
    char buf[24];
    snprintf(buf, sizeof(buf), "按键 %d", i + 1);
    g_canvas.drawString(buf, 12, y + item_h / 2);
    g_canvas.setTextDatum(middle_right);
    const char* state = s_slots[i].learned ? "已学" : "空";
    uint32_t sc = sel ? CLR_BG : (s_slots[i].learned ? CLR_GOOD : CLR_DIM);
    g_canvas.setTextColor(sc, sel ? CLR_ACCENT : CLR_BG);
    g_canvas.drawString(state, SCR_W - 12, y + item_h / 2);
  }
  g_canvas.setTextDatum(top_left);
  g_canvas.setFont(&fonts::efontCN_12);
  g_canvas.setTextColor(CLR_DIM, CLR_BG);
  g_canvas.setCursor(6, SCR_H - 16);
  g_canvas.print("A切换  B进入  长按B返回");
  push_frame();
}

static void drawDetail() {
  g_canvas.fillScreen(CLR_BG);
  char tbuf[32];
  snprintf(tbuf, sizeof(tbuf), "按键 %d", s_cursor + 1);
  draw_title(tbuf);
  g_canvas.setFont(&fonts::efontCN_16);
  g_canvas.setTextColor(CLR_TEXT, CLR_BG);
  g_canvas.setTextDatum(middle_center);
  if (s_slots[s_cursor].learned) {
    char info[48];
    snprintf(info, sizeof(info), "已就绪（%u 脉冲）", (unsigned)s_slots[s_cursor].len);
    g_canvas.setTextColor(CLR_GOOD, CLR_BG);
    g_canvas.drawString(info, SCR_W / 2, 46);
  } else {
    g_canvas.setTextColor(CLR_DIM, CLR_BG);
    g_canvas.drawString("未学习", SCR_W / 2, 46);
  }
  g_canvas.setFont(&fonts::efontCN_14);
  g_canvas.setTextColor(CLR_DIM, CLR_BG);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString("A 学习   B 发送", SCR_W / 2, 82);
  g_canvas.drawString("长按 B 返回", SCR_W / 2, 102);
  g_canvas.setTextDatum(top_left);
  push_frame();
}

static void drawMessage(const char* text, uint32_t fg) {
  g_canvas.fillScreen(CLR_BG);
  g_canvas.setTextColor(fg, CLR_BG);
  g_canvas.setFont(&fonts::efontCN_24);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(text, SCR_W / 2, SCR_H / 2);
  g_canvas.setTextDatum(top_left);
  push_frame();
}

static void drawLearningPrompt() {
  g_canvas.fillScreen(CLR_BG);
  draw_title("学习中");
  g_canvas.setFont(&fonts::efontCN_16);
  g_canvas.setTextColor(CLR_WARN, CLR_BG);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString("将遥控器对准设备", SCR_W / 2, 50);
  g_canvas.drawString("按一下想记的按键", SCR_W / 2, 74);
  g_canvas.setFont(&fonts::efontCN_12);
  g_canvas.setTextColor(CLR_DIM, CLR_BG);
  g_canvas.drawString("长按 B 取消", SCR_W / 2, 100);
  g_canvas.setTextDatum(top_left);
  push_frame();
}

static void doLearn(int idx) {
  drawLearningPrompt();
  irrecv.enableIRIn();
  decode_results results;
  uint32_t start = millis();
  while (millis() - start < 15000) {
    M5.update();
    if (M5.BtnB.pressedFor(LONG_PRESS_MS)) { beep_bad(); return; }
    if (irrecv.decode(&results)) {
      uint16_t len = results.rawlen;
      if (len > 1 && len < kCaptureBufferSize) {
        s_slots[idx].len = len;
        for (uint16_t i = 0; i < len; i++) {
          uint32_t us = (uint32_t)results.rawbuf[i] * kRawTick;
          if (us > 0xFFFF) us = 0xFFFF;
          s_slots[idx].buf[i] = (uint16_t)us;
        }
        s_slots[idx].learned = true;
        saveSlot(idx);
        beep_ok();
        irrecv.resume();
        irrecv.disableIRIn();
        drawMessage("学习成功", CLR_GOOD);
        delay(800);
        return;
      }
      irrecv.resume();
    }
    delay(5);
  }
  irrecv.disableIRIn();
  beep_bad();
  drawMessage("超时", CLR_BAD);
  delay(800);
}

static void doSend(int idx) {
  if (!s_slots[idx].learned) { beep_bad(); return; }
  irsend.begin();
  uint16_t n = s_slots[idx].len;
  if (n > 1) irsend.sendRaw(&s_slots[idx].buf[1], n - 1, 38);
  beep_ok();
  drawMessage("已发送", CLR_ACCENT);
  delay(350);
}

void app_ir_run() {
  M5.Power.setExtOutput(true);
  loadSlots();
  s_cursor = 0;
  int mode = 0;
  drawList();

  bool longpress_sent = false;
  while (true) {
    M5.update();
    screen_saver_tick();
    maybe_auto_rotate();
    if (M5.BtnB.pressedFor(LONG_PRESS_MS)) {
      if (!longpress_sent) { beep_ok(); longpress_sent = true; }
    }
    if (longpress_sent && !M5.BtnB.isPressed()) {
      longpress_sent = false;
      if (mode == 1) { mode = 0; drawList(); continue; }
      else { M5.Power.setExtOutput(false); return; }
    }
    if (M5.BtnA.wasPressed() && !longpress_sent) {
      if (mode == 0) {
        s_cursor = (s_cursor + 1) % N_SLOTS;
        beep_ok();
        drawList();
      } else {
        doLearn(s_cursor);
        drawDetail();
      }
    }
    if (M5.BtnB.wasReleased() && !longpress_sent) {
      if (mode == 0) {
        mode = 1;
        beep_ok();
        drawDetail();
      } else {
        doSend(s_cursor);
        drawDetail();
      }
    }
    delay(20);
  }
}
