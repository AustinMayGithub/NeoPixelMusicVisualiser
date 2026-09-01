

#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include "hardware/adc.h"

// ── Hardware ───────────────────────────────────────────────────────────────────
#define LED_PIN         6
#define COLS            8
#define ROWS            30
#define NUM_LEDS        (COLS * ROWS)
#define AUDIO_PIN       26
#define BUTTON_PIN      2
#define OLED_WIDTH      128
#define OLED_HEIGHT     32
#define OLED_ADDR       0x3C
#define SDA_PIN         0
#define SCL_PIN         1
#define SAMPLE_COUNT    256

// ── Button timing ──────────────────────────────────────────────────────────────
#define LONG_PRESS_MS    600
#define DEBOUNCE_MS      40
#define MENU_TIMEOUT_MS  5000

// ── EEPROM layout ──────────────────────────────────────────────────────────────
// Magic number confirms saved data is valid (change if you add/remove settings)
#define EEPROM_MAGIC     0xAB
#define EEPROM_ADDR      0     // start address
#define EEPROM_SIZE      64    // bytes to reserve

struct Settings {
  uint8_t magic;
  float   gain;
  float   decay;
  int     peakHold;
  float   peakDecay;
  int     brightness;
};

// ── Live settings ──────────────────────────────────────────────────────────────
float gain       = 3.5f;
float decay      = 0.80f;
int   peakHold   = 10;
float peakDecay  = 0.88f;
int   brightness = 180;

bool     settingsDirty  = false;   // true when a value has changed
uint32_t lastChangeMs   = 0;       // time of last change
#define  SAVE_DELAY_MS  2000       // save 2 s after last change

void loadSettings() {
  Settings s;
  EEPROM.get(EEPROM_ADDR, s);
  if (s.magic != EEPROM_MAGIC) {
    Serial.println("EEPROM: no valid data, using defaults");
    return;
  }
  gain       = s.gain;
  decay      = s.decay;
  peakHold   = s.peakHold;
  peakDecay  = s.peakDecay;
  brightness = s.brightness;
  Serial.println("EEPROM: settings loaded");
}

void saveSettings() {
  Settings s;
  s.magic      = EEPROM_MAGIC;
  s.gain       = gain;
  s.decay      = decay;
  s.peakHold   = peakHold;
  s.peakDecay  = peakDecay;
  s.brightness = brightness;
  EEPROM.put(EEPROM_ADDR, s);
  EEPROM.commit();   // required on Pico — flushes emulated EEPROM to flash
  settingsDirty = false;
  Serial.println("EEPROM: settings saved");
}

// ── State ──────────────────────────────────────────────────────────────────────
float barLevel[COLS]  = {};
float peakLevel[COLS] = {};
int   peakTimer[COLS] = {};
uint16_t samples[SAMPLE_COUNT];

Adafruit_SSD1306  display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ── Menu ───────────────────────────────────────────────────────────────────────
enum MenuState { MENU_IDLE, MENU_NAVIGATE, MENU_EDIT };
MenuState menuState    = MENU_IDLE;
int       selectedItem = 0;
uint32_t  lastActivityMs = 0;

struct MenuItem {
  const char* label;
  float* fVal; int* iVal;
  float fStep; float fMin; float fMax;
  int   iStep; int   iMin; int   iMax;
};
MenuItem menuItems[] = {
  { "Gain",       &gain,        nullptr, 0.5f,  0.5f,  10.0f, 0,  0,   0   },
  { "Decay",      &decay,       nullptr, 0.05f, 0.10f, 0.98f, 0,  0,   0   },
  { "Peak Hold",  nullptr,  &peakHold,   0,     0,     0,     1,  1,   30  },
  { "Pk Decay",   &peakDecay,   nullptr, 0.02f, 0.50f, 0.99f, 0,  0,   0   },
  { "Brightness", nullptr,  &brightness, 0,     0,     0,     10, 10,  255 },
};
const int MENU_COUNT = sizeof(menuItems) / sizeof(menuItems[0]);

