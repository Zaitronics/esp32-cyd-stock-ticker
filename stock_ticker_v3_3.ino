/*
 * ESP32 CYD Stock Ticker — v3.3
 * Board: ESP32-2432S028 ("Cheap Yellow Display")
 *
 * Displays live stock and crypto prices on the built-in 320×240 TFT.
 * Tap any ticker to see a full-screen detail view; tap again to go back.
 * Configure everything — API key, tickers, alerts, portfolio — via a
 * browser at the device's IP address.
 *
 * Data sources:
 *   Stocks  → Finnhub  (free tier, requires API key from finnhub.io)
 *   Crypto  → CoinGecko (free tier, no key needed)
 *
 * Libraries required (install via Arduino Library Manager):
 *   TFT_eSPI · XPT2046_Touchscreen · WiFiManager · ArduinoJson
 *
 * TFT_eSPI must be configured for the CYD before compiling.
 * Copy the correct User_Setup.h from the project repo into the library folder.
 */

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>


// ─── Hardware pins ────────────────────────────────────────────────────────────
// CYD pin assignments — do not change unless you rewire the board.
#define TOUCH_CS_PIN  33   // XPT2046 chip select
#define TOUCH_IRQ_PIN 36   // touch interrupt (active low)
#define BL_PIN        21   // TFT backlight (PWM)
#define LED_R          4   // RGB LED — active low
#define LED_G         16
#define LED_B         17


// ─── Global objects ───────────────────────────────────────────────────────────
// The touch controller uses the HSPI bus (separate from the TFT's VSPI).
SPIClass            touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
TFT_eSPI            tft = TFT_eSPI();
Preferences         prefs;   // ESP32 NVS (non-volatile storage)
WebServer           server(80);


// ─── Configuration constants ──────────────────────────────────────────────────
// Refresh rate is intentionally conservative for free API tiers.
// Worst case: 8 tickers + 1 market-status call = 9 requests, each 800 ms apart
// ≈ 7 s total fetch time. 15 s default leaves comfortable headroom.
// Finnhub free: 60 req/min · CoinGecko free: 30 req/min.
#define MAX_TICKERS     8
#define DEFAULT_REFRESH 15   // seconds between fetches
#define MIN_REFRESH     10   // user-adjustable floor
#define HEADER_H        26   // pixel height of the top status bar
#define FOOTER_H        18   // pixel height of the portfolio summary bar


// ─── User settings (persisted to NVS) ────────────────────────────────────────
String apiKey        = "";
String tickers[MAX_TICKERS];
float  holdings[MAX_TICKERS];   // number of shares/units held (for portfolio mode)
float  alertHigh[MAX_TICKERS];  // price alert — high threshold (0 = disabled)
float  alertLow[MAX_TICKERS];   // price alert — low threshold  (0 = disabled)
int    tickerCount   = 0;
int    refreshSec    = DEFAULT_REFRESH;
int    brightness    = 200;
bool   darkMode      = true;
bool   portfolioMode = false;


// ─── Quote data ───────────────────────────────────────────────────────────────
// One Quote struct is kept per ticker and updated by the background fetch task.
struct Quote {
  String sym;       // raw ticker string, e.g. "AAPL" or "CRYPTO:bitcoin"
  float  price;     // last trade / current price in USD
  float  pct;       // percentage change (day for stocks, 24 h for crypto)
  float  open;      // opening price used to calculate day P&L
  bool   valid;     // false until a successful fetch has completed
  int    errors;    // consecutive error count shown in the UI
  bool   isCrypto;
};

Quote quotes[MAX_TICKERS];


// ─── Runtime state ────────────────────────────────────────────────────────────
// dataMutex protects the quotes[] array which is written on core 0 (fetch task)
// and read on core 1 (display/loop). Always take the mutex before accessing it.
SemaphoreHandle_t dataMutex;
volatile bool     fetchPending = true;   // set true to request a new fetch cycle
volatile bool     fetching     = false;  // true while the fetch task is running
unsigned long     lastFetch    = 0;      // millis() timestamp of last completed fetch

enum ViewMode { VIEW_GRID, VIEW_DETAIL };
ViewMode viewMode  = VIEW_GRID;
int      detailIdx = 0;       // which ticker is shown in detail view
bool     marketOpen  = false;
bool     marketKnown = false; // false until the first successful market-status call

bool          touchWasDown    = false;
unsigned long lastTouchAction = 0;
#define TOUCH_DEBOUNCE_MS 300   // ignore repeat presses within this window

unsigned long lastWifiCheck = 0;
#define WIFI_CHECK_MS 30000     // how often to verify WiFi is still connected


// ─── Theme colours ────────────────────────────────────────────────────────────
// Colours are expressed as 16-bit RGB565 values (TFT_eSPI native format).
// Functions rather than macros let them react to the darkMode flag at runtime.
uint16_t C_BG()     { return darkMode ? (uint16_t)TFT_BLACK : (uint16_t)0xEF7D; }
uint16_t C_HEADER() { return darkMode ? (uint16_t)0x1082    : (uint16_t)0x18C3; }
uint16_t C_BORDER() { return darkMode ? (uint16_t)0x4208    : (uint16_t)0x8410; }
uint16_t C_LABEL()  { return darkMode ? (uint16_t)0xAD75    : (uint16_t)0x4208; }
uint16_t C_PANEL()  { return darkMode ? (uint16_t)0x0841    : (uint16_t)0xFFFF; }
uint16_t C_TEXT()   { return darkMode ? (uint16_t)TFT_WHITE : (uint16_t)TFT_BLACK; }
uint16_t C_MUTED()  { return darkMode ? (uint16_t)0x528A    : (uint16_t)0x8410; }

// Fixed colours — these don't change with the theme.
#define C_UP       0x07E0   // green  — price up
#define C_DOWN     0xF800   // red    — price down
#define C_FLAT     0x7BEF   // grey   — no meaningful change (within ±0.05%)
#define C_MKTOPEN  0x07E0   // green dot — market open
#define C_MKTCLOSE 0xF800   // red dot   — market closed
#define C_ALERT    0xFFE0   // yellow    — price alert indicator


// ─── LED helpers ─────────────────────────────────────────────────────────────
// The CYD's RGB LED is active-low, so we invert the logic level.
// Usage: setLED(true, false, false) → red on, green off, blue off.
void setLED(bool r, bool g, bool b) {
  digitalWrite(LED_R, !r);
  digitalWrite(LED_G, !g);
  digitalWrite(LED_B, !b);
}

