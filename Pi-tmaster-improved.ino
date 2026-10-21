// Pi-Tmaster-Improved
// 
// What it does:
//    - reads up to 4 NTC thermistor probes through an ADS1115
//    - connects to a WiFi network
//    - show the devices local IP address and probe temperatures on the OLED
//    - serves a web page (mobile friendly) with live temps and a history graph
//    - records temperature history from power-on (up to 24 hours)
//    - save temperature history to flash so it survives a reboot
//
// Arduino IDE setup:
//    Board:        Raspberry Pi Pico 2W (or Pico W), arduino-pico core
//    Flash Size:   Choose an option WITH a filesystem, e.g. "2MB (Sketch 1MB, FS 1MB)"
//                    (Tools -> Flash Size). Without it the history won't be saved.
//    Libraries:    "Adafruit ADS1X15" and "GyverOLED"
//    Files:        Keep page.h and secret.h in the same directory as this sketch
//
//    Wiring:       I2C0 SDA = GP4, SCL = GP5 (ADS1115 at 0x48 and OLED at 0x3C share the bus)
//                  Each probe: 3V3 (Pin 5) -> 10K resistor -> Midpoint -> probe -> GND,
//                  Midpoints connect to ADS1115 inputs A0..A3

#include <WiFi.h>
#include <GyverOLED.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "page.h"
#include "secret.h"


// --------------- Settings---------------
#define WIFI_PORT 80
#define NUM_PROBES 4
#define OLED_LINE_HEIGHT 14

// Shared I2C clock and data lines for the OLED and ADS
const int COMMON_SDA_PIN = 4; //gpio 4
const int COMMON_SCL_PIN = 5; //gpio 5

const float R_FIXED = 10000.0;        // ohms - the fixed divider resistor for the probes
const float VREF = 3.28;              // the actual voltage as measured on the 3v3 rail
const int SAMPLES_PER_READING = 8;    // use averaging to cut resistor noise


// Steinhart-Hart constants, original values from TVWBB thread:
// https://tvwbb.com/threads/igrill-probes-with-hm.43949/
// and adjusted with actual measurements from probes and
// entered in calculator here:
// https://www.thinksrs.com/downloads/programs/Therm%20Calc/NTCCalibrator/NTCcalculator.htm
const double SH_A = 0.7739251279e-3;
const double SH_B = 2.088025997e-4;
const double SH_C = 1.154400438e-7;

// Min and Max values for temperature, values outside this range are 
// considered invalid and treated as "no probe connected"
const float MIN_VALID_F = -20.0;
const float MAX_VALID_F = 600.0;

#define READ_INTERVAL_MS    2000      // how often to read probes / refresh OLED
#define HIST_INTERVAL_S     15        // create history point every 15 seconds
#define HIST_CAP            5760      // keep history for 24 hrs (24 * 60 * 4) - about 69KB of Ram
#define HIST_MAX_POINTS     400       // the most points ever sent to phone on graph
#define FLUSH_INTERVAL_MS   120000UL  // save new history to flash every 2 minutes
#define MIN_VALID_EPOCH     1700000000UL  // anything ealier means the clock isn't set yet
#define HIST_FILE           "/history.bin"
#define MISSING             INT16_MIN // marks a probe that wasn't connected

// Set this to 1 to print each probe channel's raw numbers to the Serial Monitor (115200 baud)
// every read. Useful for checking wiring and calibration
#define DEBUG_PROBES 1

// --------------- Globals ---------------
GyverOLED<SSH1106_128x64, OLED_BUFFER> oled; 
Adafruit_ADS1115 ads;
WebServer server(WIFI_PORT);

bool probeConnected[NUM_PROBES];
float probeTempF[NUM_PROBES];
float probeVolts[NUM_PROBES];   // for debugging
double probeOhms[NUM_PROBES];   // for debugging (-1 = not computed)

// One history record: UTC time plus each probe in 1/10ths of a degree F
struct Sample {
  uint32_t t;
  int16_t  v[NUM_PROBES];
};

Sample   hist[HIST_CAP];
uint16_t histCount = 0;           // samples currently held (max of HIST_CAP)
uint16_t histHead = 0;            // next write position in the ring
uint16_t pendingFlush = 0;        // samples not yet saved to flash
uint32_t fileRecords = 0;         // records currently in the flash file
uint32_t lastHistT = 0;
bool     fsOk = false;

unsigned long lastRead = 0;
unsigned long lastFlush = 0;
unsigned long lastWifiRetry = 0;

// A place to store the pico's IP address for display
String my_ip_address = "no Wifi";
bool networkStarted = false;    // true once Wifi is up and web server is running


