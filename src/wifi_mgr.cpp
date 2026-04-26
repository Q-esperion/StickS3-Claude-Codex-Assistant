#include "app.h"
#include "xfyun_config.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <vector>

static bool s_connected = false;
static String s_ssid;

// Injected into every WiFiManager portal page's <head>. On DOM load it walks
// text nodes + placeholders/labels and substitutes Chinese for the English
// strings that WiFiManager hard-codes in wm_strings_en.h. No library fork.
static const char WM_HEAD_CN[] = R"WMHEAD(
<style>body{font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}</style>
<script>
const _T={
"Configure WiFi":"配置 WiFi",
"Configure WiFi (No scan)":"手动输入 WiFi",
"Info":"信息",
"Exit":"退出",
"Close":"关闭",
"Update":"更新",
"Restart":"重启",
"Setup":"设置",
"Setup saved":"设置已保存",
"Erase":"擦除",
"Erase WiFi config":"清除 WiFi 设置",
"Save":"保存",
"Back":"返回",
"Refresh":"刷新",
"Show Password":"显示密码",
"SSID":"WiFi 名称",
"Password":"密码",
"Static IP":"静态 IP",
"Static gateway":"静态网关",
"Static DNS":"静态 DNS",
"Subnet":"子网掩码",
"No networks found. Refresh to scan again.":"未扫描到 WiFi，点刷新重试。",
"Saving Credentials":"正在保存",
"Trying to connect ESP to network.":"设备正在连接网络…",
"If it fails reconnect to AP to try again":"如连接失败请重新连接此热点重试",
"Credentials saved":"凭证已保存",
"No AP set":"未配置 WiFi",
"Connected":"已连接",
"Not connected":"未连接",
"Exiting":"退出中",
"Upload new firmware":"上传固件",
"Update failed!":"更新失败",
"Update successful.":"更新成功"
};
function _tr(s){const k=s.trim();return _T[k]?s.replace(k,_T[k]):s;}
addEventListener("DOMContentLoaded",()=>{
  const w=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);
  let n;while(n=w.nextNode()){const v=n.nodeValue;const t=v.trim();if(t&&_T[t])n.nodeValue=v.replace(t,_T[t]);}
  document.querySelectorAll("input[placeholder]").forEach(e=>{if(_T[e.placeholder])e.placeholder=_T[e.placeholder];});
});
</script>
)WMHEAD";

static void drawPortalScreen() {
  g_canvas.fillScreen(CLR_BG);
  draw_title("WiFi 配网");
  g_canvas.setFont(&fonts::efontCN_14);
  g_canvas.setTextColor(CLR_TEXT, CLR_BG);
  int y = 28;
  g_canvas.setCursor(6, y); g_canvas.print("1) 手机连接热点：");
  y += 16;
  g_canvas.setTextColor(CLR_ACCENT, CLR_BG);
  g_canvas.setCursor(18, y); g_canvas.print("StickS3-Setup");
  y += 16;
  g_canvas.setTextColor(CLR_TEXT, CLR_BG);
  g_canvas.setCursor(6, y); g_canvas.print("2) 浏览器打开：");
  y += 14;
  g_canvas.setTextColor(CLR_ACCENT, CLR_BG);
  g_canvas.setCursor(18, y); g_canvas.print("http://192.168.4.1");
  y += 14;
  g_canvas.setTextColor(CLR_TEXT, CLR_BG);
  g_canvas.setCursor(6, y); g_canvas.print("3) 同页底部填讯飞");
  y += 14;
  g_canvas.setTextColor(CLR_DIM, CLR_BG);
  g_canvas.setCursor(6, y); g_canvas.print("长按 B 退出配网");
  push_frame();
}

