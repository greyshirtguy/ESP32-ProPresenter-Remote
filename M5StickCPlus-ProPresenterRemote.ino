/**
 * M5StickC Plus — ProPresenter Remote (Portrait, Non-blocking, Task/Queue Architecture)
 * ----------------------------------------------------------------------------
 *
 * WiFi and ProPresenter network config is HARD-CODED!!
 * Search "USER CONFIG" and update for your WiFi and ProPresenter network config.
 *
 * READ THIS LIKE A MAP:
 * - loop(): reads buttons → sends tiny command messages to a queue → NEVER blocks.
 * - httpTask (Core 0): owns ALL networking; short timeouts; HTTP/1.0 + Connection: close.
 * - uiTask   (Core 1): owns ALL drawing; builds screens into sprites (off-screen) → blits once.
 * - statusTask: updates a heartbeat counter so you can see the system ticking.
 *
 * DATA FLOW:
 *   Buttons → cmdQ → httpTask → (ProPresenter) → uiQ → uiTask → LCD
 *
 * DESIGN RULES:
 * - Exactly one task touches each “touchy” subsystem:
 *      UI owned by uiTask; Networking owned by httpTask.
 * - Tasks sleep when idle (block on queues or vTaskDelay) → no CPU spin, no stutter.
 * - We cache UI state; redraw only when something changes → zero flicker.
 * - We display connectivity state via card color (green when reachable, red otherwise).
 *
 * GETTING FAMILIAR:
 * - Search for “QUEUE SEND” to see all producer sites, “QUEUE RECEIVE” to see consumers.
 * - Watch for “reachable” computed as (wifiConnected && !gProUnreachable) in uiTask.
 * - Sprites = off-screen buffers; pushSprite() = one clean blit to the display.
 */

#include <M5StickCPlus.h>
#include <Arduino.h>
#include <WiFi.h>
#include <PinButton.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ============================ USER CONFIG ============================
// Wi-Fi + ProPresenter endpoint.
// Update to suit your WiFi and ProPresenter:
char wifiSSID[] = "Reepicheep";
char wifiPassword[] = "UtterEast";
String pro7NetworkAddress = "192.168.1.7";
String pro7NetworkPort    = "50001";

// ========================= BUTTONS / TIMERS =========================
// Debounced front buttons (PinButton library)
// NOTE: loop() only enqueues Click events; httpTask actually performs actions.
PinButton btnM5(37);     // NEXT
PinButton btnAction(39); // PREV

// Background poll cadence and post-trigger “check again soon” delay.
// These are FreeRTOS ticks (converted from ms).
static const TickType_t kPollIntervalTicks = pdMS_TO_TICKS(500);
static const TickType_t kAfterTriggerDelay = pdMS_TO_TICKS(150);

// Reachability flag (driven by pollSlideOnce). When true, we’re treating Pro as down.
// UI uses this to choose red/green card color immediately.
static bool gProUnreachable = false;  // remembers last poll failure state


// ============================ QUEUES ================================
// Command queue: loop() → httpTask (button presses, polls, etc.)
enum CmdType : uint8_t { CMD_POLL, CMD_NEXT, CMD_PREV, CMD_NET_UP, CMD_HOME0 };
struct CmdMsg { CmdType type; };

// UI queue: httpTask → uiTask (status text, slide data, wifi signals)
enum UiType : uint8_t { UI_STATUS, UI_SLIDE, UI_WIFI };
struct UiMsg {
  UiType type;
  char   text[64];             // status text (UI_STATUS/UI_WIFI)
  int    slideIndexOneBased;   // slide number for UI (1-based; -1 if unknown)
  char   presName[64];         // presentation title
};

// Queue handles (created in setup)
static QueueHandle_t cmdQ;
static QueueHandle_t uiQ;

// ========================== MISC / STATS ============================
// Tiny heartbeat so you can visually verify tasks are alive.
// Guarded by a mutex since multiple tasks read/write.
static SemaphoreHandle_t xMutex;
static volatile uint32_t millisTime = 0;

// ============================ COLORS ================================
// Chosen for small TFT legibility (muted grey, bright but not neon greens/reds).
#define COL_BG            TFT_BLACK
#define COL_TEXT          TFT_WHITE
#define COL_MUTED         0x8410      // grey for low-priority text
#define COL_BORDER        0xC618      // light grey outline; visible on small panel
#define COL_GOOD          0x04A0      // green card fill when connected
#define COL_BAD           0xE800      // red card fill when disconnected