// --------------- Probes ---------------
bool readProbeF(int ch, float &tempF) {
  long sum = 0;
  for (int i = 0; i < SAMPLES_PER_READING; i++) {
    sum += ads.readADC_SingleEnded(ch);
  }
  int16_t avgRaw = (int16_t)(sum / SAMPLES_PER_READING);
  float volts = ads.computeVolts(avgRaw);
  probeVolts[ch] = volts;
  probeOhms[ch] = -1;

  // Unplugged probe floats to ~3.3V, shorted pulls to ~0V - neither is a reading
  if (volts <= 0.001 || volts >= VREF - 0.005) {
    return false;
  }

  double r = R_FIXED * volts / (VREF - volts);    // thermistor on the ground side
  probeOhms[ch] = r;
  double lnR = log(r);
  double kelvin = 1.0 / (SH_A + SH_B * lnR + SH_C * lnR * lnR * lnR);
  tempF = (float)((kelvin - 273.15) * 9.0 / 5.0 + 32.0);
  
  //written so that NaN fails the test and is treated as "not connected"
  return (tempF >= MIN_VALID_F && tempF <= MAX_VALID_F);
}

void readAllProbes() {
  for (int i = 0; i < NUM_PROBES; i++) {
    probeConnected[i] = readProbeF(i, probeTempF[i]);
  }
#if DEBUG_PROBES
  for (int i = 0; i < NUM_PROBES; i++) {
    Serial.print("A");
    Serial.print(i);
    Serial.print(": ");
    Serial.print(probeVolts[i], 3);
    Serial.print(" V, ");
    Serial.print(probeOhms[i], 0);
    Serial.print(" ohm, ");
    if (probeConnected[i]) {
      Serial.print(probeTempF[i], 1);
      Serial.println(" F");
    } else {
      Serial.println("not connected");
    }
  }
  Serial.println("---");
#endif
}


// --------------- OLED ---------------
void showMessage(const String &line1, const String &line2 = "") {
  oled.clear();
  oled.setCursorXY(0, 0);
  oled.print(line1);
  if (line2.length() > 0) {
    oled.setCursorXY(0, OLED_LINE_HEIGHT);
    oled.print(line2);
  }
  oled.update();
}

void updateOled() {
  oled.clear();
  String message = "IP:" + my_ip_address;
  oled.setCursorXY(0, 0);
  oled.print(message);
  for (int i = 0; i < NUM_PROBES; i++) {
    if (!probeConnected[i]) {
      message = String("Probe ") + (i + 1) + ": N/A";
    } else {
      message = String("Probe ") + (i + 1) + ": " + String(probeTempF[i], 1) + "F";
    }
    oled.setCursorXY(10, OLED_LINE_HEIGHT * (i + 1));
    oled.print(message);
  }
  oled.update();
}

// --------------- Clock ---------------
bool clockSynced() {
  return (unsigned long)time(nullptr) > MIN_VALID_EPOCH;
}

// --------------- History (RAM ring + flash file) ---------------
int ringIndex(int k) {    // k = 0 is the oldest held sample
  int oldest = (histHead + HIST_CAP - histCount) % HIST_CAP;
  return (oldest + k) % HIST_CAP;
}

// Seconds on whichever clock we have: real time once NTP has set it, otherwise
// seconds since power-on. Samples recorded before the clock is set are converted
// to real time at the moment it arrives (see rebaseHistoryIfClockJustSet).
uint32_t nowSec() {
  return clockSynced() ? (uint32_t)time(nullptr) : (uint32_t)(millis() / 1000);
}

bool wasSynched = false;

void rebaseHistoryIfClockJustSet() {
  if (wasSynched || !clockSynced()) {
    return;
  }

  wasSynched = true;
  uint32_t offset = (uint32_t)time(nullptr) - (uint32_t)(millis() / 1000);
  for (int k = 0; k < histCount; k++) {
    Sample &smp = hist[ringIndex(k)];
    if (smp.t < MIN_VALID_EPOCH) {
      smp.t += offset;    // convert from seconds-since-boot
    }
  }
  if (lastHistT < MIN_VALID_EPOCH) {
    lastHistT += offset;
  }
}

void histPush(const Sample &s, bool countAsPending) {
  hist[histHead] = s;
  histHead = (histHead + 1) % HIST_CAP;
  if (histCount < HIST_CAP) {
    histCount++;
  }
  if (countAsPending && pendingFlush < HIST_CAP) {
    pendingFlush++;
  }
}

// rewrite the flash file so it holds only what's in the ring
void histCompact() {
  File f = LittleFS.open(HIST_FILE, "w");
  if (!f) {
    return;
  }

  uint32_t written = 0;
  for (int k = 0; k < histCount; k++) {
    const Sample &smp = hist[ringIndex(k)];
    if (smp.t < MIN_VALID_EPOCH) {
      continue;     // seconds-since-boot values mean nothing after a restart
    }
    f.write((const uint8_t*)&smp, sizeof(Sample));
    written++;
  }
  f.close();
  fileRecords = written;
  pendingFlush = 0;
}

