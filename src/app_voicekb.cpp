#include "app.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include "esp_heap_caps.h"
#include <time.h>

// iFlytek credentials (stored in memory file xfyun_credentials.md)
static const char* XF_APPID      = "6a833154";
static const char* XF_API_KEY    = "49dac253f727f50582590821a8610e29";
static const char* XF_API_SECRET = "ZmZlNDFmZTliMDcyMzQ2ZWE1MjdiYTM3";
static const char* XF_HOST       = "iat-api.xfyun.cn";

// PC helper — IP / port are discovered at runtime via UDP broadcast and
// persisted in NVS. The defaults below are only used before the very first
// successful discovery (or as a fallback if the saved address still works).
static String s_pc_ip   = "192.168.31.124";
static int    s_pc_port = 8765;
static const int DISCOVERY_UDP_PORT = 8766;

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t   MAX_SAMPLES = SAMPLE_RATE * 30;  // 30 seconds
constexpr uint32_t CLR_CLAUDE  = 0xE8976Fu;

static int16_t* s_pcm = nullptr;
static size_t   s_pcm_len = 0;

static WebSocketsClient s_ws;
static String s_stt_result;
static String s_stt_error_detail;
static volatile bool s_stt_done = false;
static volatile bool s_stt_error = false;

enum State {
  ST_CHECKING,    // on entry, probe PC helper
  ST_NO_PC,       // helper unreachable
  ST_IDLE,        // ready to record
  ST_RECORDING,
  ST_UPLOADING,   // sending audio to iFlytek
  ST_DONE,
  ST_ERROR,
};
static State s_state = ST_CHECKING;
static String s_status_text;
static uint32_t s_done_t = 0;
static uint32_t s_last_ping_t = 0;
static uint32_t s_idle_entered_t = 0;   // for B-debounce when entering the app

// Live Claude Code progress (polled from PC helper /status)
static const int HIST_CAP = 4;
static String    s_claude_hist[HIST_CAP];  // currently displayed log
static int       s_claude_hist_n = 0;
static int       s_claude_age = -1;
static uint32_t  s_last_claude_poll_t = 0;

// Animation queue: events fetched from /status are enqueued here, then
// revealed one-by-one with a delay so the log flows instead of popping in bulk.
static const int PENDING_CAP = 16;
static String    s_pending[PENDING_CAP];
static int       s_pending_n = 0;
static uint32_t  s_last_seq_seen = 0;
static uint32_t  s_last_reveal_t = 0;
static const uint32_t REVEAL_INTERVAL_MS = 500;

// Independent UI refresh tick so battery / charging icon update quickly
// even when no Claude status changes.
static uint32_t s_last_ui_refresh_t = 0;

// ========= HMAC + Base64 helpers =========
static String hmac_sha256_b64(const String& data, const String& key) {
  unsigned char hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);
  char out[64];
  size_t out_len = 0;
  mbedtls_base64_encode((unsigned char*)out, sizeof(out), &out_len, hmac, 32);
  return String(out, out_len);
}
static String base64_encode_str(const String& data) {
  size_t cap = ((data.length() + 2) / 3) * 4 + 4;
  char* buf = (char*)malloc(cap);
  if (!buf) return String();   // heap exhausted — return empty, caller will fail gracefully
  size_t out_len = 0;
  mbedtls_base64_encode((unsigned char*)buf, cap, &out_len,
                        (const unsigned char*)data.c_str(), data.length());
  String r(buf, out_len);
  free(buf);
  return r;
}
static String base64_encode_bytes(const uint8_t* data, size_t len) {
  size_t cap = ((len + 2) / 3) * 4 + 4;
  char* buf = (char*)malloc(cap);
  if (!buf) return String();
  size_t out_len = 0;
  mbedtls_base64_encode((unsigned char*)buf, cap, &out_len, data, len);
  String r(buf, out_len);
  free(buf);
  return r;
}
static String url_encode(const String& s) {
  String r;
  r.reserve(s.length() * 2);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      r += c;
    } else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
      r += b;
    }
  }
  return r;
}
static String http_date() {
  time_t now = time(nullptr);
  struct tm gmt;
  gmtime_r(&now, &gmt);
  char buf[64];
  strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
  return String(buf);
}

// ========= PC helper discovery (NVS + UDP broadcast) =========