// ============================ LAYOUT ================================
// These are computed at runtime from current rotation (portrait).
static int LCD_W = 0, LCD_H = 0;
static int PAD   = 8;    // global inner padding; you tuned this elsewhere too

// Bottom area: status line just above the heartbeat band.
static int STATUS_H = 20;
static int HEART_H  = 12;
static int STATUS_Y = 0;

// Middle: square slide card; below that: title bar (marquee).
static int TITLE_Y  = 0;
static int TITLE_H  = 28;  // title height (divider drawn at its top)
static int CARD_Y   = 0;
static int CARD_H   = 0;

// ============================ SPRITES ===============================
// Sprites = off-screen framebuffers. Draw into them, then push in one blit.
// This avoids flicker/tearing and lets us redraw only regions that change.
TFT_eSprite sprStatus = TFT_eSprite(&M5.Lcd);
TFT_eSprite sprCard   = TFT_eSprite(&M5.Lcd);
TFT_eSprite sprTitle  = TFT_eSprite(&M5.Lcd);

// ============================ LAYOUT (SQUARE CARD) ============================
// Computes portrait layout and (re)allocates region sprites.
// Called in setup (and would be needed again if you change rotation).
void initLayoutAndSprites() {
  LCD_W = M5.Lcd.width();
  LCD_H = M5.Lcd.height();

  PAD       = 8;
  STATUS_H  = 20;
  HEART_H   = 12;

  // Bottom status bar sits just above heartbeat (absolute bottom).
  STATUS_Y  = LCD_H - HEART_H - STATUS_H;

  // Layout (top → bottom): [PAD] [CARD (square)] [PAD] [TITLE] [PAD] [STATUS] [HEART]
  CARD_Y    = PAD;

  // Title just below the card
  TITLE_H   = 28;                  // fixed-ish title band
  // Compute how much vertical space remains for the card, given bottom reservations.
  int remainingBelowCard = STATUS_Y - (CARD_Y + PAD + TITLE_H + PAD);
  if (remainingBelowCard < 50) remainingBelowCard = 50;

  // Make the slide card square (or as large as possible within constraints).
  int innerWidth = LCD_W - 2 * PAD;
  CARD_H = min(innerWidth, remainingBelowCard);
  if (CARD_H < 50) CARD_H = 50;    // safety minimum

  TITLE_Y   = CARD_Y + CARD_H + PAD;

  // Recreate sprites sized to new layout (safe to delete even if not created).
  sprStatus.deleteSprite();
  sprCard.deleteSprite();
  sprTitle.deleteSprite();

  // 8-bit depth is plenty for text/UI; saves RAM vs 16-bit.
  sprStatus.setColorDepth(8);
  sprCard.setColorDepth(8);
  sprTitle.setColorDepth(8);

  sprStatus.createSprite(LCD_W, STATUS_H);
  sprCard.createSprite(LCD_W, CARD_H);
  sprTitle.createSprite(LCD_W, TITLE_H);
}

// ============================ HELPERS ===============================
// Base URL for all ProPresenter v1 HTTP endpoints.
String baseUrl() { return "http://" + pro7NetworkAddress + ":" + pro7NetworkPort; }

// --- UI queue helpers (NEVER draw from httpTask; send messages instead) ---
void enqueueStatus(const char* s)            { UiMsg m{}; m.type=UI_STATUS; strncpy(m.text,s,sizeof(m.text)-1); xQueueSend(uiQ,&m,0); }
void enqueueStatusCode(const char* p,int c)  { UiMsg m{}; m.type=UI_STATUS; snprintf(m.text,sizeof(m.text),"%s %d",p,c); xQueueSend(uiQ,&m,0); }
void enqueueWifi(const char* s)              { UiMsg m{}; m.type=UI_WIFI;   strncpy(m.text,s,sizeof(m.text)-1); xQueueSend(uiQ,&m,0); }
void enqueueSlide(int idx1, const char* nm)  { UiMsg m{}; m.type=UI_SLIDE; m.slideIndexOneBased=idx1; strncpy(m.presName, nm?nm:"", sizeof(m.presName)-1); xQueueSend(uiQ,&m,0); }

