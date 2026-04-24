#include "app.h"
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
  g_canvas.setCursor(6, y); g_canvas.print("3) 选 WiFi 输密码保存");
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
  // Bilibili UID (for the dashboard follower count). Defaults fall back to
  // the values we had hardcoded originally so a blank NVS still works.
  Preferences site_p;
  site_p.begin("site", true);
  String cur_city = site_p.getString("city", "101210101");
  String cur_uid  = site_p.getString("uid",  "8466490");
  site_p.end();

  WiFiManagerParameter p_city_hdr(
      "<br/><p><b>城市代码</b>（9 位，itboy 天气 API；杭州 101210101，北京 101010100，上海 101020100）</p>");
  WiFiManagerParameter p_city("city", "城市代码", cur_city.c_str(), 12);
  WiFiManagerParameter p_uid_hdr("<br/><p><b>B 站 UID</b>（个人主页 URL 末尾数字；留空不显示）</p>");
  WiFiManagerParameter p_uid("uid", "B 站 UID", cur_uid.c_str(), 16);
  wm.addParameter(&p_city_hdr);
  wm.addParameter(&p_city);
  wm.addParameter(&p_uid_hdr);
  wm.addParameter(&p_uid);

  if (force_portal) {
    s_connected = wm.startConfigPortal("StickS3-Setup");
  } else {
    s_connected = wm.autoConnect("StickS3-Setup");
  }

  // Persist whatever's in the param fields now. If the portal ran and the
  // user edited them, getValue() returns the new strings; if autoConnect
  // used cached creds without showing the portal, getValue() still returns
  // the defaults we seeded — writing them back is a no-op.
  {
    Preferences save_p;
    save_p.begin("site", false);
    save_p.putString("city", p_city.getValue());
    save_p.putString("uid",  p_uid.getValue());
    save_p.end();
  }

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