// ── Button ─────────────────────────────────────────────────────────────────────
bool     lastBtnRaw   = HIGH;
bool     btnState     = HIGH;
uint32_t btnPressTime = 0;
bool     longFired    = false;

void pollButton() {
  bool raw = digitalRead(BUTTON_PIN);
  static uint32_t lastChange = 0;
  if (raw != lastBtnRaw) { lastBtnRaw = raw; lastChange = millis(); return; }
  if (millis() - lastChange < DEBOUNCE_MS) return;

  bool pressed = (raw == LOW);

  if (pressed && btnState == HIGH) {
    btnState = LOW; btnPressTime = millis(); longFired = false;
  }

  if (pressed && !longFired && millis() - btnPressTime >= LONG_PRESS_MS) {
    longFired = true; lastActivityMs = millis();
    if      (menuState == MENU_IDLE)     menuState = MENU_NAVIGATE;
    else if (menuState == MENU_NAVIGATE) menuState = MENU_EDIT;
    else                                 menuState = MENU_NAVIGATE;
  }

  if (!pressed && btnState == LOW) {
    btnState = HIGH;
    if (millis() - btnPressTime < LONG_PRESS_MS) {
      lastActivityMs = millis();
      if (menuState == MENU_IDLE) {
        menuState = MENU_NAVIGATE;
      } else if (menuState == MENU_NAVIGATE) {
        selectedItem = (selectedItem + 1) % MENU_COUNT;
      } else {
        // Increment value and mark dirty for deferred save
        MenuItem& m = menuItems[selectedItem];
        if (m.fVal) { *m.fVal += m.fStep; if (*m.fVal > m.fMax) *m.fVal = m.fMin; }
        else        { *m.iVal += m.iStep; if (*m.iVal > m.iMax) *m.iVal = m.iMin; }
        settingsDirty = true;
        lastChangeMs  = millis();
      }
    }
  }
}

// ── OLED ───────────────────────────────────────────────────────────────────────
void formatItem(char* buf, int sz, int idx, bool arrow) {
  MenuItem& m = menuItems[idx];
  char val[10];
  if (m.fVal) snprintf(val, sizeof(val), "%.2f", *m.fVal);
  else        snprintf(val, sizeof(val), "%d",   *m.iVal);
  snprintf(buf, sz, "%s%-10s%s", arrow ? ">" : " ", m.label, val);
}

void drawMenu() {
  display.clearDisplay();

  if (menuState == MENU_IDLE) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(22, 2);
    display.print("MUSIC VISUALISER");
    display.setCursor(14, 14);
    display.print("Press btn for menu");

    float avg = 0;
    for (int c = 0; c < COLS; c++) avg += barLevel[c];
    avg /= COLS;
    int barW = constrain((int)(avg * OLED_WIDTH), 0, OLED_WIDTH);
    display.fillRect(0, 27, barW, 5, SSD1306_WHITE);
    display.display();
    return;
  }

  int prev = (selectedItem - 1 + MENU_COUNT) % MENU_COUNT;
  int next = (selectedItem + 1) % MENU_COUNT;
  char buf[24];

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  formatItem(buf, sizeof(buf), prev, false);
  display.setCursor(0, 1);
  display.print(buf);

  display.fillRect(0, 11, OLED_WIDTH, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  formatItem(buf, sizeof(buf), selectedItem, menuState == MENU_EDIT);
  display.setCursor(2, 13);
  display.print(buf);
  display.setTextColor(SSD1306_WHITE);

  formatItem(buf, sizeof(buf), next, false);
  display.setCursor(0, 23);
  display.print(buf);

  // Mode + save indicator top-right
  display.setCursor(92, 1);
  if (settingsDirty)                    display.print("*SAVE");
  else if (menuState == MENU_EDIT)      display.print(" EDIT");
  else                                  display.print("  NAV");

  display.display();
}

// ── NeoPixel helpers ───────────────────────────────────────────────────────────
int pixelIndex(int col, int row) {
  if (col % 2 == 0) return col * ROWS + row;
  else              return col * ROWS + (ROWS - 1 - row);
}

