#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <ESP32Time.h>

// =========================
// Pin mapping from schematic
// =========================
const int TFT_SCLK = D9;
const int TFT_MOSI = D10;
const int TFT_RST  = D8;
const int TFT_DC   = D4;
const int TFT_CS   = D5;
const int TFT_BL   = D6;

const int BUZZER_PIN = D7; // BZ1+ -> D7, BZ1- -> GND

// Buttons (active LOW)
const int SW1_PIN = D0; // option 1 / back
const int SW2_PIN = D1; // option 2
const int SW3_PIN = D2; // option 3
const int SW4_PIN = D3; // option 4 / alarm list

const uint32_t DEBOUNCE_MS = 25;
const int SNOOZE_MINUTES = 5;

class MyST7789 : public Adafruit_ST7789 {
public:
  MyST7789(int8_t cs, int8_t dc, int8_t mosi, int8_t sclk, int8_t rst)
    : Adafruit_ST7789(cs, dc, mosi, sclk, rst) {}
  void setOffsets(uint8_t col, uint8_t row) {
    _colstart = _colstart2 = col;
    _rowstart = _rowstart2 = row;
  }
};

MyST7789 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
ESP32Time rtc;

enum ScreenState { SCREEN_MAIN = 0, SCREEN_ALARM_LIST = 1, SCREEN_CHALLENGE = 2 };
ScreenState currentScreen = SCREEN_MAIN;

int lastMinute = -1;

// Alarm time
int alarmHour = 6;
int alarmMinute = 0;
bool alarmTriggeredThisMinute = false;

// Challenge state
int qX = 0, qY = 0, qAnswer = 0;
int options[4] = {0, 0, 0, 0};
int correctIndex = 0; // 0..3

// Buzzer timing (non-blocking beep pattern)
bool buzzerEnabled = false;
bool buzzerState = false;
uint32_t buzzerLastToggleMs = 0;
const uint16_t BUZZER_ON_MS = 120;
const uint16_t BUZZER_OFF_MS = 180;

struct ButtonState {
  uint8_t pin;
  bool stableState;
  bool lastRead;
  uint32_t lastChangeMs;
};

ButtonState buttons[4] = {
  {(uint8_t)SW1_PIN, HIGH, HIGH, 0},
  {(uint8_t)SW2_PIN, HIGH, HIGH, 0},
  {(uint8_t)SW3_PIN, HIGH, HIGH, 0},
  {(uint8_t)SW4_PIN, HIGH, HIGH, 0}
};

// ---------------- Lopaka screens ----------------
void drawScreen_3(void) { // Main
  tft.fillScreen(0x0);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(7);
  tft.setTextWrap(false);
  tft.setCursor(41, 14);
  tft.print("12:00");

  tft.fillRect(218, 0, 51, 11, 0x4208);
  tft.setTextColor(0x0);
  tft.setTextSize(1);
  tft.setCursor(220, 2);
  tft.print("--------");

  tft.fillRect(150, 0, 63, 11, 0x4208);
  tft.setCursor(152, 2);
  tft.print("ALARM LIST");
}

void drawScreen_1(void) { // Alarm list
  tft.fillScreen(0x0);
  tft.fillRect(214, 0, 58, 15, 0xFFFF);
  tft.fillRect(145, 0, 58, 15, 0xFFFF);
  tft.fillRect(78, 0, 58, 15, 0xFFFF);
  tft.fillRect(10, 0, 58, 15, 0xFFFF);

  tft.setTextColor(0x0);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setCursor(27, 4);
  tft.print("Back");

  tft.drawRect(56, 29, 173, 39, 0xFFFF);

  tft.setTextColor(0xFFFF);
  tft.setTextSize(4);
  tft.setCursor(84, 34);
  tft.print("06.00");

  tft.setTextSize(1);
  tft.setCursor(83, 20);
  tft.print("The alarm trigger on");
}

void drawScreen_2(void) { // Challenge
  tft.fillScreen(0x0);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(4);
  tft.setTextWrap(false);
  tft.setCursor(24, 19);
  tft.print("x + y = __");

  tft.setTextSize(2);
  tft.setCursor(24, 52);
  tft.print("Answer to snooze!");

  tft.fillRect(213, 0, 58, 15, 0xFFFF);
  tft.fillRect(144, 0, 58, 15, 0xFFFF);
  tft.fillRect(77, 0, 58, 15, 0xFFFF);
  tft.fillRect(9, 0, 58, 15, 0xFFFF);

  tft.setTextColor(0x0);
  tft.setTextSize(1);
  tft.setCursor(236, 4); tft.print("00");
  tft.setCursor(167, 4); tft.print("00");
  tft.setCursor(100, 4); tft.print("00");
  tft.setCursor(32, 4);  tft.print("00");
}

// ---------------- Drawing helpers ----------------
void drawMainTime(int hh, int mm) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
  tft.fillRect(35, 10, 180, 60, 0x0000);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(7);
  tft.setCursor(41, 14);
  tft.print(buf);
}

void drawAlarmListTime() {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d.%02d", alarmHour, alarmMinute);
  tft.fillRect(80, 32, 90, 30, 0x0000);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(4);
  tft.setCursor(84, 34);
  tft.print(buf);
}

void drawChallengeQuestionAndOptions() {
  char eq[20];
  snprintf(eq, sizeof(eq), "%d + %d = __", qX, qY);
  tft.fillRect(24, 19, 240, 30, 0x0000);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(4);
  tft.setCursor(24, 19);
  tft.print(eq);

  // text x-positions for top 4 boxes (SW1..SW4)
  const int tx[4] = {32, 100, 167, 236};

  for (int i = 0; i < 4; i++) {
    char b[4];
    snprintf(b, sizeof(b), "%02d", options[i]);
    tft.setTextColor(0x0000);
    tft.setTextSize(1);
    tft.setCursor(tx[i], 4);
    tft.print(b);
  }
}

