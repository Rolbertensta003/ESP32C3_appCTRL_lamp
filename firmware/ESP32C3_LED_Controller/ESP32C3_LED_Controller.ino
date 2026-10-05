/*
  ESP32-C3 Super Mini - WiFi LED strip controller with microphone
  ================================================================
  WiFi
   - Connects to saved WiFi on boot. If none / it fails -> AP "ESP32C3-Setup"
     with a captive portal to enter WiFi credentials.
   - Reduced WiFi TX power (8.5 dBm) - also fixes Super Mini antenna issues.

  System modes (chosen automatically):
   - Music mode   : WiFi connected and the "Music" light mode selected.
                    The strip flashes on each bass beat, then fades out.
   - Auto mode    : WiFi connected, any other light mode (colour, warm, cold,
                    white, rainbow, blink, off). Double clap toggles the light.
   - No-WiFi mode : WiFi is off ("Use without WiFi" on the setup page, or the
                    setup page timed out with no network saved), or the setup
                    page is running. Double clap toggles the light.
   To get back to WiFi setup: within 10 s after power-up, hold BOOT for 3 s.

  Dashboard (http://esp32led.local or the device IP):
   Power, brightness, colour, warm/cold/pure white, rainbow, blink, music,
   auto-off timer. Settings and the last colour are saved to flash.

  Libraries: "Adafruit NeoPixel" (Library Manager). I2S uses the ESP-IDF
  driver included in the ESP32 core (driver/i2s_std.h).
  Board: "ESP32C3 Dev Module", USB CDC On Boot: "Enabled", core 3.x

  Pin mapping
   LED strip DIN -> GPIO3  (330 ohm resistor recommended)
   I2S mic WS    -> GPIO4
   I2S mic SCK   -> GPIO5
   I2S mic SD    -> GPIO6
   I2S mic L/R   -> GND (left channel)
   OLED SDA      -> GPIO8  (reserved, not driven by this sketch)
   OLED SCL      -> GPIO9  (reserved; GPIO9 is also the BOOT button)
   Strip GND to ESP32 GND and supply GND; strip 5V from an external supply.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include "driver/i2s_std.h"     // new ESP-IDF I2S driver

// =====================================================================
//                         TUNABLE SETTINGS
// =====================================================================

// ---------- Debug ----------
#define DEBUG 1                                // 1 = print sound data and events to Serial
const uint32_t DEBUG_INTERVAL_MS      = 100;   // how often sound levels are printed

// ---------- Pins ----------
#define LED_DATA_PIN   3
#define NUM_LEDS       30      // number of LEDs on your strip
#define I2S_WS_PIN     4
#define I2S_SCK_PIN    5
#define I2S_SD_PIN     6
#define BOOT_BTN       9       // shared with OLED SCL, only read, never driven
// GPIO8 = OLED SDA: left untouched

// ---------- WiFi ----------
const char*        AP_SSID         = "ESP32C3-Setup";
const char*        AP_PASSWORD     = "";                 // "" = open, or min. 8 chars
const char*        HOSTNAME        = "esp32led";         // http://esp32led.local
const wifi_power_t TX_POWER        = WIFI_POWER_8_5dBm;  // reduced TX power
const uint32_t     CONNECT_TIMEOUT = 15000;
const uint32_t     PORTAL_TIMEOUT  = 5UL * 60UL * 1000UL;
const uint32_t     BOOT_RESET_WINDOW_MS = 10000;         // BOOT hold only counts in the first 10 s
const uint32_t     BOOT_RESET_HOLD_MS   = 3000;

// ---------- Microphone / audio ----------
const uint32_t SAMPLE_RATE         = 16000;   // Hz
const int      I2S_READ_SAMPLES    = 256;     // max samples fetched per read call
const int      AUDIO_FRAME_SAMPLES = 128;     // analysis frame = 8 ms at 16 kHz
const int      MIC_SAMPLE_SHIFT    = 16;      // 32-bit I2S sample -> 16-bit (raise gain: use 14)
const i2s_std_slot_mask_t MIC_SLOT = I2S_STD_SLOT_LEFT;  // L/R pin to GND = left

// Bass band filter (integer one-pole filters, very light on the C3's CPU)
const int BASS_LP_SHIFT = 4;   // 2x low-pass, ~160 Hz corner each -> top of bass band ~150 Hz
const int BASS_HP_SHIFT = 6;   // high-pass ~40 Hz -> bottom of bass band
const int DC_SHIFT      = 9;   // removes mic DC offset (~5 Hz)

// ---------- Beat detection (Music mode) ----------
const float    BEAT_THRESHOLD_MULT  = 1.5f;   // beat when bass energy > 1 s average x this (1.3 - 2.0)
const uint16_t BEAT_MIN_BASS_LEVEL  = 120;    // noise gate: ignore bass quieter than this (RMS)
const uint32_t BEAT_MIN_GAP_MS      = 250;    // minimum time between beats
const int      BEAT_HISTORY_FRAMES  = 125;    // rolling average length: 125 x 8 ms = 1 s
const uint32_t BEAT_FLASH_HOLD_MS   = 40;     // full brightness time after a beat
const uint32_t BEAT_FADE_MS         = 350;    // fade-out time after the hold
const uint8_t  MUSIC_BASE_LEVEL     = 0;      // glow between beats (0 = dark, 0-255)

// ---------- Clap detection (Auto + No-WiFi mode) ----------
const uint16_t CLAP_THRESHOLD         = 1500;  // minimum sound level (RMS) for a clap
const float    CLAP_FLOOR_RATIO       = 4.0f;  // clap must also be 4x the background noise
const float    CLAP_RISE_RATIO        = 3.0f;  // level must jump 3x within 16 ms (sharp rise)
const uint32_t CLAP_MAX_DURATION_MS   = 100;   // clap must drop again within this time
const float    CLAP_RELEASE_RATIO     = 0.35f; // "dropped" = below 35 % of the peak
const uint32_t DOUBLE_CLAP_GAP_MIN_MS = 150;   // gap between the two claps
const uint32_t DOUBLE_CLAP_GAP_MAX_MS = 700;
const uint32_t DOUBLE_CLAP_COOLDOWN_MS = 1500; // ignore claps after a double clap

// ---------- Flash storage ----------
const uint32_t SAVE_DELAY_MS = 3000;   // wait for sliders to settle before writing flash

const byte DNS_PORT = 53;

// =====================================================================
//                              Objects
// =====================================================================
Adafruit_NeoPixel strip(NUM_LEDS, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);
WebServer   server(80);
DNSServer   dnsServer;
Preferences prefs;
IPAddress   apIP(192, 168, 4, 1);
IPAddress   netMsk(255, 255, 255, 0);

#define DBG(...) do { if (DEBUG) Serial.printf(__VA_ARGS__); } while (0)

// =====================================================================
//                         Light + system state
// =====================================================================
enum Mode : uint8_t { M_OFF, M_COLOR, M_WARM, M_COLD, M_WHITE, M_RAINBOW, M_BLINK, M_MUSIC, M_COUNT };
const char* MODE_NAMES[M_COUNT] = { "off", "color", "warm", "cold", "white", "rainbow", "blink", "music" };

// Light settings (saved under key "set")
struct __attribute__((packed)) LedSettings {
  uint8_t  mode      = M_COLOR;
  uint8_t  lastMode  = M_COLOR;   // restored by the power switch
  uint8_t  lastSolid = M_COLOR;   // restored by a double clap (last colour used)
  uint16_t warmK     = 2700;
  uint16_t coldK     = 6500;
  uint8_t  speed     = 40;
  uint16_t onMs      = 500;
  uint16_t offMs     = 500;
} st, savedSt;

// FEATURE 3: last colour + brightness (saved separately under key "colour").
// Default is white when nothing has been saved yet.
struct __attribute__((packed)) LedColour {
  uint8_t r = 255, g = 255, b = 255;
  uint8_t bri = 128;
} col, savedCol;

enum SysMode : uint8_t { SYS_AUTO, SYS_MUSIC, SYS_NOWIFI };
const char* SYS_NAMES[] = { "Auto", "Music", "No-WiFi" };

bool     needsRender    = true;
bool     offTimerActive = false;
uint32_t offTimerEnd    = 0;
uint32_t offTimerTotal  = 0;
bool     savePending    = false;
uint32_t lastChange     = 0;

// WiFi state
String   savedSSID, savedPass, networkListHtml;
bool     portalActive = false;
bool     noWifiMode   = false;
uint32_t portalStart  = 0;
bool     restartPending = false, offlinePending = false;
uint32_t pendingAt = 0;

// NOTE: all enums/structs are declared above this line, because the Arduino IDE
// inserts automatic function prototypes before the first function definition.
bool isSolidMode(uint8_t m) { return m == M_COLOR || m == M_WARM || m == M_COLD || m == M_WHITE; }

SysMode sysMode() {
  if (noWifiMode || portalActive) return SYS_NOWIFI;
  return st.mode == M_MUSIC ? SYS_MUSIC : SYS_AUTO;
}

// =====================================================================
//                              Helpers
// =====================================================================
void setReducedTxPower() {
  WiFi.setTxPower(TX_POWER);
  Serial.printf("TX power: %.1f dBm\n", (int)WiFi.getTxPower() / 4.0f);
}

String htmlEscape(const String& s) {
  String o; o.reserve(s.length());
  for (char c : s) {
    switch (c) {
      case '&': o += F("&amp;"); break;  case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;");  break;  case '"': o += F("&quot;"); break;
      case '\'': o += F("&#39;"); break; default: o += c;
    }
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
  if (m != M_OFF)      st.lastMode  = m;
  if (isSolidMode(m))  st.lastSolid = m;
}
void markChanged() { needsRender = true; savePending = true; lastChange = millis(); }

// Music mode needs WiFi (it is chosen in the dashboard). When WiFi is not
// available, fall back to the last colour so the strip is not left dark.
void leaveMusicIfActive() {
  if (st.mode == M_MUSIC) { st.mode = st.lastSolid; needsRender = true; }
}

// =====================================================================
//              FEATURE 3: persistent storage (NVS / Preferences)
// =====================================================================
void loadCredentials() {
  prefs.begin("wifi", true);
  savedSSID  = prefs.getString("ssid", "");
  savedPass  = prefs.getString("pass", "");
  noWifiMode = prefs.getBool("nowifi", false);
  prefs.end();
}
void saveCredentials(const String& s, const String& p) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", s); prefs.putString("pass", p); prefs.putBool("nowifi", false);
  prefs.end();
}
void saveNoWifiFlag(bool v) { prefs.begin("wifi", false); prefs.putBool("nowifi", v); prefs.end(); }
void clearCredentials()     { prefs.begin("wifi", false); prefs.clear(); prefs.end(); }

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
  Serial.printf("Loaded colour R%u G%u B%u, brightness %u\n", col.r, col.g, col.b, col.bri);
}

// Writes only the parts that really changed, so the flash is not worn out
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

// =====================================================================
//                     MICROPHONE (I2S, ESP-IDF driver)
// =====================================================================
i2s_chan_handle_t micRx = nullptr;
bool    micReady = false;
int32_t i2sBuf[I2S_READ_SAMPLES];

bool initMic() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num  = 8;     // 8 x 256 samples = 128 ms of buffer, covers slow web requests
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
  stdCfg.slot_cfg.slot_mask = MIC_SLOT;

  if (i2s_channel_init_std_mode(micRx, &stdCfg) != ESP_OK) return false;
  if (i2s_channel_enable(micRx) != ESP_OK) return false;
  return true;
}

// ---------- Audio analysis state ----------
int32_t  dcAcc = 0, bassLp1 = 0, bassLp2 = 0, bassDc = 0;   // filter states (x256 fixed point)
uint64_t levelSumSq = 0, bassSumSq = 0;
int      frameSamples = 0;

uint16_t soundLevel = 0;        // overall level of the last frame (RMS, 0-32767)
uint16_t bassLevel  = 0;        // bass level of the last frame (RMS)
uint32_t bassEnergy = 0;        // bass energy of the last frame (mean square)
uint32_t bassAvgEnergy = 0;     // rolling 1 s average of bass energy

uint32_t bassHistory[BEAT_HISTORY_FRAMES];
uint64_t bassHistorySum = 0;
int      bassHistoryIdx = 0, bassHistoryCount = 0;

// Beat state (Music mode)
uint32_t lastBeatAt = 0;
bool     beatSeen   = false;

// Clap state (Auto + No-WiFi mode)
enum ClapState : uint8_t { CLAP_IDLE, CLAP_SPIKE, CLAP_TOO_LONG };
ClapState clapState = CLAP_IDLE;
uint32_t spikeStart = 0;
uint16_t spikePeak  = 0;
uint16_t prevLevel1 = 0, prevLevel2 = 0;   // levels 8 ms and 16 ms ago
float    noiseFloor = 100;
int      clapCount  = 0;
bool     clapSequenceRejected = false;
uint32_t lastClapAt = 0, clapCooldownUntil = 0;

void resetClapDetector() {
  clapState = CLAP_IDLE; clapCount = 0; clapSequenceRejected = false;
}

// ---------- FEATURE 1: beat detection ----------
// Compares this frame's bass energy with the average of the last ~1 s.
void detectBeat(uint32_t now) {
  if (bassLevel >= BEAT_MIN_BASS_LEVEL &&
      bassEnergy > bassAvgEnergy * BEAT_THRESHOLD_MULT &&
      (!beatSeen || now - lastBeatAt >= BEAT_MIN_GAP_MS)) {
    lastBeatAt = now;
    beatSeen = true;
    DBG(">> BEAT  bass:%u avg:%u\n", bassLevel, (unsigned)sqrtf((float)bassAvgEnergy));
  }
}

// ---------- FEATURE 2: double clap ----------
void toggleLightByClap() {
  if (st.mode != M_OFF) setMode(M_OFF);
  else                  setMode(st.lastSolid);   // back on with the last colour
  markChanged();
  DBG(">> Light turned %s by double clap\n", st.mode == M_OFF ? "off" : "on");
}

// Called for every single clap. Decides whether it belongs to a double clap.
void onClap(uint32_t t) {
  if ((int32_t)(t - clapCooldownUntil) < 0) return;           // cooldown after a double clap
  DBG(">> CLAP  level:%u\n", spikePeak);

  if (clapSequenceRejected) { lastClapAt = t; return; }        // still inside a 3+ clap burst
  if (clapCount == 0) { clapCount = 1; lastClapAt = t; return; }

  uint32_t gap = t - lastClapAt;
  if (gap < DOUBLE_CLAP_GAP_MIN_MS) return;                    // echo / too fast, ignore
  if (gap > DOUBLE_CLAP_GAP_MAX_MS) { clapCount = 1; lastClapAt = t; return; }  // new sequence

  if (clapCount == 1) { clapCount = 2; lastClapAt = t; }       // wait: a 3rd clap would cancel it
  else {
    clapSequenceRejected = true; clapCount = 0; lastClapAt = t;
    DBG(">> 3+ claps in a row, ignored\n");
  }
}

// Confirms a double clap once no third clap followed (runs every loop)
void updateClapSequence(uint32_t now) {
  if (clapCount == 0 && !clapSequenceRejected) return;
  if (now - lastClapAt <= DOUBLE_CLAP_GAP_MAX_MS) return;
  if (clapSequenceRejected)   clapSequenceRejected = false;
  else if (clapCount == 2) {
    DBG(">> DOUBLE CLAP\n");
    clapCooldownUntil = now + DOUBLE_CLAP_COOLDOWN_MS;
    toggleLightByClap();
  }
  clapCount = 0;
}

// A clap = level rises sharply, then drops again within CLAP_MAX_DURATION_MS
void detectClap(uint32_t now) {
  switch (clapState) {
    case CLAP_IDLE: {
      float thr = max((float)CLAP_THRESHOLD, noiseFloor * CLAP_FLOOR_RATIO);
      if (soundLevel > thr && soundLevel > prevLevel2 * CLAP_RISE_RATIO) {
        clapState = CLAP_SPIKE; spikeStart = now; spikePeak = soundLevel;
      } else {
        noiseFloor += (soundLevel - noiseFloor) * 0.02f;     // slowly follow background noise
      }
      break;
    }
    case CLAP_SPIKE:
      if (soundLevel > spikePeak) spikePeak = soundLevel;
      if (soundLevel < spikePeak * CLAP_RELEASE_RATIO) {
        clapState = CLAP_IDLE;
        if (now - spikeStart <= CLAP_MAX_DURATION_MS) onClap(spikeStart);
      } else if (now - spikeStart > CLAP_MAX_DURATION_MS) {
        clapState = CLAP_TOO_LONG;                             // sustained sound, not a clap
      }
      break;
    case CLAP_TOO_LONG:
      if (soundLevel < spikePeak * CLAP_RELEASE_RATIO || now - spikeStart > 2000) {
        clapState = CLAP_IDLE;
        noiseFloor = soundLevel;
      }
      break;
  }
  prevLevel2 = prevLevel1;
  prevLevel1 = soundLevel;
}

// Runs every 8 ms (one analysis frame)
void onAudioFrame() {
  uint32_t now = millis();
  soundLevel = (uint16_t)sqrtf((float)(levelSumSq / AUDIO_FRAME_SAMPLES));
  bassEnergy = (uint32_t)(bassSumSq / AUDIO_FRAME_SAMPLES);
  bassLevel  = (uint16_t)sqrtf((float)bassEnergy);
  bassAvgEnergy = bassHistoryCount ? (uint32_t)(bassHistorySum / bassHistoryCount) : bassEnergy;

  SysMode sm = sysMode();
  if (sm == SYS_MUSIC) detectBeat(now);    // beats only in Music mode
  else                 detectClap(now);    // claps only in Auto / No-WiFi mode

  // rolling 1 s bass history (kept up to date in every mode)
  if (bassHistoryCount == BEAT_HISTORY_FRAMES) bassHistorySum -= bassHistory[bassHistoryIdx];
  else bassHistoryCount++;
  bassHistory[bassHistoryIdx] = bassEnergy;
  bassHistorySum += bassEnergy;
  bassHistoryIdx = (bassHistoryIdx + 1) % BEAT_HISTORY_FRAMES;
}

// Non-blocking: takes whatever samples are ready in the DMA buffer and returns.
// Filters use integer maths only (the C3 has no FPU).
void processAudio() {
  if (!micReady) return;
  for (int pass = 0; pass < 4; pass++) {            // catch up, but never hog the loop
    size_t bytesRead = 0;
    i2s_channel_read(micRx, i2sBuf, sizeof(i2sBuf), &bytesRead, 0);   // timeout 0 = no waiting
    int n = bytesRead / sizeof(int32_t);
    if (n == 0) return;

    for (int i = 0; i < n; i++) {
      int32_t s = i2sBuf[i] >> MIC_SAMPLE_SHIFT;   // 32-bit -> 16-bit

      // remove DC offset -> overall sound (used for claps)
      dcAcc += (s * 256 - dcAcc) >> DC_SHIFT;
      int32_t ac = s - (dcAcc >> 8);
      if (ac > 32767) ac = 32767; else if (ac < -32767) ac = -32767;
      levelSumSq += (uint32_t)(ac * ac);

      // bass band ~40-150 Hz: two low-pass stages, then remove content below ~40 Hz
      bassLp1 += (ac * 256 - bassLp1) >> BASS_LP_SHIFT;
      bassLp2 += (bassLp1 - bassLp2) >> BASS_LP_SHIFT;
      bassDc  += (bassLp2 - bassDc) >> BASS_HP_SHIFT;
      int32_t bass = (bassLp2 - bassDc) >> 8;
      bassSumSq += (uint32_t)(bass * bass);

      if (++frameSamples >= AUDIO_FRAME_SAMPLES) {
        onAudioFrame();
        frameSamples = 0; levelSumSq = 0; bassSumSq = 0;
      }
    }
    if (n < I2S_READ_SAMPLES) return;               // buffer drained
  }
}

// =====================================================================
//                           LED rendering
// =====================================================================
void updateLEDs() {
  static uint32_t lastFrame = 0, lastToggle = 0;
  static uint16_t hue = 0;
  static bool     blinkOn = true;
  static int      lastMusicLevel = -1;
  uint32_t now = millis();

  if (offTimerActive && (int32_t)(now - offTimerEnd) >= 0) {
    offTimerActive = false;
    setMode(M_OFF);
    markChanged();
    Serial.println("Timer finished, LEDs off");
  }

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
        lastToggle = now;
        needsRender = false;
        strip.setBrightness(col.bri);
        strip.fill(blinkOn ? strip.Color(col.r, col.g, col.b) : 0);
        strip.show();
      }
      return;
    }

    // FEATURE 1 output: flash in the selected colour on a beat, then ease out
    case M_MUSIC: {
      if (now - lastFrame < 10 && !needsRender) return;       // ~100 fps max
      lastFrame = now;
      int level = MUSIC_BASE_LEVEL;
      if (beatSeen) {
        uint32_t e = now - lastBeatAt;
        if (e < BEAT_FLASH_HOLD_MS) level = 255;
        else if (e < BEAT_FLASH_HOLD_MS + BEAT_FADE_MS) {
          uint32_t f = (BEAT_FLASH_HOLD_MS + BEAT_FADE_MS - e) * 255 / BEAT_FADE_MS;
          f = f * f / 255;                                     // ease-out curve
          if ((int)f > level) level = f;
        }
      }
      if (level != lastMusicLevel || needsRender) {
        lastMusicLevel = level;
        strip.setBrightness(col.bri);
        strip.fill(strip.Color(col.r * level / 255, col.g * level / 255, col.b * level / 255));
        strip.show();
      }
      needsRender = false;
      return;
    }

    default: {
      if (!needsRender) return;
      needsRender = false;
      lastMusicLevel = -1;
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
//                          Dashboard web page
// =====================================================================
const char CONTROL_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Light strip</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;500;600;700&display=swap" rel="stylesheet">
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
button,input{font:inherit;color:inherit}
button{cursor:pointer;background:none;border:0}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:var(--r-ctl)}
[hidden]{display:none!important}

.app{max-width:1040px;margin:0 auto;padding:40px 24px 56px}
.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:40px}
.brand{font-weight:600;font-size:15px;letter-spacing:-.005em}
.status{display:flex;align-items:center;gap:8px;color:var(--mute);font-size:13px}
.status i{width:7px;height:7px;border-radius:50%;background:#22A06B}
.status.bad i{background:#D14343}

.grid{display:grid;grid-template-columns:5fr 7fr;gap:24px;align-items:start}
.col{display:grid;gap:24px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--r-card);padding:28px}
h2{font-size:17px;font-weight:600;line-height:1.4;letter-spacing:-.01em}
.sub{color:var(--mute);font-size:13px;line-height:1.5}

/* Hero */
.hero{padding:32px 28px 28px}
.hero-top{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:36px}
.lamp{width:96px;height:96px;border-radius:50%;border:1px solid var(--line);position:relative;overflow:hidden;background:#F4F4F5}
.lamp span{position:absolute;inset:0;transition:background .4s ease,opacity .4s ease}
.hero h1{font-size:30px;font-weight:700;line-height:1.2;letter-spacing:-.025em;margin-bottom:6px}
.hero .sub{font-size:14px}

/* Switch */
.switch{width:48px;height:28px;border-radius:14px;background:var(--line-strong);position:relative;transition:background .2s}
.switch::after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:#fff;transition:transform .2s}
.switch[aria-checked="true"]{background:var(--accent)}
.switch[aria-checked="true"]::after{transform:translateX(20px)}

/* Ranges */
.field{margin-top:24px}
.field:first-child{margin-top:0}
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

/* Modes */
.modes{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
.tile{display:flex;flex-direction:column;align-items:flex-start;gap:14px;padding:18px;border:1px solid var(--line);border-radius:var(--r-card);background:var(--surface);text-align:left;transition:transform .18s ease,background .18s ease,border-color .18s ease}
.tile:hover{transform:scale(1.02);background:var(--hover)}
.tile .dot{width:22px;height:22px;border-radius:50%;border:1px solid rgba(0,0,0,.08)}
.tile b{display:block;font-weight:600;font-size:14px;line-height:1.3}
.tile small{display:block;color:var(--mute);font-size:12px;margin-top:2px}
.tile.active{border-color:var(--accent);box-shadow:inset 0 0 0 1px var(--accent)}
.tile.wide{grid-column:1/-1;flex-direction:row;align-items:center;gap:16px}

/* Panels */
.panel h2{margin-bottom:4px}
.panel .sub{margin-bottom:24px}
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

/* Timer */
.t-head{display:flex;justify-content:space-between;align-items:baseline}
.custom{display:flex;gap:8px;margin-top:12px}
.custom input{flex:1;min-width:0;padding:9px 12px;border:1px solid var(--line);border-radius:var(--r-ctl);background:var(--surface)}
.custom input:focus{outline:none;border-color:var(--accent)}
.primary{padding:9px 18px;border-radius:var(--r-ctl);background:var(--accent);color:#fff;font-weight:600;font-size:14px;transition:transform .18s ease,opacity .18s}
.primary:hover{transform:scale(1.02);opacity:.92}
.running{margin-top:20px}
.count{font-size:26px;font-weight:700;letter-spacing:-.02em;font-variant-numeric:tabular-nums;line-height:1.2}
.bar{height:3px;background:var(--line);border-radius:2px;margin:14px 0 12px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--accent);transition:width 1s linear}
.text-btn{color:var(--mute);font-size:13px;font-weight:500;padding:4px 0;transition:color .18s}
.text-btn:hover{color:var(--ink)}

/* Footer */
.device{display:grid;grid-template-columns:repeat(4,auto) 1fr;gap:4px 32px;margin-top:48px;padding-top:24px;border-top:1px solid var(--line);font-size:13px}
.device dt{color:var(--mute)}
.device dd{font-variant-numeric:tabular-nums}
.device .text-btn{justify-self:end;align-self:center;grid-row:span 2}

@media (max-width:820px){
 .app{padding:28px 18px 40px}
 .top{margin-bottom:28px}
 .grid{grid-template-columns:1fr}
 .col{display:contents}
 .hero{order:1}.modes{order:2}.panel{order:3}.timer{order:4}
 .card{padding:22px}
 .swatches{grid-template-columns:repeat(9,1fr);gap:6px}
 .device{grid-template-columns:1fr 1fr}
 .device .text-btn{grid-column:span 2;grid-row:auto;justify-self:start;margin-top:12px}
}
@media (max-width:420px){.modes{grid-template-columns:repeat(2,1fr)}}
@media (prefers-reduced-motion:reduce){*{transition:none!important}}
</style></head><body>
<main class="app">
 <header class="top">
  <div class="brand">Light strip</div>
  <div class="status" id="status"><i></i><span>Connecting</span></div>
 </header>

 <div class="grid">
  <div class="col">
   <section class="card hero" aria-live="polite">
    <div class="hero-top">
     <div class="lamp"><span id="lamp"></span></div>
     <button class="switch" id="power" role="switch" aria-checked="false" aria-label="Power"></button>
    </div>
    <h1 id="hTitle">&nbsp;</h1>
    <p class="sub" id="hSub">&nbsp;</p>
    <div class="field" style="margin-top:32px">
     <div class="label"><label for="bri">Brightness</label><output id="briV"></output></div>
     <input type="range" class="r-fill" id="bri" min="1" max="255">
    </div>
   </section>

   <section class="card timer">
    <div class="t-head"><h2>Auto-off timer</h2><button class="text-btn" id="tCancel" hidden>Cancel timer</button></div>
    <div id="tIdle">
     <p class="sub">Turns the light off after a set time.</p>
     <div class="chips">
      <button class="chip" data-t="15">15 min</button>
      <button class="chip" data-t="30">30 min</button>
      <button class="chip" data-t="60">1 hour</button>
      <button class="chip" data-t="120">2 hours</button>
     </div>
     <div class="custom">
      <input type="number" id="tMin" min="1" max="1440" placeholder="Minutes" inputmode="numeric" aria-label="Custom minutes">
      <button class="primary" id="tStart">Start timer</button>
     </div>
    </div>
    <div class="running" id="tRun" hidden>
     <div class="count" id="tCount">0:00</div>
     <div class="bar"><i id="tBar"></i></div>
     <p class="sub">until the light turns off</p>
    </div>
   </section>
  </div>

  <div class="col">
   <section class="modes" aria-label="Mode">
    <button class="tile" data-m="color"><span class="dot"></span><span><b>Colour</b><small>Any hue</small></span></button>
    <button class="tile" data-m="warm"><span class="dot"></span><span><b>Warm</b><small>1800 to 3500 K</small></span></button>
    <button class="tile" data-m="cold"><span class="dot"></span><span><b>Cold</b><small>5000 to 10000 K</small></span></button>
    <button class="tile" data-m="white"><span class="dot"></span><span><b>White</b><small>All channels</small></span></button>
    <button class="tile" data-m="rainbow"><span class="dot"></span><span><b>Rainbow</b><small>Moving spectrum</small></span></button>
    <button class="tile" data-m="blink"><span class="dot"></span><span><b>Blink</b><small>On and off</small></span></button>
    <button class="tile wide" data-m="music"><span class="dot"></span><span><b>Music</b><small>Flashes on each bass beat from the microphone</small></span></button>
   </section>

   <section class="card panel" data-p="color">
    <h2>Colour</h2><p class="sub">Choose any colour, or start from a swatch.</p>
    <label class="picker"><input type="color" id="col"><span id="colHex"></span></label>
    <div class="swatches" id="swatches"></div>
   </section>

   <section class="card panel" data-p="warm" hidden>
    <h2>Warm white</h2><p class="sub">Lower values feel like candlelight, higher like a classic bulb.</p>
    <div class="field">
     <div class="label"><label for="warm">Tone</label><output id="warmV"></output></div>
     <input type="range" class="r-warm" id="warm" min="1800" max="3500" step="50">
    </div>
    <div class="chips" data-for="warm">
     <button class="chip" data-v="1900">Candle</button>
     <button class="chip" data-v="2400">Evening</button>
     <button class="chip" data-v="2700">Soft</button>
     <button class="chip" data-v="3200">Studio</button>
    </div>
   </section>

   <section class="card panel" data-p="cold" hidden>
    <h2>Cold white</h2><p class="sub">Crisp light for focus. Higher values turn slightly blue.</p>
    <div class="field">
     <div class="label"><label for="cold">Tone</label><output id="coldV"></output></div>
     <input type="range" class="r-cold" id="cold" min="5000" max="10000" step="100">
    </div>
    <div class="chips" data-for="cold">
     <button class="chip" data-v="5000">Neutral</button>
     <button class="chip" data-v="5600">Daylight</button>
     <button class="chip" data-v="6500">Cool</button>
     <button class="chip" data-v="8000">Overcast</button>
    </div>
   </section>

   <section class="card panel" data-p="white" hidden>
    <h2>Pure white</h2><p class="sub">Red, green and blue at full level.</p>
    <p class="note">This is the brightest setting and draws the most power. Use the brightness slider to soften it.</p>
   </section>

   <section class="card panel" data-p="rainbow" hidden>
    <h2>Rainbow</h2><p class="sub">The full spectrum flows along the strip.</p>
    <div class="field">
     <div class="label"><label for="spd">Speed</label><output id="spdV"></output></div>
     <input type="range" class="r-fill" id="spd" min="1" max="100">
    </div>
    <div class="chips" data-for="spd">
     <button class="chip" data-v="8">Drift</button>
     <button class="chip" data-v="35">Gentle</button>
     <button class="chip" data-v="70">Lively</button>
     <button class="chip" data-v="100">Fast</button>
    </div>
   </section>

   <section class="card panel" data-p="blink" hidden>
    <h2>Blink</h2><p class="sub">Flashes one colour on a steady rhythm.</p>
    <label class="picker"><input type="color" id="bcol"><span id="bcolHex"></span></label>
    <div class="field">
     <div class="label"><label for="on">On for</label><output id="onV"></output></div>
     <input type="range" class="r-fill" id="on" min="50" max="3000" step="50">
    </div>
    <div class="field">
     <div class="label"><label for="off">Off for</label><output id="offV"></output></div>
     <input type="range" class="r-fill" id="off" min="50" max="3000" step="50">
    </div>
    <div class="chips" data-for="blink">
     <button class="chip" data-on="150" data-off="150">Quick</button>
     <button class="chip" data-on="500" data-off="500">Steady</button>
     <button class="chip" data-on="1200" data-off="1200">Slow</button>
     <button class="chip" data-on="100" data-off="1400">Beacon</button>
    </div>
   </section>
   <section class="card panel" data-p="music" hidden>
    <h2>Music</h2><p class="sub">The strip flashes on each bass beat, then fades out.</p>
    <label class="picker"><input type="color" id="mcol"><span id="mcolHex"></span></label>
    <p class="note" style="margin-top:16px">Only the bass range is used, so voices and claps don't trigger flashes. Place the microphone near the speaker.</p>
   </section>
  </div>
 </div>

 <dl class="device">
  <div><dt>Address</dt><dd id="dIp">-</dd></div>
  <div><dt>Name</dt><dd id="dHost">-</dd></div>
  <div><dt>Signal</dt><dd id="dSig">-</dd></div>
  <div><dt>Sound</dt><dd id="dSound">-</dd></div>
  <button class="text-btn" id="wifiReset">Forget WiFi network</button>
 </dl>
</main>

<script>
const $=id=>document.getElementById(id);
let S=null,left=0,total=0,busy=false,pending=null;

function kelvin(k){const t=k/100;let r,g,b;
 if(t<=66){r=255;g=99.4708025861*Math.log(t)-161.1195681661}
 else{r=329.698727446*Math.pow(t-60,-0.1332047592);g=288.1221695283*Math.pow(t-60,-0.0755148492)}
 b=t>=66?255:t<=19?0:138.5177312231*Math.log(t-10)-305.0447927307;
 const c=v=>Math.max(0,Math.min(255,Math.round(v)));return `rgb(${c(r)},${c(g)},${c(b)})`}
const hex=(r,g,b)=>'#'+[r,g,b].map(x=>x.toString(16).padStart(2,'0')).join('');
const RAINBOW='conic-gradient(from 90deg,#FF5E5E,#FFC75E,#7EE081,#5EC8FF,#9B7BFF,#FF5EC4,#FF5E5E)';

function modeFill(m){
 switch(m){case 'color':case 'blink':case 'music':return hex(S.r,S.g,S.b);case 'warm':return kelvin(S.warm);
  case 'cold':return kelvin(S.cold);case 'white':return '#FFFFFF';case 'rainbow':return RAINBOW;}
 return '#F4F4F5';
}
const TITLES={off:'Off',color:'Colour',warm:'Warm white',cold:'Cold white',white:'Pure white',rainbow:'Rainbow',blink:'Blink',music:'Music'};
function detail(){
 const p=Math.round(S.bri/2.55)+'% brightness';
 switch(S.mode){
  case 'off':return 'Use the switch to turn the light on';
  case 'color':return hex(S.r,S.g,S.b).toUpperCase()+', '+p;
  case 'warm':return S.warm+' K, '+p;
  case 'cold':return S.cold+' K, '+p;
  case 'rainbow':return 'Speed '+S.speed+', '+p;
  case 'music':return 'Flashes on each beat, '+p;
  case 'blink':return (S.on/1000)+' s on, '+(S.off/1000)+' s off';
 }return p;
}

/* ---------- networking ---------- */
function setStatus(ok){const s=$('status');s.classList.toggle('bad',!ok);s.lastChild.textContent=ok?'Online':'Not responding'}
function send(q){
 if(busy){pending=q;return}
 busy=true;
 fetch('/api/set?'+q).then(r=>r.json()).then(j=>{S=j;setStatus(true);sync(false)})
 .catch(()=>setStatus(false))
 .finally(()=>{busy=false;if(pending){const p=pending;pending=null;send(p)}});
}
function load(full){fetch('/api/state').then(r=>r.json()).then(j=>{S=j;setStatus(true);sync(full)}).catch(()=>setStatus(false))}
function timer(m){fetch('/api/timer?min='+m).then(r=>r.json()).then(j=>{S=j;sync(false)}).catch(()=>setStatus(false))}

/* ---------- rendering ---------- */
function fill(el){el.style.setProperty('--p',((el.value-el.min)/(el.max-el.min)*100)+'%')}
function labels(){
 $('briV').textContent=Math.round($('bri').value/2.55)+'%';
 $('warmV').textContent=$('warm').value+' K';
 $('coldV').textContent=$('cold').value+' K';
 $('spdV').textContent=$('spd').value;
 $('onV').textContent=($('on').value/1000).toFixed(2).replace(/0$/,'')+' s';
 $('offV').textContent=($('off').value/1000).toFixed(2).replace(/0$/,'')+' s';
 ['col','bcol','mcol'].forEach(i=>$(i+'Hex').textContent=$(i).value);
 document.querySelectorAll('.r-fill').forEach(fill);
 markChips();
}
function markChips(){
 document.querySelectorAll('[data-for] .chip').forEach(c=>{
  const f=c.parentNode.dataset.for;
  c.classList.toggle('on', f=='blink' ? (c.dataset.on==$('on').value&&c.dataset.off==$('off').value) : c.dataset.v==$(f).value);
 });
}
function sync(full){
 if(!S)return;
 const shown=S.mode=='off'?S.last:S.mode;
 document.querySelectorAll('.tile').forEach(t=>{
  t.classList.toggle('active',t.dataset.m==S.mode);
  t.querySelector('.dot').style.background=modeFill(t.dataset.m);
 });
 document.querySelectorAll('.panel').forEach(p=>p.hidden=p.dataset.p!=shown);
 $('power').setAttribute('aria-checked',S.mode!='off');
 const l=$('lamp');
 l.style.background=modeFill(S.mode);
 l.style.opacity=S.mode=='off'?1:(0.35+0.65*S.bri/255);
 $('hTitle').textContent=TITLES[S.mode];
 $('hSub').textContent=detail();
 if(full){
  $('bri').value=S.bri;$('warm').value=S.warm;$('cold').value=S.cold;$('spd').value=S.speed;
  $('on').value=S.on;$('off').value=S.off;$('col').value=$('bcol').value=$('mcol').value=hex(S.r,S.g,S.b);
  labels();
 } else { $('col').value=$('bcol').value=$('mcol').value=hex(S.r,S.g,S.b); ['col','bcol','mcol'].forEach(i=>$(i+'Hex').textContent=$(i).value); }
 $('dIp').textContent=S.ip;$('dHost').textContent=S.host+'.local';
 $('dSound').textContent=S.sys=='music'?'Beat sync':'Double clap toggles the light';
 $('dSig').textContent=S.rssi>-60?'Strong':S.rssi>-72?'Good':S.rssi>-82?'Fair':'Weak';
 left=S.timer;total=S.total||S.timer;showTimer();
}
function showTimer(){
 const run=left>0;
 $('tIdle').hidden=run;$('tRun').hidden=!run;$('tCancel').hidden=!run;
 if(!run)return;
 const h=Math.floor(left/3600),m=Math.floor(left%3600/60),s=String(left%60).padStart(2,'0');
 $('tCount').textContent=h?`${h}:${String(m).padStart(2,'0')}:${s}`:`${m}:${s}`;
 $('tBar').style.width=(total?left/total*100:0)+'%';
}

/* ---------- events ---------- */
document.querySelectorAll('.tile').forEach(t=>t.onclick=()=>send('mode='+t.dataset.m));
$('power').onclick=()=>send('power='+(S&&S.mode=='off'?1:0));
$('bri').oninput=e=>{labels();send('bri='+e.target.value)};
$('warm').oninput=e=>{labels();send('mode=warm&warm='+e.target.value)};
$('cold').oninput=e=>{labels();send('mode=cold&cold='+e.target.value)};
$('spd').oninput=e=>{labels();send('mode=rainbow&speed='+e.target.value)};
$('on').oninput=e=>{labels();send('mode=blink&on='+e.target.value)};
$('off').oninput=e=>{labels();send('mode=blink&off='+e.target.value)};
$('col').oninput=e=>{$('bcol').value=$('mcol').value=e.target.value;labels();send('mode=color&c='+e.target.value.slice(1))};
$('bcol').oninput=e=>{$('col').value=$('mcol').value=e.target.value;labels();send('mode=blink&c='+e.target.value.slice(1))};
$('mcol').oninput=e=>{$('col').value=$('bcol').value=e.target.value;labels();send('mode=music&c='+e.target.value.slice(1))};
document.querySelectorAll('[data-for] .chip').forEach(c=>c.onclick=()=>{
 const f=c.parentNode.dataset.for;
 if(f=='blink'){$('on').value=c.dataset.on;$('off').value=c.dataset.off;labels();send(`mode=blink&on=${c.dataset.on}&off=${c.dataset.off}`);return}
 $(f).value=c.dataset.v;labels();
 send(f=='spd'?'mode=rainbow&speed='+c.dataset.v:`mode=${f}&${f}=${c.dataset.v}`);
});
['FF3B30','FF9500','FFCC00','34C759','00C7BE','0A84FF','5E5CE6','FF2D55','FFFFFF'].forEach(c=>{
 const b=document.createElement('button');b.className='sw';b.style.background='#'+c;b.setAttribute('aria-label','#'+c);
 b.onclick=()=>{$('col').value=$('bcol').value=$('mcol').value='#'+c.toLowerCase();labels();send('mode=color&c='+c)};
 $('swatches').appendChild(b);
});
document.querySelectorAll('[data-t]').forEach(c=>c.onclick=()=>timer(c.dataset.t));
$('tStart').onclick=()=>{const v=parseInt($('tMin').value);if(v>0)timer(v);else $('tMin').focus()};
$('tMin').onkeydown=e=>{if(e.key=='Enter')$('tStart').click()};
$('tCancel').onclick=()=>timer(0);
$('wifiReset').onclick=()=>{if(confirm('Forget the saved WiFi network? The device restarts in setup mode and this page stops responding.'))fetch('/api/wifireset')};

setInterval(()=>{if(left>0){left--;showTimer();if(left==0)setTimeout(()=>load(false),1500)}},1000);
setInterval(()=>load(false),4000);   // picks up clap toggles and timer changes
load(true);
</script></body></html>
)rawliteral";

String stateJson() {
  int32_t  rem = offTimerActive ? (int32_t)(offTimerEnd - millis()) : 0;
  uint32_t tl  = rem > 0 ? (uint32_t)rem / 1000 : 0;
  String j = "{";
  j += "\"mode\":\"" + String(MODE_NAMES[st.mode]) + "\",";
  j += "\"last\":\"" + String(MODE_NAMES[st.lastMode]) + "\",";
  j += "\"sys\":\"" + String(sysMode() == SYS_MUSIC ? "music" : "auto") + "\",";
  j += "\"r\":" + String(col.r) + ",\"g\":" + String(col.g) + ",\"b\":" + String(col.b) + ",";
  j += "\"bri\":" + String(col.bri) + ",";
  j += "\"warm\":" + String(st.warmK) + ",\"cold\":" + String(st.coldK) + ",";
  j += "\"speed\":" + String(st.speed) + ",";
  j += "\"on\":" + String(st.onMs) + ",\"off\":" + String(st.offMs) + ",";
  j += "\"timer\":" + String(tl) + ",\"total\":" + String(offTimerActive ? offTimerTotal : 0) + ",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"host\":\"" + String(HOSTNAME) + "\",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + "}";
  return j;
}

