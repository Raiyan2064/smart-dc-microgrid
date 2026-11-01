// Smart DC Microgrid: final firmware
// ESP32 DevKit V1 + INA219 + SSD1306 OLED + Telegram, 6V 4.5Ah SLA battery
// Arduino IDE 2.x, esp32 by Espressif 3.x, ArduinoJson 6.x
// Pins are written as D numbers (same as the board silkscreen).
//
// Loads, shed order (first to last): Motor, Fan, LED
// Priority: Emergency > Fault (I_TRIP) > Overload (I_LIMIT) > Low battery

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Adafruit_INA219.h>
#include <Adafruit_SSD1306.h>
#include <stdarg.h>

// ================= EDIT ONLY THIS BLOCK =================
#include "secrets.h"

// dI of each load from Test 7, in amps (load ON minus all OFF)
const float I_LED = 0.068;
const float I_FAN = 0.14;
const float I_MOT = 0.20;

const float VCAL = 0.990;      // from the voltage calibration step
const float R_PATH = 1.2;      // ohm, wiring + fuse drop. Battery V = measured V + I x R_PATH
const float I_LIMIT_FIXED = 0; // 0 = automatic (recommended)
#define GRID_BY_PIN 1          // 1 = adapter divider on D34, 0 = battery voltage
#define TG_INSECURE 0          // set 1 only if the Telegram certificate fails
// ========================================================

// ---- pins ----
const int P_START = 32, P_STOP = 33, P_EMG = 4;
const int P_BUZ = 14, P_GRID = 34;
const int P_LOAD[3] = {25, 26, 27}; // 0 LED, 1 Fan, 2 Motor
const char *LNAME[3] = {"LED", "Fan", "Motor"};
const float I_EST[3] = {I_LED, I_FAN, I_MOT};

// ---- limits (starting values, tune after testing) ----
const float I_TRIP_DEFAULT = 1.2;                // A, fault if above for 0.1 s
const float V_OFF[3] = {5.85, 6.05, 6.15};       // load OFF below this
const float V_ON[3] = {5.95, 6.13, 6.23};        // load may return above this
const float GRID_ON_V = 6.60, GRID_OFF_V = 6.50; // only if GRID_BY_PIN 0
const unsigned long BLANK_MS = 400;              // ignore current right after a load turns on
const unsigned long OVL_MS = 400;                // overload must last this long
const unsigned long MIN_OFF_MS = 20000;          // a shed load waits this long before returning
const unsigned long STEP_MS = 1500;              // gap between two load turn-ons

// ---- objects ----
Adafruit_INA219 ina;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);
bool oledOK = false;

struct Msg
{
  char t[200];
};
QueueHandle_t qMsg;

// ---- shared with the Telegram task ----
volatile bool simOn = false, cmdReset = false, cmdStart = false, cmdStop = false;
volatile float simV = 6.30, tripTest = 0;
volatile float sV = 0, sI = 0, sLimit = 0;
volatile int sPct = 0, sState = 0; // state: 0 RUN, 1 STOP, 2 FAULT, 3 EMERGENCY
volatile bool sGrid = false, sWifi = false;
volatile int sMask = 0;
volatile unsigned long sBusyUntil = 0; // WiFi/TLS bursts raise the current for a moment

// ---- control state ----
bool on[3] = {false, false, false};
bool want[3] = {true, true, false}; // boot: LED and Fan requested, Motor not
bool shed[3] = {false, false, false};
unsigned long tChange[3] = {0, 0, 0};
bool emergency = false, fault = false, running = true;
bool gridOn = false, gridCand = false, ovl = false;
float iBase = 0, iLimit = 1, iAvg = 0, vAvg = 6.3;
unsigned long lastOnEvent = 0, lastStep = 0, ovlStart = 0, buzzUntil = 0;
unsigned long lastOvlMsg[3] = {0, 0, 0};
int tripCount = 0;

struct Btn
{
  int pin;
  bool stable;
  bool last;
  unsigned long t;
};
Btn bStart = {P_START, false, false, 0};
Btn bStop = {P_STOP, false, false, 0};
Btn bEmg = {P_EMG, false, false, 0};