void histLoad() {
  File f = LittleFS.open(HIST_FILE, "r");
  if (!f) {
    return;
  }

  uint32_t n = f.size() / sizeof(Sample);   // ignore a partial record at the end
  uint32_t skip = (n > HIST_CAP) ? (n - HIST_CAP) : 0;
  f.seek(skip * sizeof(Sample));
  Sample s;
  for (uint32_t i = skip; i < n; i++) {
    if (f.read((uint8_t*)&s, sizeof(Sample)) != sizeof(Sample)) {
      break;
    }
    histPush(s, false);
  }
  f.close();
  fileRecords = n;
  if (fileRecords > 2UL * HIST_CAP) {
    histCompact();  // keep the file from growing forever
  }
}

void histFlush() {
  if (!fsOk || pendingFlush == 0 || !clockSynced()) {
    return;   // wait for a real timestamp
  }

  // The clock may have been set since the last reading tick; make sure every waiting
  // sample has a real timestamp before anything is written to flash
  rebaseHistoryIfClockJustSet();

  if (fileRecords + pendingFlush > 2UL * HIST_CAP) {
    histCompact();
    return;
  }

  File f = LittleFS.open(HIST_FILE, "a");
  if (!f) {
    return;
  }

  uint16_t n = (pendingFlush < histCount) ? pendingFlush : histCount;
  for (int k = histCount - n; k < histCount; k++) {
    f.write((const uint8_t*)&hist[ringIndex(k)], sizeof(Sample));
  }
  f.close();
  fileRecords += n;
  pendingFlush = 0;
}

// Record a point every HIST_INTERVAL_S, whether or not the clock
// is set, and only if at least one probe is connected
void sampleHistory() {
  uint32_t t = nowSec();
  if (t - lastHistT < HIST_INTERVAL_S) {
    return;
  }

  Sample s;
  s.t = t;
  bool any = false;
  for (int i = 0; i < NUM_PROBES; i++) {
    if (probeConnected[i] && probeTempF[i] > -100.0f && probeTempF[i] < 1000.0f) {
      s.v[i] = (int16_t)lroundf(probeTempF[i] * 10.0f);
      any = true;
    } else {
      s.v[i] = MISSING;
    }
  }

  if (!any) {
    return;
  }
  lastHistT = t;
  histPush(s, true);
}


// -------------- Web handlers ---------------