// Cooperative, non-blocking-ish Wi-Fi connect.
// - Tries briefly and yields frequently so UI stays responsive.
// - Announces “Wi-Fi OK” via uiQ the moment we connect.
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  static uint32_t lastAttemptMs = 0;
  uint32_t now = millis();
  if (now - lastAttemptMs < 2000) return false;  // rate-limit attempts to avoid churn
  lastAttemptMs = now;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID, wifiPassword);
  enqueueWifi("Wi-Fi…");

  // Quick loop (~1s) with delays → yields CPU to other tasks.
  for (int i=0;i<10;i++){
    if (WiFi.status()==WL_CONNECTED){ enqueueWifi("Wi-Fi OK"); return true; }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  enqueueWifi("No Wi-Fi");
  return false;
}

// ============================ TASKS =================================
// statusTask: low-priority ticker for the on-screen heartbeat.
void statusTask(void*){
  for(;;){
    if (xSemaphoreTake(xMutex, portMAX_DELAY)==pdTRUE){ millisTime = millis(); xSemaphoreGive(xMutex); }
    vTaskDelay(pdMS_TO_TICKS(700));  // sleeps → yields CPU
  }
}

// ------------- Title marquee state (uiTask scope) ----------
// The marquee scrolls when the title is wider than the available width.
// We also add a short left-aligned “pause” so the eye can catch the start.
struct Marquee {
  String   text;
  int      x = 0;              // current scroll offset (pixels)
  int      w = 0;              // text width (px)
  bool     needed = false;     // scrolling required?
  uint32_t lastTick = 0;       // last movement tick (ms)
  uint32_t pauseStart = 0;     // when the current pause began (ms)
  bool     inPause = false;    // true while we show the left-aligned start
} gMarq;

// ------------------- UI drawing -------------------------------------
// Note: uiTask is the ONLY code that touches M5.Lcd or sprites.

void drawStatusBar(const char* text, uint16_t bg, uint16_t fg) {
  // Draw to the status sprite, then blit once to bottom region.
  sprStatus.fillSprite(bg);
  sprStatus.setTextColor(fg, bg);
  sprStatus.setTextDatum(TL_DATUM);
  sprStatus.setTextFont(2);
  sprStatus.setCursor(PAD, max(0, (STATUS_H-16)/2));
  sprStatus.print(text);
  sprStatus.pushSprite(0, STATUS_Y);
}

// ====================== CARD DRAW (AUTO MAX FONT) ====================
// Renders the central rounded card and auto-resizes the slide number to the
// largest clean font that fits neatly in both dimensions.
void drawSlideCard(int idx1, uint16_t panelColor, uint16_t borderColor) {
  sprCard.fillSprite(COL_BG);

  const int radius = 10;
  const int x = PAD, y = 0, w = LCD_W - PAD*2, h = CARD_H;

  // Filled rounded panel
  sprCard.fillRoundRect(x, y, w, h, radius, panelColor);
  // Outline (2D cues help on small TFTs)
  sprCard.drawRoundRect(x, y, w, h, radius, borderColor);

  // Prepare text (two dashes if we don’t have a slide number)
  char buf[8];
  if (idx1 > 0) { snprintf(buf, sizeof(buf), "%d", idx1); }
  else { strcpy(buf, "--"); }

  // Choose the largest neat font that fits both width & height.
  // Try fonts in order: 8 (huge digits), 6 (large digits), 4, 2 (with optional scaling).
  const int innerPad = 2;
  const int maxW = w - 2*innerPad;
  const int maxH = h - 2*innerPad;

  auto fits = [&](int font) {
    sprCard.setTextFont(font);
    int tw = sprCard.textWidth(buf);
    int th = sprCard.fontHeight();
    return (tw <= maxW) && (th <= maxH);
  };

  int chosenFont = 0;

  // Try numeric big fonts first (fast exit on first fit).
  int candidates[] = {8, 6, 4, 2};
  for (int i = 0; i < 4; ++i) {
    if (fits(candidates[i])) { chosenFont = candidates[i]; break; }
  }

  sprCard.setTextDatum(MC_DATUM);
  sprCard.setTextColor(COL_TEXT, panelColor);

  if (chosenFont > 0 && chosenFont != 2) {
    // Found a clean bitmap font that fits.
    sprCard.setTextFont(chosenFont);
  } else if (chosenFont == 2) {
    // Font 2 fits as-is.
    sprCard.setTextFont(2);
  } else {
    // Nothing fit; scale Font 2 down (integer scale) to avoid clipping.
    sprCard.setTextFont(2);
    int baseW = sprCard.textWidth(buf);
    int baseH = sprCard.fontHeight();
    float sx = (baseW > 0) ? (float)maxW / baseW : 1.0f;
    float sy = (baseH > 0) ? (float)maxH / baseH : 1.0f;
    int scale = (int)floorf(min(sx, sy));
    if (scale < 1) scale = 1;
    sprCard.setTextSize(scale);
  }

  // Centered draw and cleanup.
  sprCard.drawString(buf, x + w/2, y + h/2);
  sprCard.setTextSize(1);      // hygiene reset for later calls
  sprCard.pushSprite(0, CARD_Y);
}