// Backlight brightness via PWM. Valid range: 10 (dim) – 255 (full).
void applyBrightness() {
  analogWriteFrequency(BL_PIN, 5000);
  analogWrite(BL_PIN, brightness);
}

void setBrightness(int val) {
  brightness = constrain(val, 10, 255);
  applyBrightness();
}


// ─── Ticker string helpers ────────────────────────────────────────────────────
// Crypto tickers are stored with a "CRYPTO:" prefix so we know which API to use.
// Example: "CRYPTO:bitcoin" → fetched from CoinGecko, displayed as "BITCOIN".
bool   isCryptoTicker(const String &s) { return s.startsWith("CRYPTO:"); }
String cryptoId(const String &s)       { return s.substring(7); } // strip prefix

// Returns the short label shown on screen (max 6 chars for crypto).
String displaySym(const String &sym) {
  if (isCryptoTicker(sym)) {
    String id = cryptoId(sym);
    id.toUpperCase();
    if (id.length() > 8) id = id.substring(0, 8);
    return id;
  }
  return sym;
}


// ─── Preferences (NVS) ───────────────────────────────────────────────────────
// Settings are stored in the ESP32's built-in flash under the "ticker" namespace.
// loadPrefs() is called once at boot; savePrefs() is called after the web form
// is submitted. Defaults are applied when no saved data exists.

void loadPrefs() {
  prefs.begin("ticker", true); // read-only
  apiKey        = prefs.getString("apikey",    "");
  refreshSec    = prefs.getInt("refresh",      DEFAULT_REFRESH);
  brightness    = prefs.getInt("bright",       200);
  darkMode      = prefs.getBool("dark",        true);
  portfolioMode = prefs.getBool("portfolio",   false);
  tickerCount   = prefs.getInt("tcount",       0);
  for (int i = 0; i < tickerCount; i++) {
    tickers[i]   = prefs.getString(("t"  + String(i)).c_str(), "");
    holdings[i]  = prefs.getFloat( ("h"  + String(i)).c_str(), 0.0f);
    alertHigh[i] = prefs.getFloat( ("ah" + String(i)).c_str(), 0.0f);
    alertLow[i]  = prefs.getFloat( ("al" + String(i)).c_str(), 0.0f);
  }
  prefs.end();

  // First run — populate with three default tickers.
  if (tickerCount == 0) {
    tickers[0] = "AAPL"; holdings[0] = 0; alertHigh[0] = 0; alertLow[0] = 0;
    tickers[1] = "MSFT"; holdings[1] = 0; alertHigh[1] = 0; alertLow[1] = 0;
    tickers[2] = "NVDA"; holdings[2] = 0; alertHigh[2] = 0; alertLow[2] = 0;
    tickerCount = 3;
  }

  // Initialise the quote structs so the display has something to render immediately.
  for (int i = 0; i < tickerCount; i++) {
    quotes[i]          = Quote{};
    quotes[i].sym      = tickers[i];
    quotes[i].isCrypto = isCryptoTicker(tickers[i]);
  }
}

void savePrefs() {
  prefs.begin("ticker", false); // read-write
  prefs.putString("apikey",    apiKey);
  prefs.putInt("refresh",      refreshSec);
  prefs.putInt("bright",       brightness);
  prefs.putBool("dark",        darkMode);
  prefs.putBool("portfolio",   portfolioMode);
  prefs.putInt("tcount",       tickerCount);
  for (int i = 0; i < tickerCount; i++) {
    prefs.putString(("t"  + String(i)).c_str(), tickers[i]);
    prefs.putFloat( ("h"  + String(i)).c_str(), holdings[i]);
    prefs.putFloat( ("ah" + String(i)).c_str(), alertHigh[i]);
    prefs.putFloat( ("al" + String(i)).c_str(), alertLow[i]);
  }
  prefs.end();
}


// ─── API: market status (Finnhub) ────────────────────────────────────────────
// Fetches whether the US stock market is currently open. The result drives the
// coloured dot in the header. Skipped if no API key is configured.
void fetchMarketStatus() {
  if (apiKey.isEmpty()) return;
  HTTPClient http;
  http.begin("https://finnhub.io/api/v1/stock/market-status?exchange=US&token=" + apiKey);
  http.setTimeout(6000);
  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      marketOpen  = doc["isOpen"] | false;
      marketKnown = true;
    }
  }
  http.end();
}


// ─── API: stock quote (Finnhub) ───────────────────────────────────────────────
// Fetches the current price and today's open for a single stock ticker.
// The mutex is taken only to write the completed Quote, keeping the lock
// window as short as possible.
void fetchFinnhub(int idx) {
  if (apiKey.isEmpty()) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    quotes[idx].errors++;
    xSemaphoreGive(dataMutex);
    return;
  }

  String url = "https://finnhub.io/api/v1/quote?symbol=" + tickers[idx] + "&token=" + apiKey;
  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);

  Quote q;
  q.sym      = tickers[idx];
  q.valid    = false;
  q.errors   = quotes[idx].errors;
  q.isCrypto = false;

  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      float c = doc["c"]; // current price
      float o = doc["o"]; // today's open
      float pc = doc["pc"];  // previous close
      
      if (c > 0) {
        q.price = c;
        q.open  = pc;
        q.pct   = (pc > 0) ? ((c - pc) / pc * 100.0f) : 0.0f;
        q.valid = true;
        q.errors = 0;
      } else {
        q.errors++;
      }
    } else {
      q.errors++;
    }
  } else {
    q.errors++;
  }
  http.end();

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  quotes[idx] = q;
  xSemaphoreGive(dataMutex);
}


// ─── API: crypto quote (CoinGecko) ───────────────────────────────────────────
// Fetches the current USD price and 24-hour change for a single crypto asset.
// No API key is required. The "open" price is back-calculated from the
// 24-hour change so day P&L works the same way as stocks.
void fetchCoinGecko(int idx) {
  String id  = cryptoId(tickers[idx]); // e.g. "bitcoin", "ethereum"
  String url = "https://api.coingecko.com/api/v3/simple/price?ids=" + id
             + "&vs_currencies=usd&include_24hr_change=true";
  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  http.addHeader("Accept", "application/json");

  Quote q;
  q.sym      = tickers[idx];
  q.valid    = false;
  q.errors   = quotes[idx].errors;
  q.isCrypto = true;

  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      float price  = doc[id]["usd"]            | 0.0f;
      float change = doc[id]["usd_24h_change"] | 0.0f;
      if (price > 0) {
        q.price  = price;
        q.pct    = change;
        q.open   = price / (1.0f + change / 100.0f); // reverse-calculate open
        q.valid  = true;
        q.errors = 0;
      } else {
        q.errors++;
      }
    } else {
      q.errors++;
    }
  } else {
    q.errors++;
  }
  http.end();

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  quotes[idx] = q;
  xSemaphoreGive(dataMutex);
}