static Preferences s_pc_prefs;

static void loadPcAddress() {
  s_pc_prefs.begin("pc", true);
  s_pc_ip   = s_pc_prefs.getString("ip",   s_pc_ip);
  s_pc_port = s_pc_prefs.getInt   ("port", s_pc_port);
  s_pc_prefs.end();
  stick_log("info", String("pc addr loaded: ") + s_pc_ip + ":" + s_pc_port);
}

static void savePcAddress() {
  s_pc_prefs.begin("pc", false);
  s_pc_prefs.putString("ip",   s_pc_ip);
  s_pc_prefs.putInt   ("port", s_pc_port);
  s_pc_prefs.end();
}

// Broadcast "STICK?" on UDP, wait for "STICK! <ip>:<port>" from any helper.
// Returns true if an address was found (and s_pc_ip/s_pc_port updated).
static bool discoverHelper() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiUDP udp;
  if (!udp.begin(0)) return false;

  IPAddress bcast(255, 255, 255, 255);
  const char* query = "STICK?";
  // Two sends 250ms apart increases odds of catching a just-woken helper.
  for (int attempt = 0; attempt < 2; attempt++) {
    udp.beginPacket(bcast, DISCOVERY_UDP_PORT);
    udp.write((const uint8_t*)query, 6);
    udp.endPacket();

    uint32_t t0 = millis();
    while (millis() - t0 < 800) {
      int sz = udp.parsePacket();
      if (sz > 0) {
        char buf[128];
        int len = udp.read(buf, sizeof(buf) - 1);
        if (len <= 0) { delay(5); continue; }
        buf[len] = 0;
        if (strncmp(buf, "STICK! ", 7) == 0) {
          char* p = buf + 7;
          char* colon = strchr(p, ':');
          if (colon) {
            *colon = 0;
            String ip = p;
            int port = atoi(colon + 1);
            if (ip.length() > 0 && port > 0) {
              s_pc_ip = ip;
              s_pc_port = port;
              savePcAddress();
              udp.stop();
              stick_log("info", String("pc discovered: ") + s_pc_ip + ":" + s_pc_port);
              return true;
            }
          }
        }
      }
      delay(10);
    }
  }
  udp.stop();
  stick_log("warn", "pc discovery: no response");
  return false;
}

// ========= PC helper =========
static bool pingPC() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(1500);
  String url = String("http://") + s_pc_ip + ":" + s_pc_port + "/ping";
  if (!http.begin(url)) return false;
  int code = http.GET();
  http.end();
  return code == 200;
}

// Fetch Claude Code progress. Server returns history items with monotonic
// seq ids; we enqueue everything with seq > s_last_seq_seen, guaranteed to
// catch every event even if the same text repeats back-to-back.
static void pollClaudeStatus() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(1500);
  String url = String("http://") + s_pc_ip + ":" + s_pc_port + "/status";
  if (!http.begin(url)) { stick_log("warn", "poll: begin failed"); return; }
  int code = http.GET();
  if (code != 200) {
    stick_log("warn", String("poll: http ") + code);
    http.end();
    return;
  }
  String body = http.getString();
  JsonDocument doc;
  auto err = deserializeJson(doc, body);
  if (err) {
    stick_log("warn", String("poll: parse ") + err.c_str());
    http.end();
    return;
  }
  s_claude_age = doc["age_sec"] | -1;
  uint32_t latest_seq = doc["latest_seq"] | 0;
  // Helper restart detection: seq counter resets to 0 in the helper process,
  // so a smaller seq means we should stop waiting for events we'll never see.
  if (latest_seq < s_last_seq_seen) {
    stick_log("info", String("poll: helper restart detected, seq ")
                      + s_last_seq_seen + " -> " + latest_seq);
    s_last_seq_seen = 0;
    s_claude_hist_n = 0;
    s_pending_n = 0;
  }
  // First-ever poll after board boot: just sync the seq cursor so we don't
  // replay historical events from before we were even running.
  static bool s_poll_primed = false;
  if (!s_poll_primed) {
    s_last_seq_seen = latest_seq;
    s_poll_primed = true;
    stick_log("info", String("poll: primed at seq ") + latest_seq);
    http.end();
    return;
  }
  if (latest_seq > s_last_seq_seen) {
    JsonArray hist = doc["history"].as<JsonArray>();
    int added = 0;
    for (JsonVariant v : hist) {
      uint32_t seq = v["seq"] | 0;
      if (seq > s_last_seq_seen && s_pending_n < PENDING_CAP) {
        s_pending[s_pending_n++] = String((const char*)(v["text"] | ""));
        added++;
      }
    }
    uint32_t prev_seq = s_last_seq_seen;
    s_last_seq_seen = latest_seq;
    char buf[96];
    snprintf(buf, sizeof(buf),
             "poll: seq %u->%u added=%d pending=%d",
             (unsigned)prev_seq, (unsigned)latest_seq, added, s_pending_n);
    stick_log("debug", buf);
  }
  http.end();
}