// Divider should be BETWEEN card and title → draw at TOP of the title sprite.
// This also contains the marquee (horizontal scroll with a pause at the start).
void drawTitleMarqueeTick(uint16_t bg, uint16_t fg) {
  sprTitle.fillSprite(bg);
  sprTitle.fillRect(0, 0, LCD_W, 2, fg); // divider line between card and title

  sprTitle.setTextDatum(TL_DATUM);
  sprTitle.setTextColor(fg, bg);
  sprTitle.setTextFont(4);

  const int lh   = sprTitle.fontHeight();
  const int y0   = max(2, (TITLE_H - lh)/2 + 2);
  const int maxW = LCD_W - 2*PAD;

  if (!gMarq.needed) {
    // Fits: center it, no animation.
    int centeredX = PAD + (maxW - gMarq.w)/2;
    if (centeredX < PAD) centeredX = PAD;
    sprTitle.setCursor(centeredX, y0);
    sprTitle.print(gMarq.text);
    sprTitle.pushSprite(0, TITLE_Y);
    return;
  }

  // Marquee: ~30% faster — 2 px every 75 ms.
  const uint16_t stepPx = 2;
  const uint16_t tickMs = 75;
  const int spacer = 24;                 // gap between repeats
  const int totalSpan = gMarq.w + spacer;

  uint32_t now = millis();

  // Left-aligned pause at the start (and after each wrap).
  if (gMarq.inPause) {
    // Draw left-aligned start of the text
    sprTitle.setCursor(PAD, y0);
    sprTitle.print(gMarq.text);

    if (now - gMarq.pauseStart >= 500) { // 500 ms pause
      gMarq.inPause = false;
      gMarq.lastTick = now;
      gMarq.x = 0;                        // start scroll from the left edge
    }

    sprTitle.pushSprite(0, TITLE_Y);
    return;
  }

  // Scrolling phase
  if (now - gMarq.lastTick >= tickMs) {
    gMarq.lastTick = now;
    gMarq.x -= stepPx;
    if (gMarq.x < -totalSpan) {
      // Wrap and pause again to show the start
      gMarq.x = 0;
      gMarq.inPause = true;
      gMarq.pauseStart = now;
    }
  }

  // Draw two copies for seamless wrap (classic marquee trick).
  int x1 = PAD + gMarq.x;
  int x2 = x1 + gMarq.w + spacer;
  sprTitle.setCursor(x1, y0); sprTitle.print(gMarq.text);
  sprTitle.setCursor(x2, y0); sprTitle.print(gMarq.text);

  sprTitle.pushSprite(0, TITLE_Y);
}