// ─── Price alerts ─────────────────────────────────────────────────────────────
// After every fetch cycle, check whether any ticker has crossed its alert
// threshold. If so, flash the RGB LED yellow (red + green) three times.
void checkAlerts() {
  bool any = false;
  for (int i = 0; i < tickerCount; i++) {
    if (!quotes[i].valid) continue;
    if (alertHigh[i] > 0 && quotes[i].price >= alertHigh[i]) any = true;
    if (alertLow[i]  > 0 && quotes[i].price <= alertLow[i])  any = true;
  }
  if (any) {
    for (int f = 0; f < 3; f++) {
      digitalWrite(LED_R, LOW); digitalWrite(LED_G, LOW); // LED on (active low)
      delay(150);
      digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); // LED off
      delay(150);
    }
  }
}


// ─── Fetch task (runs on core 0) ─────────────────────────────────────────────
// The ESP32 has two cores. The display and web server run on core 1 (loop()).
// This task runs permanently on core 0, sleeping until fetchPending is set.
// Staggering requests by 800 ms keeps us safely within both API rate limits.
void fetchTask(void* param) {
  for (;;) {
    if (fetchPending) {
      fetchPending = false;
      fetching     = true;
      setLED(false, false, true); // blue = fetching

      fetchMarketStatus();

      for (int i = 0; i < tickerCount; i++) {
        if (isCryptoTicker(tickers[i])) fetchCoinGecko(i);
        else                             fetchFinnhub(i);
        vTaskDelay(pdMS_TO_TICKS(800)); // pace requests to respect rate limits
      }

      checkAlerts();
      lastFetch = millis();
      fetching  = false;
      setLED(false, false, false); // LED off
    }
    vTaskDelay(pdMS_TO_TICKS(200)); // poll fetchPending every 200 ms
  }
}


// ─── Layout helper ────────────────────────────────────────────────────────────
// Returns the pixel height of the area available for ticker grid cells,
// accounting for the header and the optional portfolio footer.
int gridAreaHeight() {
  return 240 - HEADER_H - (portfolioMode ? FOOTER_H : 0);
}


// ─── Header drawing ───────────────────────────────────────────────────────────
// The header is split into two independently-refreshable regions to avoid
// flickering the market status text every time the spinner animates.
//
//   Left  (0–149 px)  : market open/closed indicator
//   Right (296–318 px): spinning fetch indicator (4 dots, one lit at a time)

static int spinFrame = 0;

void drawHeaderStatus() {
  tft.fillRect(0, 0, 150, HEADER_H, C_HEADER());
  tft.setTextColor(TFT_WHITE, C_HEADER());
  tft.setTextDatum(ML_DATUM);

  if (marketKnown) {
    // Coloured dot + "OPEN" or "CLOSED" text
    tft.fillCircle(10, HEADER_H / 2, 4, marketOpen ? C_MKTOPEN : C_MKTCLOSE);
    tft.drawString(marketOpen ? " OPEN" : " CLOSED", 17, HEADER_H / 2, 2);
  } else {
    tft.drawString("STOCK TICKER", 8, HEADER_H / 2, 2);
  }
}

void drawHeaderSpinner() {
  tft.fillRect(296, 2, 22, HEADER_H - 4, C_HEADER());

  if (fetching) {
    // Four dots arranged in a cross; one cyan dot rotates around them.
    const int cx = 308, cy = HEADER_H / 2, r = 5;
    const int8_t dx[] = { 0,  r,  0, -r };
    const int8_t dy[] = {-r,  0,  r,  0 };
    for (int d = 0; d < 4; d++) {
      uint16_t c = ((d + spinFrame) % 4 == 0) ? (uint16_t)TFT_CYAN : C_MUTED();
      tft.fillCircle(cx + dx[d], cy + dy[d], 2, c);
    }
    spinFrame++;
  }
}

void drawHeader() {
  tft.fillRect(0, 0, 320, HEADER_H, C_HEADER());
  drawHeaderStatus();
  drawHeaderSpinner();
}