// Move at most one item from the pending queue into the visible history.
// Called frequently from the main loop; paced by REVEAL_INTERVAL_MS.
static bool revealPending() {
  if (s_pending_n == 0) return false;
  if (millis() - s_last_reveal_t < REVEAL_INTERVAL_MS) return false;
  s_last_reveal_t = millis();
  String next = s_pending[0];
  for (int i = 0; i < s_pending_n - 1; i++) s_pending[i] = s_pending[i + 1];
  s_pending_n--;
  if (s_claude_hist_n >= HIST_CAP) {
    for (int i = 0; i < HIST_CAP - 1; i++) s_claude_hist[i] = s_claude_hist[i + 1];
    s_claude_hist[HIST_CAP - 1] = next;
  } else {
    s_claude_hist[s_claude_hist_n++] = next;
  }
  // Stop-hook chime: when Claude finishes a reply (marker 'C'), play a brief
  // two-note tone. Speaker is ended while this app runs (mic leakage mitigation),
  // so re-enable it just for the chime.
  if (next.length() >= 2 && (uint8_t)next.charAt(0) == 0x01 && next.charAt(1) == 'C') {
    M5.Speaker.begin();
    apply_volume();
    M5.Speaker.tone(1800, 80);
    delay(90);
    M5.Speaker.tone(1400, 120);
    delay(130);
    M5.Speaker.end();
  }
  return true;
}

static bool sendToPC(const String& text) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(3000);
  String url = String("http://") + s_pc_ip + ":" + s_pc_port + "/type";
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "application/json; charset=utf-8");

  JsonDocument doc;
  doc["text"] = text;
  doc["enter"] = true;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  http.end();
  return code == 200;
}

// ========= iFlytek STT streaming =========
static void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length)) {
        s_stt_error = true; s_stt_done = true;
        s_stt_error_detail = "JSON parse";
        return;
      }
      int code = doc["code"] | -1;
      if (code != 0) {
        s_stt_error = true; s_stt_done = true;
        const char* msg = doc["message"] | "";
        s_stt_error_detail = String("code ") + code + " " + msg;
        return;
      }
      JsonArray ws = doc["data"]["result"]["ws"];
      if (!ws.isNull()) {
        String frag;
        for (JsonObject w : ws) {
          JsonArray cw = w["cw"];
          if (!cw.isNull() && cw.size() > 0) frag += String((const char*)(cw[0]["w"] | ""));
        }
        const char* pgs = doc["data"]["result"]["pgs"] | "apd";
        if (strcmp(pgs, "apd") == 0) s_stt_result += frag;
        else s_stt_result = frag;
      }
      int status = doc["data"]["status"] | 0;
      if (status == 2) s_stt_done = true;
      break;
    }
    case WStype_DISCONNECTED:
      if (!s_stt_done) { s_stt_error = true; s_stt_done = true; }
      break;
    default: break;
  }
}