void wifi_setup(bool force_portal) {
  g_canvas.fillScreen(CLR_BG);
  draw_title("WiFi");
  g_canvas.setFont(&fonts::efontCN_16);
  g_canvas.setTextColor(CLR_TEXT, CLR_BG);
  g_canvas.setCursor(6, 32); g_canvas.print("正在连接...");
  push_frame();

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConnectTimeout(15);
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager* m) {
    drawPortalScreen();
  });

  // Chinese-ize the captive-portal pages. Title is the <h1> at the top;
  // the injected script runs on DOM load and swaps known English strings
  // for Chinese. Menu is trimmed to the two actions a user ever needs.
  wm.setTitle("StickS3 配网");
  wm.setCustomHeadElement(WM_HEAD_CN);
  std::vector<const char*> menu = { "wifi", "exit" };
  wm.setMenu(menu);

  // Extra site config on the same form: city code (for itboy weather) and
  // Bilibili UID (for the dashboard follower count). Defaults are empty so
  // personal location/account data is not baked into open-source builds.
  Preferences site_p;
  site_p.begin("site", true);
  String cur_city = site_p.getString("city", "");
  String cur_uid  = site_p.getString("uid",  "");
  site_p.end();
  String cur_xf_appid, cur_xf_key, cur_xf_secret;
  xfyun_load_credentials(cur_xf_appid, cur_xf_key, cur_xf_secret);

  WiFiManagerParameter p_city_hdr(
      "<br/><p><b>城市代码</b>（9 位，itboy 天气 API；杭州 101210101，北京 101010100，上海 101020100）</p>");
  WiFiManagerParameter p_city("city", "城市代码", cur_city.c_str(), 12);
  WiFiManagerParameter p_uid_hdr("<br/><p><b>B 站 UID</b>（个人主页 URL 末尾数字；留空不显示）</p>");
  WiFiManagerParameter p_uid("uid", "B 站 UID", cur_uid.c_str(), 16);
  WiFiManagerParameter p_xf_hdr(
      "<br/><p><b>科大讯飞语音识别</b>（IAT 应用的 APPID / APISecret / APIKey）</p>"
      "<p>推荐把三项一起粘到下面一栏，例如 APPID=... APISecret=... APIKey=...；留空则使用单独三栏。</p>");
  WiFiManagerParameter p_xf_bundle("xf_all", "一键粘贴三项", "", 220);
  WiFiManagerParameter p_xf_appid("xf_appid", "讯飞 APPID", cur_xf_appid.c_str(), 24);
  WiFiManagerParameter p_xf_secret("xf_secret", "讯飞 APISecret", cur_xf_secret.c_str(), 96);
  WiFiManagerParameter p_xf_key("xf_key", "讯飞 APIKey", cur_xf_key.c_str(), 80);
  wm.addParameter(&p_city_hdr);
  wm.addParameter(&p_city);
  wm.addParameter(&p_uid_hdr);
  wm.addParameter(&p_uid);
  wm.addParameter(&p_xf_hdr);
  wm.addParameter(&p_xf_bundle);
  wm.addParameter(&p_xf_appid);
  wm.addParameter(&p_xf_secret);
  wm.addParameter(&p_xf_key);

  bool portal_params_saved = false;
  auto savePortalParams = [&]() {
    Preferences save_p;
    save_p.begin("site", false);
    save_p.putString("city", p_city.getValue());
    save_p.putString("uid",  p_uid.getValue());
    save_p.end();
    String appid = p_xf_appid.getValue();
    String key = p_xf_key.getValue();
    String secret = p_xf_secret.getValue();
    String parsed_appid, parsed_key, parsed_secret;
    if (xfyun_parse_credentials_blob(String(p_xf_bundle.getValue()), parsed_appid, parsed_key, parsed_secret)) {
      appid = parsed_appid;
      key = parsed_key;
      secret = parsed_secret;
      stick_log("info", "xfyun config parsed from bundle");
    }
    xfyun_save_credentials(appid.c_str(), key.c_str(), secret.c_str());
    portal_params_saved = true;
  };
  wm.setSaveParamsCallback(savePortalParams);

  if (force_portal) {
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal("StickS3-Setup");
    stick_log("info", "wifi portal started; long B exits");

    uint32_t portal_start = millis();
    bool longpress_sent = false;
    s_connected = false;
    while (true) {
      M5.update();
      wm.process();

      if (M5.BtnB.pressedFor(LONG_PRESS_MS)) {
        if (!longpress_sent) {
          beep_ok();
          longpress_sent = true;
        }
      }
      if (longpress_sent && !M5.BtnB.isPressed()) {
        wm.stopConfigPortal();
        s_connected = (WiFi.status() == WL_CONNECTED);
        stick_log("info", "wifi portal exited by long B");
        break;
      }

      if (millis() - portal_start > 300000UL) {
        wm.stopConfigPortal();
        s_connected = (WiFi.status() == WL_CONNECTED);
        break;
      }

      delay(20);
    }
  } else {
    s_connected = wm.autoConnect("StickS3-Setup");
  }

  // Persist whatever's in the param fields now. If the portal ran and the
  // user edited them, getValue() returns the new strings; if autoConnect
  // used cached creds without showing the portal, getValue() still returns
  // the defaults we seeded — writing them back is a no-op.
  savePortalParams();

  g_canvas.fillScreen(CLR_BG);
  draw_title("WiFi");
  g_canvas.setFont(&fonts::efontCN_16);
  if (s_connected) {
    // Kick off NTP time sync globally so apps that need signed timestamps
    // (e.g. voice keyboard → iFlytek) have valid time.
    configTzTime("CST-8", "ntp.aliyun.com", "ntp.ntsc.ac.cn", "pool.ntp.org");

    // Keep the radio alive across transient drops (AP reboots, weak signal).
    // setAutoReconnect makes the Arduino core retry in the background; our
    // own event handler nudges it faster and logs each recovery.
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
      if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        WiFi.reconnect();
      }
    });

    s_ssid = WiFi.SSID();
    if (portal_params_saved) {
      String xf_appid, xf_key, xf_secret;
      bool xf_ok = xfyun_load_credentials(xf_appid, xf_key, xf_secret);
      stick_log("info", String("xfyun config saved: ")
                         + (xf_ok ? "ok" : "missing")
                         + " appid_len=" + xf_appid.length()
                         + " key_len=" + xf_key.length()
                         + " secret_len=" + xf_secret.length());
    }
    g_canvas.setTextColor(CLR_GOOD, CLR_BG);
    g_canvas.setCursor(6, 32); g_canvas.print("已连接:");
    g_canvas.setTextColor(CLR_TEXT, CLR_BG);
    g_canvas.setCursor(6, 52); g_canvas.print(s_ssid);
    g_canvas.setCursor(6, 72); g_canvas.print(WiFi.localIP().toString());
  } else {
    s_ssid = "";
    g_canvas.setTextColor(CLR_WARN, CLR_BG);
    g_canvas.setCursor(6, 32); g_canvas.print("离线模式");
    g_canvas.setFont(&fonts::efontCN_14);
    g_canvas.setTextColor(CLR_DIM, CLR_BG);
    g_canvas.setCursor(6, 56); g_canvas.print("稍后可在桌面重试");
  }
  push_frame();
  delay(1200);
}

bool wifi_is_connected() { return WiFi.status() == WL_CONNECTED; }
const char* wifi_ssid()  { return s_ssid.c_str(); }

void wifi_clear_saved_config() {
  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.resetSettings();
  WiFi.disconnect(true, true);

  Preferences site_p;
  site_p.begin("site", false);
  site_p.clear();
  site_p.end();

  xfyun_clear_credentials();
  s_connected = false;
  s_ssid = "";
  stick_log("info", "wifi/xfyun config cleared");
}