// ─── Grid cell ────────────────────────────────────────────────────────────────
// Draws one ticker card. Layout adapts automatically:
//   1–4 tickers → single column, font 4 (large)
//   5–8 tickers → two columns,   font 2 (medium)
//
// Each cell shows three fields on a single horizontal line:
//   [SYMBOL]     [$PRICE]     [▲ +X.XX%]
//
// An optional second row shows portfolio value and day P&L when
// portfolioMode is enabled and a holding quantity is configured.
// A small yellow dot in the top-right corner indicates an active price alert.
void drawQuoteGrid(int idx, Quote &q) {
  int cols  = (tickerCount <= 4) ? 1 : 2;
  int rows  = (tickerCount + cols - 1) / cols;
  int cellW = 320 / cols;
  int cellH = gridAreaHeight() / rows;
  int x     = (idx % cols) * cellW;
  int y     = HEADER_H + (idx / cols) * cellH;

  tft.fillRect(x + 1, y + 1, cellW - 2, cellH - 2, C_PANEL());
  tft.drawRect(x, y, cellW, cellH, C_BORDER());

  int midY = y + cellH / 2;

  // Show a placeholder if the quote hasn't loaded yet or has errored.
  if (!q.valid) {
    tft.setTextColor(C_MUTED(), C_PANEL());
    tft.setTextDatum(ML_DATUM);
    tft.drawString(displaySym(q.sym), x + 6, midY, 2);
    tft.setTextDatum(MR_DATUM);
    char eb[12];
    sprintf(eb, q.errors ? "err:%d" : "--", q.errors);
    tft.drawString(eb, x + cellW - 6, midY, 2);
    if (alertHigh[idx] > 0 || alertLow[idx] > 0)
      tft.fillCircle(x + cellW - 6, y + 6, 3, C_ALERT);
    return;
  }

  uint16_t pctColor = (q.pct > 0.05f) ? C_UP : (q.pct < -0.05f) ? C_DOWN : C_FLAT;

  // Start with the larger font; fall back to font 2 if text won't fit.
  int fnt = (cols == 1) ? 4 : 2;

  // Format the price with appropriate decimal precision.
  char priceBuf[16];
  if      (q.price >= 10000) sprintf(priceBuf, "$%.0f",  q.price);
  else if (q.price >= 1000)  sprintf(priceBuf, "$%.1f",  q.price);
  else if (q.price >= 10)    sprintf(priceBuf, "$%.2f",  q.price);
  else if (q.price >= 0.01f) sprintf(priceBuf, "$%.4f",  q.price);
  else                        sprintf(priceBuf, "$%.6f",  q.price);

  char pctBuf[12];
  sprintf(pctBuf, "%+.2f%%", q.pct);

  String symStr = displaySym(q.sym);
  int symW      = tft.textWidth(symStr,   fnt);
  int priceW    = tft.textWidth(priceBuf, fnt);
  int pctW      = tft.textWidth(pctBuf,   fnt);
  int arrowW    = (fnt == 4) ? 10 : 7;
  int arrowGap  = 3;
  int totalW    = symW + priceW + arrowW + arrowGap + pctW;
  int padding   = 8;

  // If the content is too wide even for the chosen font, force font 2.
  if (totalW + padding * 2 > cellW && fnt == 4) {
    fnt    = 2;
    symW   = tft.textWidth(symStr,   fnt);
    priceW = tft.textWidth(priceBuf, fnt);
    pctW   = tft.textWidth(pctBuf,   fnt);
    arrowW = 7;
    totalW = symW + priceW + arrowW + arrowGap + pctW;
  }

  // In 2-column mode, use font 1 (smallest) for the percentage to save space.
  int pctFnt = (cols == 1) ? fnt : 1;
  pctW = tft.textWidth(pctBuf, pctFnt);

  // Horizontal positions: symbol left, price centre, pct right.
  // In 2-col mode, nudge price slightly left to avoid overlapping the pct.
  int symX   = x + padding;
  int priceX = (cols == 1)
                 ? x + (cellW - priceW) / 2
                 : x + cellW * 4.5 / 10 - priceW / 2;
  int pctX   = x + cellW - padding - pctW;

  // Arrow triangle — height scales with font size.
  int ah   = (fnt == 4) ? 6 : 4;
  int arrY = midY - ah / 2;
  int ac   = x + cellW - padding - pctW - arrowGap - arrowW / 2;

  // ── Symbol
  tft.setTextColor(C_LABEL(), C_PANEL());
  tft.setTextDatum(ML_DATUM);
  tft.drawString(symStr, symX, midY, fnt);

  // ── Price
  tft.setTextColor(C_TEXT(), C_PANEL());
  tft.setTextDatum(ML_DATUM);
  tft.drawString(priceBuf, priceX, midY, fnt);

  // ── Direction arrow (▲ up, ▼ down, — flat)
  // Drawn as stacked horizontal lines to form a filled triangle.
  if (q.pct > 0.05f) {
    for (int row = 0; row < ah; row++)
      tft.drawFastHLine(ac - row, arrY + row, row * 2 + 1, C_UP);
  } else if (q.pct < -0.05f) {
    for (int row = 0; row < ah; row++)
      tft.drawFastHLine(ac - (ah - 1 - row), arrY + row, (ah - 1 - row) * 2 + 1, C_DOWN);
  } else {
    tft.drawFastHLine(ac - ah / 2, midY, ah + 1, C_FLAT);
  }

  // ── Percentage change
  tft.setTextColor(pctColor, C_PANEL());
  tft.setTextDatum(ML_DATUM);
  tft.drawString(pctBuf, pctX, midY, pctFnt);

  // ── Portfolio sub-row (only when holdings are configured and cell is tall enough)
  if (portfolioMode && holdings[idx] > 0 && cellH >= 50) {
    float val   = q.price * holdings[idx];
    float dayPL = (q.price - q.open) * holdings[idx];
    char portBuf[28];
    if (val >= 1000) sprintf(portBuf, "$%.0f  %+.0f", val, dayPL);
    else             sprintf(portBuf, "$%.2f  %+.2f", val, dayPL);
    tft.setTextColor(dayPL >= 0 ? (uint16_t)C_UP : (uint16_t)C_DOWN, C_PANEL());
    tft.setTextDatum(MC_DATUM);
    tft.drawString(portBuf, x + cellW / 2, midY + (fnt == 4 ? 20 : 13), 1);
  }

  // ── Alert indicator dot — solid yellow if breached, outline if merely set
  bool breached = (alertHigh[idx] > 0 && q.price >= alertHigh[idx])
               || (alertLow[idx]  > 0 && q.price <= alertLow[idx]);
  bool alertSet = alertHigh[idx] > 0 || alertLow[idx] > 0;
  if      (breached) tft.fillCircle(x + cellW - 5, y + 5, 4, C_ALERT);
  else if (alertSet) tft.drawCircle(x + cellW - 5, y + 5, 3, C_ALERT);
}