static String doSTT() {
  s_stt_result = "";
  s_stt_done = false;
  s_stt_error = false;
  s_stt_error_detail = "";

  time_t now_t = time(nullptr);
  if (now_t < 1700000000) { s_stt_error_detail = "时间未同步"; return ""; }

  String date = http_date();
  String sig_origin = String("host: ") + XF_HOST + "\n"
                    + "date: " + date + "\n"
                    + "GET /v2/iat HTTP/1.1";
  String sig_b64 = hmac_sha256_b64(sig_origin, String(XF_API_SECRET));

  String auth_origin = String("api_key=\"") + XF_API_KEY +
                       "\", algorithm=\"hmac-sha256\", "
                       "headers=\"host date request-line\", "
                       "signature=\"" + sig_b64 + "\"";
  String auth_b64 = base64_encode_str(auth_origin);

  String path = String("/v2/iat?authorization=") + url_encode(auth_b64)
              + "&date=" + url_encode(date)
              + "&host=" + XF_HOST;

  s_ws.onEvent(wsEvent);
  s_ws.setReconnectInterval(0);
  s_ws.beginSSL(XF_HOST, 443, path.c_str());

  uint32_t t0 = millis();
  while (millis() - t0 < 6000) {
    s_ws.loop();
    if (s_ws.isConnected()) break;
    delay(10);
  }
  if (!s_ws.isConnected()) {
    s_ws.disconnect();
    s_stt_error_detail = "WS 连接失败";
    return "";
  }

  const size_t FRAME_BYTES = 1280;
  size_t pcm_bytes = s_pcm_len * 2;
  const uint8_t* pcm_p = (const uint8_t*)s_pcm;

  size_t offset = 0;
  bool first = true;
  uint32_t last_frame_t = millis();

  while (offset < pcm_bytes) {
    size_t chunk = pcm_bytes - offset;
    if (chunk > FRAME_BYTES) chunk = FRAME_BYTES;
    String audio_b64 = base64_encode_bytes(pcm_p + offset, chunk);

    String msg;
    if (first) {
      msg  = String("{\"common\":{\"app_id\":\"") + XF_APPID + "\"},"
           + "\"business\":{\"language\":\"zh_cn\",\"domain\":\"iat\","
             "\"accent\":\"mandarin\",\"vad_eos\":5000},"
           + "\"data\":{\"status\":0,\"format\":\"audio/L16;rate=16000\","
             "\"encoding\":\"raw\",\"audio\":\"" + audio_b64 + "\"}}";
      first = false;
    } else {
      msg  = String("{\"data\":{\"status\":1,\"format\":\"audio/L16;rate=16000\","
             "\"encoding\":\"raw\",\"audio\":\"") + audio_b64 + "\"}}";
    }
    s_ws.sendTXT(msg);
    offset += chunk;

    while (millis() - last_frame_t < 40) { s_ws.loop(); delay(1); }
    last_frame_t = millis();
  }

  String end_msg = "{\"data\":{\"status\":2,\"format\":\"audio/L16;rate=16000\","
                   "\"encoding\":\"raw\",\"audio\":\"\"}}";
  s_ws.sendTXT(end_msg);

  t0 = millis();
  while (millis() - t0 < 10000) {
    s_ws.loop();
    if (s_stt_done) break;
    delay(5);
  }
  s_ws.disconnect();
  return s_stt_error ? String("") : s_stt_result;
}

// ========= Mic =========
static void startRecording() {
  s_pcm_len = 0;
  M5.Mic.begin();
  delay(5);
  // Mic.begin() can re-enable the codec's speaker amp path as a side effect.
  // Force it off again so we don't hear ADC/clock leakage amplified.
  M5.Speaker.end();
}
static void stopRecording() {
  M5.Mic.end();
}
static void recordChunk() {
  const size_t CHUNK = 512;
  if (s_pcm_len + CHUNK > MAX_SAMPLES) return;
  if (M5.Mic.record(&s_pcm[s_pcm_len], CHUNK, SAMPLE_RATE)) {
    while (M5.Mic.isRecording()) { delay(1); }
    s_pcm_len += CHUNK;
  }
}

// ========= UI =========
// Layout (240 x 135, rotation 3):
//   y 0..21  title bar (app name + battery + charge bolt)
//   y 22..120 main area, split by vertical rule at x=170 into
//     left column (x 4..168)   : scrolling "Claude activity" log — 4 lines
//     right column (x 174..236): mascot + state label + extra info (time)
//   y 121..134 footer hint

static const int LEFT_PAD   = 4;
static const int DIV_X      = 170;
static const int RIGHT_CX   = 205;
static const int LOG_TOP    = 26;
static const int LOG_LINE_H = 17;