void uiTask(void*){
  // Screen boot paint
  M5.Lcd.fillScreen(COL_BG);
  initLayoutAndSprites();

  // Initial placeholders
  drawStatusBar("Booting…", COL_BG, COL_MUTED);
  drawSlideCard(-1, COL_BAD, COL_BORDER);   // start pessimistic (not reachable yet)
  gMarq = Marquee{};
  drawTitleMarqueeTick(COL_BG, COL_TEXT);

  // Cached UI state — we redraw only on change to avoid flicker.
  static int     lastSlide = -9999;
  static String  lastTitle = "";
  static char    lastStatus[64] = {0};
  static bool    wifiConnected = false;
  static bool    lastReachable = false;     // track last red/green state we painted

  const TickType_t UI_TICK = pdMS_TO_TICKS(40); // wake often enough for smooth marquee
  UiMsg msg{};
  for(;;){
    // QUEUE RECEIVE (UI): Wait for UI messages, but time out to animate marquee
    if (xQueueReceive(uiQ, &msg, UI_TICK)==pdTRUE){

      if (msg.type == UI_STATUS || msg.type == UI_WIFI) {
        // Track Wi-Fi connectivity (driven by ensureWiFi announcements)
        if (msg.type == UI_WIFI) {
          bool was = wifiConnected;
          wifiConnected = (strstr(msg.text, "OK") || strstr(msg.text, "Connected"));
          (void)was; // we recompute reachability below
        }

        // Pick status color (your simplified rules)
        uint16_t fg = COL_MUTED;
        if (strstr(msg.text, "OK") || strstr(msg.text, "Connected") ||
            strstr(msg.text, "NEXT") || strstr(msg.text, "PREV") ||
            strstr(msg.text, "HOME") || strstr(msg.text, "Wi-Fi")) {
          fg = COL_GOOD;
        } else if (strstr(msg.text, "Unreachable") || strstr(msg.text, "Request Failed")) {
          fg = COL_BAD;
        }

        // Redraw status only when text changes (avoids flicker).
        if (strcmp(lastStatus, msg.text)!=0) {
          strncpy(lastStatus, msg.text, sizeof(lastStatus)-1);
          drawStatusBar(lastStatus, COL_BG, fg);
        }

        // >>> Flip the card color immediately on reachability transitions
        bool reachable = wifiConnected && !gProUnreachable;
        if (reachable != lastReachable) {
          lastReachable = reachable;
          drawSlideCard(lastSlide, reachable ? COL_GOOD : COL_BAD, COL_BORDER);
        }
      }

      else if (msg.type == UI_SLIDE) {
        // Slide number repaint using current reachability color
        bool reachable = wifiConnected && !gProUnreachable;
        if (msg.slideIndexOneBased != lastSlide){
          lastSlide = msg.slideIndexOneBased;
          drawSlideCard(lastSlide, reachable ? COL_GOOD : COL_BAD, COL_BORDER);
        }
        // Title update + marquee initialization
        if (lastTitle != String(msg.presName)){
          lastTitle = String(msg.presName);
          gMarq.text = lastTitle;
          sprTitle.setTextFont(4);
          gMarq.w = sprTitle.textWidth(gMarq.text);
          int maxW = LCD_W - 2*PAD;
          gMarq.needed = (gMarq.w > maxW);
          gMarq.x = 0;
          gMarq.lastTick = millis();
          gMarq.inPause = gMarq.needed;          // pause only if we’ll scroll
          gMarq.pauseStart = gMarq.inPause ? gMarq.lastTick : 0;
          drawTitleMarqueeTick(COL_BG, COL_TEXT);
        }
      }

    } else {
      // Timeout: UI tick for marquee animation
      drawTitleMarqueeTick(COL_BG, COL_TEXT);
    }

    // Heartbeat at bottom (seconds since boot, integer)
    M5.Lcd.setTextColor(COL_MUTED, COL_BG);
    M5.Lcd.setTextFont(1);
    int hbY = LCD_H - 10;
    if (xSemaphoreTake(xMutex, portMAX_DELAY)==pdTRUE){
      M5.Lcd.fillRect(0, hbY-2, 80, 12, COL_BG);
      M5.Lcd.setCursor(PAD, hbY);
      M5.Lcd.printf("%lu", millisTime / 1000); // whole seconds
      xSemaphoreGive(xMutex);
    }
  }
}

// ------------------- Networking (httpTask) --------------------------
// Single-shot GET with short timeouts and HTTP/1.0 (no keep-alive) to avoid
// stall traps that can last seconds on embedded stacks.
bool httpGET_once(const String& url, int& code, String& body) {
  WiFiClient client; client.setNoDelay(true); client.setTimeout(2000);
  HTTPClient http;   http.useHTTP10(true);
  if (!http.begin(client, url)) { code = -1000; return false; }
  http.addHeader("Connection", "close"); http.setTimeout(2000);
  code = http.GET();
  if (code > 0) body = http.getString();
  http.end();
  return code > 0;
}