// Append a value given in tenths of a degree (824 -> "82.4"). If anything goes
// wrong, write "null" instead, so the JSON can never end up with a blank entry.
void appendTenths(String &s, int v) {
  if (v > 99999) {
    v = 99999;
  }
  if (v < -99999) {
    v = -99999;
  }

  char buf[16];
  int len = snprintf(buf, sizeof(buf), "%s%d.%d", (v < 0) ? "-" : "", abs(v) / 10, abs(v) %10);
  size_t before = s.length();
  if (len > 0 && len < (int)sizeof(buf)) {
    s += buf;
  }
  if (s.length() == before) {
    s += "null";
  }

}

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleCurrent() {
  String s;
  s.reserve(220);
  s += "{\"up\":";
  s += (unsigned long)millis();
  s += ",\"t\":";
  s += (unsigned long)(clockSynced() ? time(nullptr) : 0);
  s += ",\"synced\":";
  
  s += clockSynced() ? "true" : "false";
  s += ",\"fs\":";
  s += fsOk ? "true" : "false";
  s += ",\"saved\":";
  s += (unsigned long)fileRecords;
  s += ",\"ring\":";
  s += (unsigned long)histCount;
  s += ",\"p\":[";
  for (int i = 0; i < NUM_PROBES; i++) {
    if (i) {
      s += ',';
    }
    if (probeConnected[i] && probeTempF[i] > -100.0f && probeTempF[i] < 1000.0f) {
      appendTenths(s, (int)lroundf(probeTempF[i] * 10.0f));
    } else {
      s += "null";
    }
  }
  s += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

void handleHistory() {
  long w = server.hasArg("window") ? server.arg("window").toInt() : 7200;
  if (w < 60) {
    w = 60;
  }
  if (w > 172800L) {
    w = 172800L;
  }

  bool synced = clockSynced();
  uint32_t nowT = nowSec();     // real time if the clock is set, otherwise seconds since power-on
  uint32_t cutoff = (nowT > (uint32_t)w) ? nowT - (uint32_t)w : 0;


  // first sample inside the requested window. Records saved before the last
  // restart carry real times, so until the clock is set again they can't be placed
  // and are skipped (the always come first in the ring)
  int first = 0;
  while (first < histCount ) {
    uint32_t ts = hist[ringIndex(first)].t;
    bool otherTimebase = !synced && ts >= MIN_VALID_EPOCH;
    if (ts < cutoff || otherTimebase) {
      first++;
    } else {
      break;
    }
  }
  int count = histCount - first;

  // thin the data so the phone never gets more than HIST_MAX_POINTS
  int stride = (count > HIST_MAX_POINTS) ? (count + HIST_MAX_POINTS - 1) / HIST_MAX_POINTS : 1;

  // walk backwards from the newest sample so it's always included
  static uint16_t idx[HIST_MAX_POINTS + 2];
  int n = 0;
  for (int k = histCount - 1; k >= first && n <= HIST_MAX_POINTS; k -= stride) {
    idx[n++] = (uint16_t)k;
  }

  String s;
  s.reserve((size_t)n * 44 + 160);
  s += "{\"now\":";
  s += (unsigned long)nowT;
  s += ",\"step\":";
  s += (unsigned long)(stride * HIST_INTERVAL_S);
  s += ",\"t\":[";
  for (int j = n - 1; j >= 0; j--) {
    s += (unsigned long)hist[ringIndex(idx[j])].t;
    if (j) {
      s += ',';
    }
  }
  s += "],\"p\":[";
  for (int p = 0; p < NUM_PROBES; p++) {
    if (p) {
      s += ',';
    }
    s += '[';
    for (int j = n -1; j >= 0; j--) {
      int16_t v = hist[ringIndex(idx[j])].v[p];
      if (v == MISSING) {
        s += "null";
      } else {
        appendTenths(s, v);
      }
      if (j) {
          s += ',';
        }
    }
    s += ']';
  }
  s += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}


// --------------- Setup / loop ---------------

// Everything that needs WiFi. Runs once, the first time WiFi is connected - at startup if
// the network is there, or later in the loop() if it was not.
void startNetwork() {
  my_ip_address = WiFi.localIP().toString();
  Serial.printf("\nConnected to Wifi\n\nIP: %s:%d\n", my_ip_address.c_str(), WIFI_PORT);

  NTP.begin("pool.ntp.org","time.nist.gov"); // sets the clock in the background

  server.on("/", handleRoot);
  server.on("/api/current", handleCurrent);
  server.on("/api/history", handleHistory);
  server.onNotFound(handleNotFound);
  server.begin();
  networkStarted = true;
}

void setup() {
  // pause half a second so everything can get started up
  delay(500);

  Serial.begin(115200);

  // set up the I2C bus first, so the pins are chosen before anything starts it
  Wire.setSDA(COMMON_SDA_PIN);
  Wire.setSCL(COMMON_SCL_PIN);
  Wire.begin();

  // initialize and clear the display
  oled.init();
  oled.setScale(1);
  oled.setContrast(128);
  oled.clear();
  oled.update();

  if (!ads.begin(0x48, &Wire)) {
    showMessage("ADS1115 not found", "check wiring");
    while (true) {
      delay(1000);
    }
  }
  ads.setGain(GAIN_ONE);    // +/- 4.096V range covers a 3.3V divider

  fsOk = LittleFS.begin();
  if (!fsOk) {
    LittleFS.format();
    fsOk = LittleFS.begin();
  }
  if (fsOk) {
    histLoad();
  } else {
    showMessage("No flash filesystem", "history not saved");
    delay(3000);
  }

  showMessage("Connecting WiFi...");
  WiFi.begin(NETWORK_SSID, NETWORK_PW);
  Serial.printf("Attempting to connect to '%s'", NETWORK_SSID);
  
  //Wait up to 20 s for WiFi, then carry on
  unsigned long wifiWaitStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiWaitStart < 20000) {
    Serial.print(".");
    delay(500);
  }
    
  if (WiFi.status() == WL_CONNECTED) {
    startNetwork();
  }

  readAllProbes();
  updateOled();
}


void loop() {
  if (networkStarted) {
    server.handleClient();
  }

  unsigned long now = millis();
  if (now - lastRead >= READ_INTERVAL_MS) {
    lastRead = now;
    my_ip_address = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("no Wifi");
    readAllProbes();
    updateOled();
    rebaseHistoryIfClockJustSet();
    sampleHistory();
  }

  if (now - lastFlush >= FLUSH_INTERVAL_MS) {
    lastFlush = now;
    histFlush();
  }

  // Reconnect if WiFi drops or never came up (retry at most every 10 seconds). The first time
  // it is connected, start the web server and the clock
  if (WiFi.status() == WL_CONNECTED) {
    if (!networkStarted) {
      startNetwork();
    }
  } else if (now - lastWifiRetry > 10000) {
    lastWifiRetry = now;
    WiFi.begin(NETWORK_SSID, NETWORK_PW);
  }

  delay(2);
}