uint32_t rowColour(int row) {
  float t = (float)row / (float)(ROWS - 1);
  uint8_t r = 0, g = 0;
  if (t < 0.5f) { r = (uint8_t)(t * 2.0f * 255); g = 255; }
  else           { r = 255; g = (uint8_t)((1.0f - (t - 0.5f) * 2.0f) * 255); }
  r = (uint8_t)((uint32_t)r * brightness / 255);
  g = (uint8_t)((uint32_t)g * brightness / 255);
  return strip.Color(r, g, 0);
}

uint32_t peakColour() {
  uint8_t w = (uint8_t)(200UL * brightness / 255);
  return strip.Color(w, w, w);
}

// ── Audio ──────────────────────────────────────────────────────────────────────
void collectSamples() {
  adc_select_input(0);
  for (int i = 0; i < SAMPLE_COUNT; i++) samples[i] = adc_read();
}

float bandEnergy(int band) {
  int binSize = SAMPLE_COUNT / COLS;
  int start   = band * binSize;
  long sumSq  = 0;
  for (int i = start; i < start + binSize; i++) {
    int v = (int)samples[i] - 2048;
    sumSq += (long)v * v;
  }
  return (sqrtf((float)sumSq / binSize) / 2048.0f) * gain;
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Load saved settings from EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadSettings();

  // I2C
  Wire.setSDA(SDA_PIN);
  Wire.setSCL(SCL_PIN);
  Wire.begin();

  // I2C scan
  Serial.println("Scanning I2C...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("  Device at 0x"); Serial.println(addr, HEX);
    }
  }

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED FAIL");
    pinMode(LED_BUILTIN, OUTPUT);
    while (true) {
      digitalWrite(LED_BUILTIN, HIGH); delay(200);
      digitalWrite(LED_BUILTIN, LOW);  delay(200);
    }
  }
  Serial.println("OLED OK");
  display.setRotation(2);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(22, 12);
  display.print("MUSIC VISUALISER");
  display.display();

  // Button
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // ADC
  adc_init();
  adc_gpio_init(AUDIO_PIN);
  adc_select_input(0);

  // NeoPixels
  strip.begin();
  strip.clear();
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, strip.Color(0, 20, 40));
  strip.show();
  delay(500);
  strip.clear();
  strip.show();

  Serial.println("Setup complete.");
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
  pollButton();

  // Auto-idle
  if (menuState != MENU_IDLE && millis() - lastActivityMs > MENU_TIMEOUT_MS)
    menuState = MENU_IDLE;

  // Deferred EEPROM save — 2 s after last change
  if (settingsDirty && millis() - lastChangeMs >= SAVE_DELAY_MS)
    saveSettings();

  // Sample audio
  collectSamples();

  // Update levels
  for (int col = 0; col < COLS; col++) {
    float energy = constrain(bandEnergy(col), 0.0f, 1.0f);
    if (energy > barLevel[col]) barLevel[col] = energy;
    else                        barLevel[col] *= decay;

    if (barLevel[col] >= peakLevel[col]) {
      peakLevel[col] = barLevel[col];
      peakTimer[col] = peakHold;
    } else {
      if (peakTimer[col] > 0) peakTimer[col]--;
      else                    peakLevel[col] *= peakDecay;
    }
  }

  // Render NeoPixels
  strip.clear();
  for (int col = 0; col < COLS; col++) {
    int litRows = (int)(barLevel[col]  * ROWS);
    int peakRow = (int)(peakLevel[col] * (ROWS - 1));
    for (int row = 0; row < litRows; row++)
      strip.setPixelColor(pixelIndex(col, row), rowColour(row));
    if (peakRow >= litRows && peakLevel[col] > 0.05f)
      strip.setPixelColor(pixelIndex(col, peakRow), peakColour());
  }
  strip.show();

  // OLED throttled
  static uint32_t lastOled = 0;
  if (millis() - lastOled > 50) {
    lastOled = millis();
    drawMenu();
  }
}