// Polls /v1/presentation/slide_index.
// On any failure, posts "Pro Unreachable" once and sets gProUnreachable.
// On success, clears the banner and posts slide+title to the UI.
bool pollSlideOnce(String& errMsg) {
  int code = 0; String payload;
  if (!httpGET_once(baseUrl() + "/v1/presentation/slide_index", code, payload)) {
    // Transport-level failure → unreachable
    errMsg = "Poll fail";
    if (!gProUnreachable) { gProUnreachable = true; enqueueStatus("Pro Unreachable"); }
    return false;
  }

  if (code == HTTP_CODE_OK) {
    JSONVar obj = JSON.parse(payload);
    if (JSON.typeof(obj) == "undefined") {
      errMsg = "Bad JSON";
      if (!gProUnreachable) { gProUnreachable = true; enqueueStatus("Pro Unreachable"); }
      return false;
    }

    int idx1 = -1; const char* name = "";
    if (obj.hasOwnProperty("presentation_index")) {
      JSONVar pi = obj["presentation_index"];
      if (pi.hasOwnProperty("index")) idx1 = (int)pi["index"] + 1;
      if (pi.hasOwnProperty("presentation_id") && pi["presentation_id"].hasOwnProperty("name"))
        name = (const char*)pi["presentation_id"]["name"];
    }

    // Success → clear any prior “Unreachable” banner and send fresh UI state.
    if (gProUnreachable) { gProUnreachable = false; enqueueStatus("Pro Connected"); }
    enqueueSlide(idx1, name);
    return true;
  } else {
    // HTTP error codes (404/500/etc.) → treat as unreachable for the UI
    errMsg = "HTTP";
    if (!gProUnreachable) { gProUnreachable = true; enqueueStatus("Pro Unreachable"); }
    return false;
  }
}