// ---------------- helpers ----------------
void notify(const char *fmt, ...)
{
  Msg m;
  va_list a;
  va_start(a, fmt);
  vsnprintf(m.t, sizeof(m.t), fmt, a);
  va_end(a);
  if (uxQueueSpacesAvailable(qMsg) == 0)
  { // queue full: drop the oldest
    Msg d;
    xQueueReceive(qMsg, &d, 0);
  }
  xQueueSend(qMsg, &m, 0);
}

float readI() { return ina.getCurrent_mA() / 1000.0f; }
float readV()
{
  return (ina.getBusVoltage_V() + ina.getShuntVoltage_mV() / 1000.0f) * VCAL;
}

int battPct(float v)
{
  static const float VP[6] = {5.85, 5.95, 6.05, 6.15, 6.25, 6.35};
  if (v <= VP[0])
    return 0;
  if (v >= VP[5])
    return 100;
  for (int i = 0; i < 5; i++)
  {
    if (v < VP[i + 1])
      return (int)(20 * i + 20 * (v - VP[i]) / (VP[i + 1] - VP[i]));
  }
  return 100;
}

void setLoad(int i, bool s)
{
  if (on[i] == s)
    return;
  on[i] = s;
  tChange[i] = millis();
  if (s)
    lastOnEvent = millis();
  digitalWrite(P_LOAD[i], s ? HIGH : LOW);
}

void shedLoad(int i)
{
  shed[i] = true;
  setLoad(i, false);
}

void allOffLatched()
{ // emergency or fault: remember what was on
  for (int i = 0; i < 3; i++)
  {
    if (on[i])
      shed[i] = true;
    setLoad(i, false);
  }
}

int highestOn()
{
  for (int i = 2; i >= 0; i--)
    if (on[i])
      return i;
  return -1;
}

void maskText(char *out, size_t n)
{
  snprintf(out, n, "LED %s, Fan %s, Motor %s",
           on[0] ? "ON" : "OFF", on[1] ? "ON" : "OFF", on[2] ? "ON" : "OFF");
}

void beep(unsigned long ms) { buzzUntil = millis() + ms; }

void updateBuzzer()
{
  bool b;
  if (emergency || fault)
    b = ((millis() / 250) % 2) == 0;
  else
    b = (long)(buzzUntil - millis()) > 0;
  digitalWrite(P_BUZ, b ? HIGH : LOW);
}

bool readBtn(Btn &b, bool &edge)
{ // debounced, edge = just pressed
  bool raw = (digitalRead(b.pin) == LOW);
  edge = false;
  if (raw != b.last)
  {
    b.last = raw;
    b.t = millis();
  }
  if (millis() - b.t > 30 && raw != b.stable)
  {
    b.stable = raw;
    edge = raw;
  }
  return b.stable;
}

bool gridSense(float v)
{
  static bool st = false, init = false;
#if GRID_BY_PIN
  int r = analogRead(P_GRID);
  if (!init)
  {
    st = r > 1500;
    init = true;
  }
  if (r > 1500)
    st = true;
  else if (r < 1000)
    st = false;
#else
  if (!init)
  {
    st = v > GRID_ON_V;
    init = true;
  }
  if (v > GRID_ON_V)
    st = true;
  else if (v < GRID_OFF_V)
    st = false;
#endif
  return st;
}

void drawOled(float v, float i, int pct)
{
  if (!oledOK)
    return;
  static const char *names[4] = {"RUN", "STOP", "FAULT", "EMERGENCY"};
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2); // big: battery V and %, current and source
  oled.setCursor(0, 0);
  oled.printf("%.2fV %3d%%\n", v, pct);
  oled.printf("%.3fA %s\n", i, simOn ? "SIM" : (gridOn ? "CHG" : "BAT"));
  oled.setTextSize(1); // small: loads, state, WiFi
  oled.setCursor(0, 40);
  oled.printf("LED %s Fan %s Mot %s\n", on[0] ? "ON" : "--", on[1] ? "ON" : "--", on[2] ? "ON" : "--");
  oled.printf("%s  WiFi %s\n", names[sState], sWifi ? "OK" : "--");
  oled.display();
}