void sendState() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", stateJson());
}

void handleControlPage() { server.send_P(200, "text/html", CONTROL_PAGE); }

void handleApiSet() {
  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    for (uint8_t i = 0; i < M_COUNT; i++) if (m == MODE_NAMES[i]) setMode(i);
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
  long mins = server.arg("min").toInt();
  if (mins > 0) {
    mins = constrain(mins, 1, 1440);
    offTimerTotal  = (uint32_t)mins * 60UL;
    offTimerEnd    = millis() + offTimerTotal * 1000UL;
    offTimerActive = true;
    if (st.mode == M_OFF) { setMode(st.lastMode); markChanged(); }
    Serial.printf("Timer set: %ld min\n", mins);
  } else {
    offTimerActive = false;
    Serial.println("Timer cancelled");
  }
  sendState();
}

void scheduleRestart() { restartPending = true; pendingAt = millis() + 1500; }

void handleWifiReset() {
  server.send(200, "text/plain", "WiFi forgotten. Restarting in setup mode.");
  clearCredentials();
  scheduleRestart();          // non-blocking: restart happens from loop()
}

void startControlServer() {
  server.on("/",              HTTP_GET, handleControlPage);
  server.on("/api/state",     HTTP_GET, sendState);
  server.on("/api/set",       HTTP_GET, handleApiSet);
  server.on("/api/timer",     HTTP_GET, handleApiTimer);
  server.on("/api/wifireset", HTTP_GET, handleWifiReset);
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
    String esc = htmlEscape(ssid);
    networkListHtml += "<button type='button' class='net' data-ssid=\"" + esc + "\"><span class='nm'><b>" + esc +
                       "</b><small>" + (open ? "Open" : "Secured") + "</small></span><span class='bars b" +
                       String(bars) + "'><i></i><i></i><i></i><i></i></span></button>";
  }
  WiFi.scanDelete();
}

