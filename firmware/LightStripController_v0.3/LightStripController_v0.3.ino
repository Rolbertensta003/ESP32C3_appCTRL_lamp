/*
  ============================================================================
   Light Strip Controller  -  Version 0.1  -  Designed by Ro-Han G.
   Board: ESP32-C3 Super Mini
  ============================================================================

  FEATURES
   - WiFi setup through a captive portal, up to 5 saved networks, reduced
     WiFi TX power (8.5 dBm, also fixes Super Mini antenna issues)
   - Web dashboard (http://esp32led.local): lamp preview with pull cord,
     volume-style brightness, colour, warm / cold / pure white, rainbow,
     blink, music (kick / snare / hi-hat / bass), on delay + off delay, OLED note editor,
     saved WiFi list with manual add
   - Compiles on ESP32 core 3.x (desktop IDE) and 2.x (Arduino Cloud editor)
   - I2S microphone: drum detection in Music mode (kick, snare, hi-hat,
     sustained bass / wobble), double clap toggle
     in Auto and No-WiFi mode
   - OLED + rotary encoder: home screen (network + IP), scrollable menu,
     note shown after the knob has been idle for a while
   - Last colour, brightness, settings and the note survive power loss
   - Battery voltage (analog read on GPIO2) with percentage, shown on the
     OLED home screen, the Battery menu screen and the web dashboard

  PERFORMANCE NOTES
   - The OLED is only redrawn when something on the current screen changed
     (a full SSD1306 refresh blocks the loop for ~25 ms at 400 kHz)
   - Music mode skips strip.show() when the frame did not change (silence)
   - No-WiFi mode yields the CPU each loop instead of spinning at 100 %
   - USB serial never blocks when no computer is connected (battery use)
   - Dashboard: cached with an ETag, font loads without blocking the page,
     polling pauses while the browser tab is hidden

  SYSTEM MODES (chosen automatically)
   Music   : WiFi connected and the Music light mode selected
   Auto    : WiFi connected, any other light mode. Double clap toggles.
   No-WiFi : WiFi off, or the setup portal is running. Double clap toggles.

  ROTARY ENCODER
   Home / note screen : press = open menu, turn = wake up
   Lists              : turn = scroll, press = select
   Back               : turn LEFT more than 2 clicks past the top of a list
                        (or past the lowest value of a setting),
                        press on info screens, long press = home screen

  LIBRARIES (Library Manager)
   Adafruit NeoPixel, Adafruit GFX Library, Adafruit SSD1306 (+ Adafruit BusIO)

  ARDUINO IDE SETTINGS
   Board            : ESP32C3 Dev Module
   USB CDC On Boot  : Enabled
   Partition Scheme : Huge APP (3MB No OTA/1MB SPIFFS)   <- recommended (sketch is ~1.2 MB,
                      the default 1.3 MB scheme is almost full)

  PIN MAPPING
   LED strip DIN  -> GPIO3   (330 ohm resistor recommended)
   LED strip DIN  -> GPIO10   (330 ohm resistor recommended)
   I2S mic WS     -> GPIO4
   I2S mic SCK    -> GPIO5
   I2S mic SD     -> GPIO6   (mic L/R pin to GND = left channel)
   OLED SDA       -> GPIO8
   OLED SCL       -> GPIO9
   Encoder CLK/A  -> GPIO0
   Encoder DT/B   -> GPIO1
   Encoder SW     -> GPIO7
   Encoder +/VCC  -> 3V3, GND -> GND
   Battery +      -> R1 27k -> GPIO2 -> R2 22k -> GND   (voltage divider)
   Strip 5V from an external supply, all grounds connected together.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
// I2S driver: the new ESP-IDF 5 driver (core 3.x, desktop IDE) or the legacy
// driver (core 2.x, used by the Arduino Cloud online editor). Chosen automatically.
#include "esp_idf_version.h"
#if ESP_IDF_VERSION_MAJOR >= 5
  #define USE_NEW_I2S 1
  #include "driver/i2s_std.h"
#else
  #define USE_NEW_I2S 0
  #include "driver/i2s.h"
#endif
#include "soc/gpio_struct.h"

#define FW_VERSION  "0.1"
#define FW_DESIGNER "Ro-Han G."

// =====================================================================
//                          TUNABLE SETTINGS
// =====================================================================

// ---------- Debug ----------
#define DEBUG 1                                 // 1 = print sound data + events
const uint32_t DEBUG_INTERVAL_MS = 100;

// ---------- Power / performance ----------
const uint32_t CPU_MHZ       = 160;   // 80 saves battery (WiFi needs at least 80)
const uint32_t IDLE_YIELD_MS = 1;     // No-WiFi mode: give the CPU back this long per loop

// ---------- Pins ----------
#define LED_DATA_PIN  3     // main led strip 
#define LED2_DATA_PIN 10   // secondary strip, mirrors the main one
#define NUM_LEDS      30        // number of LEDs on your strip
#define I2S_WS_PIN    4
#define I2S_SCK_PIN   5
#define I2S_SD_PIN    6
#define OLED_SDA_PIN  8
#define OLED_SCL_PIN  9
#define ENC_CLK_PIN   0
#define ENC_DT_PIN    1
#define ENC_SW_PIN    7
#define BATT_PIN      2         // ADC1 pin with the battery voltage divider

// ---------- Battery voltage ----------
const float    R1               = 27000.0;  // battery + to GPIO2
const float    R2               = 22000.0;  // GPIO2 to GND
const int      NUM_SAMPLES      = 64;       // ADC readings averaged per update
const float    CALIBRATION      = 1.012;    // Adjust after comparing with a multimeter
const float    BATT_DIVIDER     = (R1 + R2) / R2;   // = 2.227, 4.20 V battery -> 1.89 V on the pin
const uint32_t BATT_READ_MS     = 2000;     // how often the battery is read
const float    BATT_SMOOTHING   = 0.2f;     // 0..1, lower = steadier value
const float    BATT_PRESENT_MIN = 2.5f;     // below this: no battery connected
const uint8_t  BATT_LOW_PCT     = 20;       // "Low" warning at or below this

// ---------- OLED ----------
#define OLED_WIDTH    128
#define OLED_HEIGHT   64
#define OLED_ADDR     0x3C      // most 0.96" SSD1306 modules; some use 0x3D
const uint32_t OLED_I2C_HZ      = 400000; // many SSD1306 modules also run at 800000 (faster redraws)
const uint32_t OLED_REFRESH_MS  = 1000;   // redraw rate for live values (countdowns)
const uint32_t NOTE_PAGE_MS     = 3000;   // long notes flip pages this often
const uint32_t MESSAGE_MS       = 1600;   // short confirmation messages
const uint16_t NOTE_IDLE_DEFAULT_S = 20;  // show note after the knob is idle this long

// ---------- Rotary encoder ----------
const int      ENC_STEPS_PER_DETENT = 4;      // most KY-040 knobs: 4 (try 2 if it skips)
const bool     ENC_REVERSE          = false;  // true if turning right scrolls up
const int      BACK_LEFT_COUNTS     = 3;      // "more than 2" left clicks = back
const uint32_t BTN_DEBOUNCE_MS      = 30;
const uint32_t BTN_LONG_PRESS_MS    = 700;    // long press = home screen

// ---------- WiFi ----------
const char*        AP_SSID          = "LAMP WIFI SETUP";
const char*        AP_PASSWORD      = "";                 // "" = open, or min. 8 chars
const char*        HOSTNAME         = "LAMP";         // http://esp32led.local
const wifi_power_t TX_POWER         = WIFI_POWER_8_5dBm;  // reduced TX power
const uint32_t     CONNECT_TIMEOUT  = 12000;              // per saved network
const uint32_t     PORTAL_TIMEOUT   = 5UL * 60UL * 1000UL;
const int          MAX_SAVED_WIFI   = 5;

// ---------- Microphone / audio ----------
const uint32_t SAMPLE_RATE         = 16000;
const int      I2S_READ_SAMPLES    = 256;
const int      AUDIO_FRAME_SAMPLES = 128;     // 8 ms analysis frames
const int      MIC_SAMPLE_SHIFT    = 16;      // 32-bit -> 16-bit (use 14 for more gain)
const bool     MIC_LEFT_CHANNEL    = true;    // mic L/R pin to GND = left (set false if level stays 0)
const int      BASS_LP_SHIFT       = 4;       // ~150 Hz top of bass band
const int      BASS_HP_SHIFT       = 6;       // ~40 Hz bottom of bass band
const int      DC_SHIFT            = 9;

// ---------- Drum detection (Music mode) ----------
// Three frequency bands are watched separately:
//   LOW  40-150 Hz    kick drum, beatbox "b", sub bass / dubstep wobble
//   MID  200-1500 Hz  snare body, beatbox "k" / "pf"
//   HIGH 4-8 kHz      hi-hat, snare rattle, beatbox "ts"
// Watch the Serial Plotter (DEBUG 1) while music plays and tune these.
const float    MID_LOW_HZ          = 200.0f;
const float    MID_HIGH_HZ         = 1500.0f;
const float    HIGH_HZ             = 4000.0f;
const int      BEAT_HISTORY_FRAMES = 125;     // 1 s rolling average (kick band)
const float    DRUM_AVG_RATE       = 0.01f;   // mid / high average, about 1 s

// Kick: sudden jump in the LOW band
const float    KICK_MULT         = 1.5f;      // x the 1 s average
const float    KICK_RISE         = 1.2f;      // x the previous 8 ms frame
const uint16_t KICK_MIN_LEVEL    = 120;
const uint32_t KICK_GAP_MS       = 200;
// Snare: jump in MID together with a noise burst in HIGH
const float    SNARE_MULT        = 1.8f;
const float    SNARE_RISE        = 1.3f;
const float    SNARE_NOISE_MULT  = 1.4f;      // HIGH must be this far above its average
const float    SNARE_SOLO_MULT   = 3.0f;      // ...or MID alone this far above its average
const uint16_t SNARE_MIN_LEVEL   = 150;
const uint32_t SNARE_GAP_MS      = 150;
// Hi-hat: jump in HIGH without a MID jump
const float    HAT_MULT          = 2.0f;
const float    HAT_RISE          = 1.5f;
const uint16_t HAT_MIN_LEVEL     = 60;
const uint32_t HAT_GAP_MS        = 70;
// Wobble / sustained bass: LOW band stays loud for a while
const uint16_t WOBBLE_MIN_LEVEL  = 150;
const uint16_t WOBBLE_FULL_LEVEL = 1200;
const uint32_t WOBBLE_HOLD_MS    = 300;

// Light reaction
const uint32_t KICK_HOLD_MS      = 40;
const uint32_t KICK_FADE_MS      = 300;
const uint32_t SNARE_SPREAD_MS   = 120;       // burst reaches the strip ends this fast
const uint32_t SNARE_FADE_MS     = 260;
const uint32_t HAT_FADE_MS       = 90;
const int      HAT_SPARKS        = 3;         // pixels lit per hi-hat
const uint8_t  WOBBLE_MAX_GLOW   = 110;       // kept below the kick flash
const uint8_t  MUSIC_BASE_LEVEL  = 0;
// Colours (the kick uses the colour chosen in the app or with the knob)
const uint8_t  SNARE_RGB[3]  = { 0,   200, 255 };   // cyan
const uint8_t  HAT_RGB[3]    = { 255, 255, 255 };   // white
const uint8_t  WOBBLE_RGB[3] = { 150, 0,   255 };   // purple

// ---------- Clap detection (Auto + No-WiFi mode) ----------
const uint16_t CLAP_THRESHOLD          = 1500;
const float    CLAP_FLOOR_RATIO        = 4.0f;
const float    CLAP_RISE_RATIO         = 3.0f;
const uint32_t CLAP_MAX_DURATION_MS    = 100;
const float    CLAP_RELEASE_RATIO      = 0.35f;
const uint32_t DOUBLE_CLAP_GAP_MIN_MS  = 150;
const uint32_t DOUBLE_CLAP_GAP_MAX_MS  = 700;
const uint32_t DOUBLE_CLAP_COOLDOWN_MS = 1500;

// ---------- Flash storage ----------
const uint32_t SAVE_DELAY_MS = 3000;

const byte DNS_PORT = 53;

// =====================================================================
//     TYPES  (all declared before the first function: the Arduino IDE
//     inserts automatic prototypes above the first function)
// =====================================================================
enum Mode : uint8_t { M_OFF, M_COLOR, M_WARM, M_COLD, M_WHITE, M_RAINBOW, M_BLINK, M_MUSIC, M_COUNT };
enum SysMode : uint8_t { SYS_AUTO, SYS_MUSIC, SYS_NOWIFI };
enum ClapState : uint8_t { CLAP_IDLE, CLAP_SPIKE, CLAP_TOO_LONG };
enum Screen : uint8_t {
  SCR_HOME, SCR_NOTE, SCR_MENU, SCR_BRIGHTNESS, SCR_COLOUR, SCR_ON_DELAY, SCR_OFF_DELAY,
  SCR_WIFI_INFO, SCR_SAVED_WIFI, SCR_WIFI_ACTION, SCR_BATTERY, SCR_ABOUT, SCR_MESSAGE
};

struct __attribute__((packed)) LedSettings {
  uint8_t  mode      = M_COLOR;
  uint8_t  lastMode  = M_COLOR;   // restored by the power switch
  uint8_t  lastSolid = M_COLOR;   // restored by a double clap
  uint16_t warmK     = 2700;
  uint16_t coldK     = 6500;
  uint8_t  speed     = 40;
  uint16_t onMs      = 500;
  uint16_t offMs     = 500;
};

struct __attribute__((packed)) LedColour {   // last colour + brightness (default white)
  uint8_t r = 255, g = 255, b = 255;
  uint8_t bri = 128;
};

struct DelayTimer { bool active = false; uint32_t end = 0; uint32_t total = 0; };

struct DrumHit { bool seen = false; uint32_t at = 0; };

struct ColourPreset { const char* name; uint8_t mode; uint8_t r, g, b; };

struct NoteFont { const char* name; const GFXfont* font; uint8_t size; uint8_t lineH; uint8_t baseline; };

// =====================================================================
//                         Constant tables
// =====================================================================
const char* MODE_NAMES[M_COUNT] = { "off", "color", "warm", "cold", "white", "rainbow", "blink", "music" };
const char* SYS_NAMES[]         = { "Auto", "Music", "No-WiFi" };

const ColourPreset PRESETS[] = {
  { "White",      M_WHITE, 255, 255, 255 },
  { "Warm white", M_WARM,  0,   0,   0   },
  { "Cold white", M_COLD,  0,   0,   0   },
  { "Red",        M_COLOR, 255, 0,   0   },
  { "Orange",     M_COLOR, 255, 100, 0   },
  { "Amber",      M_COLOR, 255, 160, 20  },
  { "Yellow",     M_COLOR, 255, 220, 0   },
  { "Green",      M_COLOR, 0,   220, 60  },
  { "Teal",       M_COLOR, 0,   200, 140 },
  { "Cyan",       M_COLOR, 0,   200, 255 },
  { "Blue",       M_COLOR, 0,   80,  255 },
  { "Indigo",     M_COLOR, 80,  40,  255 },
  { "Purple",     M_COLOR, 170, 0,   255 },
  { "Pink",       M_COLOR, 255, 40,  140 },
};
const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

// Fonts the OLED can show (Adafruit GFX built-in + FreeFonts)
const NoteFont NOTE_FONTS[] = {
  { "Classic small",  nullptr,            1, 10, 0  },
  { "Classic medium", nullptr,            2, 18, 0  },
  { "Classic large",  nullptr,            3, 26, 0  },
  { "Sans",           &FreeSans9pt7b,     1, 18, 13 },
  { "Sans bold",      &FreeSansBold9pt7b, 1, 18, 13 },
  { "Serif",          &FreeSerif9pt7b,    1, 18, 13 },
  { "Mono",           &FreeMono9pt7b,     1, 16, 11 },
};
const int NUM_FONTS = sizeof(NOTE_FONTS) / sizeof(NOTE_FONTS[0]);

const uint16_t DELAY_STEPS[] = { 0, 1, 2, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 240, 360, 480, 720 };
const int NUM_DELAY_STEPS = sizeof(DELAY_STEPS) / sizeof(DELAY_STEPS[0]);

const char* MENU_ITEMS[] = { "Light", "Brightness", "Colour", "On delay", "Off delay", "WiFi details", "Saved WiFi", "Battery", "About" };
const int MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

// Changes with every build, so browsers re-download the dashboard after a flash
const char PAGE_ETAG[] = "\"" FW_VERSION "-" __DATE__ "-" __TIME__ "\"";

// =====================================================================
//                              Objects
// =====================================================================
//Adafruit_NeoPixel strip(NUM_LEDS, LED_DATA_PIN, NEO_GRB + NEO_KHZ800); 
//
struct DualStrip : Adafruit_NeoPixel {
  Adafruit_NeoPixel mirror;
  bool mirrorBegun = false;
  DualStrip(uint16_t n, int16_t p, neoPixelType t)
    : Adafruit_NeoPixel(n, p, t), mirror(n, LED2_DATA_PIN, t) {}
  void show() {
    if (!mirrorBegun) { mirror.begin(); mirrorBegun = true; }
    memcpy(mirror.getPixels(), getPixels(), numPixels() * (wOffset == rOffset ? 3 : 4));
    Adafruit_NeoPixel::show();
    mirror.show();
  }
};
DualStrip strip(NUM_LEDS, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);


// Same I2C clock during and after each transfer (the library default drops to 100 kHz after)
Adafruit_SSD1306  oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1, OLED_I2C_HZ, OLED_I2C_HZ);
WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;
IPAddress   apIP(192, 168, 4, 1);
IPAddress   netMsk(255, 255, 255, 0);

#define DBG(...) do { if (DEBUG) Serial.printf(__VA_ARGS__); } while (0)

// =====================================================================
//                              Globals
// =====================================================================
LedSettings st, savedSt;
LedColour   col, savedCol;
DelayTimer  onTimer, offTimer;

bool     needsRender = true;
bool     savePending = false;
uint32_t lastChange  = 0;

// WiFi
String   wifiSsid[MAX_SAVED_WIFI], wifiPass[MAX_SAVED_WIFI];
int      wifiCount = 0;
String   preferredSsid, connectedSsid, networkListHtml;
bool     portalActive = false, noWifiMode = false, portalOnce = false;
uint32_t portalStart  = 0;
bool     restartPending = false, offlinePending = false;
uint32_t pendingAt = 0;

// OLED note
String   noteText  = "Hello";
uint8_t  noteFont  = 3;
uint8_t  noteAlign = 1;           // 0 = left, 1 = centre
uint16_t noteIdleS = NOTE_IDLE_DEFAULT_S;
String   noteLines[16];
int      noteLineCount = 0;
bool     noteWrapDirty = true;

// OLED picture (sent from the app, shown on the note screen instead of the note)
const int IMG_BYTES = OLED_WIDTH * OLED_HEIGHT / 8;   // 1024 for 128x64
uint8_t  imgBits[IMG_BYTES];
bool     imgOn = false;

// OLED UI
bool     oledOk = false, oledDirty = true;
uint32_t lastOledDraw = 0, lastInput = 0, msgUntil = 0;
Screen   screen = SCR_HOME, msgReturn = SCR_MENU;
String   msgLine1, msgLine2;
int      menuSel = 0, listSel = 0, listTop = 0, overLeft = 0;
int      editPct = 50, editIndex = 0, wifiSel = 0;

// Encoder (updated in the interrupt)
volatile int32_t encRaw   = 0;
volatile uint8_t encState = 0;
int32_t encUsed = 0;

// =====================================================================
//                              Helpers
// =====================================================================
bool isSolidMode(uint8_t m) { return m == M_COLOR || m == M_WARM || m == M_COLD || m == M_WHITE; }

// Single-evaluation clamp to the signed 16-bit audio range
static inline int32_t clamp16(int32_t v) { return v > 32767 ? 32767 : (v < -32767 ? -32767 : v); }

SysMode sysMode() {
  if (noWifiMode || portalActive) return SYS_NOWIFI;
  return st.mode == M_MUSIC ? SYS_MUSIC : SYS_AUTO;
}

void setReducedTxPower() {
  WiFi.setTxPower(TX_POWER);
  Serial.printf("TX power: %.1f dBm\n", (int)WiFi.getTxPower() / 4.0f);
}

String htmlEscape(const String& s) {
  String o; o.reserve(s.length() + 16);
  for (char c : s) {
    switch (c) {
      case '&': o += F("&amp;"); break;  case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;");  break;  case '"': o += F("&quot;"); break;
      case '\'': o += F("&#39;"); break; default: o += c;
    }
  }
  return o;
}

String jsonEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

uint32_t kelvinToColor(uint16_t kelvin) {
  float t = kelvin / 100.0f, r, g, b;
  if (t <= 66) { r = 255; g = 99.4708025861f * logf(t) - 161.1195681661f; }
  else         { r = 329.698727446f * powf(t - 60, -0.1332047592f);
                 g = 288.1221695283f * powf(t - 60, -0.0755148492f); }
  if (t >= 66) b = 255;
  else if (t <= 19) b = 0;
  else b = 138.5177312231f * logf(t - 10) - 305.0447927307f;
  return strip.Color(constrain((int)r, 0, 255), constrain((int)g, 0, 255), constrain((int)b, 0, 255));
}

void setMode(uint8_t m) {
  st.mode = m;
  if (m != M_OFF)     st.lastMode  = m;
  if (isSolidMode(m)) st.lastSolid = m;
}

void markChanged() { needsRender = true; savePending = true; lastChange = millis(); oledDirty = true; }

void togglePower() {
  setMode(st.mode == M_OFF ? st.lastMode : M_OFF);
  markChanged();
}

// Music mode needs the dashboard; without WiFi fall back to the last colour
void leaveMusicIfActive() {
  if (st.mode == M_MUSIC) { st.mode = st.lastSolid; needsRender = true; }
}

int brightnessPct() { return (col.bri * 100 + 127) / 255; }

String formatMinutes(uint32_t m) {
  if (m == 0) return "Off";
  if (m < 60) return String(m) + " min";
  if (m % 60 == 0) return String(m / 60) + " h";
  return String(m / 60) + " h " + String(m % 60) + " min";
}

String formatCountdown(uint32_t s) {
  char buf[12];
  if (s >= 3600) snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
  else           snprintf(buf, sizeof(buf), "%lu:%02lu", s / 60, s % 60);
  return String(buf);
}

String currentColourName() {
  if (st.mode == M_WARM)  return "Warm white";
  if (st.mode == M_COLD)  return "Cold white";
  if (st.mode == M_WHITE) return "White";
  for (int i = 3; i < NUM_PRESETS; i++)
    if (PRESETS[i].r == col.r && PRESETS[i].g == col.g && PRESETS[i].b == col.b) return PRESETS[i].name;
  return "Custom";
}

int currentPresetIndex() {
  if (st.mode == M_WHITE) return 0;
  if (st.mode == M_WARM)  return 1;
  if (st.mode == M_COLD)  return 2;
  for (int i = 3; i < NUM_PRESETS; i++)
    if (PRESETS[i].r == col.r && PRESETS[i].g == col.g && PRESETS[i].b == col.b) return i;
  return 0;
}

// Colour chosen on the OLED. In Music / Blink only the colour changes.
void applyPreset(int i) {
  const ColourPreset& p = PRESETS[i];
  if (p.mode == M_COLOR) {
    col.r = p.r; col.g = p.g; col.b = p.b;
    if (st.mode != M_MUSIC && st.mode != M_BLINK) setMode(M_COLOR);
  } else {
    setMode(p.mode);
  }
  markChanged();
}

// =====================================================================
//                         On / off delay timers
// =====================================================================
uint32_t timerLeftS(const DelayTimer& t) {
  if (!t.active) return 0;
  int32_t rem = (int32_t)(t.end - millis());
  return rem > 0 ? (uint32_t)rem / 1000 : 0;
}

void startTimer(DelayTimer& t, uint32_t minutes) {
  if (minutes == 0) { t.active = false; oledDirty = true; return; }
  t.total  = minutes * 60UL;
  t.end    = millis() + t.total * 1000UL;
  t.active = true;
  oledDirty = true;
}

void updateTimers(uint32_t now) {
  if (offTimer.active && (int32_t)(now - offTimer.end) >= 0) {
    offTimer.active = false;
    setMode(M_OFF); markChanged();
    Serial.println("Off delay finished: light off");
  }
  if (onTimer.active && (int32_t)(now - onTimer.end) >= 0) {
    onTimer.active = false;
    if (st.mode == M_OFF) { setMode(st.lastMode); markChanged(); }
    Serial.println("On delay finished: light on");
  }
}

// =====================================================================
//                        Battery voltage (ADC)
// =====================================================================
float   battV   = -1;       // smoothed battery voltage, -1 = not read yet
uint8_t battPct = 0;
bool    battOk  = false;    // false = no battery detected on the divider

// Single Li-ion / LiPo cell: voltage -> percent (resting voltage)
const float BATT_CURVE[][2] = {
  { 4.20f, 100 }, { 4.10f, 90 }, { 4.00f, 80 }, { 3.92f, 70 }, { 3.87f, 60 }, { 3.82f, 50 },
  { 3.79f, 40 },  { 3.77f, 30 }, { 3.74f, 20 }, { 3.68f, 10 }, { 3.45f, 5 },  { 3.30f, 0 },
};
const int BATT_CURVE_N = sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0]);

uint8_t batteryPercent(float v) {
  if (v >= BATT_CURVE[0][0]) return 100;
  for (int i = 1; i < BATT_CURVE_N; i++) {
    if (v >= BATT_CURVE[i][0]) {
      float v1 = BATT_CURVE[i][0], v2 = BATT_CURVE[i - 1][0];
      float p1 = BATT_CURVE[i][1], p2 = BATT_CURVE[i - 1][1];
      return (uint8_t)(p1 + (v - v1) * (p2 - p1) / (v2 - v1) + 0.5f);
    }
  }
  return 0;
}

// Averaged analog read, converted to the battery voltage
float readBatteryVoltage() {
  uint32_t mv = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) mv += analogReadMilliVolts(BATT_PIN);
  float pinV = (mv / (float)NUM_SAMPLES) / 1000.0f;
  return pinV * BATT_DIVIDER * CALIBRATION;
}

void initBattery() {
  analogReadResolution(12);
  analogSetPinAttenuation(BATT_PIN, ADC_11db);   // 0 to about 3.1 V on the pin
}

void updateBattery(uint32_t now) {
  static uint32_t lastRead = 0, lastLog = 0;
  if (battV >= 0 && now - lastRead < BATT_READ_MS) return;
  lastRead = now;
  float v = readBatteryVoltage();
  bool ok = v >= BATT_PRESENT_MIN;
  if (!ok)                battV = v;                               // no battery: no smoothing
  else if (battV < BATT_PRESENT_MIN) battV = v;                    // first reading
  else                    battV += (v - battV) * BATT_SMOOTHING;
  uint8_t p = ok ? batteryPercent(battV) : 0;
  if (ok != battOk || p != battPct) oledDirty = true;
  battOk = ok; battPct = p;
  if (now - lastLog >= 30000 || lastLog == 0) {
    lastLog = now ? now : 1;
    if (battOk) Serial.printf("Battery: %.2f V  %u%%\n", battV, battPct);
    else        Serial.printf("Battery: not detected (%.2f V)\n", v);
  }
}

String batteryVoltText() { return battOk ? String(battV, 2) + " V" : String("--"); }
String batteryPctText()  { return battOk ? String(battPct) + "%" : String("No batt"); }

// =====================================================================
//                  Persistent storage (NVS / Preferences)
// =====================================================================
void saveNetworks() {
  prefs.begin("wifi", false);
  prefs.putInt("n", wifiCount);
  for (int i = 0; i < MAX_SAVED_WIFI; i++) {
    String ks = "s" + String(i), kp = "p" + String(i);
    if (i < wifiCount) { prefs.putString(ks.c_str(), wifiSsid[i]); prefs.putString(kp.c_str(), wifiPass[i]); }
    else { prefs.remove(ks.c_str()); prefs.remove(kp.c_str()); }
  }
  prefs.putString("pref", preferredSsid);
  prefs.end();
}

void loadWifiSettings() {
  prefs.begin("wifi", true);
  wifiCount = prefs.getInt("n", -1);
  if (wifiCount < 0) {                          // migrate the single network of older versions
    String s = prefs.getString("ssid", "");
    wifiCount = 0;
    if (s.length()) { wifiSsid[0] = s; wifiPass[0] = prefs.getString("pass", ""); wifiCount = 1; }
  } else {
    wifiCount = min(wifiCount, MAX_SAVED_WIFI);
    for (int i = 0; i < wifiCount; i++) {
      wifiSsid[i] = prefs.getString(("s" + String(i)).c_str(), "");
      wifiPass[i] = prefs.getString(("p" + String(i)).c_str(), "");
    }
  }
  preferredSsid = prefs.getString("pref", "");
  noWifiMode    = prefs.getBool("nowifi", false);
  portalOnce    = prefs.getBool("portal1", false);
  prefs.end();
  if (portalOnce) { prefs.begin("wifi", false); prefs.putBool("portal1", false); prefs.end(); }
}

void setWifiFlag(const char* key, bool v) { prefs.begin("wifi", false); prefs.putBool(key, v); prefs.end(); }

int findNetwork(const String& ssid) {
  for (int i = 0; i < wifiCount; i++) if (wifiSsid[i] == ssid) return i;
  return -1;
}

// Adds a network, or updates its password. When full, the oldest is replaced.
void addNetwork(const String& ssid, const String& pass) {
  int i = findNetwork(ssid);
  if (i < 0) {
    if (wifiCount >= MAX_SAVED_WIFI) {
      for (int k = 1; k < MAX_SAVED_WIFI; k++) { wifiSsid[k - 1] = wifiSsid[k]; wifiPass[k - 1] = wifiPass[k]; }
      wifiCount = MAX_SAVED_WIFI - 1;
    }
    i = wifiCount++;
  }
  wifiSsid[i] = ssid; wifiPass[i] = pass;
  saveNetworks();
  setWifiFlag("nowifi", false);
  Serial.printf("Saved WiFi network: %s\n", ssid.c_str());
}

void forgetNetwork(const String& ssid) {
  int i = findNetwork(ssid);
  if (i < 0) return;
  for (int k = i + 1; k < wifiCount; k++) { wifiSsid[k - 1] = wifiSsid[k]; wifiPass[k - 1] = wifiPass[k]; }
  wifiCount--;
  if (preferredSsid == ssid) preferredSsid = "";
  saveNetworks();
  Serial.printf("Forgot WiFi network: %s\n", ssid.c_str());
}

void forgetAllNetworks() { prefs.begin("wifi", false); prefs.clear(); prefs.end(); wifiCount = 0; }

void loadLedState() {
  prefs.begin("led", true);
  if (prefs.getBytesLength("set") == sizeof(LedSettings))  prefs.getBytes("set", &st, sizeof(LedSettings));
  if (prefs.getBytesLength("colour") == sizeof(LedColour)) prefs.getBytes("colour", &col, sizeof(LedColour));
  prefs.end();
  if (st.mode >= M_COUNT) st.mode = M_COLOR;
  if (st.lastMode >= M_COUNT || st.lastMode == M_OFF) st.lastMode = M_COLOR;
  if (!isSolidMode(st.lastSolid)) st.lastSolid = M_COLOR;
  if (col.bri == 0) col.bri = 128;
  savedSt = st; savedCol = col;
}

// Writes only the parts that really changed (protects the flash)
void saveIfChanged() {
  if (memcmp(&col, &savedCol, sizeof(LedColour)) != 0) {
    prefs.begin("led", false); prefs.putBytes("colour", &col, sizeof(LedColour)); prefs.end();
    savedCol = col;
    DBG("Colour saved: R%u G%u B%u, brightness %u\n", col.r, col.g, col.b, col.bri);
  }
  if (memcmp(&st, &savedSt, sizeof(LedSettings)) != 0) {
    prefs.begin("led", false); prefs.putBytes("set", &st, sizeof(LedSettings)); prefs.end();
    savedSt = st;
    DBG("Light settings saved\n");
  }
}

void loadImage();
void loadNote() {
  prefs.begin("oled", true);
  noteText  = prefs.getString("text", "Hello");
  noteFont  = prefs.getUChar("font", 3);
  noteAlign = prefs.getUChar("align", 1);
  noteIdleS = prefs.getUShort("idle", NOTE_IDLE_DEFAULT_S);
  prefs.end();
  if (noteFont >= NUM_FONTS) noteFont = 3;
  if (noteIdleS < 5) noteIdleS = NOTE_IDLE_DEFAULT_S;
  noteWrapDirty = true;
  loadImage();
}

void loadImage() {
  prefs.begin("oled", true);
  imgOn = prefs.getBytesLength("img") == IMG_BYTES && prefs.getBytes("img", imgBits, IMG_BYTES) == IMG_BYTES;
  prefs.end();
}

void saveImage() {
  prefs.begin("oled", false);
  if (imgOn) prefs.putBytes("img", imgBits, IMG_BYTES);
  else       prefs.remove("img");
  prefs.end();
}

void saveNote() {
  prefs.begin("oled", false);
  if (prefs.getString("text", "") != noteText) prefs.putString("text", noteText);
  if (prefs.getUChar("font", 255)  != noteFont)  prefs.putUChar("font", noteFont);
  if (prefs.getUChar("align", 255) != noteAlign) prefs.putUChar("align", noteAlign);
  if (prefs.getUShort("idle", 0)   != noteIdleS) prefs.putUShort("idle", noteIdleS);
  prefs.end();
}

// Only characters the OLED fonts can draw (printable ASCII + line breaks)
String sanitizeNote(const String& in) {
  String o; o.reserve(min((int)in.length(), 120));
  for (char c : in) {
    if (c == '\r') continue;
    if (c == '\n' || (c >= 0x20 && c <= 0x7E)) o += c;
    if (o.length() >= 120) break;
  }
  return o;
}

// =====================================================================
//                     MICROPHONE (I2S, ESP-IDF driver)
// =====================================================================
#if USE_NEW_I2S
i2s_chan_handle_t micRx = nullptr;
#endif
bool    micReady = false;
int32_t i2sBuf[I2S_READ_SAMPLES];

int32_t  dcAcc = 0, bassLp1 = 0, bassLp2 = 0, bassDc = 0;
uint64_t levelSumSq = 0, bassSumSq = 0;
int      frameSamples = 0;
uint16_t soundLevel = 0, bassLevel = 0;
uint32_t bassEnergy = 0, bassAvgEnergy = 0;
uint32_t bassHistory[BEAT_HISTORY_FRAMES];
uint64_t bassHistorySum = 0;
int      bassHistoryIdx = 0, bassHistoryCount = 0;

// Extra bands for drum detection (integer one-pole filters, Q15 coefficients)
int32_t  coefMidHi = 0, coefMidLo = 0, coefHigh = 0;
int32_t  midLpA = 0, midLpB = 0, highLp = 0;
uint64_t midSumSq = 0, highSumSq = 0;
uint16_t midLevel = 0, highLevel = 0;
uint32_t midEnergy = 0, highEnergy = 0;
float    midAvgEnergy = 0, highAvgEnergy = 0;
uint32_t prevBassE = 0, prevMidE = 0, prevHighE = 0;
float    bassEnv = 0;
uint32_t bassSustainSince = 0;
bool     wobbleActive = false;
DrumHit  kickHit, snareHit, hatHit;

ClapState clapState = CLAP_IDLE;
uint32_t spikeStart = 0;
uint16_t spikePeak  = 0, prevLevel1 = 0, prevLevel2 = 0;
float    noiseFloor = 100;
int      clapCount  = 0;
bool     clapSequenceRejected = false;
uint32_t lastClapAt = 0, clapCooldownUntil = 0;

bool initMic() {
#if USE_NEW_I2S
  // ---- new driver (driver/i2s_std.h) ----
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num  = 8;     // 128 ms of buffer, covers OLED + web work
  chanCfg.dma_frame_num = 256;
  if (i2s_new_channel(&chanCfg, nullptr, &micRx) != ESP_OK) return false;

  i2s_std_config_t stdCfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK_PIN,
      .ws   = (gpio_num_t)I2S_WS_PIN,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)I2S_SD_PIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  stdCfg.slot_cfg.slot_mask = MIC_LEFT_CHANNEL ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;
  if (i2s_channel_init_std_mode(micRx, &stdCfg) != ESP_OK) return false;
  return i2s_channel_enable(micRx) == ESP_OK;
#else
  // ---- legacy driver (driver/i2s.h), same settings ----
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format       = MIC_LEFT_CHANNEL ? I2S_CHANNEL_FMT_ONLY_LEFT : I2S_CHANNEL_FMT_ONLY_RIGHT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = 0;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = I2S_SCK_PIN;
  pins.ws_io_num    = I2S_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = I2S_SD_PIN;
  return i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK;
#endif
}

// Non-blocking read of whatever the DMA buffer holds (timeout 0)
size_t readMic(int32_t* buf, size_t bytes) {
  size_t got = 0;
#if USE_NEW_I2S
  i2s_channel_read(micRx, buf, bytes, &got, 0);
#else
  i2s_read(I2S_NUM_0, buf, bytes, &got, 0);
#endif
  return got;
}

void resetClapDetector() { clapState = CLAP_IDLE; clapCount = 0; clapSequenceRejected = false; }

// Q15 coefficient of a one-pole low-pass with cutoff fc
int32_t onePoleCoef(float fc) { return (int32_t)((1.0f - expf(-2.0f * PI * fc / SAMPLE_RATE)) * 32768.0f); }

void initBandFilters() {
  coefMidHi = onePoleCoef(MID_HIGH_HZ);
  coefMidLo = onePoleCoef(MID_LOW_HZ);
  coefHigh  = onePoleCoef(HIGH_HZ);
}

void resetDrums() {
  kickHit = DrumHit(); snareHit = DrumHit(); hatHit = DrumHit();
  wobbleActive = false; bassSustainSince = 0;
}

bool gapOk(const DrumHit& h, uint32_t now, uint32_t gap) { return !h.seen || now - h.at >= gap; }
void fireHit(DrumHit& h, uint32_t now) { h.seen = true; h.at = now; }

// Every 8 ms frame: a band "hits" when its energy jumps above its own ~1 s
// average AND above the previous frame (fast attack, not a held note).
void detectDrums(uint32_t now) {
  bool kick = bassLevel >= KICK_MIN_LEVEL &&
              bassEnergy > bassAvgEnergy * KICK_MULT &&
              bassEnergy > prevBassE * KICK_RISE;
  bool midJump = midLevel >= SNARE_MIN_LEVEL &&
                 midEnergy > midAvgEnergy * SNARE_MULT &&
                 midEnergy > prevMidE * SNARE_RISE;
  bool highJump = highLevel >= HAT_MIN_LEVEL &&
                  highEnergy > highAvgEnergy * HAT_MULT &&
                  highEnergy > prevHighE * HAT_RISE;
  bool highNoise = highEnergy > highAvgEnergy * SNARE_NOISE_MULT;

  if (kick && gapOk(kickHit, now, KICK_GAP_MS)) {
    fireHit(kickHit, now);
    DBG(">> KICK   bass:%u\n", bassLevel);
  }

  // Snare = body (mid) + rattle (high). A kick's click alone is not a snare.
  bool snare = midJump && (highNoise || (!kick && midEnergy > midAvgEnergy * SNARE_SOLO_MULT));
  if (snare) {
    if (gapOk(snareHit, now, SNARE_GAP_MS)) {
      fireHit(snareHit, now);
      DBG(">> SNARE  mid:%u high:%u\n", midLevel, highLevel);
    }
  } else if (highJump && !midJump && gapOk(hatHit, now, HAT_GAP_MS)) {
    fireHit(hatHit, now);
    DBG(">> HAT    high:%u\n", highLevel);
  }

  // Sustained bass (dubstep wobble, long 808 notes)
  if (bassEnv >= WOBBLE_MIN_LEVEL) { if (!bassSustainSince) bassSustainSince = now ? now : 1; }
  else bassSustainSince = 0;
  bool w = bassSustainSince && now - bassSustainSince >= WOBBLE_HOLD_MS;
  if (w != wobbleActive) { wobbleActive = w; DBG(">> WOBBLE %s\n", w ? "start" : "end"); }
}

void toggleLightByClap() {
  if (st.mode != M_OFF) setMode(M_OFF);
  else                  setMode(st.lastSolid);     // on with the last colour
  markChanged();
  DBG(">> Light turned %s by double clap\n", st.mode == M_OFF ? "off" : "on");
}

void onClap(uint32_t t) {
  if ((int32_t)(t - clapCooldownUntil) < 0) return;
  DBG(">> CLAP  level:%u\n", spikePeak);
  if (clapSequenceRejected) { lastClapAt = t; return; }
  if (clapCount == 0) { clapCount = 1; lastClapAt = t; return; }
  uint32_t gap = t - lastClapAt;
  if (gap < DOUBLE_CLAP_GAP_MIN_MS) return;
  if (gap > DOUBLE_CLAP_GAP_MAX_MS) { clapCount = 1; lastClapAt = t; return; }
  if (clapCount == 1) { clapCount = 2; lastClapAt = t; }
  else { clapSequenceRejected = true; clapCount = 0; lastClapAt = t; DBG(">> 3+ claps in a row, ignored\n"); }
}

void updateClapSequence(uint32_t now) {
  if (clapCount == 0 && !clapSequenceRejected) return;
  if (now - lastClapAt <= DOUBLE_CLAP_GAP_MAX_MS) return;
  if (clapSequenceRejected) clapSequenceRejected = false;
  else if (clapCount == 2) {
    DBG(">> DOUBLE CLAP\n");
    clapCooldownUntil = now + DOUBLE_CLAP_COOLDOWN_MS;
    toggleLightByClap();
  }
  clapCount = 0;
}

void detectClap(uint32_t now) {
  switch (clapState) {
    case CLAP_IDLE: {
      float thr = max((float)CLAP_THRESHOLD, noiseFloor * CLAP_FLOOR_RATIO);
      if (soundLevel > thr && soundLevel > prevLevel2 * CLAP_RISE_RATIO) {
        clapState = CLAP_SPIKE; spikeStart = now; spikePeak = soundLevel;
      } else {
        noiseFloor += (soundLevel - noiseFloor) * 0.02f;
      }
      break;
    }
    case CLAP_SPIKE:
      if (soundLevel > spikePeak) spikePeak = soundLevel;
      if (soundLevel < spikePeak * CLAP_RELEASE_RATIO) {
        clapState = CLAP_IDLE;
        if (now - spikeStart <= CLAP_MAX_DURATION_MS) onClap(spikeStart);
      } else if (now - spikeStart > CLAP_MAX_DURATION_MS) {
        clapState = CLAP_TOO_LONG;
      }
      break;
    case CLAP_TOO_LONG:
      if (soundLevel < spikePeak * CLAP_RELEASE_RATIO || now - spikeStart > 2000) {
        clapState = CLAP_IDLE; noiseFloor = soundLevel;
      }
      break;
  }
  prevLevel2 = prevLevel1;
  prevLevel1 = soundLevel;
}

void onAudioFrame() {
  uint32_t now = millis();
  soundLevel    = (uint16_t)sqrtf((float)(levelSumSq / AUDIO_FRAME_SAMPLES));
  bassEnergy    = (uint32_t)(bassSumSq / AUDIO_FRAME_SAMPLES);
  bassLevel     = (uint16_t)sqrtf((float)bassEnergy);
  bassAvgEnergy = bassHistoryCount ? (uint32_t)(bassHistorySum / bassHistoryCount) : bassEnergy;
  midEnergy     = (uint32_t)(midSumSq  / AUDIO_FRAME_SAMPLES);
  highEnergy    = (uint32_t)(highSumSq / AUDIO_FRAME_SAMPLES);
  midLevel      = (uint16_t)sqrtf((float)midEnergy);
  highLevel     = (uint16_t)sqrtf((float)highEnergy);
  // bass envelope: fast attack, slow release (smooths the wobble LFO dips)
  bassEnv += ((float)bassLevel - bassEnv) * (bassLevel > bassEnv ? 0.5f : 0.05f);

  if (sysMode() == SYS_MUSIC) detectDrums(now);
  else                        detectClap(now);

  // averages are updated after detection, so a hit is compared with the past
  if (bassHistoryCount == BEAT_HISTORY_FRAMES) bassHistorySum -= bassHistory[bassHistoryIdx];
  else bassHistoryCount++;
  bassHistory[bassHistoryIdx] = bassEnergy;
  bassHistorySum += bassEnergy;
  if (++bassHistoryIdx >= BEAT_HISTORY_FRAMES) bassHistoryIdx = 0;
  midAvgEnergy  += ((float)midEnergy  - midAvgEnergy)  * DRUM_AVG_RATE;
  highAvgEnergy += ((float)highEnergy - highAvgEnergy) * DRUM_AVG_RATE;
  prevBassE = bassEnergy; prevMidE = midEnergy; prevHighE = highEnergy;
}

// Non-blocking: reads only what the DMA already holds. Integer filters only.
void processAudio() {
  if (!micReady) return;
  for (int pass = 0; pass < 4; pass++) {
    size_t bytesRead = readMic(i2sBuf, sizeof(i2sBuf));
    int n = bytesRead / sizeof(int32_t);
    if (n == 0) return;
    for (int i = 0; i < n; i++) {
      int32_t s = i2sBuf[i] >> MIC_SAMPLE_SHIFT;
      dcAcc += (s * 256 - dcAcc) >> DC_SHIFT;
      int32_t ac = clamp16(s - (dcAcc >> 8));
      levelSumSq += (uint32_t)(ac * ac);
      bassLp1 += (ac * 256 - bassLp1) >> BASS_LP_SHIFT;
      bassLp2 += (bassLp1 - bassLp2) >> BASS_LP_SHIFT;
      bassDc  += (bassLp2 - bassDc) >> BASS_HP_SHIFT;
      int32_t bass = (bassLp2 - bassDc) >> 8;
      bassSumSq += (uint32_t)(bass * bass);
      // MID = LP(1500 Hz) - LP(200 Hz), HIGH = signal - LP(4000 Hz)
      int32_t x = ac * 256;
      midLpA += (int32_t)(((int64_t)(x - midLpA) * coefMidHi) >> 15);
      midLpB += (int32_t)(((int64_t)(x - midLpB) * coefMidLo) >> 15);
      highLp += (int32_t)(((int64_t)(x - highLp) * coefHigh)  >> 15);
      int32_t mid  = clamp16((midLpA - midLpB) >> 8);
      int32_t high = clamp16((x - highLp) >> 8);
      midSumSq  += (uint32_t)(mid * mid);
      highSumSq += (uint32_t)(high * high);
      if (++frameSamples >= AUDIO_FRAME_SAMPLES) {
        onAudioFrame();
        frameSamples = 0; levelSumSq = 0; bassSumSq = 0; midSumSq = 0; highSumSq = 0;
      }
    }
    if (n < I2S_READ_SAMPLES) return;
  }
}

// =====================================================================
//                           LED rendering
// =====================================================================
uint8_t  sparkle[NUM_LEDS];
uint32_t lastHatSpawn = 0;
float    wobbleGlow   = 0;

// 255 during holdMs, then eases out to 0 over fadeMs
uint8_t hitEnvelope(const DrumHit& h, uint32_t now, uint32_t holdMs, uint32_t fadeMs) {
  if (!h.seen) return 0;
  uint32_t e = now - h.at;
  if (e < holdMs) return 255;
  if (e >= holdMs + fadeMs) return 0;
  uint32_t f = (holdMs + fadeMs - e) * 255 / fadeMs;
  return f * f / 255;
}

inline uint8_t clampByte(int v) { return v > 255 ? 255 : (uint8_t)v; }

// Music mode: each element has its own colour and pattern, mixed together
//   kick   -> whole strip flashes in the chosen colour
//   snare  -> burst grows from the centre outwards
//   hi-hat -> a few random pixels sparkle
//   wobble -> background glow that follows the bass level
// The frame is only sent to the strip when it differs from the last one.
void renderMusic(uint32_t now, uint32_t dt, bool force) {
  uint8_t kickE  = hitEnvelope(kickHit,  now, KICK_HOLD_MS, KICK_FADE_MS);
  uint8_t snareE = hitEnvelope(snareHit, now, 0, SNARE_FADE_MS);
  int k = kickE > MUSIC_BASE_LEVEL ? kickE : MUSIC_BASE_LEVEL;

  int target = 0;
  if (wobbleActive)
    target = constrain(map((long)bassEnv, WOBBLE_MIN_LEVEL, WOBBLE_FULL_LEVEL, 20, WOBBLE_MAX_GLOW), 0, WOBBLE_MAX_GLOW);
  wobbleGlow += (target - wobbleGlow) * 0.25f;
  int glow = (int)wobbleGlow;

  uint32_t dec = dt * 255 / HAT_FADE_MS; if (dec < 1) dec = 1;
  for (int i = 0; i < NUM_LEDS; i++) sparkle[i] = sparkle[i] > dec ? sparkle[i] - dec : 0;
  if (hatHit.seen && hatHit.at != lastHatSpawn) {
    lastHatSpawn = hatHit.at;
    for (int s = 0; s < HAT_SPARKS; s++) sparkle[random(NUM_LEDS)] = 255;
  }

  uint32_t se = snareHit.seen ? now - snareHit.at : SNARE_SPREAD_MS;
  if (se > SNARE_SPREAD_MS) se = SNARE_SPREAD_MS;
  int reach = 1 + (int)(NUM_LEDS * se / SNARE_SPREAD_MS);   // in half pixels from the centre

  // Parts that are the same for every pixel, computed once per frame
  const int baseR = col.r * k / 255 + WOBBLE_RGB[0] * glow / 255;
  const int baseG = col.g * k / 255 + WOBBLE_RGB[1] * glow / 255;
  const int baseB = col.b * k / 255 + WOBBLE_RGB[2] * glow / 255;
  const int snR = SNARE_RGB[0] * snareE / 255, snG = SNARE_RGB[1] * snareE / 255, snB = SNARE_RGB[2] * snareE / 255;

  strip.setBrightness(col.bri);
  for (int i = 0; i < NUM_LEDS; i++) {
    int r = baseR, g = baseG, b = baseB;
    if (snareE && abs(2 * i - (NUM_LEDS - 1)) <= reach) { r += snR; g += snG; b += snB; }
    if (uint8_t sp = sparkle[i]) {
      r += HAT_RGB[0] * sp / 255; g += HAT_RGB[1] * sp / 255; b += HAT_RGB[2] * sp / 255;
    }
    strip.setPixelColor(i, clampByte(r), clampByte(g), clampByte(b));
  }

  static uint8_t lastPx[NUM_LEDS * 3];
  const uint8_t* px = strip.getPixels();
  if (!force && memcmp(px, lastPx, sizeof(lastPx)) == 0) return;   // nothing changed (e.g. silence)
  memcpy(lastPx, px, sizeof(lastPx));
  strip.show();
}

void updateLEDs() {
  static uint32_t lastFrame = 0, lastToggle = 0;
  static uint16_t hue = 0;
  static bool     blinkOn = true;
  uint32_t now = millis();

  switch (st.mode) {
    case M_RAINBOW:
      if (now - lastFrame >= 20) {
        lastFrame = now;
        hue += map(st.speed, 1, 100, 16, 1200);
        strip.setBrightness(col.bri);
        strip.rainbow(hue, 1, 255, 255, true);
        strip.show();
      }
      needsRender = false;
      return;

    case M_BLINK: {
      uint16_t d = blinkOn ? st.onMs : st.offMs;
      if (needsRender || now - lastToggle >= d) {
        blinkOn = needsRender ? true : !blinkOn;
        lastToggle = now; needsRender = false;
        strip.setBrightness(col.bri);
        strip.fill(blinkOn ? strip.Color(col.r, col.g, col.b) : 0);
        strip.show();
      }
      return;
    }

    case M_MUSIC: {     // kick / snare / hi-hat / wobble, see renderMusic()
      if (now - lastFrame < 10 && !needsRender) return;
      uint32_t dt = now - lastFrame;
      lastFrame = now;
      renderMusic(now, dt > 100 ? 100 : dt, needsRender);
      needsRender = false;
      return;
    }

    default: {
      if (!needsRender) return;
      needsRender = false;
      uint32_t c = 0;
      switch (st.mode) {
        case M_COLOR: c = strip.Color(col.r, col.g, col.b); break;
        case M_WARM:  c = kelvinToColor(st.warmK);          break;
        case M_COLD:  c = kelvinToColor(st.coldK);          break;
        case M_WHITE: c = strip.Color(255, 255, 255);       break;
        default:      c = 0;                                break;
      }
      strip.setBrightness(col.bri);
      strip.fill(c);
      strip.show();
    }
  }
}

// =====================================================================
//                        ROTARY ENCODER + BUTTON
// =====================================================================
// Quadrature decoding in the interrupt; reads the GPIO register directly
void IRAM_ATTR encoderISR() {
  static const int8_t table[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
  uint32_t in = GPIO.in.val;
  uint8_t s = (((in >> ENC_CLK_PIN) & 1) << 1) | ((in >> ENC_DT_PIN) & 1);
  encState = ((encState << 2) | s) & 0x0F;
  encRaw += table[encState];
}

void initEncoder() {
  pinMode(ENC_CLK_PIN, INPUT_PULLUP);
  pinMode(ENC_DT_PIN,  INPUT_PULLUP);
  pinMode(ENC_SW_PIN,  INPUT_PULLUP);
  encState = (digitalRead(ENC_CLK_PIN) << 1) | digitalRead(ENC_DT_PIN);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK_PIN), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT_PIN),  encoderISR, CHANGE);
}

// Returns whole detents turned since the last call (+ = right, - = left)
int readEncoderSteps() {
  int32_t raw = encRaw;
  int32_t d = (raw - encUsed) / ENC_STEPS_PER_DETENT;
  if (d) encUsed += d * ENC_STEPS_PER_DETENT;
  return ENC_REVERSE ? -d : d;
}

void pollButton(uint32_t now, bool& shortPress, bool& longPress) {
  static bool stable = HIGH, lastRead = HIGH, longFired = false;
  static uint32_t changedAt = 0, downAt = 0;
  bool r = digitalRead(ENC_SW_PIN);
  if (r != lastRead) { lastRead = r; changedAt = now; }
  if (now - changedAt >= BTN_DEBOUNCE_MS && r != stable) {
    stable = r;
    if (stable == LOW) { downAt = now; longFired = false; }
    else if (!longFired) shortPress = true;
  }
  if (stable == LOW && !longFired && now - downAt >= BTN_LONG_PRESS_MS) { longFired = true; longPress = true; }
}

// =====================================================================
//                              OLED DRAWING
// =====================================================================
void goScreen(Screen s) { screen = s; overLeft = 0; listTop = 0; oledDirty = true; }

void showMessage(const String& l1, const String& l2, Screen ret) {
  msgLine1 = l1; msgLine2 = l2; msgReturn = ret; msgUntil = millis() + MESSAGE_MS;
  goScreen(SCR_MESSAGE);
}

String fitText(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 1) + ".";
}

void drawCentered(const String& s, int y) {
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  oled.setCursor((OLED_WIDTH - (int)w) / 2 - x1, y);
  oled.print(s);
}

void drawHeader(const String& title, const String& right) {
  oled.setFont(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0); oled.print(fitText(title, 21 - right.length() - 1));
  if (right.length()) { oled.setCursor(OLED_WIDTH - right.length() * 6, 0); oled.print(right); }
  oled.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
}

String lightStatusShort() { return st.mode == M_OFF ? String("Off") : String(brightnessPct()) + "%"; }

// ---- generic list (menu, colours, saved WiFi, actions) ----
int listCount() {
  switch (screen) {
    case SCR_MENU:        return MENU_COUNT;
    case SCR_COLOUR:      return NUM_PRESETS;
    case SCR_SAVED_WIFI:  return wifiCount + 2;
    case SCR_WIFI_ACTION: return 2;
    default:              return 0;
  }
}

String listLabel(int i) {
  switch (screen) {
    case SCR_MENU:   return MENU_ITEMS[i];
    case SCR_COLOUR: return PRESETS[i].name;
    case SCR_SAVED_WIFI:
      if (i < wifiCount) return wifiSsid[i];
      if (i == wifiCount) return "Set up new WiFi";
      return noWifiMode ? "Turn WiFi on" : "Use without WiFi";
    case SCR_WIFI_ACTION: return i == 0 ? "Connect now" : "Forget";
    default: return "";
  }
}

String listValue(int i) {
  if (screen == SCR_MENU) {
    switch (i) {
      case 0: return st.mode == M_OFF ? "Off" : "On";
      case 1: return String(brightnessPct()) + "%";
      case 3: return onTimer.active  ? String((timerLeftS(onTimer)  + 59) / 60) + "m" : "Off";
      case 4: return offTimer.active ? String((timerLeftS(offTimer) + 59) / 60) + "m" : "Off";
      case 6: return String(wifiCount);
      case 7: return battOk ? String(battPct) + "%" : String("--");
      case 8: return "v" FW_VERSION;
    }
  }
  if (screen == SCR_COLOUR && i == currentPresetIndex()) return "*";
  if (screen == SCR_SAVED_WIFI && i < wifiCount && wifiSsid[i] == connectedSsid && WiFi.status() == WL_CONNECTED) return "Now";
  return "";
}

void drawList(const String& title) {
  const int rows = 4, y0 = 13, rowH = 12;
  int count = listCount();
  drawHeader(title, "");
  if (listSel < listTop) listTop = listSel;
  if (listSel >= listTop + rows) listTop = listSel - rows + 1;
  for (int r = 0; r < rows; r++) {
    int i = listTop + r;
    if (i >= count) break;
    int y = y0 + r * rowH;
    bool sel = (i == listSel);
    if (sel) { oled.fillRoundRect(0, y, 122, rowH, 2, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else oled.setTextColor(SSD1306_WHITE);
    String v = listValue(i);
    int maxChars = (116 - (int)v.length() * 6 - (v.length() ? 4 : 0)) / 6;
    oled.setCursor(4, y + 2); oled.print(fitText(listLabel(i), maxChars));
    if (v.length()) { oled.setCursor(118 - v.length() * 6, y + 2); oled.print(v); }
  }
  oled.setTextColor(SSD1306_WHITE);
  if (count > rows) {                                      // scroll bar
    int trackH = rows * rowH, barH = max(6, trackH * rows / count);
    int barY = y0 + (trackH - barH) * listTop / (count - rows);
    oled.fillRect(125, barY, 2, barH, SSD1306_WHITE);
  }
}

// ---- home screen: network name + IP ----
// Small battery icon (12 x 7 px) filled to the current percent
void drawBatteryIcon(int x, int y) {
  oled.drawRect(x, y, 10, 7, SSD1306_WHITE);
  oled.fillRect(x + 10, y + 2, 2, 3, SSD1306_WHITE);
  if (battOk) oled.fillRect(x + 2, y + 2, (6 * battPct + 50) / 100, 3, SSD1306_WHITE);
}

// Home header: battery icon + percent + voltage on the left, light status on the right
void drawHomeHeader() {
  oled.setFont(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  drawBatteryIcon(0, 0);
  oled.setCursor(15, 0);
  oled.print(battOk ? String(battPct) + "% " + String(battV, 2) + "V" : String("No battery"));
  String right = lightStatusShort();
  oled.setCursor(OLED_WIDTH - right.length() * 6, 0); oled.print(right);
  oled.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
}

void drawHome() {
  drawHomeHeader();
  oled.setFont(); oled.setTextSize(1);
  if (portalActive) {
    oled.setCursor(0, 15); oled.print("Setup mode. Join WiFi");
    oled.setCursor(0, 27); oled.print(fitText(AP_SSID, 21));
    oled.setCursor(0, 42); oled.print("then open");
    oled.setFont(&FreeSans9pt7b); oled.setCursor(0, 62); oled.print(apIP.toString());
  } else if (noWifiMode) {
    oled.setFont(&FreeSansBold9pt7b); oled.setCursor(0, 29); oled.print("No-WiFi mode");
    oled.setFont(); oled.setCursor(0, 40); oled.print("Double clap to turn");
    oled.setCursor(0, 50); oled.print("the light on or off");
  } else {
    bool up = WiFi.status() == WL_CONNECTED;
    oled.setCursor(0, 15); oled.print(up ? "Network" : "Reconnecting to");
    oled.setCursor(0, 25); oled.print(fitText(connectedSsid, 21));
    oled.setCursor(0, 39); oled.print("IP address");
    String ip = WiFi.localIP().toString();
    oled.setFont(&FreeSans9pt7b);
    int16_t x1, y1; uint16_t w, h;
    oled.getTextBounds(ip, 0, 62, &x1, &y1, &w, &h);
    if (w > OLED_WIDTH) { oled.setFont(); oled.setCursor(0, 52); }
    else oled.setCursor(0, 62);
    oled.print(ip);
  }
  oled.setFont();
}

// ---- note screen ----
void setNoteFont(int f) { oled.setFont(NOTE_FONTS[f].font); oled.setTextSize(NOTE_FONTS[f].size); }

int noteTextWidth(const String& s) {
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(s, 0, 30, &x1, &y1, &w, &h);
  return w;
}

void pushNoteLine(const String& l) { if (noteLineCount < 16) noteLines[noteLineCount++] = l; }

// Word-wraps the note to the OLED width for the chosen font
void wrapNote() {
  noteLineCount = 0;
  setNoteFont(noteFont);
  int start = 0;
  while (start <= (int)noteText.length()) {
    int nl = noteText.indexOf('\n', start);
    if (nl < 0) nl = noteText.length();
    String para = noteText.substring(start, nl), line = "";
    int p = 0;
    while (p <= (int)para.length()) {
      int sp = para.indexOf(' ', p);
      if (sp < 0) sp = para.length();
      String word = para.substring(p, sp);
      String cand = line.length() ? line + " " + word : word;
      if (noteTextWidth(cand) <= OLED_WIDTH) line = cand;
      else {
        if (line.length()) pushNoteLine(line);
        while (word.length() > 1 && noteTextWidth(word) > OLED_WIDTH) {   // very long word
          int k = word.length();
          while (k > 1 && noteTextWidth(word.substring(0, k)) > OLED_WIDTH) k--;
          pushNoteLine(word.substring(0, k));
          word = word.substring(k);
        }
        line = word;
      }
      p = sp + 1;
    }
    pushNoteLine(line);
    start = nl + 1;
  }
  noteWrapDirty = false;
}

int notePerPage() { return max(1, OLED_HEIGHT / NOTE_FONTS[noteFont].lineH); }

void drawNote() {
  if (imgOn) { oled.drawBitmap(0, 0, imgBits, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE); return; }
  if (noteWrapDirty) wrapNote();
  const NoteFont& f = NOTE_FONTS[noteFont];
  int perPage = notePerPage();
  int pages   = (noteLineCount + perPage - 1) / perPage;
  int page    = pages > 1 ? (millis() / NOTE_PAGE_MS) % pages : 0;
  int first   = page * perPage;
  int shown   = min(perPage, noteLineCount - first);
  int y0      = (OLED_HEIGHT - shown * f.lineH) / 2;
  setNoteFont(noteFont);
  oled.setTextColor(SSD1306_WHITE);
  for (int j = 0; j < shown; j++) {
    const String& l = noteLines[first + j];
    int x = noteAlign == 1 ? (OLED_WIDTH - noteTextWidth(l)) / 2 : 0;
    int y = y0 + j * f.lineH + (f.font ? f.baseline : 1);
    oled.setCursor(max(0, x), y);
    oled.print(l);
  }
  if (pages > 1)                                           // page dots
    for (int i = 0; i < pages; i++) oled.fillRect(OLED_WIDTH - 4, 2 + i * 4, 2, 2, i == page ? SSD1306_WHITE : SSD1306_BLACK);
  oled.setFont(); oled.setTextSize(1);
}

void drawBrightness() {
  drawHeader("Brightness", "");
  oled.setTextSize(3);
  drawCentered(String(editPct) + "%", 20);
  oled.setTextSize(1);
  oled.drawRoundRect(4, 50, 120, 9, 3, SSD1306_WHITE);
  oled.fillRoundRect(6, 52, 116 * editPct / 100, 5, 2, SSD1306_WHITE);
}

void drawDelay(bool isOn) {
  DelayTimer& t = isOn ? onTimer : offTimer;
  drawHeader(isOn ? "On delay" : "Off delay", "");
  uint16_t m = DELAY_STEPS[editIndex];
  oled.setTextSize(2);
  drawCentered(m == 0 ? (t.active ? String("Cancel") : String("Off")) : formatMinutes(m), 20);
  oled.setTextSize(1);
  if (t.active) drawCentered(String(isOn ? "On in " : "Off in ") + formatCountdown(timerLeftS(t)), 42);
  drawCentered(m == 0 ? (t.active ? "Press to cancel" : "Turn to set") : "Press to start", 54);
}

void drawWifiInfo() {
  drawHeader("WiFi details", "");
  oled.setFont(); oled.setTextSize(1);
  String l[5];
  if (noWifiMode)        { l[0] = "WiFi is off"; l[1] = "Mode: No-WiFi"; l[2] = "Double clap toggles"; }
  else if (portalActive) { l[0] = "Setup portal on"; l[1] = String("AP: ") + AP_SSID; l[2] = "IP: " + apIP.toString(); }
  else {
    l[0] = "SSID: " + connectedSsid;
    l[1] = "IP: " + WiFi.localIP().toString();
    l[2] = "Signal: " + String(WiFi.RSSI()) + " dBm";
    l[3] = String("Name: ") + HOSTNAME + ".local";
    l[4] = String("Sound: ") + SYS_NAMES[sysMode()];
  }
  for (int i = 0; i < 5; i++) { oled.setCursor(0, 14 + i * 10); oled.print(fitText(l[i], 21)); }
}

void drawBattery() {
  drawHeader("Battery", battOk && battPct <= BATT_LOW_PCT ? "Low" : "");
  if (!battOk) {
    oled.setFont(&FreeSansBold9pt7b); drawCentered("No battery", 34);
    oled.setFont(); oled.setTextSize(1);
    drawCentered("Pin " + String(BATT_PIN) + ": " + String(battV < 0 ? 0 : battV, 2) + " V", 50);
    return;
  }
  oled.setTextSize(3);
  drawCentered(String(battPct) + "%", 16);
  oled.setTextSize(1);
  drawCentered(String(battV, 2) + " V", 41);
  oled.drawRoundRect(4, 52, 120, 9, 3, SSD1306_WHITE);
  oled.fillRoundRect(6, 54, 116 * battPct / 100, 5, 2, SSD1306_WHITE);
}

void drawAbout() {
  drawHeader("About", "");
  oled.setFont(&FreeSansBold9pt7b); drawCentered("Light strip", 29);
  oled.setFont(); oled.setTextSize(1);
  drawCentered("Version " FW_VERSION, 36);
  drawCentered("Designed by " FW_DESIGNER, 50);
}

void drawMessage() {
  oled.setFont(&FreeSansBold9pt7b); drawCentered(msgLine1, 30);
  oled.setFont(); oled.setTextSize(1); drawCentered(msgLine2, 44);
}

void drawStatus(const String& l1, const String& l2) {       // used during start-up
  if (!oledOk) return;
  oled.clearDisplay();
  drawHeader("Light strip", "v" FW_VERSION);
  oled.setFont(&FreeSansBold9pt7b); drawCentered(l1, 36);
  oled.setFont(); oled.setTextSize(1); drawCentered(fitText(l2, 21), 48);
  oled.display();
}

void drawScreen() {
  oled.clearDisplay();
  oled.setFont(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  switch (screen) {
    case SCR_HOME:        drawHome(); break;
    case SCR_NOTE:        drawNote(); break;
    case SCR_MENU:        drawList("Menu"); break;
    case SCR_COLOUR:      drawList("Colour"); break;
    case SCR_SAVED_WIFI:  drawList("Saved WiFi"); break;
    case SCR_WIFI_ACTION: drawList(fitText(wifiSsid[wifiSel], 21)); break;
    case SCR_BRIGHTNESS:  drawBrightness(); break;
    case SCR_ON_DELAY:    drawDelay(true); break;
    case SCR_OFF_DELAY:   drawDelay(false); break;
    case SCR_WIFI_INFO:   drawWifiInfo(); break;
    case SCR_BATTERY:     drawBattery(); break;
    case SCR_ABOUT:       drawAbout(); break;
    case SCR_MESSAGE:     drawMessage(); break;
  }
  oled.display();
}

// =====================================================================
//                         OLED MENU LOGIC
// =====================================================================
void scheduleRestart() { restartPending = true; pendingAt = millis() + 1500; }

// Scrolls a list. Returns true when turned left more than 2 clicks past the top.
bool listRotate(int steps, int count) {
  while (steps > 0) { steps--; overLeft = 0; if (listSel < count - 1) listSel++; }
  while (steps < 0) {
    steps++;
    if (listSel > 0) listSel--;
    else if (++overLeft >= BACK_LEFT_COUNTS) { overLeft = 0; return true; }
  }
  return false;
}

// Info screens: any 3 left clicks go back
bool infoRotate(int steps) {
  if (steps > 0) overLeft = 0;
  if (steps < 0) overLeft -= steps;
  if (overLeft >= BACK_LEFT_COUNTS) { overLeft = 0; return true; }
  return false;
}

void openMenu(int sel) { goScreen(SCR_MENU); listSel = sel; }

void selectMenuItem(int i) {
  switch (i) {
    case 0: togglePower(); break;
    case 1: editPct = brightnessPct(); goScreen(SCR_BRIGHTNESS); break;
    case 2: goScreen(SCR_COLOUR); listSel = currentPresetIndex(); break;
    case 3: editIndex = 0; goScreen(SCR_ON_DELAY); break;
    case 4: editIndex = 0; goScreen(SCR_OFF_DELAY); break;
    case 5: goScreen(SCR_WIFI_INFO); break;
    case 6: goScreen(SCR_SAVED_WIFI); listSel = 0; break;
    case 7: goScreen(SCR_BATTERY); break;
    case 8: goScreen(SCR_ABOUT); break;
  }
}

void handleUi(uint32_t now) {
  int  steps = readEncoderSteps();
  bool sp = false, lp = false;
  pollButton(now, sp, lp);
  bool input = steps || sp || lp;
  if (input) { lastInput = now; oledDirty = true; }

  if (screen == SCR_MESSAGE) {
    if (sp || (int32_t)(now - msgUntil) >= 0) {
      Screen r = msgReturn; goScreen(r);
      if (r == SCR_MENU) listSel = menuSel;
    }
    return;
  }
  if (lp && screen != SCR_HOME && screen != SCR_NOTE) { goScreen(SCR_HOME); return; }

  switch (screen) {
    case SCR_HOME:
    case SCR_NOTE:
      if (sp) openMenu(0);
      else if (steps && screen == SCR_NOTE) goScreen(SCR_HOME);
      break;

    case SCR_MENU:
      if (listRotate(steps, MENU_COUNT)) goScreen(SCR_HOME);
      else if (sp) { menuSel = listSel; selectMenuItem(listSel); }
      menuSel = (screen == SCR_MENU) ? listSel : menuSel;
      break;

    case SCR_BRIGHTNESS:
      while (steps > 0) { steps--; overLeft = 0; editPct = min(100, editPct < 5 ? 5 : editPct + 5); }
      while (steps < 0) {
        steps++;
        if (editPct > 5) editPct -= 5;
        else if (editPct > 1) editPct = 1;
        else if (++overLeft >= BACK_LEFT_COUNTS) { openMenu(menuSel); return; }
      }
      if (input && !sp) { col.bri = max(1, editPct * 255 / 100); markChanged(); }
      if (sp) openMenu(menuSel);
      break;

    case SCR_COLOUR: {
      int before = listSel;
      if (listRotate(steps, NUM_PRESETS)) { openMenu(menuSel); break; }
      if (listSel != before) applyPreset(listSel);           // live preview on the strip
      if (sp) { applyPreset(listSel); openMenu(menuSel); }
      break;
    }

    case SCR_ON_DELAY:
    case SCR_OFF_DELAY: {
      bool isOn = screen == SCR_ON_DELAY;
      while (steps > 0) { steps--; overLeft = 0; if (editIndex < NUM_DELAY_STEPS - 1) editIndex++; }
      while (steps < 0) {
        steps++;
        if (editIndex > 0) editIndex--;
        else if (++overLeft >= BACK_LEFT_COUNTS) { openMenu(menuSel); return; }
      }
      if (sp) {
        uint16_t m = DELAY_STEPS[editIndex];
        startTimer(isOn ? onTimer : offTimer, m);
        if (m == 0) showMessage("Cancelled", isOn ? "No on delay" : "No off delay", SCR_MENU);
        else        showMessage(isOn ? "On delay set" : "Off delay set", String(isOn ? "On in " : "Off in ") + formatMinutes(m), SCR_MENU);
      }
      break;
    }

    case SCR_WIFI_INFO:
    case SCR_BATTERY:
    case SCR_ABOUT:
      if (infoRotate(steps) || sp) openMenu(menuSel);
      break;

    case SCR_SAVED_WIFI:
      if (listRotate(steps, listCount())) { openMenu(menuSel); break; }
      if (sp) {
        if (listSel < wifiCount) { wifiSel = listSel; goScreen(SCR_WIFI_ACTION); listSel = 0; }
        else if (listSel == wifiCount) {
          setWifiFlag("nowifi", false); setWifiFlag("portal1", true);
          showMessage("WiFi setup", "Restarting...", SCR_HOME); scheduleRestart();
        } else if (noWifiMode) {
          setWifiFlag("nowifi", false);
          showMessage("WiFi on", "Restarting...", SCR_HOME); scheduleRestart();
        } else {
          setWifiFlag("nowifi", true);
          offlinePending = true; pendingAt = now + 300;
          showMessage("WiFi off", "Double clap toggles", SCR_HOME);
        }
      }
      break;

    case SCR_WIFI_ACTION:
      if (listRotate(steps, 2)) { goScreen(SCR_SAVED_WIFI); listSel = wifiSel; break; }
      if (sp) {
        if (listSel == 0) {
          preferredSsid = wifiSsid[wifiSel]; saveNetworks(); setWifiFlag("nowifi", false);
          showMessage("Connecting", fitText(preferredSsid, 21), SCR_HOME); scheduleRestart();
        } else {
          String s = wifiSsid[wifiSel];
          forgetNetwork(s);
          showMessage("Forgotten", fitText(s, 21), SCR_SAVED_WIFI);
          listSel = 0;
        }
      }
      break;

    default: break;
  }

  // Idle: after the knob has not been used for a while, show the note
  if (!input && screen != SCR_NOTE && now - lastInput > (uint32_t)noteIdleS * 1000UL) {
    Screen target = (noteText.length() || imgOn) ? SCR_NOTE : SCR_HOME;
    if (screen != target) goScreen(target);
  }
}

// Screens whose content changes on its own (countdown, RSSI, note pages).
// Everything else is redrawn only when oledDirty is set, because each
// full refresh blocks the loop for ~25 ms.
bool screenNeedsTick() {
  switch (screen) {
    case SCR_ON_DELAY:  return onTimer.active;
    case SCR_OFF_DELAY: return offTimer.active;
    case SCR_WIFI_INFO: return !noWifiMode && !portalActive;
    case SCR_NOTE:      return !imgOn && (noteWrapDirty || noteLineCount > notePerPage());
    default:            return false;
  }
}

void updateOled(uint32_t now) {
  if (!oledOk) return;
  if (oledDirty || (now - lastOledDraw >= OLED_REFRESH_MS && screenNeedsTick())) {
    oledDirty = false; lastOledDraw = now;
    drawScreen();
  }
}

// =====================================================================
//                          Dashboard web page
// =====================================================================
const char CONTROL_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Light strip</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;500;600;700&display=swap" rel="stylesheet" media="print" onload="this.media='all'">
<style>
:root{
 --bg:#FAFAFA;--surface:#FFFFFF;--line:#E7E7E4;--line-strong:#D6D6D2;
 --ink:#27272A;--mute:#64748B;--accent:#1E293B;--hover:#F4F4F2;
 --r-card:12px;--r-ctl:8px;
 --font:'Plus Jakarta Sans',-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;
}
*{box-sizing:border-box;margin:0}
html{-webkit-text-size-adjust:100%}
body{background:var(--bg);color:var(--ink);font:400 15px/1.6 var(--font);-webkit-font-smoothing:antialiased}
button,input,textarea{font:inherit;color:inherit}
button{cursor:pointer;background:none;border:0}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:var(--r-ctl)}
[hidden]{display:none!important}
ul{list-style:none;padding:0}

.app{max-width:1080px;margin:0 auto;padding:40px 24px 48px}
.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:32px}
.brand{font-weight:600;font-size:15px;letter-spacing:-.005em}
.brand small{color:var(--mute);font-weight:500;font-size:12px;margin-left:8px}
.status{display:flex;align-items:center;gap:8px;color:var(--mute);font-size:13px}
.status i{width:7px;height:7px;border-radius:50%;background:#22A06B}
.status.bad i{background:#D14343}

.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--r-card);padding:28px}
h2{font-size:17px;font-weight:600;line-height:1.4;letter-spacing:-.01em}
.sub{color:var(--mute);font-size:13px;line-height:1.5}
.stack{display:grid;gap:24px}
.grid{display:grid;grid-template-columns:7fr 5fr;gap:24px;align-items:start;margin-top:24px}

/* ---------- Hero with lamp ---------- */
.hero{display:grid;grid-template-columns:auto 1fr auto;gap:40px;align-items:center;padding:28px 32px 28px 24px}
.lampbox{width:200px;height:232px}
.lampbox svg{width:100%;height:100%;overflow:visible;display:block}
#beamPath,#halo,#bulb{transition:opacity .5s ease,fill .5s ease}
.pull{cursor:pointer;outline:none}
.pull .cordg{transition:transform .25s cubic-bezier(.3,1.6,.5,1)}
.pull:hover .bead{fill:var(--hover)}
.pull.pulling .cordg{transform:translateY(10px)}
.pull:focus-visible .bead{stroke:var(--accent);stroke-width:2.5}
.lampbox.rainbow #beamPath,.lampbox.rainbow #halo{animation:hue 6s linear infinite}
@keyframes hue{to{filter:hue-rotate(360deg)}}
.hero-text h1{font-size:34px;font-weight:700;line-height:1.15;letter-spacing:-.03em;margin-bottom:8px}
.hero-text .sub{font-size:14px}
.power{display:flex;align-items:center;gap:12px;margin-top:28px;font-size:14px;font-weight:500}
.power .hint{color:var(--mute);font-weight:400;font-size:13px}
.switch{width:48px;height:28px;border-radius:14px;background:var(--line-strong);position:relative;transition:background .2s;flex:none}
.switch::after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:#fff;transition:transform .2s}
.switch[aria-checked="true"]{background:var(--accent)}
.switch[aria-checked="true"]::after{transform:translateX(20px)}