// ─── Detail view ──────────────────────────────────────────────────────────────
// Tapping a grid cell opens this full-screen view for that ticker.
// Shows: symbol, current price (large), % change, open price,
// portfolio value/P&L, and any configured alert thresholds.
void drawDetail(int idx, Quote &q) {
  tft.fillRect(0, HEADER_H, 320, 240 - HEADER_H, C_BG());

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_MUTED(), C_BG());
  tft.drawString("TAP TO GO BACK", 10, HEADER_H + 4, 1);

  if (!q.valid) {
    tft.setTextColor(C_MUTED(), C_BG());
    tft.setTextDatum(MC_DATUM);
    tft.drawString(displaySym(q.sym) + " — no data", 160, 130, 2);
    return;
  }

  uint16_t pctColor = (q.pct > 0.05f) ? C_UP : (q.pct < -0.05f) ? C_DOWN : C_FLAT;

  // Symbol and optional crypto badge
  tft.setTextColor(C_LABEL(), C_BG());
  tft.setTextDatum(TL_DATUM);
  tft.drawString(displaySym(q.sym), 10, 46, 4);
  if (q.isCrypto) {
    tft.setTextColor(C_MUTED(), C_BG());
    tft.drawString("CRYPTO", 10, 82, 1);
  }

  // Large price — same adaptive precision as the grid view
  char buf[24];
  if      (q.price >= 10000) sprintf(buf, "$%.0f",  q.price);
  else if (q.price >= 1000)  sprintf(buf, "$%.2f",  q.price);
  else if (q.price >= 10)    sprintf(buf, "$%.2f",  q.price);
  else if (q.price >= 0.01f) sprintf(buf, "$%.4f",  q.price);
  else                        sprintf(buf, "$%.6f",  q.price);
  tft.setTextColor(C_TEXT(), C_BG());
  tft.setTextDatum(TL_DATUM);
  tft.drawString(buf, 10, 96, 6);

  // Percentage change — top right
  sprintf(buf, "%+.2f%%", q.pct);
  tft.setTextColor(pctColor, C_BG());
  tft.setTextDatum(TR_DATUM);
  tft.drawString(buf, 312, 46, 4);

  // Open price
  tft.setTextColor(C_MUTED(), C_BG());
  tft.setTextDatum(TL_DATUM);
  sprintf(buf, "Open  $%.2f", q.open);
  tft.drawString(buf, 10, 160, 2);

  // Portfolio row
  if (portfolioMode && holdings[idx] > 0) {
    float val   = q.price * holdings[idx];
    float dayPL = (q.price - q.open) * holdings[idx];
    char plBuf[40];
    sprintf(plBuf, "Held: $%.2f  Day: %+.2f", val, dayPL);
    tft.setTextColor(dayPL >= 0 ? (uint16_t)C_UP : (uint16_t)C_DOWN, C_BG());
    tft.drawString(plBuf, 10, 180, 1);
  }

  // Alert thresholds
  if (alertHigh[idx] > 0 || alertLow[idx] > 0) {
    String aStr = "Alert: ";
    char ab[20];
    if (alertHigh[idx] > 0) { sprintf(ab, "H>$%.2f ", alertHigh[idx]); aStr += ab; }
    if (alertLow[idx]  > 0) { sprintf(ab, "L<$%.2f",  alertLow[idx]);  aStr += ab; }
    bool br = (alertHigh[idx] > 0 && q.price >= alertHigh[idx])
           || (alertLow[idx]  > 0 && q.price <= alertLow[idx]);
    tft.setTextColor(br ? (uint16_t)C_ALERT : C_MUTED(), C_BG());
    tft.drawString(aStr, 10, 196, 1);
  }

  // Error count (bottom right) — helps diagnose API issues
  if (q.errors > 0) {
    tft.setTextColor(C_MUTED(), C_BG());
    tft.setTextDatum(BR_DATUM);
    sprintf(buf, "err:%d", q.errors);
    tft.drawString(buf, 312, 238, 1);
  }
}


// ─── Portfolio footer ─────────────────────────────────────────────────────────
// A slim bar at the bottom of the screen showing the total portfolio value
// and aggregate day P&L across all tickers with holdings configured.
// Only rendered when portfolioMode is enabled and at least one holding exists.
void drawPortfolioFooter() {
  if (!portfolioMode) return;
  float total = 0, dayPL = 0;
  bool  anyH  = false;
  for (int i = 0; i < tickerCount; i++) {
    if (quotes[i].valid && holdings[i] > 0) {
      total += quotes[i].price * holdings[i];
      dayPL += (quotes[i].price - quotes[i].open) * holdings[i];
      anyH   = true;
    }
  }
  if (!anyH) return;

  tft.fillRect(0, 240 - FOOTER_H, 320, FOOTER_H, C_HEADER());
  char buf[48];
  sprintf(buf, "Portfolio  $%.2f  Day %+.2f", total, dayPL);
  tft.setTextColor(dayPL >= 0 ? (uint16_t)C_UP : (uint16_t)C_DOWN, C_HEADER());
  tft.setTextDatum(MC_DATUM);
  tft.drawString(buf, 160, 240 - FOOTER_H / 2, 1);
}


// ─── Full repaint ─────────────────────────────────────────────────────────────
// Redraws the entire screen from scratch. Called after a view transition
// (grid ↔ detail) or after settings are saved via the web UI.
void drawAll() {
  drawHeader();
  if (viewMode == VIEW_GRID) {
    tft.fillRect(0, HEADER_H, 320, gridAreaHeight(), C_BG());
    for (int i = 0; i < tickerCount; i++) drawQuoteGrid(i, quotes[i]);
    if (portfolioMode) drawPortfolioFooter();
  } else {
    drawDetail(detailIdx, quotes[detailIdx]);
  }
}


// ─── Touch input ─────────────────────────────────────────────────────────────
// Called every loop iteration. Detects a fresh finger-down event (ignoring
// held touches and bounces) and translates the raw ADC coordinates to screen
// pixels using the calibration constants below.
//
// Calibration: the XPT2046 returns 12-bit ADC values (~200–3800).
// map(p.x, 200, 3800, 0, 320) converts them to pixel coordinates.
// If touch registration feels off, adjust the 200/3800 bounds.
void handleTouch() {
  bool isDown = touch.tirqTouched() && touch.touched();

  if (isDown && !touchWasDown) {
    touchWasDown = true;
    unsigned long now = millis();
    if (now - lastTouchAction < TOUCH_DEBOUNCE_MS) return;
    lastTouchAction = now;

    TS_Point p = touch.getPoint();
    int tx = map(p.x, 200, 3800, 0, 320);
    int ty = map(p.y, 200, 3800, 0, 240);

    if (viewMode == VIEW_GRID) {
      // Work out which cell was tapped and open its detail view.
      int cols  = (tickerCount <= 4) ? 1 : 2;
      int rows  = (tickerCount + cols - 1) / cols;
      int cellW = 320 / cols;
      int cellH = gridAreaHeight() / rows;
      int col   = tx / cellW;
      int row   = (ty - HEADER_H) / cellH;
      int idx   = row * cols + col;
      if (idx >= 0 && idx < tickerCount) {
        detailIdx = idx;
        viewMode  = VIEW_DETAIL;
        drawAll();
      }
    } else {
      // Any tap in detail view returns to the grid.
      viewMode = VIEW_GRID;
      drawAll();
    }
  }
  if (!isDown) touchWasDown = false;
}