const char PORTAL_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Connect to WiFi</title>
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
@media (prefers-reduced-motion:reduce){*{transition:none!important}}
</style></head><body><div class="wrap">
)rawliteral";

const char PORTAL_FOOT[] PROGMEM = R"rawliteral(
</div><script>
document.querySelectorAll('.net').forEach(n=>n.onclick=()=>{
 document.querySelectorAll('.net').forEach(x=>x.classList.remove('sel'));
 n.classList.add('sel');document.getElementById('ssid').value=n.dataset.ssid;document.getElementById('pass').focus();
});
const sh=document.getElementById('sh');if(sh)sh.onchange=()=>document.getElementById('pass').type=sh.checked?'text':'password';
</script></body></html>
)rawliteral";

void handlePortalRoot() {
  String page = FPSTR(PORTAL_HEAD);
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
            "<p class='hint'>Without WiFi, double clap to turn the light on or off.</p>");
  page += FPSTR(PORTAL_FOOT);
  server.send(200, "text/html", page);
}

void handlePortalScan() {
  scanNetworks();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handlePortalSave() {
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  ssid.trim();
  if (ssid.length() == 0) { server.send(400, "text/html", "Enter a network name. <a href='/'>Go back</a>"); return; }
  saveCredentials(ssid, pass);
  String page = FPSTR(PORTAL_HEAD);
  page += "<div class='done'><h1>Connecting</h1><p>The light strip is restarting and joining <b>" + htmlEscape(ssid) +
          "</b>.</p><p>Reconnect your phone to your usual WiFi, then open <b>http://" + String(HOSTNAME) +
          ".local</b> to control the light.</p><p>If the password was wrong, the <b>" + String(AP_SSID) +
          "</b> network appears again within a minute.</p></div>";
  page += FPSTR(PORTAL_FOOT);
  server.send(200, "text/html", page);
  scheduleRestart();
}

// "Use without WiFi": remembered across restarts until WiFi is set up again
void handlePortalOffline() {
  saveNoWifiFlag(true);
  String page = FPSTR(PORTAL_HEAD);
  page += F("<div class='done'><h1>Running without WiFi</h1><p>The setup network turns off now. "
            "Double clap to turn the light on or off.</p><p>To set up WiFi later, power the strip on, "
            "then within 10 seconds hold the <b>BOOT</b> button for 3 seconds.</p></div>");
  page += FPSTR(PORTAL_FOOT);
  server.send(200, "text/html", page);
  offlinePending = true; pendingAt = millis() + 1500;   // let the page reach the phone first
}

void handleCaptive() {
  server.sendHeader("Location", String("http://") + apIP.toString() + "/", true);
  server.send(302, "text/plain", "");
}

void startPortal() {
  Serial.println("Starting captive portal...");
  leaveMusicIfActive();
  WiFi.disconnect(true);
  delay(100);                                  // setup only, not in loop()
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
  Serial.printf("Portal ready: join \"%s\" -> http://%s\n", AP_SSID, apIP.toString().c_str());
}

// =====================================================================
//                            No-WiFi mode
// =====================================================================
void enterNoWifiMode(const char* reason) {
  if (portalActive) { server.stop(); dnsServer.stop(); WiFi.softAPdisconnect(true); }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  portalActive = false;
  noWifiMode   = true;
  leaveMusicIfActive();
  Serial.printf("No-WiFi mode (%s). Double clap toggles the light.\n", reason);
}

// =====================================================================
//                            STA connection
// =====================================================================
bool connectToWiFi() {
  if (savedSSID.length() == 0) { Serial.println("No saved credentials."); return false; }
  Serial.printf("Connecting to \"%s\"", savedSSID.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  setReducedTxPower();
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  WiFi.setTxPower(TX_POWER);
  WiFi.setAutoReconnect(true);

  uint32_t start = millis();                   // runs in setup() only
  while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT) {
    updateLEDs();
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected! IP: %s  RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("Connection failed.");
  return false;
}

// =====================================================================
//                   BOOT button (non-blocking WiFi reset)
// =====================================================================
// GPIO9 is also OLED SCL, so it is only read, never driven. Holding BOOT
// during reset would enter download mode, so the hold is checked shortly
// AFTER start-up instead.
void checkBootButton(uint32_t now) {
  static uint32_t pressedAt = 0;
  if (now > BOOT_RESET_WINDOW_MS + BOOT_RESET_HOLD_MS) return;
  if (digitalRead(BOOT_BTN) == LOW) {
    if (pressedAt == 0) pressedAt = now;
    else if (now - pressedAt >= BOOT_RESET_HOLD_MS) {
      Serial.println("BOOT held: WiFi settings erased, restarting into setup.");
      clearCredentials();
      ESP.restart();
    }
  } else {
    pressedAt = 0;
  }
}

// =====================================================================
//                              Debug output
// =====================================================================
void debugPrint(uint32_t now) {
#if DEBUG
  static uint32_t lastPrint = 0;
  if (now - lastPrint < DEBUG_INTERVAL_MS) return;
  lastPrint = now;
  Serial.printf("mode:%s light:%s level:%u bass:%u bassAvg:%u\n",
                SYS_NAMES[sysMode()], MODE_NAMES[st.mode], soundLevel, bassLevel,
                (unsigned)sqrtf((float)bassAvgEnergy));
#endif
}

// =====================================================================
//                           Setup / Loop
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32-C3 Super Mini - WiFi + LED + Microphone");

  pinMode(BOOT_BTN, INPUT_PULLUP);

  // LED strip: restore the last colour and settings immediately
  strip.begin();
  strip.clear();
  strip.show();
  loadLedState();
  needsRender = true;
  updateLEDs();

  // Microphone
  micReady = initMic();
  Serial.println(micReady ? "Microphone ready (I2S, 16 kHz)" : "Microphone init FAILED - check wiring");

  WiFi.persistent(false);
  loadCredentials();

  if (noWifiMode)          enterNoWifiMode("chosen on setup page");
  else if (connectToWiFi()) startControlServer();
  else                      startPortal();

  DBG("System mode: %s\n", SYS_NAMES[sysMode()]);
}

void loop() {
  uint32_t now = millis();

  processAudio();                         // read mic + detect beats / claps

  // react to system mode changes (e.g. Music selected in the dashboard)
  static SysMode lastSys = sysMode();
  SysMode sm = sysMode();
  if (sm != lastSys) {
    resetClapDetector();
    beatSeen = false;
    DBG("System mode: %s\n", SYS_NAMES[sm]);
    lastSys = sm;
  }
  if (sm != SYS_MUSIC) updateClapSequence(now);

  updateLEDs();
  debugPrint(now);
  checkBootButton(now);

  if (savePending && now - lastChange > SAVE_DELAY_MS) { savePending = false; saveIfChanged(); }

  // scheduled actions (replace delay() before restart)
  if (restartPending && (int32_t)(now - pendingAt) >= 0) ESP.restart();
  if (offlinePending && (int32_t)(now - pendingAt) >= 0) { offlinePending = false; enterNoWifiMode("chosen on setup page"); }

  if (noWifiMode) return;

  if (portalActive) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (WiFi.softAPgetStationNum() == 0 && now - portalStart > PORTAL_TIMEOUT) {
      if (savedSSID.length() > 0) { Serial.println("Portal timeout, restarting to retry WiFi..."); ESP.restart(); }
      else enterNoWifiMode("setup timed out");     // not saved: setup page returns on next boot
    }
    return;
  }

  server.handleClient();

  static uint32_t lostSince = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (lostSince == 0) { lostSince = now; Serial.println("WiFi lost, auto-reconnecting..."); }
    else if (now - lostSince > 60000) { Serial.println("Reconnect failed, restarting..."); ESP.restart(); }
  } else if (lostSince != 0) {
    lostSince = 0;
    Serial.println("WiFi reconnected.");
  }
}