// UTF-8 aware truncation: cut at a character boundary, not inside a multi-byte codepoint.
static String utf8Trim(const String& s, size_t max_bytes) {
  if (s.length() <= max_bytes) return s;
  size_t cut = max_bytes;
  // Back up while the byte at 'cut' is a UTF-8 continuation byte (10xxxxxx).
  while (cut > 0 && (((uint8_t)s.charAt(cut)) & 0xC0) == 0x80) cut--;
  return s.substring(0, cut) + "…";
}

// Split a string into up to 2 display rows. Break at UTF-8 char boundary
// near max_bytes_per_row. Returns number of rows used (0, 1, or 2).
static int wrapLine(const String& src, size_t max_bytes, String out[2]) {
  if (src.length() == 0) { out[0] = ""; return 0; }
  if (src.length() <= max_bytes) { out[0] = src; return 1; }

  // Find character-boundary split near max_bytes for row 1
  size_t cut = max_bytes;
  while (cut > 0 && (((uint8_t)src.charAt(cut)) & 0xC0) == 0x80) cut--;
  out[0] = src.substring(0, cut);

  String rest = src.substring(cut);
  if (rest.length() <= max_bytes) {
    out[1] = rest;
    return 2;
  }
  // Rest is still too long — truncate at char boundary + ellipsis
  cut = max_bytes;
  while (cut > 0 && (((uint8_t)rest.charAt(cut)) & 0xC0) == 0x80) cut--;
  out[1] = rest.substring(0, cut) + "…";
  return 2;
}

// Event line may carry an invisible marker in its first 2 bytes:
//   \x01 U <text>  →  user prompt
//   \x01 C <text>  →  Claude reply
//   (no marker)    →  tool event or system message
// Returns the payload (no marker), writes the marker char (0 if none) to out_kind.
static String peelMarker(const String& raw, char& out_kind) {
  out_kind = 0;
  if (raw.length() >= 2 && (uint8_t)raw.charAt(0) == 0x01) {
    out_kind = raw.charAt(1);
    return raw.substring(2);
  }
  return raw;
}

static void drawLeftColumn() {
  g_canvas.setFont(&fonts::efontCN_12);
  g_canvas.setTextDatum(top_left);

  // No header — let rows start right below the title bar.
  int y0 = LOG_TOP;
  if (s_claude_hist_n == 0) {
    g_canvas.setTextColor(CLR_DIM, CLR_BG);
    g_canvas.drawString("（等待消息）", LEFT_PAD + 6, y0 + 4);
    return;
  }

  // Budget: one more row now that the header is gone.
  const int MAX_ROWS = 6;
  const size_t ROW_BYTES = 30;

  // Build list of rendered rows for up to the last 4 events, newest last.
  struct Row { String text; uint32_t color; bool bullet; };
  Row rows[MAX_ROWS];
  int rn = 0;

  // Walk history newest-first; for each event, expand to 1–2 rows; stop when full.
  int start = s_claude_hist_n - 1;
  for (int i = start; i >= 0 && rn < MAX_ROWS; i--) {
    char kind = 0;
    String body = peelMarker(s_claude_hist[i], kind);
    String parts[2];
    int pn = wrapLine(body, ROW_BYTES, parts);
    if (rn + pn > MAX_ROWS) break;
    bool is_latest = (i == s_claude_hist_n - 1);

    // Pick row color based on marker kind:
    //   'U' → user prompt (cyan)
    //   'C' → Claude reply (soft amber)
    //   other → tool/system: accent for latest, dim otherwise
    uint32_t c;
    if      (kind == 'U') c = CLR_ACCENT2;
    else if (kind == 'C') c = CLR_WARN;
    else                  c = is_latest ? CLR_ACCENT : CLR_DIM;

    for (int p = pn - 1; p >= 0; p--) {
      for (int k = rn; k > 0; k--) rows[k] = rows[k - 1];
      rows[0] = { parts[p], c, is_latest && (p == 0) };
      rn++;
    }
  }

  // Hard-clip to the left column so long ASCII lines (e.g. Bash(...)) can't
  // paint past the divider into the right column.
  g_canvas.setClipRect(0, LOG_TOP, DIV_X - 1, SCR_H - LOG_TOP);
  for (int i = 0; i < rn; i++) {
    g_canvas.setTextColor(rows[i].color, CLR_BG);
    if (rows[i].bullet) {
      g_canvas.fillCircle(LEFT_PAD + 3, y0 + i * LOG_LINE_H + 6, 2, CLR_ACCENT);
    }
    g_canvas.drawString(rows[i].text, LEFT_PAD + 10, y0 + i * LOG_LINE_H);
  }
  g_canvas.clearClipRect();
}