// httpTask: runs on Core 0 (with Wi-Fi stack) and owns all network I/O.
// Pattern: drain commands → do scheduled poll → small delay (cooperative).
void httpTask(void*){
  vTaskDelay(pdMS_TO_TICKS(50));                 // stagger boot a hair
  TickType_t nextForcedPoll = 0, lastBgPoll = xTaskGetTickCount();

  for(;;){
    // QUEUE RECEIVE (CMD): drain without blocking to be snappy
    CmdMsg cmd; bool didWork=false;
    while (xQueueReceive(cmdQ, &cmd, 0)==pdTRUE){
      didWork=true;
      switch(cmd.type){
        case CMD_NEXT:{
          enqueueStatus("NEXT…");
          if (ensureWiFi()){
            int code=0; String body;
            if (httpGET_once(baseUrl()+"/v1/trigger/next", code, body) && code==HTTP_CODE_NO_CONTENT){
              enqueueStatus("NEXT OK");
              nextForcedPoll = xTaskGetTickCount() + kAfterTriggerDelay; // “check soon”
            } else enqueueStatusCode("Request Failed", code);
          }
        } break;
        case CMD_PREV:{
          enqueueStatus("PREV…");
          if (ensureWiFi()){
            int code=0; String body;
            if (httpGET_once(baseUrl()+"/v1/trigger/previous", code, body) && code==HTTP_CODE_NO_CONTENT){
              enqueueStatus("PREV OK");
              nextForcedPoll = xTaskGetTickCount() + kAfterTriggerDelay;
            } else enqueueStatusCode("Request Failed", code);
          }
        } break;
        case CMD_HOME0:{
          enqueueStatus("HOME…");
          if (ensureWiFi()){
            int code=0; String body;
            if (httpGET_once(baseUrl()+"/v1/presentation/active/0/trigger", code, body) && code==HTTP_CODE_NO_CONTENT){
              enqueueStatus("HOME OK");
              nextForcedPoll = xTaskGetTickCount() + kAfterTriggerDelay;
            } else enqueueStatusCode("Request Failed", code);
          }
        } break;
        case CMD_NET_UP: (void)ensureWiFi(); break;  // gentle reconnect attempts
        case CMD_POLL:{
          if (ensureWiFi()){
            String err; if (!pollSlideOnce(err)){ vTaskDelay(pdMS_TO_TICKS(150)); pollSlideOnce(err); }
          }
        } break;
      }
    }

    // “Check soon” after NEXT/PREV/HOME to pick up the new slide index.
    TickType_t now = xTaskGetTickCount();
    if (nextForcedPoll && now >= nextForcedPoll){
      nextForcedPoll = 0;
      if (ensureWiFi()){
        String err; if (!pollSlideOnce(err)){ vTaskDelay(pdMS_TO_TICKS(150)); pollSlideOnce(err); }
      }
    }

    // Background cadence poll (only if we didn’t just do work).
    if (!didWork && (now - lastBgPoll) >= kPollIntervalTicks){
      lastBgPoll = now;
      if (ensureWiFi()){
        String err; if (!pollSlideOnce(err)){ vTaskDelay(pdMS_TO_TICKS(150)); pollSlideOnce(err); }
      }
    }

    // Friendly yield; keeps CPU cool and lets other tasks run.
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================== SETUP/LOOP ==========================
// setup(): hardware init, portrait rotation, queue + task creation.
void setup() {
  xMutex = xSemaphoreCreateMutex();
  M5.begin();
  M5.Lcd.setRotation(0);   // Portrait; change to 2 if your device orientation prefers it
  initLayoutAndSprites();

  M5.Lcd.fillScreen(COL_BG);
  M5.Lcd.setTextColor(COL_TEXT, COL_BG);
  WiFi.setSleep(false);    // reduce latency variability

  // Create small, fixed-size queues (tiny messages = predictable + fast).
  cmdQ = xQueueCreate(16, sizeof(CmdMsg));
  uiQ  = xQueueCreate(16, sizeof(UiMsg));

  // Create tasks:
  // - uiTask on Core 1 (with Arduino loop)
  // - httpTask on Core 0 (same core as Wi-Fi stack) → fewer cross-core hops
  xTaskCreatePinnedToCore(statusTask, "statusTask", 2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(uiTask,     "uiTask",     6144, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(httpTask,   "httpTask",   6144, nullptr, 4, nullptr, 0);

  // Kick off an initial poll
  enqueueWifi("Booting");
  CmdMsg c{CMD_POLL}; xQueueSend(cmdQ, &c, 0);  // QUEUE SEND (CMD)
}

// loop(): reads buttons, enqueues commands, and never blocks.
void loop() {
  M5.update();
  btnAction.update();
  btnM5.update();

  // Button → command (NEXT/PREV). Keep loop fast: enqueue and move on.
  if (btnAction.isClick()) { enqueueStatus("CLICK PREV"); CmdMsg c{CMD_PREV}; xQueueSend(cmdQ, &c, 0); }  // QUEUE SEND (CMD)
  if (btnM5.isClick())     { enqueueStatus("CLICK NEXT"); CmdMsg c{CMD_NEXT}; xQueueSend(cmdQ, &c, 0); }  // QUEUE SEND (CMD)

  // Power button short press → HOME (index 0) using AXP event semantics.
  int p = M5.Axp.GetBtnPress(); // 2 = short tap, 1 = long press, 0 = none
  if (p == 2) {
    enqueueStatus("PWR→HOME");
    CmdMsg c{CMD_HOME0}; xQueueSend(cmdQ, &c, 0);  // QUEUE SEND (CMD)
  }

  // Gentle reconnect nudges when disconnected (rate limited).
  static uint32_t lastNetKick = 0;
  uint32_t now = millis();
  if (now - lastNetKick > 3000) {
    lastNetKick = now;
    if (WiFi.status() != WL_CONNECTED) { CmdMsg c{CMD_NET_UP}; xQueueSend(cmdQ, &c, 0); }  // QUEUE SEND (CMD)
  }

  // Wi-Fi status transition announcer → keeps bottom status honest.
  static bool lastConn = false;
  bool nowConn = (WiFi.status() == WL_CONNECTED);
  if (nowConn != lastConn) {
    lastConn = nowConn;
    enqueueWifi(nowConn ? "Wi-Fi OK" : "No Wi-Fi");  // QUEUE SEND (UI)
  }

  // Be cooperative with the scheduler (loop runs on Core 1).
  vTaskDelay(pdMS_TO_TICKS(10));
}