// ─── Partial repaint (prices only) ───────────────────────────────────────────
// After a fetch cycle completes, only the price data needs updating — not the
// header. This avoids a full drawAll() flicker. The mutex is held for the
// entire repaint so we never render a partially-updated quotes[] array.
void redrawBody() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  if (viewMode == VIEW_GRID) {
    for (int i = 0; i < tickerCount; i++) drawQuoteGrid(i, quotes[i]);
    if (portfolioMode) drawPortfolioFooter();
  } else {
    drawDetail(detailIdx, quotes[detailIdx]);
  }
  xSemaphoreGive(dataMutex);
}


// ─── Web UI: main page (GET /) ────────────────────────────────────────────────
// Serves a mobile-friendly HTML page with two sections:
//   1. A live prices table (read-only, refreshed on page load).
//   2. A settings form for API key, tickers, refresh rate, and alerts.
// The page respects the current dark/light mode theme.
void handleRoot() {
  // Build the comma-separated ticker list for the text input.
  String tickerList = "";
  for (int i = 0; i < tickerCount; i++) {
    if (i) tickerList += ",";
    tickerList += tickers[i];
  }

  // Build the live-prices table rows while holding the mutex.
  String rows = "";
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  float totalVal = 0, totalPL = 0;
  for (int i = 0; i < tickerCount; i++) {
    String sym   = displaySym(quotes[i].sym);
    String price = quotes[i].valid ? "$" + String(quotes[i].price, 2) : "--";
    String pct   = quotes[i].valid
                   ? (quotes[i].pct >= 0 ? "+" : "") + String(quotes[i].pct, 2) + "%" : "--";
    String clr   = quotes[i].valid ? (quotes[i].pct >= 0 ? "#00cc44" : "#ff4444") : "#888";
    String arrow = quotes[i].valid
                   ? (quotes[i].pct > 0.05f ? "&#9650;" : quotes[i].pct < -0.05f ? "&#9660;" : "&mdash;")
                   : "";
    String valStr = "", plStr = "";
    if (quotes[i].valid && holdings[i] > 0) {
      float v = quotes[i].price * holdings[i];
      float d = (quotes[i].price - quotes[i].open) * holdings[i];
      totalVal += v; totalPL += d;
      valStr = "$" + String(v, 2);
      plStr  = (d >= 0 ? "+" : "") + String(d, 2);
    }
    rows += "<tr>"
          + String("<td>") + sym + (quotes[i].isCrypto ? " <span class='badge'>C</span>" : "") + "</td>"
          + "<td style='font-weight:700'>" + price + "</td>"
          + "<td style='color:" + clr + "'>" + arrow + " " + pct + "</td>"
          + "<td>" + valStr + "</td>"
          + "<td style='color:" + clr + "'>" + plStr + "</td>"
          + "</tr>";
  }
  xSemaphoreGive(dataMutex);

  // Holdings & alerts input rows (one per ticker).
  String holdRows = "";
  for (int i = 0; i < tickerCount; i++) {
    holdRows += "<tr><td>" + displaySym(tickers[i]) + "</td>"
      + "<td><input type='number' name='hold" + i + "' value='" + String(holdings[i], 4)
      + "' step='any' min='0' style='width:90px'></td>"
      + "<td><input type='number' name='ahi"  + i + "' value='" + String(alertHigh[i], 2)
      + "' step='any' min='0' placeholder='0=off' style='width:90px'></td>"
      + "<td><input type='number' name='alo"  + i + "' value='" + String(alertLow[i], 2)
      + "' step='any' min='0' placeholder='0=off' style='width:90px'></td></tr>";
  }

  // Theme-aware CSS variable values.
  const char* bg    = darkMode ? "#0a0a0f" : "#f0f0f5";
  const char* card  = darkMode ? "#13131a" : "#ffffff";
  const char* bord  = darkMode ? "#222230" : "#d0d0e0";
  const char* text  = darkMode ? "#c8ccd4" : "#1a1a2e";
  const char* muted = darkMode ? "#555"    : "#888";
  const char* inp   = darkMode ? "#1c1c28" : "#f8f8ff";
  const char* ibord = darkMode ? "#2a2a40" : "#b0b0cc";
  const char* hint  = darkMode ? "#444"    : "#999";
  String dmChk = darkMode      ? " checked" : "";
  String pmChk = portfolioMode ? " checked" : "";

  String html =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='UTF-8'><title>Stock Ticker</title>"
    "<style>"
      "*{box-sizing:border-box;margin:0;padding:0}"
      "body{font-family:'SF Mono','Fira Mono',monospace;background:" + String(bg) + ";color:" + String(text) + ";padding:20px 14px}"
      "h1{font-size:10px;letter-spacing:4px;text-transform:uppercase;color:" + String(muted) + ";margin-bottom:3px}"
      "h2{font-size:22px;font-weight:700;margin-bottom:18px}"
      "h3{font-size:10px;letter-spacing:3px;text-transform:uppercase;color:" + String(muted) + ";margin-bottom:10px}"
      ".card{background:" + String(card) + ";border:1px solid " + String(bord) + ";border-radius:10px;padding:16px;margin-bottom:14px;max-width:500px}"
      "label{display:block;font-size:11px;color:" + String(muted) + ";margin:10px 0 3px}"
      "input[type=text],input[type=password],input[type=number]{width:100%;padding:9px 11px;background:" + String(inp) + ";border:1px solid " + String(ibord) + ";color:" + String(text) + ";border-radius:6px;font-family:inherit;font-size:13px;outline:none}"
      "input:focus{border-color:#0af}"
      ".hint{font-size:10px;color:" + String(hint) + ";margin-top:3px}"
      ".row{display:flex;align-items:center;gap:8px;margin-top:10px}"
      ".row label{margin:0}"
      "input[type=checkbox]{width:16px;height:16px;accent-color:#0080ff}"
      "button{margin-top:14px;width:100%;max-width:500px;padding:12px;background:#0080ff;color:#fff;border:none;border-radius:8px;font-size:14px;font-weight:600;font-family:inherit;cursor:pointer}"
      "button:hover{background:#0062cc}"
      "table{width:100%;border-collapse:collapse;font-size:13px}"
      "th{font-size:9px;letter-spacing:2px;text-transform:uppercase;color:" + String(muted) + ";text-align:left;padding:5px 0;border-bottom:1px solid " + String(bord) + "}"
      "td{padding:6px 4px;border-bottom:1px solid " + String(bord) + "}"
      ".badge{font-size:9px;background:#0080ff22;color:#0080ff;padding:1px 4px;border-radius:3px}"
      ".meta{font-size:10px;color:" + String(muted) + ";margin-top:6px}"
      "a{color:#0af;text-decoration:none}"
    "</style></head><body>"
    "<h1>ESP32 · CYD</h1><h2>Stock Ticker</h2>"

    "<div class='card'><h3>Live Prices</h3>"
    "<table><thead><tr><th>Symbol</th><th>Price</th><th>Change</th><th>Value</th><th>Day P&L</th></tr></thead>"
    "<tbody>" + rows + "</tbody></table>"
    + (portfolioMode && totalVal > 0
        ? "<div class='meta'>Portfolio: $" + String(totalVal, 2)
          + " &nbsp; Day P&L: " + (totalPL >= 0 ? "+" : "") + "$" + String(totalPL, 2) + "</div>"
        : "")
    + "<div class='meta'>"
    + (marketKnown ? (marketOpen ? "&#9679; Market OPEN" : "&#9679; Market CLOSED") : "Market status unknown")
    + " &nbsp;|&nbsp; <a href='/refresh'>Force refresh</a>"
    + " &nbsp;|&nbsp; <a href='/api/quotes'>JSON API</a></div>"
    "</div>"

    "<form method='POST' action='/save'>"

    "<div class='card'><h3>Finnhub API</h3>"
      "<label>API Key</label>"
      "<input type='password' name='apikey' value='" + apiKey + "' placeholder='get free key at finnhub.io'>"
      "<div class='hint'>Free tier: 60 req/min &nbsp;·&nbsp; <a href='https://finnhub.io' target='_blank'>finnhub.io</a></div>"
    "</div>"

    "<div class='card'><h3>Tickers</h3>"
      "<label>Symbols (comma-separated, up to 8)</label>"
      "<input type='text' name='tickers' value='" + tickerList + "' placeholder='AAPL,MSFT,CRYPTO:bitcoin'>"
      "<div class='hint'>Stocks: AAPL &nbsp;·&nbsp; Crypto: CRYPTO:bitcoin (CoinGecko ID, no key needed) &nbsp;·&nbsp; Forex: OANDA:EUR_USD</div>"
    "</div>"

    "<div class='card'><h3>Display</h3>"
      "<label>Refresh interval (seconds)</label>"
      "<input type='number' name='refresh' min='" + String(MIN_REFRESH) + "' max='3600' value='" + String(refreshSec) + "'>"
      "<div class='hint'>Minimum " + String(MIN_REFRESH) + "s &nbsp;·&nbsp; Default " + String(DEFAULT_REFRESH) + "s (safe for up to 8 tickers on free API tiers)<br>"
      "Finnhub: 60 req/min &nbsp;·&nbsp; CoinGecko: 30 req/min &nbsp;·&nbsp; Each ticker takes ~0.8 s to fetch</div>"
      "<label>Backlight (10–255)</label>"
      "<input type='number' name='bright' min='10' max='255' value='" + String(brightness) + "'>"
      "<div class='row'>"
        "<input type='checkbox' name='darkmode' id='dm' value='1'" + dmChk + ">"
        "<label for='dm'>Dark mode</label>"
      "</div>"
      "<div class='row'>"
        "<input type='checkbox' name='portfolio' id='pm' value='1'" + pmChk + ">"
        "<label for='pm'>Portfolio mode (value &amp; P&amp;L)</label>"
      "</div>"
    "</div>"

    "<div class='card'><h3>Holdings &amp; Alerts</h3>"
      "<table><thead><tr><th>Symbol</th><th>Shares/Units</th><th>Alert High ($)</th><th>Alert Low ($)</th></tr></thead>"
      "<tbody>" + holdRows + "</tbody></table>"
      "<div class='hint'>Set 0 to disable. Alerts flash the yellow LED when the threshold is crossed.</div>"
    "</div>"

    "<button type='submit'>&#9654; Save &amp; Apply</button>"
    "</form>"
    "<div style='height:28px'></div></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}


// ─── Web UI: save settings (POST /save) ───────────────────────────────────────
// Parses the form submission, validates bounds, saves to NVS, triggers an
// immediate data refresh, repaints the display, then redirects back to /.
void handleSave() {
  if (server.hasArg("apikey")) {
    apiKey = server.arg("apikey");
    apiKey.trim();
  }

  if (server.hasArg("tickers")) {
    String raw = server.arg("tickers");
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    tickerCount = 0;
    int start   = 0;
    for (int i = 0; i <= (int)raw.length(); i++) {
      if (i == raw.length() || raw[i] == ',') {
        String t = raw.substring(start, i);
        t.trim();
        if (t.length() && tickerCount < MAX_TICKERS) {
          // Normalise: stock symbols to uppercase; preserve CoinGecko IDs as-is.
          if (!t.startsWith("CRYPTO:") && !t.startsWith("crypto:")) {
            t.toUpperCase();
          } else {
            t = "CRYPTO:" + t.substring(t.indexOf(':') + 1);
          }
          tickers[tickerCount]         = t;
          holdings[tickerCount]        = 0;
          alertHigh[tickerCount]       = 0;
          alertLow[tickerCount]        = 0;
          quotes[tickerCount]          = Quote{};
          quotes[tickerCount].sym      = t;
          quotes[tickerCount].isCrypto = isCryptoTicker(t);
          tickerCount++;
        }
        start = i + 1;
      }
    }
    xSemaphoreGive(dataMutex);
  }

  for (int i = 0; i < tickerCount; i++) {
    if (server.hasArg("hold" + String(i))) holdings[i]  = server.arg("hold" + String(i)).toFloat();
    if (server.hasArg("ahi"  + String(i))) alertHigh[i] = server.arg("ahi"  + String(i)).toFloat();
    if (server.hasArg("alo"  + String(i))) alertLow[i]  = server.arg("alo"  + String(i)).toFloat();
  }

  if (server.hasArg("refresh")) {
    refreshSec = server.arg("refresh").toInt();
    if (refreshSec < MIN_REFRESH) refreshSec = MIN_REFRESH;
  }
  if (server.hasArg("bright")) setBrightness(server.arg("bright").toInt());

  darkMode      = server.hasArg("darkmode");
  portfolioMode = server.hasArg("portfolio");

  savePrefs();
  fetchPending = true;
  drawAll();

  // Brief "Saved" confirmation page, then redirect back to the main UI.
  const char* rbg = darkMode ? "#0a0a0f" : "#f0f0f5";
  const char* rfg = darkMode ? "#00cc44" : "#1a1a2e";
  server.send(200, "text/html; charset=utf-8",
    String("<!DOCTYPE html><html><head><meta charset='UTF-8'>")
    + "<meta http-equiv='refresh' content='2;url=/'>"
    + "<style>body{font-family:monospace;background:" + rbg + ";color:" + rfg
    + ";padding:40px;text-align:center}</style>"
    + "</head><body><p>&#10003; Saved.</p></body></html>");
}


// ─── Web UI: JSON API (GET /api/quotes) ───────────────────────────────────────
// Returns the current quote data as a JSON array. Useful for integrating the
// ticker into dashboards or automations (CORS header included).
void handleApiQuotes() {
  String json = "[";
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < tickerCount; i++) {
    if (i) json += ",";
    json += "{\"sym\":\""  + displaySym(quotes[i].sym) + "\","
          + "\"price\":"   + String(quotes[i].price, 4) + ","
          + "\"pct\":"     + String(quotes[i].pct, 2)   + ","
          + "\"valid\":"   + (quotes[i].valid    ? "true" : "false") + ","
          + "\"crypto\":"  + (quotes[i].isCrypto ? "true" : "false") + "}";
  }
  xSemaphoreGive(dataMutex);
  json += "]";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// Force an immediate data refresh from any browser or automation.
void handleForceRefresh() {
  fetchPending = true;
  server.send(200, "text/plain", "ok");
}


// ─── Setup ────────────────────────────────────────────────────────────────────
// Runs once on boot. Initialises hardware, connects to WiFi (launching a
// captive-portal AP named "StockTicker-Setup" if no credentials are saved),
// syncs time via NTP, registers web routes, then kicks off the fetch task.
void setup() {
  Serial.begin(115200);
  delay(300);

  // RGB LED — start red to indicate the boot sequence is in progress.
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setLED(true, false, false);
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  // TFT — rotation 3 = landscape, USB port on the right.
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);

  // Touch — uses HSPI on pins 25 (CLK), 39 (MISO), 32 (MOSI), 33 (CS).
  // setRotation(3) must match the TFT rotation; do not change this value.
  touchSPI.begin(25, 39, 32, TOUCH_CS_PIN);
  touch.begin(touchSPI);
  touch.setRotation(3);

  loadPrefs();
  applyBrightness();

  tft.fillScreen(C_BG());
  tft.setTextColor(TFT_CYAN, C_BG());
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting WiFi...", 160, 120, 2);

  // Create the mutex before starting any task that accesses quotes[].
  dataMutex = xSemaphoreCreateMutex();

  // WiFiManager — connects automatically if credentials are saved, otherwise
  // opens a captive-portal AP so the user can enter their network details.
  // The Finnhub API key can also be entered in the portal.
  WiFiManager wm;
  WiFiManagerParameter param_key("apikey", "Finnhub API Key", apiKey.c_str(), 64);
  wm.addParameter(&param_key);
  wm.setConfigPortalTimeout(120); // give up after 2 minutes of no connection
  wm.setAPCallback([](WiFiManager*) {
    tft.fillScreen(C_BG());
    tft.setTextColor(TFT_YELLOW, C_BG());
    tft.setTextDatum(MC_DATUM);
    tft.drawString("WiFi: StockTicker-Setup", 160, 110, 2);
    tft.drawString("192.168.4.1", 160, 130, 2);
    setLED(false, true, false); // green = AP mode
  });

  if (!wm.autoConnect("StockTicker-Setup")) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("WiFi failed! Restarting...", 160, 120, 2);
    delay(3000);
    ESP.restart();
  }

  // Persist any API key entered through the captive portal.
  String k = String(param_key.getValue());
  k.trim();
  if (k.length()) { apiKey = k; savePrefs(); }

  setLED(false, true, false); // green = connected

  // NTP time sync — best-effort; used for display purposes only.
  tft.fillScreen(C_BG());
  tft.setTextColor(TFT_CYAN, C_BG());
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Syncing time...", 160, 120, 2);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Show the device's IP address so the user knows where to find the web UI.
  tft.fillScreen(C_BG());
  tft.setTextColor(TFT_GREEN, C_BG());
  tft.setTextDatum(MC_DATUM);
  tft.drawString("http://" + WiFi.localIP().toString(), 160, 120, 2);
  delay(2000);

  // Register web server routes.
  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/save",       HTTP_POST, handleSave);
  server.on("/refresh",    HTTP_GET,  handleForceRefresh);
  server.on("/api/quotes", HTTP_GET,  handleApiQuotes);
  server.begin();

  setLED(false, false, false); // LED off — ready

  // Pin the fetch task to core 0; stack size 8 KB is sufficient for HTTP + JSON.
  xTaskCreatePinnedToCore(fetchTask, "fetch", 8192, NULL, 1, NULL, 0);
  fetchPending = true; // trigger the first fetch immediately
}