// Infer Claude's current activity from the newest event we know about —
// prefer pending (not-yet-revealed) over displayed hist so the state label
// reflects what's coming, not what's already on screen.
static const char* claudeActivityLabel(uint32_t& color_out) {
  const String* latest = nullptr;
  if (s_pending_n > 0)           latest = &s_pending[s_pending_n - 1];
  else if (s_claude_hist_n > 0)  latest = &s_claude_hist[s_claude_hist_n - 1];
  if (!latest) { color_out = CLR_DIM; return "等待"; }
  const String& last = *latest;
  // Marker-based classification:
  //   \x01U = user prompt just submitted → Claude is thinking
  //   \x01C = Claude finished replying   → idle
  // Anything else = a tool event (PreToolUse/PostToolUse) → running.
  if (last.length() >= 2 && (uint8_t)last.charAt(0) == 0x01) {
    char k = last.charAt(1);
    if (k == 'U') { color_out = CLR_WARN; return "思考中"; }
    if (k == 'C') { color_out = CLR_GOOD; return "空闲"; }
  }
  color_out = CLR_ACCENT;
  return "运行中";
}

static void drawRightColumn() {
  // Mascot — pulses during recording OR when Claude is actively working.
  // Look at both the pending queue and the visible hist so the animation
  // starts as soon as we hear about a new activity (even before reveal).
  int r = 12;
  auto starts_busy = [](const String& s) {
    // \x01U = user prompt → Claude is thinking → busy.
    // \x01C = Claude reply → idle → not busy.
    // Anything else = tool event → busy.
    if (s.length() >= 2 && (uint8_t)s.charAt(0) == 0x01) {
      return s.charAt(1) != 'C';
    }
    return true;
  };
  bool claude_busy = false;
  if (s_pending_n > 0)          claude_busy = starts_busy(s_pending[s_pending_n-1]);
  else if (s_claude_hist_n > 0) claude_busy = starts_busy(s_claude_hist[s_claude_hist_n-1]);
  if (s_state == ST_RECORDING || (s_state == ST_IDLE && claude_busy)) {
    static const int pulse[] = {10, 11, 13, 14, 13, 11};
    r = pulse[(millis() / 100) % 6];
  }
  draw_claude_mascot(RIGHT_CX, 42, r, CLR_CLAUDE);

  // State label — prefer showing Claude's activity when board itself is idle.
  const char* label = "";
  uint32_t color = CLR_DIM;
  switch (s_state) {
    case ST_CHECKING:  label = "检测";   color = CLR_WARN; break;
    case ST_NO_PC:     label = "无助手"; color = CLR_BAD;  break;
    case ST_IDLE:      label = claudeActivityLabel(color);  break;
    case ST_RECORDING: label = "录音中"; color = CLR_BAD;  break;
    case ST_UPLOADING: label = "识别中"; color = CLR_WARN; break;
    case ST_DONE:      label = "已输入"; color = CLR_GOOD; break;
    case ST_ERROR:     label = "出错";   color = CLR_BAD;  break;
  }
  g_canvas.setFont(&fonts::efontCN_14);
  g_canvas.setTextColor(color, CLR_BG);
  g_canvas.setTextDatum(middle_center);
  g_canvas.drawString(label, RIGHT_CX, 72);

  // Extra info line (timer / age / error detail)
  char extra[24] = "";
  if (s_state == ST_RECORDING) {
    snprintf(extra, sizeof(extra), "%.1fs", (float)s_pcm_len / SAMPLE_RATE);
  } else if (s_state == ST_IDLE && s_claude_age >= 0) {
    if (s_claude_age < 60)        snprintf(extra, sizeof(extra), "%ds", s_claude_age);
    else if (s_claude_age < 3600) snprintf(extra, sizeof(extra), "%dm", s_claude_age / 60);
    else                          snprintf(extra, sizeof(extra), "%dh", s_claude_age / 3600);
  } else if (s_state == ST_ERROR && s_status_text.length() > 0) {
    String e = s_status_text;
    if (e.length() > 6) e = e.substring(0, 6);
    strncpy(extra, e.c_str(), sizeof(extra) - 1);
  }
  if (extra[0]) {
    g_canvas.setFont(&fonts::efontCN_12);
    g_canvas.setTextColor(CLR_DIM, CLR_BG);
    g_canvas.drawString(extra, RIGHT_CX, 92);
  }

  // Button hint, small, below the state info. Two lines so both actions fit.
  g_canvas.setFont(&fonts::efontCN_12);
  g_canvas.setTextColor(CLR_DIM, CLR_BG);
  const char* hint1 = "按住A说话";
  const char* hint2 = "长按B返回";
  if (s_state == ST_RECORDING) { hint1 = "松开A发送"; hint2 = "长按B返回"; }
  else if (s_state == ST_NO_PC) { hint1 = "启动助手"; hint2 = "短按B重试"; }
  g_canvas.drawString(hint1, RIGHT_CX, 110);
  g_canvas.drawString(hint2, RIGHT_CX, 124);
  g_canvas.setTextDatum(top_left);
}