void showText(const char *a, const char *b)
{
  if (!oledOK)
    return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println(a);
  oled.println(b);
  oled.display();
}

// ---------------- Telegram task (core 0) ----------------
void netBusy() { sBusyUntil = millis() + 1500; }     // call before a network action
void netDone() { sBusyUntil = millis() + 600; }      // call after it
void netBusyLong() { sBusyUntil = millis() + 9000; } // sendMessage may retry for up to 8 s

void handleCmd(const String &text)
{
  char r[200];
  static const char *names[4] = {"RUN", "STOP", "FAULT", "EMERGENCY"};
  if (text == "/status")
  {
    snprintf(r, sizeof(r),
             "State %s. Grid %s%s. Battery %.2f V (%d%%). Current %.3f A (limit %.2f A). "
             "LED %s, Fan %s, Motor %s.",
             names[sState], sGrid ? "ON" : "OFF", simOn ? " (SIM)" : "",
             (double)sV, (int)sPct, (double)sI, (double)sLimit,
             (sMask & 1) ? "ON" : "OFF", (sMask & 2) ? "ON" : "OFF", (sMask & 4) ? "ON" : "OFF");
  }
  else if (text == "/reset")
  {
    cmdReset = true;
    snprintf(r, sizeof(r), "Reset requested.");
  }
  else if (text == "/start")
  {
    cmdStart = true;
    snprintf(r, sizeof(r), "Start requested (all loads, Motor included).");
  }
  else if (text == "/stop")
  {
    cmdStop = true;
    snprintf(r, sizeof(r), "Stop requested. All loads OFF.");
  }
  else if (text.startsWith("/sim"))
  {
    String a = text.substring(4);
    a.trim();
    if (a == "off")
    {
      simOn = false;
      snprintf(r, sizeof(r), "SIM off. Real battery reading in use.");
    }
    else
    {
      float v = a.toFloat();
      if (v >= 4.0 && v <= 7.5)
      {
        simV = v;
        simOn = true;
        snprintf(r, sizeof(r), "SIM battery %.2f V (test mode).", (double)v);
      }
      else
      {
        snprintf(r, sizeof(r), "Usage: /sim 6.00 or /sim off");
      }
    }
  }
  else if (text.startsWith("/trip"))
  {
    String a = text.substring(5);
    a.trim();
    if (a == "off")
    {
      tripTest = 0;
      snprintf(r, sizeof(r), "Trip limit back to default.");
    }
    else
    {
      float v = a.toFloat();
      if (v >= 0.2 && v <= 2.0)
      {
        tripTest = v;
        snprintf(r, sizeof(r), "Test trip limit %.2f A. Use /trip off to restore.", (double)v);
      }
      else
      {
        snprintf(r, sizeof(r), "Usage: /trip 0.30 or /trip off");
      }
    }
  }
  else
  {
    snprintf(r, sizeof(r), "Commands: /status /start /stop /reset /sim 6.00 /sim off /trip 0.30 /trip off");
  }
  netBusyLong();
  bot.sendMessage(CHAT_ID, r, "");
  netDone();
}

void tgTask(void *)
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#if TG_INSECURE
  client.setInsecure();