// ─── Main loop (core 1) ───────────────────────────────────────────────────────
// Handles web requests and touch input, schedules periodic fetches, runs a
// WiFi watchdog, and drives the spinner animation + price repaint at 4 Hz.
void loop() {
  server.handleClient();
  handleTouch();

  // Trigger a new fetch when the refresh interval has elapsed.
  if (!fetching && (millis() - lastFetch) >= (unsigned long)refreshSec * 1000UL)
    fetchPending = true;

  // WiFi watchdog — attempt reconnection if the link drops.
  if (millis() - lastWifiCheck > WIFI_CHECK_MS) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      setLED(true, false, false); // red = no WiFi
      WiFi.reconnect();
    }
  }

  // 4 Hz tick — drives the spinner animation and detects fetch completion.
  static unsigned long lastTick        = 0;
  static bool          lastFetching    = false;
  static bool          lastMarketOpen  = false;
  static bool          lastMarketKnown = false;

  if (millis() - lastTick > 250) {
    lastTick = millis();

    // Redraw the market status indicator only when it changes.
    bool statusChanged = (marketOpen != lastMarketOpen) || (marketKnown != lastMarketKnown);
    if (statusChanged) {
      drawHeaderStatus();
      lastMarketOpen  = marketOpen;
      lastMarketKnown = marketKnown;
    }

    // Animate the spinner while fetching (and one final frame after it stops).
    if (fetching || lastFetching) {
      drawHeaderSpinner();
    }

    // When the fetch task finishes, repaint the price data with the new values.
    if (lastFetching && !fetching) {
      redrawBody();
    }

    lastFetching = fetching;
  }
}