static void drawFooter() {
  // Hint text moved into the right column below the state info. Keeping this
  // empty means the left-column log can run all the way to the bottom edge.
}

static void drawUI() {
  g_canvas.fillScreen(CLR_BG);
  draw_title("Claude 小秘书");

  // Column divider — runs full height below the title bar now that the footer is gone.
  g_canvas.drawFastVLine(DIV_X, 22, SCR_H - 22, CLR_DIM);

  drawLeftColumn();
  drawRightColumn();
  drawFooter();
  push_frame();
}

void app_voicekb_run() {
  // Disable amp during this app — BLE is gone now but mic side effects can
  // still excite the amp. Keeps it quiet.
  M5.Speaker.end();

  if (!s_pcm) {
    s_pcm = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_pcm) {
      s_state = ST_ERROR;
      s_status_text = "PSRAM 分配失败";
      drawUI();
      delay(2000);
      M5.Speaker.begin(); apply_volume();
      return;
    }
  }
  s_pcm_len = 0;

  s_state = ST_CHECKING;
  drawUI();

  // Load last-known PC address, then: try ping; if that fails, run UDP
  // auto-discovery; if that fails too, fall back to NO_PC state.
  loadPcAddress();
  bool pc_ok = pingPC();
  if (!pc_ok) {
    stick_log("info", "ping failed, running UDP discovery");
    if (discoverHelper()) pc_ok = pingPC();
  }
  s_last_ping_t = millis();
  s_state = pc_ok ? ST_IDLE : ST_NO_PC;
  s_idle_entered_t = millis();
  drawUI();

  bool longpress_sent = false;

  while (true) {
    M5.update();
    screen_saver_tick();

    if (M5.BtnB.pressedFor(LONG_PRESS_MS)) {
      if (!longpress_sent) longpress_sent = true;
    }
    if (longpress_sent && !M5.BtnB.isPressed()) {
      M5.Speaker.begin(); apply_volume();
      return;
    }

    // Periodically ping PC when idle to detect helper going away
    if ((s_state == ST_IDLE || s_state == ST_NO_PC) &&
        millis() - s_last_ping_t > 5000) {
      s_last_ping_t = millis();
      bool ok = pingPC();
      if (ok && s_state == ST_NO_PC) { s_state = ST_IDLE; drawUI(); }
      else if (!ok && s_state == ST_IDLE) { s_state = ST_NO_PC; drawUI(); }
    }

    // Poll Claude Code status from helper frequently while idle so the
    // log reflects real-time activity (my tool calls often finish in <1s).
    if (s_state == ST_IDLE && millis() - s_last_claude_poll_t > 300) {
      s_last_claude_poll_t = millis();
      pollClaudeStatus();
    }

    // Animation: reveal queued status events one at a time.
    if (s_state == ST_IDLE && revealPending()) {
      drawUI();
    }

    // Heartbeat log every 10s so we can tell from stick_log.txt that the
    // voice-kb main loop is alive and confirm its view of polling state.
    static uint32_t s_hb_t = 0;
    if (millis() - s_hb_t > 10000) {
      s_hb_t = millis();
      char buf[96];
      const char* st = "?";
      switch (s_state) {
        case ST_CHECKING:  st="CHK"; break;
        case ST_NO_PC:     st="NOPC"; break;
        case ST_IDLE:      st="IDLE"; break;
        case ST_RECORDING: st="REC"; break;
        case ST_UPLOADING: st="UP"; break;
        case ST_DONE:      st="DONE"; break;
        case ST_ERROR:     st="ERR"; break;
      }
      snprintf(buf, sizeof(buf),
               "hb: %s seq=%u pending=%d hist=%d heap=%uK",
               st, (unsigned)s_last_seq_seen, s_pending_n, s_claude_hist_n,
               (unsigned)(ESP.getFreeHeap() / 1024));
      stick_log("debug", buf);
    }

    // Independent UI refresh. Faster when Claude is "busy" so the mascot
    // pulse animation is visible; slower when idle to save CPU/flicker.
    // Consider pending queue too so refresh speeds up even before reveal.
    auto _is_busy_line = [](const String& s) {
      return s.startsWith("执行") || s.startsWith("编辑")
          || s.startsWith("读")   || s.startsWith("思考中");
    };
    bool busy = false;
    if (s_pending_n > 0)          busy = _is_busy_line(s_pending[s_pending_n-1]);
    else if (s_claude_hist_n > 0) busy = _is_busy_line(s_claude_hist[s_claude_hist_n-1]);
    uint32_t refresh_ms = busy ? 300 : 800;
    if ((s_state == ST_IDLE || s_state == ST_NO_PC) &&
        millis() - s_last_ui_refresh_t > refresh_ms) {
      s_last_ui_refresh_t = millis();
      drawUI();
    }

    // Short-press B while NO_PC = retry (ping + re-discover); while IDLE = send test.
    // Ignore B for 2 seconds after entering IDLE so that the release of the
    // B press used to enter this app doesn't accidentally trigger the test.
    if (M5.BtnB.wasReleased() && !longpress_sent) {
      if (s_state == ST_NO_PC && millis() - s_idle_entered_t > 2000) {
        s_state = ST_CHECKING; drawUI();
        bool ok = pingPC();
        if (!ok && discoverHelper()) ok = pingPC();
        s_last_ping_t = millis();
        s_state = ok ? ST_IDLE : ST_NO_PC;
        s_idle_entered_t = millis();
        drawUI();
      } else if (s_state == ST_IDLE && millis() - s_idle_entered_t > 2000) {
        s_state = ST_UPLOADING; drawUI();
        String test = "hello from StickS3";
        if (sendToPC(test)) {
          s_status_text = test;
          s_state = ST_DONE;
        } else {
          s_status_text = "PC 发送失败";
          s_state = ST_ERROR;
        }
        s_done_t = millis();
        drawUI();
      }
    }

    // BtnA = record
    if (s_state == ST_IDLE && M5.BtnA.wasPressed()) {
      startRecording();
      s_state = ST_RECORDING;
      drawUI();
    }
    if (s_state == ST_RECORDING) {
      recordChunk();
      if (!M5.BtnA.isPressed()) {
        stopRecording();
        if (s_pcm_len < SAMPLE_RATE / 4) {
          s_state = ST_IDLE; drawUI();
        } else {
          s_state = ST_UPLOADING; drawUI();
          String text = doSTT();
          if (text.length() > 0) {
            if (sendToPC(text)) {
              s_status_text = text;
              s_state = ST_DONE;
            } else {
              s_status_text = String("PC 发送失败: ") + text;
              s_state = ST_ERROR;
            }
          } else {
            s_status_text = s_stt_error_detail.length() ? s_stt_error_detail : "识别无结果";
            s_state = ST_ERROR;
            stick_log("warn", String("stt fail: ") + s_status_text);
          }
          s_done_t = millis();
          drawUI();
        }
      } else {
        static uint32_t last_ui = 0;
        if (millis() - last_ui > 100) { last_ui = millis(); drawUI(); }
      }
    }

    uint32_t show_ms = (s_state == ST_ERROR) ? 4000 : 2000;
    if ((s_state == ST_DONE || s_state == ST_ERROR) &&
        millis() - s_done_t > show_ms) {
      s_state = pingPC() ? ST_IDLE : ST_NO_PC;
      s_last_ping_t = millis();
      s_done_t = 0;
      drawUI();
    }

    delay(5);
  }
}