/* volume-style brightness (top right) */
.vol{display:flex;flex-direction:column;align-items:center;gap:10px;align-self:start}
.vol output{font-size:13px;font-weight:600;font-variant-numeric:tabular-nums}
.vol .cap{font-size:12px;color:var(--mute)}
.vslider{width:46px;height:170px;border-radius:14px;background:var(--hover);border:1px solid var(--line);position:relative;overflow:hidden;cursor:ns-resize;touch-action:none;transition:transform .18s}
.vslider:hover{transform:scale(1.02)}
.vfill{position:absolute;left:0;right:0;bottom:0;background:var(--accent)}
.vslider .ticks{position:absolute;inset:14px 0;display:flex;flex-direction:column;justify-content:space-between;align-items:center;pointer-events:none}
.vslider .ticks i{width:10px;height:1px;background:rgba(127,127,127,.35)}
.vol svg{color:var(--mute)}

/* ---------- Modes ---------- */
.modes{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
.tile{display:flex;flex-direction:column;align-items:flex-start;gap:14px;padding:18px;border:1px solid var(--line);border-radius:var(--r-card);background:var(--surface);text-align:left;transition:transform .18s ease,background .18s ease,border-color .18s ease}
.tile:hover{transform:scale(1.02);background:var(--hover)}
.tile .dot{width:22px;height:22px;border-radius:50%;border:1px solid rgba(0,0,0,.08)}
.tile b{display:block;font-weight:600;font-size:14px;line-height:1.3}
.tile small{display:block;color:var(--mute);font-size:12px;margin-top:2px}
.tile.active{border-color:var(--accent);box-shadow:inset 0 0 0 1px var(--accent)}
.tile.wide{grid-column:1/-1;flex-direction:row;align-items:center;gap:16px}

.panel h2{margin-bottom:4px}
.panel>.sub{margin-bottom:24px}
.field{margin-top:24px}
.label{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:10px;font-size:14px;font-weight:500}
.label output{color:var(--mute);font-weight:400;font-variant-numeric:tabular-nums}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:24px;background:transparent;cursor:pointer}
input[type=range]::-webkit-slider-runnable-track{height:4px;border-radius:2px;background:var(--track)}
input[type=range]::-moz-range-track{height:4px;border-radius:2px;background:var(--track)}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;margin-top:-8px;border-radius:50%;background:#fff;border:1px solid var(--line-strong);transition:transform .15s}
input[type=range]::-moz-range-thumb{width:20px;height:20px;border-radius:50%;background:#fff;border:1px solid var(--line-strong)}
input[type=range]:active::-webkit-slider-thumb{transform:scale(1.1)}
.r-fill{--track:linear-gradient(to right,var(--accent) var(--p,50%),var(--line) var(--p,50%))}
.r-warm{--track:linear-gradient(to right,#FF7E00,#FFA54F,#FFC489)}
.r-cold{--track:linear-gradient(to right,#FFE4CE,#F3F2FF,#C9DAFF)}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:16px}
.chip{padding:7px 14px;border:1px solid var(--line);border-radius:var(--r-ctl);font-size:13px;font-weight:500;transition:transform .18s ease,background .18s ease}
.chip:hover{transform:scale(1.02);background:var(--hover)}
.chip.on{background:var(--accent);border-color:var(--accent);color:#fff}
.picker{display:flex;align-items:center;gap:14px;padding:10px;border:1px solid var(--line);border-radius:var(--r-ctl);cursor:pointer;transition:background .18s}
.picker:hover{background:var(--hover)}
.picker input{width:40px;height:40px;border:0;padding:0;background:none;cursor:pointer;border-radius:6px}
.picker input::-webkit-color-swatch-wrapper{padding:0}
.picker input::-webkit-color-swatch{border:1px solid rgba(0,0,0,.08);border-radius:6px}
.picker span{font-variant-numeric:tabular-nums;font-weight:500;text-transform:uppercase}
.swatches{display:grid;grid-template-columns:repeat(9,1fr);gap:8px;margin-top:16px}
.sw{aspect-ratio:1;border-radius:50%;border:1px solid rgba(0,0,0,.08);transition:transform .18s}
.sw:hover{transform:scale(1.08)}
.note{padding:14px 16px;background:var(--bg);border-radius:var(--r-ctl);color:var(--mute);font-size:13px}

/* ---------- Schedule ---------- */
.srow{padding-top:20px}
.srow+.srow{margin-top:20px;border-top:1px solid var(--line)}
.shead{display:flex;justify-content:space-between;align-items:baseline}
.shead b{font-size:14px;font-weight:600}
.custom{display:flex;gap:8px;margin-top:12px}
.input,.custom input{flex:1;min-width:0;padding:9px 12px;border:1px solid var(--line);border-radius:var(--r-ctl);background:var(--surface)}
.input:focus,.custom input:focus,textarea:focus{outline:none;border-color:var(--accent)}
.primary{padding:9px 18px;border-radius:var(--r-ctl);background:var(--accent);color:#fff;font-weight:600;font-size:14px;transition:transform .18s ease,opacity .18s}
.primary:hover{transform:scale(1.02);opacity:.92}
.count{font-size:26px;font-weight:700;letter-spacing:-.02em;font-variant-numeric:tabular-nums;line-height:1.2;margin-top:10px}
.bar{height:3px;background:var(--line);border-radius:2px;margin:12px 0 8px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--accent);transition:width 1s linear}
.text-btn{color:var(--mute);font-size:13px;font-weight:500;padding:4px 0;transition:color .18s}
.text-btn:hover{color:var(--ink)}

/* ---------- OLED note ---------- */
.bezel{background:#101114;border-radius:10px;padding:12px;margin:20px 0 16px}
.oled-wrap{max-width:300px;margin:0 auto;container-type:inline-size}
.oled{aspect-ratio:2/1;width:100%;background:#000;color:#EAF1FF;overflow:hidden;display:flex;flex-direction:column;justify-content:center;white-space:pre-wrap;overflow-wrap:anywhere;padding:0 1%}
.oled.center{text-align:center}
.oled.f0{font:700 7.4cqw/1.28 'Courier New',monospace}
.oled.f1{font:700 14.8cqw/1.15 'Courier New',monospace}
.oled.f2{font:700 22cqw/1.1 'Courier New',monospace}
.oled.f3{font:400 10.5cqw/1.35 Helvetica,Arial,sans-serif}
.oled.f4{font:700 10.5cqw/1.35 Helvetica,Arial,sans-serif}
.oled.f5{font:400 11cqw/1.3 Georgia,'Times New Roman',serif}
.oled.f6{font:400 13cqw/1.2 'Courier New',monospace}
.oled:empty::before{content:"Empty note: the display shows the IP address instead";color:#5B6475;font:400 6cqw/1.4 var(--font);text-align:center}
textarea{width:100%;min-height:84px;resize:vertical;padding:10px 12px;border:1px solid var(--line);border-radius:var(--r-ctl);background:var(--surface);line-height:1.5}
.meta{display:flex;justify-content:space-between;color:var(--mute);font-size:12px;margin-top:6px}
.lab{display:block;font-size:14px;font-weight:500;margin-top:20px}
.seg{display:inline-flex;border:1px solid var(--line);border-radius:var(--r-ctl);padding:3px;margin-top:10px;gap:2px}
.seg button{padding:6px 12px;border-radius:6px;font-size:13px;font-weight:500;color:var(--mute);transition:background .18s,color .18s}
.seg button:hover{background:var(--hover);color:var(--ink)}
.seg button.on{background:var(--accent);color:#fff}
.save-row{display:flex;align-items:center;gap:14px;margin-top:24px}
.ok{color:#22A06B;font-size:13px}

/* ---------- Saved WiFi ---------- */
.wifi{margin-top:24px}
.wifi-grid{display:grid;grid-template-columns:1fr 1fr;gap:48px}
.nets{margin-top:20px;border:1px solid var(--line);border-radius:var(--r-card)}
.nets li{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:14px 16px;transition:background .18s}
.nets li+li{border-top:1px solid var(--line)}
.nets li:hover{background:var(--hover)}
.nets .nm{min-width:0}
.nets .nm b{display:block;font-weight:600;font-size:14px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.nets .nm small{color:var(--mute);font-size:12px}
.nets .nm small.live{color:#22A06B}
.nets .acts{display:flex;gap:16px;flex:none}
.nets .empty{color:var(--mute);font-size:14px}
.form label{display:block;font-size:14px;font-weight:500;margin:16px 0 8px}
.form .input{width:100%}
.form .show{display:flex;align-items:center;gap:8px;font-weight:400;color:var(--mute);font-size:13px;margin:12px 0 20px}

/* ---------- Footer ---------- */
.device{display:grid;grid-template-columns:repeat(5,auto) 1fr;gap:4px 32px;margin-top:48px;padding-top:24px;border-top:1px solid var(--line);font-size:13px}
.device dt{color:var(--mute)}
.device dd{font-variant-numeric:tabular-nums;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:200px}
.device .text-btn{justify-self:end;align-self:center}
.credit{display:flex;justify-content:space-between;margin-top:28px;color:var(--mute);font-size:12px}

@media (max-width:860px){
 .app{padding:28px 18px 40px}
 .grid{grid-template-columns:1fr}
 .hero{grid-template-columns:auto 1fr;gap:20px;padding:24px 22px}
 .hero-text{grid-column:1/-1;order:3}
 .lampbox{width:150px;height:174px}
 .vol{justify-self:end}
 .card{padding:22px}
 .wifi-grid{grid-template-columns:1fr;gap:32px}
 .device{grid-template-columns:1fr 1fr}
 .device .text-btn{grid-column:span 2;justify-self:start;margin-top:12px}
}
@media (max-width:420px){.modes{grid-template-columns:repeat(2,1fr)}.swatches{gap:6px}}
@media (prefers-reduced-motion:reduce){*{transition:none!important;animation:none!important}}
</style></head><body>
<main class="app">
 <header class="top">
  <div class="brand">Light strip<small>v0.1</small></div>
  <div class="status" id="status"><i></i><span>Connecting</span></div>
 </header>

 <!-- Hero: lamp, state and volume-style brightness -->
 <section class="card hero" aria-live="polite">
  <div class="lampbox" id="lampbox">
   <svg viewBox="0 0 200 232" aria-hidden="false" role="img" aria-label="Lamp preview">
    <defs>
     <linearGradient id="beam" x1="0" y1="0" x2="0" y2="1">
      <stop id="beamStop" offset="0" stop-color="#FFD9A8" stop-opacity=".9"/>
      <stop id="beamStop2" offset="1" stop-color="#FFD9A8" stop-opacity="0"/>
     </linearGradient>
    </defs>
    <rect x="84" y="0" width="32" height="5" rx="2.5" fill="#27272A"/>
    <line x1="100" y1="5" x2="100" y2="54" stroke="#27272A" stroke-width="1.25"/>
    <path id="beamPath" d="M64 108 L14 232 L186 232 L136 108 Z" fill="url(#beam)" opacity="0"/>
    <circle id="halo" cx="100" cy="112" r="30" fill="#FFD9A8" opacity="0"/>
    <path d="M56 108 C56 76 75 56 100 56 C125 56 144 76 144 108 Z" fill="#FFFFFF" stroke="#27272A" stroke-width="1.5" stroke-linejoin="round"/>
    <path d="M70 92 C74 78 84 68 97 65" fill="none" stroke="#E7E7E4" stroke-width="1.5" stroke-linecap="round"/>
    <rect x="94" y="50" width="12" height="8" rx="2" fill="#27272A"/>
    <line x1="52" y1="108" x2="148" y2="108" stroke="#27272A" stroke-width="1.5" stroke-linecap="round"/>
    <path id="bulb" d="M87 108 A13 13 0 0 0 113 108 Z" fill="#EDEDEA" stroke="#27272A" stroke-width="1"/>
    <g class="pull" id="pull" role="button" tabindex="0" aria-label="Pull cord, turns the light on or off">
     <rect x="124" y="104" width="22" height="66" fill="transparent"/>
     <g class="cordg">
      <line x1="135" y1="108" x2="135" y2="150" stroke="#27272A" stroke-width="1"/>
      <circle class="bead" cx="135" cy="155" r="4.5" fill="#FFFFFF" stroke="#27272A" stroke-width="1.25"/>
     </g>
    </g>
   </svg>
  </div>
  <div class="hero-text">
   <h1 id="hTitle">&nbsp;</h1>
   <p class="sub" id="hSub">&nbsp;</p>
   <div class="power">
    <button class="switch" id="power" role="switch" aria-checked="false" aria-label="Power"></button>
    <span id="pLabel">Off</span><span class="hint">or pull the cord</span>
   </div>
  </div>
  <div class="vol">
   <output id="briV">0%</output>
   <div class="vslider" id="vbri" role="slider" tabindex="0" aria-label="Brightness" aria-valuemin="1" aria-valuemax="100" aria-valuenow="50">
    <div class="vfill" id="vfill"></div>
    <div class="ticks"><i></i><i></i><i></i><i></i><i></i></div>
   </div>
   <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>
   <span class="cap">Brightness</span>
  </div>
 </section>

 <div class="grid">
  <div class="stack">
   <section class="modes" aria-label="Mode">
    <button class="tile" data-m="color"><span class="dot"></span><span><b>Colour</b><small>Any hue</small></span></button>
    <button class="tile" data-m="warm"><span class="dot"></span><span><b>Warm</b><small>1800 to 3500 K</small></span></button>
    <button class="tile" data-m="cold"><span class="dot"></span><span><b>Cold</b><small>5000 to 10000 K</small></span></button>
    <button class="tile" data-m="white"><span class="dot"></span><span><b>White</b><small>All channels</small></span></button>
    <button class="tile" data-m="rainbow"><span class="dot"></span><span><b>Rainbow</b><small>Moving spectrum</small></span></button>
    <button class="tile" data-m="blink"><span class="dot"></span><span><b>Blink</b><small>On and off</small></span></button>
    <button class="tile wide" data-m="music"><span class="dot"></span><span><b>Music</b><small>Kick, snare, hi-hat and bass each light up differently</small></span></button>
   </section>

   <section class="card panel" data-p="color">
    <h2>Colour</h2><p class="sub">Choose any colour, or start from a swatch.</p>
    <label class="picker"><input type="color" id="col"><span id="colHex"></span></label>
    <div class="swatches" id="swatches"></div>
   </section>
   <section class="card panel" data-p="warm" hidden>
    <h2>Warm white</h2><p class="sub">Lower values feel like candlelight, higher like a classic bulb.</p>
    <div class="field"><div class="label"><label for="warm">Tone</label><output id="warmV"></output></div>
     <input type="range" class="r-warm" id="warm" min="1800" max="3500" step="50"></div>
    <div class="chips" data-for="warm">
     <button class="chip" data-v="1900">Candle</button><button class="chip" data-v="2400">Evening</button>
     <button class="chip" data-v="2700">Soft</button><button class="chip" data-v="3200">Studio</button></div>
   </section>
   <section class="card panel" data-p="cold" hidden>
    <h2>Cold white</h2><p class="sub">Crisp light for focus. Higher values turn slightly blue.</p>
    <div class="field"><div class="label"><label for="cold">Tone</label><output id="coldV"></output></div>
     <input type="range" class="r-cold" id="cold" min="5000" max="10000" step="100"></div>
    <div class="chips" data-for="cold">
     <button class="chip" data-v="5000">Neutral</button><button class="chip" data-v="5600">Daylight</button>
     <button class="chip" data-v="6500">Cool</button><button class="chip" data-v="8000">Overcast</button></div>
   </section>
   <section class="card panel" data-p="white" hidden>
    <h2>Pure white</h2><p class="sub">Red, green and blue at full level.</p>
    <p class="note">This is the brightest setting and draws the most power. Use the brightness slider to soften it.</p>
   </section>
   <section class="card panel" data-p="rainbow" hidden>
    <h2>Rainbow</h2><p class="sub">The full spectrum flows along the strip.</p>
    <div class="field"><div class="label"><label for="spd">Speed</label><output id="spdV"></output></div>
     <input type="range" class="r-fill" id="spd" min="1" max="100"></div>
    <div class="chips" data-for="spd">
     <button class="chip" data-v="8">Drift</button><button class="chip" data-v="35">Gentle</button>
     <button class="chip" data-v="70">Lively</button><button class="chip" data-v="100">Fast</button></div>
   </section>
   <section class="card panel" data-p="blink" hidden>
    <h2>Blink</h2><p class="sub">Flashes one colour on a steady rhythm.</p>
    <label class="picker"><input type="color" id="bcol"><span id="bcolHex"></span></label>
    <div class="field"><div class="label"><label for="on">On for</label><output id="onV"></output></div>
     <input type="range" class="r-fill" id="on" min="50" max="3000" step="50"></div>
    <div class="field"><div class="label"><label for="off">Off for</label><output id="offV"></output></div>
     <input type="range" class="r-fill" id="off" min="50" max="3000" step="50"></div>
    <div class="chips" data-for="blink">
     <button class="chip" data-on="150" data-off="150">Quick</button><button class="chip" data-on="500" data-off="500">Steady</button>
     <button class="chip" data-on="1200" data-off="1200">Slow</button><button class="chip" data-on="100" data-off="1400">Beacon</button></div>
   </section>
   <section class="card panel" data-p="music" hidden>
    <h2>Music</h2><p class="sub">Kick flashes the whole strip in this colour. Snare bursts from the centre, hi-hats sparkle, and sustained bass glows.</p>
    <label class="picker"><input type="color" id="mcol"><span id="mcolHex"></span></label>
    <p class="note" style="margin-top:16px">Place the microphone near the speaker. Works best with electronic music, dubstep and beatbox.</p>
   </section>
  </div>

  <div class="stack">
   <section class="card">
    <h2>Schedule</h2><p class="sub">Turn the light on or off after a delay.</p>
    <div class="srow" data-type="on">
     <div class="shead"><b>On delay</b><button class="text-btn" data-cancel hidden>Cancel</button></div>
     <div class="sidle">
      <div class="chips"><button class="chip" data-min="15">15 min</button><button class="chip" data-min="30">30 min</button><button class="chip" data-min="60">1 hour</button><button class="chip" data-min="120">2 hours</button></div>
      <div class="custom"><input type="number" min="1" max="1440" placeholder="Minutes" inputmode="numeric" aria-label="On delay in minutes"><button class="primary" data-start>Start</button></div>
     </div>
     <div class="srun" hidden><div class="count"></div><div class="bar"><i></i></div><p class="sub">until the light turns on</p></div>
    </div>
    <div class="srow" data-type="off">
     <div class="shead"><b>Off delay</b><button class="text-btn" data-cancel hidden>Cancel</button></div>
     <div class="sidle">
      <div class="chips"><button class="chip" data-min="15">15 min</button><button class="chip" data-min="30">30 min</button><button class="chip" data-min="60">1 hour</button><button class="chip" data-min="120">2 hours</button></div>
      <div class="custom"><input type="number" min="1" max="1440" placeholder="Minutes" inputmode="numeric" aria-label="Off delay in minutes"><button class="primary" data-start>Start</button></div>
     </div>
     <div class="srun" hidden><div class="count"></div><div class="bar"><i></i></div><p class="sub">until the light turns off</p></div>
    </div>
   </section>

   <section class="card">
    <h2>Display note</h2>
    <p class="sub">Shown on the OLED once the knob has been idle for a while.</p>
    <div class="bezel"><div class="oled-wrap"><div class="oled f3 center" id="oledPrev"></div></div></div>
    <label for="noteText" class="lab" style="margin-top:0">Message</label>
    <textarea id="noteText" maxlength="120" placeholder="Write a short note" style="margin-top:8px"></textarea>
    <div class="meta"><span>Letters, numbers and symbols. Preview is approximate.</span><span id="noteCount">0/120</span></div>
    <span class="lab">Font</span>
    <div class="chips" id="fontChips" style="margin-top:10px"></div>
    <span class="lab">Alignment</span>
    <div class="seg" id="alignSeg"><button data-a="0">Left</button><button data-a="1">Centre</button></div>
    <span class="lab">Show after</span>
    <div class="seg" id="idleSeg"><button data-i="10">10 s</button><button data-i="20">20 s</button><button data-i="60">1 min</button><button data-i="300">5 min</button></div>
    <div class="save-row"><button class="primary" id="noteSave">Save to display</button><span class="ok" id="noteMsg" hidden>Saved. Showing on the display now.</span></div>
   </section>
  </div>
 </div>

 <!-- Saved WiFi -->
 <section class="card wifi">
  <div class="wifi-grid">
   <div>
    <h2>Saved WiFi</h2>
    <p class="sub" id="netCap">The strip joins the strongest saved network when it starts.</p>
    <ul class="nets" id="netList"><li class="empty">Loading</li></ul>
   </div>
   <form class="form" id="addNet" autocomplete="off">
    <h2>Add a network</h2>
    <p class="sub">Saved for later. The strip tries it the next time it starts.</p>
    <label for="nSsid">Network name</label>
    <input class="input" id="nSsid" maxlength="32" required autocapitalize="none">
    <label for="nPass">Password</label>
    <input class="input" id="nPass" type="password" maxlength="64">
    <label class="show"><input type="checkbox" id="nShow">Show password</label>
    <div class="save-row" style="margin-top:0"><button class="primary" type="submit">Save network</button><span class="ok" id="netMsg" hidden></span></div>
   </form>
  </div>
 </section>

 <dl class="device">
  <div><dt>Address</dt><dd id="dIp">-</dd></div>
  <div><dt>Network</dt><dd id="dSsid">-</dd></div>
  <div><dt>Signal</dt><dd id="dSig">-</dd></div>
  <div><dt>Sound</dt><dd id="dSound">-</dd></div>
  <div><dt>Battery</dt><dd id="dBat">-</dd></div>
  <button class="text-btn" id="wifiReset">Forget all WiFi and restart setup</button>
 </dl>
 <footer class="credit"><span>Version 0.1</span><span>Designed by Ro-Han G.</span></footer>
</main>

<script>
const $=id=>document.getElementById(id);
let S=null,busy=false,pending=null,dragging=false,noteTouched=false;
const left={on:0,off:0},total={on:0,off:0};
let N={text:'',font:3,align:1,idle:20};

function kelvin(k){const t=k/100;let r,g,b;
 if(t<=66){r=255;g=99.4708025861*Math.log(t)-161.1195681661}
 else{r=329.698727446*Math.pow(t-60,-0.1332047592);g=288.1221695283*Math.pow(t-60,-0.0755148492)}
 b=t>=66?255:t<=19?0:138.5177312231*Math.log(t-10)-305.0447927307;
 const c=v=>Math.max(0,Math.min(255,Math.round(v)));return `rgb(${c(r)},${c(g)},${c(b)})`}
const hex=(r,g,b)=>'#'+[r,g,b].map(x=>x.toString(16).padStart(2,'0')).join('');
const RAINBOW='conic-gradient(from 90deg,#FF5E5E,#FFC75E,#7EE081,#5EC8FF,#9B7BFF,#FF5EC4,#FF5E5E)';
function lightColor(m){switch(m){case 'color':case 'blink':case 'music':return hex(S.r,S.g,S.b);
 case 'warm':return kelvin(S.warm);case 'cold':return kelvin(S.cold);case 'white':return '#FFFFFF';case 'rainbow':return '#FF7A7A'}return '#EDEDEA'}
function modeFill(m){return m=='rainbow'?RAINBOW:lightColor(m)}
const TITLES={off:'Off',color:'Colour',warm:'Warm white',cold:'Cold white',white:'Pure white',rainbow:'Rainbow',blink:'Blink',music:'Music'};
function detail(){const p=Math.round(S.bri/2.55)+'% brightness';
 switch(S.mode){case 'off':return 'Pull the cord or use the switch to turn it on';
  case 'color':return hex(S.r,S.g,S.b).toUpperCase()+', '+p;case 'warm':return S.warm+' K, '+p;case 'cold':return S.cold+' K, '+p;
  case 'rainbow':return 'Speed '+S.speed+', '+p;case 'music':return 'Kick, snare, hi-hat and bass, '+p;
  case 'blink':return (S.on/1000)+' s on, '+(S.off/1000)+' s off'}return p}

/* ---------- networking ---------- */
function setStatus(ok){const s=$('status');s.classList.toggle('bad',!ok);s.lastChild.textContent=ok?'Online':'Not responding'}
function send(q){
 if(busy){pending=q;return}
 busy=true;
 fetch('/api/set?'+q).then(r=>r.json()).then(j=>{S=j;setStatus(true);sync(false)}).catch(()=>setStatus(false))
 .finally(()=>{busy=false;if(pending){const p=pending;pending=null;send(p)}});
}
function post(url,data){return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)}).then(r=>r.json())}
function load(full){fetch('/api/state').then(r=>r.json()).then(j=>{S=j;setStatus(true);sync(full)}).catch(()=>setStatus(false))}
function timer(type,m){fetch(`/api/timer?type=${type}&min=${m}`).then(r=>r.json()).then(j=>{S=j;sync(false)}).catch(()=>setStatus(false))}

/* ---------- lamp ---------- */
function drawLamp(){
 const on=S.mode!='off',c=lightColor(S.mode),k=S.bri/255;
 $('bulb').setAttribute('fill',on?c:'#EDEDEA');
 $('halo').setAttribute('fill',c);$('halo').style.opacity=on?(0.14+0.3*k):0;
 $('beamStop').setAttribute('stop-color',c);$('beamStop2').setAttribute('stop-color',c);
 $('beamPath').style.opacity=on?(0.12+0.6*k):0;
 $('lampbox').classList.toggle('rainbow',on&&S.mode=='rainbow');
}
function pullCord(){const p=$('pull');p.classList.add('pulling');setTimeout(()=>p.classList.remove('pulling'),260);send('power='+(S&&S.mode=='off'?1:0))}
$('pull').onclick=pullCord;
$('pull').onkeydown=e=>{if(e.key=='Enter'||e.key==' '){e.preventDefault();pullCord()}};

/* ---------- volume-style brightness ---------- */
const vs=$('vbri');
function showBri(v){const p=Math.max(1,Math.round(v/2.55));$('vfill').style.height=p+'%';$('briV').textContent=p+'%';vs.setAttribute('aria-valuenow',p)}
function briFrom(e){const r=vs.getBoundingClientRect();let p=1-(e.clientY-r.top)/r.height;p=Math.min(1,Math.max(0.01,p));return Math.max(1,Math.round(p*255))}
function applyBri(v){if(!S)return;S.bri=v;showBri(v);drawLamp();$('hSub').textContent=detail();send('bri='+v)}
vs.addEventListener('pointerdown',e=>{dragging=true;vs.setPointerCapture(e.pointerId);applyBri(briFrom(e))});
vs.addEventListener('pointermove',e=>{if(dragging)applyBri(briFrom(e))});
['pointerup','pointercancel','lostpointercapture'].forEach(ev=>vs.addEventListener(ev,()=>dragging=false));
vs.addEventListener('keydown',e=>{if(!S)return;let v=S.bri;
 if(e.key=='ArrowUp'||e.key=='ArrowRight')v+=13;else if(e.key=='ArrowDown'||e.key=='ArrowLeft')v-=13;else return;
 e.preventDefault();applyBri(Math.max(1,Math.min(255,v)))});
vs.addEventListener('wheel',e=>{if(!S)return;e.preventDefault();applyBri(Math.max(1,Math.min(255,S.bri+(e.deltaY<0?8:-8))))},{passive:false});

/* ---------- mode controls ---------- */
function fill(el){el.style.setProperty('--p',((el.value-el.min)/(el.max-el.min)*100)+'%')}
function labels(){
 $('warmV').textContent=$('warm').value+' K';$('coldV').textContent=$('cold').value+' K';
 $('spdV').textContent=$('spd').value;
 $('onV').textContent=($('on').value/1000).toFixed(2).replace(/0$/,'')+' s';
 $('offV').textContent=($('off').value/1000).toFixed(2).replace(/0$/,'')+' s';
 ['col','bcol','mcol'].forEach(i=>$(i+'Hex').textContent=$(i).value);
 document.querySelectorAll('.r-fill').forEach(fill);
 document.querySelectorAll('[data-for] .chip').forEach(c=>{const f=c.parentNode.dataset.for;
  c.classList.toggle('on',f=='blink'?(c.dataset.on==$('on').value&&c.dataset.off==$('off').value):c.dataset.v==$(f).value)});
}
function setPickers(h){['col','bcol','mcol'].forEach(i=>$(i).value=h)}
document.querySelectorAll('.tile').forEach(t=>t.onclick=()=>send('mode='+t.dataset.m));
$('power').onclick=()=>send('power='+(S&&S.mode=='off'?1:0));
$('warm').oninput=e=>{labels();send('mode=warm&warm='+e.target.value)};
$('cold').oninput=e=>{labels();send('mode=cold&cold='+e.target.value)};
$('spd').oninput=e=>{labels();send('mode=rainbow&speed='+e.target.value)};
$('on').oninput=e=>{labels();send('mode=blink&on='+e.target.value)};
$('off').oninput=e=>{labels();send('mode=blink&off='+e.target.value)};
[['col','color'],['bcol','blink'],['mcol','music']].forEach(([id,m])=>$(id).oninput=e=>{setPickers(e.target.value);labels();send(`mode=${m}&c=`+e.target.value.slice(1))});
document.querySelectorAll('[data-for] .chip').forEach(c=>c.onclick=()=>{
 const f=c.parentNode.dataset.for;
 if(f=='blink'){$('on').value=c.dataset.on;$('off').value=c.dataset.off;labels();send(`mode=blink&on=${c.dataset.on}&off=${c.dataset.off}`);return}
 $(f).value=c.dataset.v;labels();send(f=='spd'?'mode=rainbow&speed='+c.dataset.v:`mode=${f}&${f}=${c.dataset.v}`)});
['FF3B30','FF9500','FFCC00','34C759','00C7BE','0A84FF','5E5CE6','FF2D55','FFFFFF'].forEach(c=>{
 const b=document.createElement('button');b.className='sw';b.style.background='#'+c;b.setAttribute('aria-label','#'+c);
 b.onclick=()=>{setPickers('#'+c.toLowerCase());labels();send('mode=color&c='+c)};$('swatches').appendChild(b)});

/* ---------- schedule ---------- */
const fmt=s=>{const h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=String(s%60).padStart(2,'0');return h?`${h}:${String(m).padStart(2,'0')}:${x}`:`${m}:${x}`};
function showTimers(){['on','off'].forEach(t=>{
 const row=document.querySelector(`.srow[data-type="${t}"]`),run=left[t]>0;
 row.querySelector('.sidle').hidden=run;row.querySelector('.srun').hidden=!run;row.querySelector('[data-cancel]').hidden=!run;
 if(run){row.querySelector('.count').textContent=fmt(left[t]);row.querySelector('.bar i').style.width=(total[t]?left[t]/total[t]*100:0)+'%'}})}
document.querySelectorAll('.srow').forEach(row=>{const t=row.dataset.type;
 row.querySelectorAll('[data-min]').forEach(c=>c.onclick=()=>timer(t,c.dataset.min));
 const inp=row.querySelector('input');
 row.querySelector('[data-start]').onclick=()=>{const v=parseInt(inp.value);if(v>0){timer(t,v);inp.value=''}else inp.focus()};
 inp.onkeydown=e=>{if(e.key=='Enter')row.querySelector('[data-start]').click()};
 row.querySelector('[data-cancel]').onclick=()=>timer(t,0)});

/* ---------- OLED note ---------- */
const FONTS=['Classic small','Classic medium','Classic large','Sans','Sans bold','Serif','Mono'];
FONTS.forEach((f,i)=>{const b=document.createElement('button');b.className='chip';b.textContent=f;b.dataset.f=i;
 b.onclick=()=>{N.font=i;noteTouched=true;renderNote()};$('fontChips').appendChild(b)});
document.querySelectorAll('#alignSeg button').forEach(b=>b.onclick=()=>{N.align=+b.dataset.a;noteTouched=true;renderNote()});
document.querySelectorAll('#idleSeg button').forEach(b=>b.onclick=()=>{N.idle=+b.dataset.i;noteTouched=true;renderNote()});
function renderNote(){
 const p=$('oledPrev');p.className='oled f'+N.font+(N.align==1?' center':'');p.textContent=N.text;
 $('noteCount').textContent=N.text.length+'/120';
 if(document.activeElement!=$('noteText'))$('noteText').value=N.text;
 document.querySelectorAll('#fontChips .chip').forEach(c=>c.classList.toggle('on',+c.dataset.f==N.font));
 document.querySelectorAll('#alignSeg button').forEach(b=>b.classList.toggle('on',+b.dataset.a==N.align));
 document.querySelectorAll('#idleSeg button').forEach(b=>b.classList.toggle('on',+b.dataset.i==N.idle));
}
$('noteText').oninput=e=>{const clean=e.target.value.replace(/[^\x20-\x7E\n]/g,'');if(clean!=e.target.value)e.target.value=clean;N.text=clean;noteTouched=true;$('noteMsg').hidden=true;renderNote()};
$('noteSave').onclick=()=>post('/api/note',{text:N.text,font:N.font,align:N.align,idle:N.idle})
 .then(j=>{S=j;noteTouched=false;const m=$('noteMsg');m.textContent=N.text?'Saved. Showing on the display now.':'Saved. The display shows the IP address.';m.hidden=false;setTimeout(()=>m.hidden=true,4000)})
 .catch(()=>setStatus(false));

/* ---------- saved WiFi ---------- */
function el(tag,cls,txt){const e=document.createElement(tag);if(cls)e.className=cls;if(txt!=null)e.textContent=txt;return e}
function renderNets(j){
 const ul=$('netList');ul.innerHTML='';
 if(!j.list.length)ul.appendChild(el('li','empty','No saved networks yet.'));
 j.list.forEach(n=>{
  const li=el('li'),nm=el('span','nm'),acts=el('span','acts');
  nm.appendChild(el('b',null,n.ssid));nm.appendChild(el('small',n.connected?'live':null,n.connected?'Connected':'Saved'));
  if(!n.connected){const c=el('button','text-btn','Connect');c.onclick=()=>{if(confirm(`Restart and join "${n.ssid}"? This page stops responding until you reconnect.`))post('/api/wifi/connect',{ssid:n.ssid}).then(()=>netMsg('Restarting to join '+n.ssid))};acts.appendChild(c)}
  const f=el('button','text-btn','Forget');
  f.onclick=()=>{if(confirm(`Forget "${n.ssid}"?`+(n.connected?' The strip stays connected until it restarts.':'')))post('/api/wifi/forget',{ssid:n.ssid}).then(renderNets)};
  acts.appendChild(f);li.appendChild(nm);li.appendChild(acts);ul.appendChild(li);
 });
 $('netCap').textContent=`${j.list.length} of ${j.max} saved. The strip joins the strongest one when it starts.`;
}
function netMsg(t){const m=$('netMsg');m.textContent=t;m.hidden=false;setTimeout(()=>m.hidden=true,5000)}
function loadNets(){fetch('/api/wifi').then(r=>r.json()).then(renderNets).catch(()=>{})}
$('nShow').onchange=e=>$('nPass').type=e.target.checked?'text':'password';
$('addNet').onsubmit=e=>{e.preventDefault();const s=$('nSsid').value.trim();if(!s)return;
 post('/api/wifi/add',{ssid:s,pass:$('nPass').value}).then(j=>{if(j.error){netMsg(j.error);return}renderNets(j);$('addNet').reset();$('nPass').type='password';netMsg('Saved '+s)}).catch(()=>netMsg('Could not save'))};
$('wifiReset').onclick=()=>{if(confirm('Forget every saved network and restart in setup mode?'))fetch('/api/wifireset')};

/* ---------- sync ---------- */
function sync(full){
 if(!S)return;
 const shown=S.mode=='off'?S.last:S.mode;
 document.querySelectorAll('.tile').forEach(t=>{t.classList.toggle('active',t.dataset.m==S.mode);t.querySelector('.dot').style.background=modeFill(t.dataset.m)});
 document.querySelectorAll('.panel').forEach(p=>p.hidden=p.dataset.p!=shown);
 $('power').setAttribute('aria-checked',S.mode!='off');$('pLabel').textContent=S.mode=='off'?'Off':'On';
 $('hTitle').textContent=TITLES[S.mode];$('hSub').textContent=detail();
 if(!dragging)showBri(S.bri);
 drawLamp();
 if(full){
  $('warm').value=S.warm;$('cold').value=S.cold;$('spd').value=S.speed;$('on').value=S.on;$('off').value=S.off;
  setPickers(hex(S.r,S.g,S.b));labels();
 }else if(![$('col'),$('bcol'),$('mcol')].includes(document.activeElement)){setPickers(hex(S.r,S.g,S.b));['col','bcol','mcol'].forEach(i=>$(i+'Hex').textContent=$(i).value)}
 if(full||!noteTouched){N={text:S.note,font:S.nfont,align:S.nalign,idle:S.nidle};renderNote()}
 left.on=S.onT;total.on=S.onTotal||S.onT;left.off=S.offT;total.off=S.offTotal||S.offT;showTimers();
 $('dIp').textContent=S.ip;$('dSsid').textContent=S.ssid||'-';
 $('dSig').textContent=S.rssi>-60?'Strong':S.rssi>-72?'Good':S.rssi>-82?'Fair':'Weak';
 $('dSound').textContent=S.sys=='music'?'Beat sync':'Double clap toggles';
 $('dBat').textContent=S.bok?S.bp+'%, '+S.bv.toFixed(2)+' V'+(S.bp<=15?' (low)':''):'Not detected';
}
setInterval(()=>{let ch=false;['on','off'].forEach(t=>{if(left[t]>0){left[t]--;ch=true;if(!left[t])setTimeout(()=>load(false),1500)}});if(ch)showTimers()},1000);
setInterval(()=>{if(!document.hidden)load(false)},4000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden){load(false);loadNets()}});
load(true);loadNets();
</script></body></html>
)rawliteral";

String stateJson() {
  String j;
  j.reserve(768);                      // one allocation instead of dozens of re-allocations
  j += "{\"ver\":\"" FW_VERSION "\",";
  j += "\"mode\":\"";  j += MODE_NAMES[st.mode];     j += "\",";
  j += "\"last\":\"";  j += MODE_NAMES[st.lastMode]; j += "\",";
  j += "\"sys\":\"";   j += sysMode() == SYS_MUSIC ? "music" : "auto"; j += "\",";
  j += "\"r\":";       j += col.r;
  j += ",\"g\":";      j += col.g;
  j += ",\"b\":";      j += col.b;
  j += ",\"bri\":";    j += col.bri;
  j += ",\"warm\":";   j += st.warmK;
  j += ",\"cold\":";   j += st.coldK;
  j += ",\"speed\":";  j += st.speed;
  j += ",\"on\":";     j += st.onMs;
  j += ",\"off\":";    j += st.offMs;
  j += ",\"onT\":";    j += timerLeftS(onTimer);
  j += ",\"onTotal\":";  j += onTimer.active ? onTimer.total : 0;
  j += ",\"offT\":";   j += timerLeftS(offTimer);
  j += ",\"offTotal\":"; j += offTimer.active ? offTimer.total : 0;
  j += ",\"note\":\""; j += jsonEscape(noteText); j += "\"";
  j += ",\"nfont\":";  j += noteFont;
  j += ",\"nalign\":"; j += noteAlign;
  j += ",\"nidle\":";  j += noteIdleS;
  j += ",\"img\":";    j += imgOn ? "true" : "false";
  j += ",\"ssid\":\""; j += jsonEscape(connectedSsid); j += "\"";
  j += ",\"ip\":\"";   j += WiFi.localIP().toString(); j += "\"";
  j += ",\"host\":\""; j += HOSTNAME; j += "\"";
  j += ",\"bok\":";    j += battOk ? "true" : "false";
  j += ",\"bv\":";     j += String(battV < 0 ? 0 : battV, 2);
  j += ",\"bp\":";     j += battPct;
  j += ",\"rssi\":";   j += WiFi.RSSI();
  j += "}";
  return j;
}

String wifiJson() {
  String j;
  j.reserve(64 + wifiCount * 96);
  j += "{\"max\":"; j += MAX_SAVED_WIFI; j += ",\"list\":[";
  bool up = WiFi.status() == WL_CONNECTED;
  for (int i = 0; i < wifiCount; i++) {
    if (i) j += ",";
    bool c = up && wifiSsid[i] == connectedSsid;
    j += "{\"ssid\":\""; j += jsonEscape(wifiSsid[i]); j += "\",\"connected\":"; j += c ? "true" : "false"; j += "}";
  }
  j += "]}";
  return j;
}

void sendJson(const String& j) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}
void sendState() { sendJson(stateJson()); }

// The ~28 kB page is only sent again when the firmware changed
void handleControlPage() {
  if (server.header("If-None-Match") == PAGE_ETAG) { server.send(304); return; }
  server.sendHeader("ETag", PAGE_ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", CONTROL_PAGE);
}

void handleApiSet() {
  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    for (uint8_t i = 0; i < M_COUNT; i++) if (m == MODE_NAMES[i]) { setMode(i); break; }
  }
  if (server.hasArg("power")) setMode(server.arg("power") == "1" ? st.lastMode : M_OFF);
  if (server.hasArg("c")) {
    uint32_t v = strtoul(server.arg("c").c_str(), nullptr, 16);
    col.r = (v >> 16) & 0xFF; col.g = (v >> 8) & 0xFF; col.b = v & 0xFF;
  }
  if (server.hasArg("bri"))   col.bri  = constrain(server.arg("bri").toInt(),   1, 255);
  if (server.hasArg("warm"))  st.warmK = constrain(server.arg("warm").toInt(),  1800, 3500);
  if (server.hasArg("cold"))  st.coldK = constrain(server.arg("cold").toInt(),  5000, 10000);
  if (server.hasArg("speed")) st.speed = constrain(server.arg("speed").toInt(), 1, 100);
  if (server.hasArg("on"))    st.onMs  = constrain(server.arg("on").toInt(),    50, 5000);
  if (server.hasArg("off"))   st.offMs = constrain(server.arg("off").toInt(),   50, 5000);
  markChanged();
  sendState();
}

void handleApiTimer() {
  bool isOn = server.arg("type") == "on";
  long mins = constrain(server.arg("min").toInt(), 0, 1440);
  startTimer(isOn ? onTimer : offTimer, mins);
  if (!isOn && mins > 0 && st.mode == M_OFF) { setMode(st.lastMode); markChanged(); }
  Serial.printf("%s delay: %ld min\n", isOn ? "On" : "Off", mins);
  sendState();
}

void handleApiNote() {
  if (server.hasArg("text"))  noteText  = sanitizeNote(server.arg("text"));
  if (server.hasArg("font"))  noteFont  = constrain(server.arg("font").toInt(), 0, NUM_FONTS - 1);
  if (server.hasArg("align")) noteAlign = server.arg("align").toInt() ? 1 : 0;
  if (server.hasArg("idle"))  noteIdleS = constrain(server.arg("idle").toInt(), 5, 3600);
  saveNote();
  noteWrapDirty = true;
  if (noteText.length()) goScreen(SCR_NOTE); else goScreen(SCR_HOME);
  lastInput = millis();
  sendState();
}

static inline uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// POST bmp=<2048 hex chars> (128x64, row by row, leftmost pixel in the high bit) or clear=1
void handleApiImage() {
  if (server.hasArg("clear")) {
    imgOn = false;
  } else {
    const String& h = server.arg("bmp");
    if (h.length() != IMG_BYTES * 2) {
      server.send(400, "application/json", "{\"error\":\"Picture must be 128x64\"}");
      return;
    }
    const char* p = h.c_str();
    for (int i = 0; i < IMG_BYTES; i++) imgBits[i] = (hexNibble(p[i * 2]) << 4) | hexNibble(p[i * 2 + 1]);
    imgOn = true;
  }
  saveImage();
  if (imgOn || noteText.length()) goScreen(SCR_NOTE); else goScreen(SCR_HOME);
  lastInput = millis();
  sendState();
}

void handleApiWifiList() { sendJson(wifiJson()); }

void handleApiWifiAdd() {
  String s = server.arg("ssid"); s.trim();
  if (s.length() == 0 || s.length() > 32 || server.arg("pass").length() > 64) {
    server.send(400, "application/json", "{\"error\":\"Enter a network name up to 32 characters\"}");
    return;
  }
  addNetwork(s, server.arg("pass"));
  sendJson(wifiJson());
}

void handleApiWifiForget() { forgetNetwork(server.arg("ssid")); sendJson(wifiJson()); }

void handleApiWifiConnect() {
  String s = server.arg("ssid");
  if (findNetwork(s) < 0) { server.send(404, "application/json", "{\"error\":\"Not saved\"}"); return; }
  preferredSsid = s; saveNetworks();
  sendJson(wifiJson());
  showMessage("Connecting", fitText(s, 21), SCR_HOME);
  scheduleRestart();
}

void handleWifiReset() {
  server.send(200, "text/plain", "WiFi forgotten. Restarting in setup mode.");
  forgetAllNetworks();
  scheduleRestart();
}

void startControlServer() {
  static const char* HEADERS[] = { "If-None-Match" };      // needed for the page ETag
  server.collectHeaders(HEADERS, 1);
  server.on("/",                HTTP_GET,  handleControlPage);
  server.on("/api/state",       HTTP_GET,  sendState);
  server.on("/api/set",         HTTP_GET,  handleApiSet);
  server.on("/api/timer",       HTTP_GET,  handleApiTimer);
  server.on("/api/note",        HTTP_POST, handleApiNote);
  server.on("/api/image",       HTTP_POST, handleApiImage);
  server.on("/api/wifi",        HTTP_GET,  handleApiWifiList);
  server.on("/api/wifi/add",    HTTP_POST, handleApiWifiAdd);
  server.on("/api/wifi/forget", HTTP_POST, handleApiWifiForget);
  server.on("/api/wifi/connect",HTTP_POST, handleApiWifiConnect);
  server.on("/api/wifireset",   HTTP_GET,  handleWifiReset);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Open http://%s.local or http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
  }
}

// =====================================================================
//                     Captive portal (WiFi setup page)
// =====================================================================
void scanNetworks() {
  Serial.println("Scanning networks...");
  int n = WiFi.scanNetworks();
  networkListHtml = "";
  if (n <= 0) { networkListHtml = F("<p class='empty'>No networks found. Move closer to your router and scan again.</p>"); return; }
  networkListHtml.reserve(n * 220);
  int idx[n];
  for (int i = 0; i < n; i++) idx[i] = i;
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (WiFi.RSSI(idx[j]) > WiFi.RSSI(idx[i])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
  for (int k = 0; k < n; k++) {
    int i = idx[k];
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool dup = false;
    for (int m = 0; m < k; m++) if (WiFi.SSID(idx[m]) == ssid) { dup = true; break; }
    if (dup) continue;
    int rssi = WiFi.RSSI(i);
    int bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    bool saved = findNetwork(ssid) >= 0;
    String esc = htmlEscape(ssid);
    networkListHtml += "<button type='button' class='net' data-ssid=\"" + esc + "\"><span class='nm'><b>" + esc +
                       "</b><small>" + (saved ? "Saved, " : "") + (open ? "Open" : "Secured") +
                       "</small></span><span class='bars b" + String(bars) + "'><i></i><i></i><i></i><i></i></span></button>";
  }
  WiFi.scanDelete();
}

const char PORTAL_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Light strip setup</title>
<style>
:root{--bg:#FAFAFA;--surface:#fff;--line:#E7E7E4;--ink:#27272A;--mute:#64748B;--accent:#1E293B;--hover:#F4F4F2}
*{box-sizing:border-box;margin:0}
body{background:var(--bg);color:var(--ink);font:400 15px/1.6 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased;padding:48px 20px}
button,input{font:inherit;color:inherit}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
.wrap{max-width:420px;margin:0 auto}
h1{font-size:28px;font-weight:700;line-height:1.25;letter-spacing:-.02em;margin-bottom:8px}
.lead{color:var(--mute);margin-bottom:32px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:8px;margin-bottom:12px}
.net{display:flex;width:100%;align-items:center;justify-content:space-between;padding:12px 14px;border:0;border-radius:8px;background:none;text-align:left;cursor:pointer;transition:background .18s,transform .18s}
.net:hover{background:var(--hover);transform:scale(1.01)}
.net.sel{background:var(--accent);color:#fff}
.net.sel small{color:#CBD5E1}
.nm b{display:block;font-weight:600;font-size:15px}
.nm small{color:var(--mute);font-size:12px}
.bars{display:flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:3px;border-radius:1px;background:currentColor;opacity:.2}
.bars i:nth-child(1){height:4px}.bars i:nth-child(2){height:7px}.bars i:nth-child(3){height:10px}.bars i:nth-child(4){height:14px}
.b1 i:nth-child(-n+1),.b2 i:nth-child(-n+2),.b3 i:nth-child(-n+3),.b4 i{opacity:1}
.empty{padding:14px;color:var(--mute);font-size:14px}
.rescan{display:inline-block;color:var(--mute);font-size:13px;font-weight:500;text-decoration:none;margin-bottom:32px}
.rescan:hover{color:var(--ink)}
label{display:block;font-size:14px;font-weight:500;margin-bottom:8px}
input[type=text],input[type=password]{width:100%;padding:11px 14px;border:1px solid var(--line);border-radius:8px;background:var(--surface);margin-bottom:20px;font-size:16px}
input:focus{outline:none;border-color:var(--accent)}
.show{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--mute);margin:-8px 0 28px;font-weight:400}
.primary{width:100%;padding:13px;border:0;border-radius:8px;background:var(--accent);color:#fff;font-weight:600;cursor:pointer;transition:transform .18s,opacity .18s}
.primary:hover{transform:scale(1.02);opacity:.92}
.done p{color:var(--mute);margin-bottom:12px}
.done b{color:var(--ink)}
.secondary{width:100%;padding:13px;border:1px solid var(--line);border-radius:8px;background:var(--surface);font-weight:600;cursor:pointer;margin-top:12px;transition:transform .18s,background .18s}
.secondary:hover{transform:scale(1.02);background:var(--hover)}
.hint{color:var(--mute);font-size:13px;margin-top:12px}
.credit{display:flex;justify-content:space-between;margin-top:48px;padding-top:20px;border-top:1px solid var(--line);color:var(--mute);font-size:12px}
@media (prefers-reduced-motion:reduce){*{transition:none!important}}
</style></head><body><div class="wrap">
)rawliteral";
const char PORTAL_FOOT[] PROGMEM = R"rawliteral(
<footer class='credit'><span>Version 0.1</span><span>Designed by Ro-Han G.</span></footer>
</div><script>
document.querySelectorAll('.net').forEach(n=>n.onclick=()=>{
 document.querySelectorAll('.net').forEach(x=>x.classList.remove('sel'));
 n.classList.add('sel');document.getElementById('ssid').value=n.dataset.ssid;document.getElementById('pass').focus();
});
const sh=document.getElementById('sh');if(sh)sh.onchange=()=>document.getElementById('pass').type=sh.checked?'text':'password';
</script></body></html>
)rawliteral";

// Starts a portal page with enough room reserved for the whole response
String portalPage(size_t extra) {
  String page;
  page.reserve(sizeof(PORTAL_HEAD) + sizeof(PORTAL_FOOT) + extra);
  page += FPSTR(PORTAL_HEAD);
  return page;
}

void handlePortalRoot() {
  String page = portalPage(networkListHtml.length() + 1024);
  page += F("<h1>Connect to WiFi</h1><p class='lead'>Choose the network your light strip should join.</p><div class='card'>");
  page += networkListHtml;
  page += F("</div><a class='rescan' href='/scan'>Scan again</a>"
            "<form method='POST' action='/save'>"
            "<label for='ssid'>Network name</label>"
            "<input type='text' id='ssid' name='ssid' maxlength='32' required autocomplete='off' autocapitalize='none'>"
            "<label for='pass'>Password</label>"
            "<input type='password' id='pass' name='pass' maxlength='64'>"
            "<label class='show'><input type='checkbox' id='sh'>Show password</label>"
            "<button class='primary' type='submit'>Connect</button></form>"
            "<form method='POST' action='/offline'><button class='secondary' type='submit'>Use without WiFi</button></form>"
            "<p class='hint'>Without WiFi, double clap or use the knob to control the light.</p>");
  page += FPSTR(PORTAL_FOOT);
  server.send(200, "text/html", page);
}

void handlePortalScan() { scanNetworks(); server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); }

void handlePortalSave() {
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  ssid.trim();
  if (ssid.length() == 0) { server.send(400, "text/html", "Enter a network name. <a href='/'>Go back</a>"); return; }
  addNetwork(ssid, pass);
  preferredSsid = ssid; saveNetworks();
  String page = portalPage(640);
  page += "<div class='done'><h1>Connecting</h1><p>The light strip is restarting and joining <b>" + htmlEscape(ssid) +
          "</b>.</p><p>Reconnect your phone to your usual WiFi, then open <b>http://" + String(HOSTNAME) +
          ".local</b>. The address is also shown on the display.</p><p>If the password was wrong, the <b>" + String(AP_SSID) +
          "</b> network appears again within a minute.</p></div>";
  page += FPSTR(PORTAL_FOOT);
  server.send(200, "text/html", page);
  scheduleRestart();
}

void handlePortalOffline() {
  setWifiFlag("nowifi", true);
  String page = portalPage(512);
  page += F("<div class='done'><h1>Running without WiFi</h1><p>The setup network turns off now. "
            "Double clap, or use the knob, to control the light.</p><p>To set up WiFi later, press the knob, "
            "open <b>Saved WiFi</b> and choose <b>Turn WiFi on</b>.</p></div>");
  page += FPSTR(PORTAL_FOOT);
  server.send(200, "text/html", page);
  offlinePending = true; pendingAt = millis() + 1500;
}

void handleCaptive() {
  server.sendHeader("Location", String("http://") + apIP.toString() + "/", true);
  server.send(302, "text/plain", "");
}

void startPortal() {
  Serial.println("Starting captive portal...");
  leaveMusicIfActive();
  WiFi.disconnect(true);
  delay(100);                                   // setup only
  WiFi.mode(WIFI_AP_STA);
  setReducedTxPower();
  scanNetworks();
  WiFi.softAPConfig(apIP, apIP, netMsk);
  if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(AP_SSID, AP_PASSWORD);
  else                          WiFi.softAP(AP_SSID);
  setReducedTxPower();
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/",        HTTP_GET,  handlePortalRoot);
  server.on("/scan",    HTTP_GET,  handlePortalScan);
  server.on("/save",    HTTP_POST, handlePortalSave);
  server.on("/offline", HTTP_POST, handlePortalOffline);
  server.on("/generate_204", handleCaptive);
  server.on("/gen_204", handleCaptive);
  server.on("/hotspot-detect.html", handleCaptive);
  server.on("/library/test/success.html", handleCaptive);
  server.on("/connecttest.txt", handleCaptive);
  server.on("/ncsi.txt", handleCaptive);
  server.on("/fwlink", handleCaptive);
  server.onNotFound(handleCaptive);
  server.begin();
  portalActive = true;
  portalStart = millis();
  oledDirty = true;
  Serial.printf("Portal ready: join \"%s\" -> http://%s\n", AP_SSID, apIP.toString().c_str());
}

void enterNoWifiMode(const char* reason) {
  if (portalActive) { server.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true); }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  portalActive = false;
  noWifiMode   = true;
  leaveMusicIfActive();
  oledDirty = true;
  Serial.printf("No-WiFi mode (%s)\n", reason);
}

// =====================================================================
//                       Connect to a saved network
// =====================================================================
// Tries the preferred network first, then the others from strongest to weakest.
bool connectToSavedWiFi() {
  if (wifiCount == 0) { Serial.println("No saved networks."); return false; }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  setReducedTxPower();
  drawStatus("Searching", "for saved WiFi");

  int order[MAX_SAVED_WIFI], rssi[MAX_SAVED_WIFI];
  for (int i = 0; i < wifiCount; i++) { order[i] = i; rssi[i] = -1000; }
  int n = WiFi.scanNetworks();
  for (int k = 0; k < n; k++) {
    int i = findNetwork(WiFi.SSID(k));
    if (i >= 0 && WiFi.RSSI(k) > rssi[i]) rssi[i] = WiFi.RSSI(k);
  }
  WiFi.scanDelete();
  for (int i = 0; i < wifiCount; i++) if (wifiSsid[i] == preferredSsid) rssi[i] += 1000;
  for (int a = 0; a < wifiCount - 1; a++)
    for (int b = a + 1; b < wifiCount; b++)
      if (rssi[order[b]] > rssi[order[a]]) { int t = order[a]; order[a] = order[b]; order[b] = t; }

  for (int k = 0; k < wifiCount; k++) {
    int i = order[k];
    Serial.printf("Connecting to \"%s\"", wifiSsid[i].c_str());
    drawStatus("Connecting", wifiSsid[i]);
    WiFi.begin(wifiSsid[i].c_str(), wifiPass[i].c_str());
    WiFi.setTxPower(TX_POWER);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT) {
      updateLEDs();
      delay(100);                                           // setup only
    }
    if (WiFi.status() == WL_CONNECTED) {
      connectedSsid = wifiSsid[i];
      WiFi.setAutoReconnect(true);
      Serial.printf("\nConnected! IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }
    Serial.println(" failed");
    WiFi.disconnect();
  }
  return false;
}

// =====================================================================
//                              Debug output
// =====================================================================
void debugPrint(uint32_t now) {
#if DEBUG
  static uint32_t lastPrint = 0;
  if (now - lastPrint < DEBUG_INTERVAL_MS) return;
  lastPrint = now;
  Serial.printf("bass:%u mid:%u high:%u bassAvg:%u midAvg:%u highAvg:%u\n",
                bassLevel, midLevel, highLevel,
                (unsigned)sqrtf((float)bassAvgEnergy),
                (unsigned)sqrtf(midAvgEnergy), (unsigned)sqrtf(highAvgEnergy));
#endif
}

// =====================================================================
//                           Setup / Loop
// =====================================================================
void setup() {
  setCpuFrequencyMhz(CPU_MHZ);
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);     // never wait for a USB host (running on battery, nothing connected)
#endif
  delay(500);
  Serial.println("\nLight strip v" FW_VERSION " - designed by " FW_DESIGNER);

  // OLED
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(OLED_I2C_HZ);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  Serial.println(oledOk ? "OLED ready" : "OLED not found - check wiring / address");
  drawStatus("Starting", "Version " FW_VERSION);

  // LED strip: restore the last colour and settings
  strip.begin(); strip.clear(); strip.show();
  loadLedState();
  loadNote();
  needsRender = true;
  updateLEDs();

  initEncoder();
  initBattery();
  updateBattery(millis());
  initBandFilters();
  micReady = initMic();
  Serial.println(micReady ? "Microphone ready (I2S, 16 kHz)" : "Microphone init FAILED - check wiring");

  WiFi.persistent(false);
  loadWifiSettings();

  if (noWifiMode)                     enterNoWifiMode("saved choice");
  else if (portalOnce)                startPortal();
  else if (connectToSavedWiFi())      startControlServer();
  else                                startPortal();

  lastInput = millis();
  goScreen(SCR_HOME);
  DBG("System mode: %s\n", SYS_NAMES[sysMode()]);
}

void loop() {
  uint32_t now = millis();

  processAudio();

  static SysMode lastSys = sysMode();
  SysMode sm = sysMode();
  if (sm != lastSys) { resetClapDetector(); resetDrums(); DBG("System mode: %s\n", SYS_NAMES[sm]); lastSys = sm; }
  if (sm != SYS_MUSIC) updateClapSequence(now);

  updateTimers(now);
  updateBattery(now);
  updateLEDs();
  handleUi(now);
  updateOled(now);
  debugPrint(now);

  if (savePending && now - lastChange > SAVE_DELAY_MS) { savePending = false; saveIfChanged(); }
  if (restartPending && (int32_t)(now - pendingAt) >= 0) ESP.restart();
  if (offlinePending && (int32_t)(now - pendingAt) >= 0) { offlinePending = false; enterNoWifiMode("chosen by user"); }

  if (noWifiMode) {
    // Nothing else to serve: let the CPU idle briefly instead of spinning at 100 %.
    // The I2S DMA holds 128 ms of audio, so 1 ms costs nothing in detection.
    delay(IDLE_YIELD_MS);
    return;
  }

  if (portalActive) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (WiFi.softAPgetStationNum() == 0 && now - portalStart > PORTAL_TIMEOUT) {
      if (wifiCount > 0) { Serial.println("Portal timeout, restarting to retry WiFi..."); ESP.restart(); }
      else enterNoWifiMode("setup timed out");
    }
    return;
  }

  server.handleClient();

  static uint32_t lostSince = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (lostSince == 0) { lostSince = now; oledDirty = true; Serial.println("WiFi lost, auto-reconnecting..."); }
    else if (now - lostSince > 60000) { Serial.println("Reconnect failed, restarting..."); ESP.restart(); }
  } else if (lostSince != 0) {
    lostSince = 0; oledDirty = true;
    Serial.println("WiFi reconnected.");
  }
}