#else
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
#endif
  unsigned long lastPoll = 0, lastTry = millis();
  bool flushed = false, havePend = false, wifiShown = false;
  int sendFail = 0;
  Msg pend;
  for (;;)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      sWifi = false; // no netBusy() here: overload protection must keep working without WiFi
      if (millis() - lastTry > 15000)
      {
        lastTry = millis();
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    sWifi = true;
    if (!wifiShown)
    {
      wifiShown = true;
      Serial.printf("WiFi OK, IP %s\n", WiFi.localIP().toString().c_str());
    }
    if (!flushed)
    { // ignore commands that were sent while the board was off
      flushed = true;
      netBusy();
      int n = bot.getUpdates(bot.last_message_received + 1);
      while (n > 0)
        n = bot.getUpdates(bot.last_message_received + 1);
      netDone();
    }
    if (!havePend && xQueueReceive(qMsg, &pend, 0) == pdTRUE)
      havePend = true;
    if (havePend)
    {
      netBusyLong();
      bool ok = bot.sendMessage(CHAT_ID, pend.t, "");
      netDone();
      Serial.printf("Telegram send %s\n", ok ? "OK" : "FAILED");
      if (ok)
      {
        havePend = false;
        sendFail = 0;
      }
      else if (++sendFail >= 3)
      { // do not retry forever, check CHAT_ID and token
        havePend = false;
        sendFail = 0;
        Serial.println("Telegram: message dropped after 3 tries. Check CHAT_ID and BOT_TOKEN.");
      }
      else
      {
        vTaskDelay(pdMS_TO_TICKS(3000));
      }
    }
    if (millis() - lastPoll > 5000)
    {
      lastPoll = millis();
      netBusy();
      int n = bot.getUpdates(bot.last_message_received + 1);
      netDone();
      while (n > 0)
      {
        for (int i = 0; i < n; i++)
        {
          if (bot.messages[i].chat_id == String(CHAT_ID))
            handleCmd(bot.messages[i].text);
        }
        netBusy();
        n = bot.getUpdates(bot.last_message_received + 1);
        netDone();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------------- setup ----------------
void setup()
{
  pinMode(P_BUZ, OUTPUT);
  digitalWrite(P_BUZ, LOW);
  for (int i = 0; i < 3; i++)
  {
    pinMode(P_LOAD[i], OUTPUT);
    digitalWrite(P_LOAD[i], LOW);
  }
  pinMode(P_START, INPUT_PULLUP);
  pinMode(P_STOP, INPUT_PULLUP);
  pinMode(P_EMG, INPUT_PULLUP);

  Serial.begin(115200);
  Wire.begin(21, 22);
  if (!ina.begin())
  {
    Serial.println("INA219 not found. Loads stay OFF.");
    while (true)
      delay(1000);
  }
  ina.setCalibration_32V_2A();
  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOK)
    Serial.println("OLED not found, continuing without it");

  qMsg = xQueueCreate(8, sizeof(Msg));
  // The Telegram library waits in busy loops. Priority 0 is the same as the IDLE task of core 0,
  // so they take turns, the watchdog is fed, and a slow network cannot reboot the board.
  xTaskCreatePinnedToCore(tgTask, "tg", 16384, NULL, 0, NULL, 0);

  // Measure the idle current (ESP32 + WiFi + OLED, all loads OFF) to set I_LIMIT.
  showText("Starting...", "waiting for WiFi");
  unsigned long t0 = millis();
  while (!sWifi && millis() - t0 < 20000)
    delay(100);
  delay(2000);
  showText("Measuring idle", "current, 3 s");
  float sum = 0;
  int n = 0;
  t0 = millis();
  while (n < 150 && millis() - t0 < 15000)
  { // 150 quiet samples = 3 s
    if ((long)(sBusyUntil - millis()) <= 0)
    { // skip WiFi/TLS bursts
      sum += readI();
      n++;
    }
    delay(20);
  }
  iBase = (n > 0) ? sum / n : readI();
  iLimit = (I_LIMIT_FIXED > 0) ? I_LIMIT_FIXED : iBase + I_LED + I_FAN + 0.5f * I_MOT;
  iAvg = iBase;
  vAvg = readV() + iBase * R_PATH;
  gridOn = gridSense(vAvg);
  gridCand = gridOn;
  for (int i = 0; i < 3; i++)
    tChange[i] = millis() - MIN_OFF_MS;

  Serial.printf("Idle %.3f A, I_LIMIT %.3f A, V %.2f, grid %d\n", iBase, iLimit, vAvg, gridOn);
  if (I_MOT < 0.15)
    Serial.println("WARNING: I_MOT is small, overload margin is thin");
  notify("System online. Grid %s. Battery %.2f V (%d%%). Idle %.2f A, I_LIMIT %.2f A.",
         gridOn ? "ON" : "OFF", vAvg, battPct(vAvg), iBase, iLimit);
}

// ---------------- main loop ----------------
void loop()
{
  static unsigned long tick = 0, tShow = 0, tGrid = 0;
  unsigned long now = millis();
  if (now - tick < 20)
  {
    delay(1);
    return;
  }
  tick = now;

  float iRaw = readI();
  float vMeas = readV();
  // INA219 recovery: a glitch on the I2C bus can leave it reading exactly 0 A and 0 V,
  // or wipe its calibration. Re-initialise it, and refresh the calibration every 2 s.
  static int zeroCnt = 0;
  static unsigned long tCal = 0, tRec = 0;
  if (iRaw == 0.0f && vMeas == 0.0f)
    zeroCnt++;
  else
    zeroCnt = 0;
  if (zeroCnt >= 10 && now - tRec >= 2000)
  { // 0.2 s of nothing
    tRec = now;
    zeroCnt = 0;
    Wire.end();
    Wire.begin(21, 22);
    ina.begin();
    Serial.println("INA219 re-initialised");
  }
  if (now - tCal >= 2000)
  {
    tCal = now;
    ina.setCalibration_32V_2A();
  }
  if (iRaw < 0)
    iRaw = 0; // the load never feeds current back, so this is noise
  if (vMeas < 4.0f || vMeas > 7.6f)
    vMeas = vAvg;                     // motor noise can corrupt one reading
  iAvg += 0.2f * (iRaw - iAvg);       // about 0.1 s average
  float vRaw = vMeas + iAvg * R_PATH; // add back the voltage lost in wires and fuse
  vAvg += 0.004f * (vRaw - vAvg);     // about 5 s average
  float vUse = simOn ? simV : vAvg;
  int pct = battPct(vUse);

  // buttons and phone commands
  bool eS, eT, eE;
  bool emgNow = readBtn(bEmg, eE);
  readBtn(bStart, eS);
  readBtn(bStop, eT);
  bool startEdge = eS, stopEdge = eT;
  if (cmdStart)
  {
    cmdStart = false;
    startEdge = true;
  }
  if (cmdStop)
  {
    cmdStop = false;
    stopEdge = true;
  }
  bool resetNow = cmdReset;
  cmdReset = false;

  // grid detection with a 1.5 s debounce
  bool g = gridSense(vAvg);
  if (g != gridCand)
  {
    gridCand = g;
    tGrid = now;
  }
  if (gridCand != gridOn && now - tGrid > 1500)
  {
    gridOn = gridCand;
    if (gridOn)
      notify("[GRID BACK] Charger connected. Battery %.2f V (%d%%).", vAvg, battPct(vAvg));
    else
      notify("[GRID LOST] Running on battery. Battery %.2f V (%d%%).", vAvg, battPct(vAvg));
  }

  // 1. emergency (highest priority)
  if (emgNow && !emergency)
  {
    emergency = true;
    allOffLatched();
    char mt[60];
    maskText(mt, sizeof(mt));
    notify("[EMERGENCY] Button pressed. ALL loads OFF. Loads now: %s. Send /reset or press START.", mt);
  }

  // reset and START clear the latches (not while the EMERGENCY button is held)
  if ((resetNow || startEdge) && !emgNow && (emergency || fault))
  {
    emergency = false;
    fault = false;
    tripCount = 0;
    notify("[RESET] Latch cleared. Loads return one by one.");
  }
  if (startEdge && !emgNow)
  {
    running = true;
    for (int i = 0; i < 3; i++)
    {
      want[i] = true;
      shed[i] = false;
    }
  }
  if (stopEdge)
  {
    running = false;
    for (int i = 0; i < 3; i++)
    {
      want[i] = false;
      shed[i] = false;
      setLoad(i, false);
    }
  }

  // 2. fault: short circuit or stalled motor
  float trip = (tripTest > 0) ? tripTest : I_TRIP_DEFAULT;
  bool blank = (now - lastOnEvent) < BLANK_MS;
  if (!emergency && !fault)
  {
    if (!blank && iRaw > trip)
      tripCount++;
    else
      tripCount = 0;
    if (tripCount >= 5)
    { // 5 samples = 0.1 s
      fault = true;
      allOffLatched();
      notify("[FAULT] Overcurrent %.2f A > %.2f A. ALL loads OFF. Send /reset.", iRaw, trip);
    }
  }

  // 3. overload: shed the lowest priority load that is on
  //    (not judged during a WiFi/TLS burst, the burst alone can add 0.1 A)
  bool netBurst = (long)(sBusyUntil - now) > 0;
  float limEff = iLimit + (sWifi ? 0.0f : 0.05f); // with no WiFi the radio keeps scanning, a little more current
  if (!emergency && !fault)
  {
    if (highestOn() >= 0 && !blank && !netBurst && iAvg > limEff)
    {
      if (!ovl)
      {
        ovl = true;
        ovlStart = now;
      }
      else if (now - ovlStart >= OVL_MS)
      {
        int k = highestOn();
        float iNow = iAvg;
        shedLoad(k);
        beep(1000);
        if (lastOvlMsg[k] == 0 || now - lastOvlMsg[k] > 30000)
        {
          lastOvlMsg[k] = now;
          char mt[60];
          maskText(mt, sizeof(mt));
          notify("[OVERLOAD] %.2f A > %.2f A limit. %s OFF. Loads now: %s. Battery %.2f V (%d%%).",
                 iNow, iLimit, LNAME[k], mt, vUse, pct);
        }
        ovlStart = now;
      }
    }
    else
    {
      ovl = false;
    }
  }

  // 4. low battery (when not on the charger, or in /sim test mode)
  bool battMode = (!gridOn || simOn);
  if (battMode && !emergency && !fault)
  {
    for (int i = 2; i >= 0; i--)
    {
      if (on[i] && vUse < V_OFF[i])
      {
        shedLoad(i);
        beep(500);
        char mt[60];
        maskText(mt, sizeof(mt));
        notify("[LOW BATTERY] %.2f V (%d%%). %s OFF. Loads now: %s.", vUse, pct, LNAME[i], mt);
      }
    }
  }

  // 5. turn on / restore, one load at a time, LED first, Motor last
  if (running && !emergency && !fault && now - lastStep >= STEP_MS)
  {
    for (int i = 0; i < 3; i++)
    {
      if (!want[i] || on[i])
        continue;
      bool batOk = !battMode || vUse >= V_ON[i];
      if (!shed[i])
      { // first start or manual request
        if (batOk)
        {
          setLoad(i, true);
          lastStep = now;
        }
      }
      else if (now - tChange[i] >= MIN_OFF_MS && batOk &&
               iAvg + I_EST[i] < iLimit - 0.05f)
      { // restore after a shed
        setLoad(i, true);
        shed[i] = false;
        lastStep = now;
        char mt[60];
        maskText(mt, sizeof(mt));
        notify("[RESTORED] %s ON. Loads now: %s. Battery %.2f V (%d%%).", LNAME[i], mt, vUse, pct);
      }
      break; // keep the order: do not skip a waiting higher priority load
    }
  }

  updateBuzzer();

  // status for OLED, Serial and Telegram, twice a second
  if (now - tShow >= 500)
  {
    tShow = now;
    sV = vUse;
    sI = iAvg;
    sLimit = iLimit;
    sPct = pct;
    sGrid = gridOn;
    sMask = (on[0] ? 1 : 0) | (on[1] ? 2 : 0) | (on[2] ? 4 : 0);
    sState = emergency ? 3 : (fault ? 2 : (running ? 0 : 1));
    drawOled(vUse, iAvg, pct);
    Serial.printf("V=%.2f I=%.3f lim=%.2f [L=%d F=%d M=%d] grid=%d state=%d%s\n",
                  vUse, iAvg, iLimit, on[0], on[1], on[2], gridOn, sState, simOn ? " SIM" : "");
  }
}