// ---------------- Buzzer ----------------
void buzzerOn() {
  buzzerEnabled = true;
}

void buzzerOff() {
  buzzerEnabled = false;
  buzzerState = false;
  digitalWrite(BUZZER_PIN, LOW);
}

void updateBuzzer() {
  if (!buzzerEnabled) return;

  uint32_t now = millis();
  uint16_t interval = buzzerState ? BUZZER_ON_MS : BUZZER_OFF_MS;
  if (now - buzzerLastToggleMs >= interval) {
    buzzerLastToggleMs = now;
    buzzerState = !buzzerState;
    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
  }
}

// ---------------- Challenge logic ----------------
bool existsInArray(int arr[], int n, int v) {
  for (int i = 0; i < n; i++) if (arr[i] == v) return true;
  return false;
}

int makeWrongAnswerNear(int correct) {
  int delta;
  do {
    delta = random(-8, 9); // -8..+8
  } while (delta == 0);
  int w = correct + delta;
  if (w < 0) w = 0;
  if (w > 99) w = 99;
  return w;
}

void generateChallenge() {
  // question range keeps answers in 0..99 two-digit display friendly
  qX = random(1, 20); // 1..19
  qY = random(1, 20); // 1..19
  qAnswer = qX + qY;

  for (int i = 0; i < 4; i++) options[i] = -1;
  correctIndex = random(0, 4);
  options[correctIndex] = qAnswer;

  for (int i = 0; i < 4; i++) {
    if (i == correctIndex) continue;
    int w;
    do {
      w = makeWrongAnswerNear(qAnswer);
    } while (w == qAnswer || existsInArray(options, 4, w));
    options[i] = w;
  }
}

void snoozeAlarm() {
  int total = alarmHour * 60 + alarmMinute + SNOOZE_MINUTES;
  total %= (24 * 60);
  alarmHour = total / 60;
  alarmMinute = total % 60;
}

// ---------------- Screen transitions ----------------
void enterMainScreen() {
  currentScreen = SCREEN_MAIN;
  drawScreen_3();
  int hh = rtc.getHour(true), mm = rtc.getMinute();
  drawMainTime(hh, mm);
  lastMinute = mm;
  buzzerOff();
}

void enterAlarmListScreen() {
  currentScreen = SCREEN_ALARM_LIST;
  drawScreen_1();
  drawAlarmListTime();
  buzzerOff();
}

void enterChallengeScreen() {
  currentScreen = SCREEN_CHALLENGE;
  drawScreen_2();
  generateChallenge();
  drawChallengeQuestionAndOptions();
  buzzerOn();
}

// ---------------- Input ----------------
void onButtonPressed(uint8_t pin) {
  if (currentScreen == SCREEN_MAIN) {
    if (pin == SW4_PIN) { // ALARM LIST
      enterAlarmListScreen();
      return;
    }
  } else if (currentScreen == SCREEN_ALARM_LIST) {
    if (pin == SW1_PIN) { // Back
      enterMainScreen();
      return;
    }
  } else if (currentScreen == SCREEN_CHALLENGE) {
    int picked = -1;
    if (pin == SW1_PIN) picked = 0;
    else if (pin == SW2_PIN) picked = 1;
    else if (pin == SW3_PIN) picked = 2;
    else if (pin == SW4_PIN) picked = 3;

    if (picked >= 0) {
      if (picked == correctIndex) {
        // success: snooze and exit challenge
        snoozeAlarm();
        enterMainScreen();
      } else {
        // wrong: keep alarm active + new random challenge
        generateChallenge();
        drawChallengeQuestionAndOptions();
      }
      return;
    }
  }
}

void updateButtons() {
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    bool reading = digitalRead(buttons[i].pin);
    if (reading != buttons[i].lastRead) {
      buttons[i].lastChangeMs = now;
      buttons[i].lastRead = reading;
    }
    if ((now - buttons[i].lastChangeMs) > DEBOUNCE_MS) {
      if (reading != buttons[i].stableState) {
        buttons[i].stableState = reading;
        if (buttons[i].stableState == LOW) onButtonPressed(buttons[i].pin);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  randomSeed((uint32_t)esp_random());

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW); // active-low backlight

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(SW1_PIN, INPUT_PULLUP);
  pinMode(SW2_PIN, INPUT_PULLUP);
  pinMode(SW3_PIN, INPUT_PULLUP);
  pinMode(SW4_PIN, INPUT_PULLUP);

  tft.init(76, 284);
  tft.setOffsets(82, 18);
  tft.invertDisplay(false);
  tft.setRotation(1);

  rtc.setTime(0, 0, 12, 1, 1, 2026); // sec,min,hour,day,month,year
  enterMainScreen();
}

void loop() {
  updateButtons();
  updateBuzzer();

  int hh = rtc.getHour(true);
  int mm = rtc.getMinute();

  // Trigger challenge at alarm time (e.g., 06:00)
  if (hh == alarmHour && mm == alarmMinute) {
    if (!alarmTriggeredThisMinute && currentScreen != SCREEN_CHALLENGE) {
      alarmTriggeredThisMinute = true;
      enterChallengeScreen();
    }
  } else {
    alarmTriggeredThisMinute = false;
  }

  if (currentScreen == SCREEN_MAIN) {
    if (mm != lastMinute) {
      drawMainTime(hh, mm);
      lastMinute = mm;
    }
  }

  delay(5);
}
