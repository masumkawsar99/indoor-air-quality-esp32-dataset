/*
  Low-Cost ESP32-Based Indoor Air Quality Monitoring System
  with Humidity Compensation and Short-Term Forecasting
  ---------------------------------------------------------------
  Hardware : ESP32 + Winsen MP-135 module (AO -> GPIO 33)
             + DHT22 (GPIO 25) + SSD1306 OLED (SDA 21, SCL 22)
  Every 30 s the ESP32:
    1. reads gas_adc, temperature, humidity            (same as before)
    2. computes Rs, Rs/R0 ratio, humidity/temp-compensated ratio
    3. computes the relative Pollution Index (100 = usual level)
    4. shows the CURRENT air quality class on the OLED
    5. runs the on-device LightGBM model -> forecast for +10 min
    6. uploads everything to the same Google Sheet

  Files in this sketch folder:
    AirQualityLive.ino   (this file)
    aq_model.h           (auto-generated model, do not edit by hand)

  Calibration constants and model (aq_model.h): from the final Colab run on
  Air_Quality_Dataset.xlsx (calibration week 16-22 Oct, model trained 16 Oct - 1 Nov 2026).

  TinyML measurements are printed on the Serial Monitor (115200 baud):
    "[TinyML]" line at start-up  -> free heap, sketch size
    every sample                 -> inference time (us) and pipeline time (us)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <math.h>
#include <time.h>
#include "aq_model.h"

// ================ WIFI / SHEETS =================
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SCRIPT_URL =
  "https://script.google.com/macros/s/YOUR_DEPLOYMENT_ID/exec";

// ================ PINS =================
#define GAS_PIN 33
#define DHT_PIN 25
#define DHT_TYPE DHT22
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_ADDR 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// ================ TIMING =================
const unsigned long SAMPLE_INTERVAL_MS     = 30000UL;   // must stay 30 s (model trained on 30 s steps)
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000UL;
const unsigned long STARTUP_DELAY_MS       = 120000UL;

// ================ CALIBRATION CONSTANTS (from dataset) =================
const double VC        = 5.0;      // sensor circuit voltage (V)
const double VREF      = 3.3;      // nominal ESP32 ADC full scale (V)
const double ADC_MAX   = 4095.0;
const double RL_KOHM   = 1.0;      // module load resistor (assumed, cancels in ratio)
const double R0_KOHM   = 11.819308790383172;   // baseline Rs (75th pct, 16-22 Oct)
const double COMP_A    = 4.436749605725945;    // log(ratio) = A + B_H*H + B_T*T
const double COMP_BH   = -0.027601594227602402;
const double COMP_BT   = -0.0879629875908463;
const double TH_P75    = 1.0533419804842419;   // class thresholds on index/100
const double TH_P95    = 1.2347326390539075;
const double TH_P99    = 1.3970872738011029;

// ================ BUFFER SIZES =================
const int BASE_N     = 360;  // 3 h of 30 s samples -> baseline median
const int MIN_BASE   = 120;  // need >= 1 h of history before the index is shown
const int SMOOTH_N   = 5;    // 2.5 min smoothing
const int IDX_N      = 121;  // index history for lag/rolling features (0..120)
const int SHORT_N    = 11;   // gas / humidity / ratio_comp history (0..10)

const char* CLASS_NAME[4]  = {"Normal", "Slightly Elevated", "Elevated", "Spike"};
const char* CLASS_SHORT[4] = {"NORMAL", "SLIGHT", "ELEVATED", "SPIKE"};

// ================ OBJECTS =================
DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================ RING BUFFER =================
struct Ring {
  double* v; int cap; int head; int count;
  void init(int c) { cap = c; v = new double[c]; head = 0; count = 0; }
  void push(double x) { v[head] = x; head = (head + 1) % cap; if (count < cap) count++; }
  // k = 0 -> newest, k = 1 -> previous, ...
  double ago(int k) const { int i = head - 1 - k; while (i < 0) i += cap; return v[i]; }
};
Ring invBuf, idxBuf, gasBuf, humBuf, rcompBuf;
double sortTmp[BASE_N];

// ================ STATE =================
float temperatureC = NAN, humidityRH = NAN;
float lastTemp = NAN, lastHum = NAN, prevHum = NAN, prevTemp = NAN;
int   gasADC = 0, prevGasADC = -1;
double rsK = NAN, ratio = NAN, ratioComp = NAN, pollutionIndex = NAN, forecastIndex = NAN;
int   currentClass = -1, forecastClass = -1;
bool  saturated = false;
unsigned long inferUs = 0, pipelineUs = 0, maxInferUs = 0;   // TinyML timing
unsigned long lastSampleTime = 0, lastWiFiAttempt = 0;
bool  startupComplete = false;

// ================ HELPERS =================
String num(double v, int dec = 2) { return isfinite(v) ? String(v, dec) : String(""); }

int classify(double r) {           // r = index / 100
  if (r > TH_P99) return 3;
  if (r > TH_P95) return 2;
  if (r > TH_P75) return 1;
  return 0;
}

double medianOfPrevious(const Ring& b, int n) {   // median of last n values in b
  for (int k = 0; k < n; k++) sortTmp[k] = b.ago(k);
  // insertion sort is fine for n <= 360 every 30 s
  for (int i = 1; i < n; i++) { double x = sortTmp[i]; int j = i - 1; while (j >= 0 && sortTmp[j] > x) { sortTmp[j + 1] = sortTmp[j]; j--; } sortTmp[j + 1] = x; }
  return (n % 2) ? sortTmp[n / 2] : 0.5 * (sortTmp[n / 2 - 1] + sortTmp[n / 2]);
}

double meanIdx(int w) { double s = 0; for (int k = 0; k < w; k++) s += idxBuf.ago(k); return s / w; }
double maxIdx(int w)  { double m = -1e9; for (int k = 0; k < w; k++) m = max(m, idxBuf.ago(k)); return m; }

bool hourOfDay(double& h) {
  struct tm t;
  if (!getLocalTime(&t, 50)) return false;
  h = t.tm_hour + t.tm_min / 60.0;
  return true;
}

// ================ CORE PIPELINE =================
void processSample() {
  unsigned long t0 = micros();
  inferUs = 0;
  gasADC = analogRead(GAS_PIN);
  temperatureC = dht.readTemperature();
  humidityRH  = dht.readHumidity();
  if (isfinite(temperatureC)) lastTemp = temperatureC;
  if (isfinite(humidityRH))   lastHum  = humidityRH;
  saturated = (gasADC >= 4095);

  pollutionIndex = NAN; currentClass = -1; forecastIndex = NAN; forecastClass = -1;
  if (gasADC <= 0 || !isfinite(lastTemp) || !isfinite(lastHum)) return;

  // 1) Rs, ratio, humidity/temperature compensation
  double v  = gasADC / ADC_MAX * VREF;
  rsK       = (VC / v - 1.0) * RL_KOHM;
  ratio     = rsK / R0_KOHM;
  ratioComp = ratio / exp(COMP_A + COMP_BH * lastHum + COMP_BT * lastTemp);
  double inv = 1.0 / ratioComp;                 // higher = more gas

  // 2) relative Pollution Index: 2.5-min mean / median of previous 3 h
  if (invBuf.count >= MIN_BASE) {
    int nb = min(invBuf.count, BASE_N);
    double base = medianOfPrevious(invBuf, nb); // excludes current sample
    double s = inv; int ns = 1;
    for (int k = 0; k < SMOOTH_N - 1 && k < invBuf.count; k++) { s += invBuf.ago(k); ns++; }
    double r = (s / ns) / base;
    pollutionIndex = 100.0 * r;
    currentClass = classify(r);
  }

  // 3) update histories
  invBuf.push(inv);
  gasBuf.push(gasADC);
  humBuf.push(lastHum);
  rcompBuf.push(ratioComp);
  if (isfinite(pollutionIndex)) idxBuf.push(pollutionIndex / 100.0);
  else { idxBuf.count = 0; idxBuf.head = 0; }  // restart feature history if index invalid

  // 4) features + on-device model (needs 121 index values = 1 h, 10 gas samples)
  double hr;
  if (idxBuf.count >= IDX_N && gasBuf.count >= 10 && humBuf.count >= SHORT_N && rcompBuf.count >= SHORT_N && hourOfDay(hr)) {
    double gm = 0, gs = 0;
    for (int k = 0; k < 10; k++) gm += gasBuf.ago(k);
    gm /= 10;
    for (int k = 0; k < 10; k++) { double d = gasBuf.ago(k) - gm; gs += d * d; }
    gs = sqrt(gs / 9.0);                          // sample std (ddof = 1), like pandas

    double x[23];
    double idx = idxBuf.ago(0);
    x[0]  = idx;
    x[1]  = idxBuf.ago(1);
    x[2]  = idxBuf.ago(2);
    x[3]  = idxBuf.ago(5);
    x[4]  = idxBuf.ago(10);
    x[5]  = idxBuf.ago(20);
    x[6]  = idx - idxBuf.ago(5);
    x[7]  = idx - idxBuf.ago(10);
    x[8]  = ratioComp;
    x[9]  = ratioComp - rcompBuf.ago(10);
    x[10] = gs / gm;
    x[11] = lastTemp;
    x[12] = lastHum;
    x[13] = lastHum - humBuf.ago(10);
    x[14] = sin(2 * M_PI * hr / 24.0);
    x[15] = cos(2 * M_PI * hr / 24.0);
    x[16] = currentClass;
    x[17] = maxIdx(20);
    x[18] = meanIdx(20);
    x[19] = maxIdx(60);
    x[20] = meanIdx(60);
    x[21] = maxIdx(120);
    x[22] = meanIdx(120);

    unsigned long ti = micros();
    double delta = aq_model_delta(x);
    inferUs = micros() - ti;
    if (inferUs > maxInferUs) maxInferUs = inferUs;
    double fr = idx + delta;
    forecastIndex = 100.0 * fr;
    forecastClass = classify(fr);
  }
  pipelineUs = micros() - t0;   // includes DHT22 read (~5 ms) and sensor ADC read

  Serial.printf("ADC %d | T %.1f H %.1f | Rs %.2f k | ratio %.3f comp %.3f | Index %s | Now %s | +10min %s (%s)%s\n",
                gasADC, lastTemp, lastHum, rsK, ratio, ratioComp,
                num(pollutionIndex, 1).c_str(),
                currentClass >= 0 ? CLASS_NAME[currentClass] : "warming up",
                num(forecastIndex, 1).c_str(),
                forecastClass >= 0 ? CLASS_NAME[forecastClass] : "-",
                saturated ? " | ADC SATURATED" : "");
  Serial.printf("[TinyML] inference %lu us (max %lu us) | full pipeline %lu us | free heap %u bytes\n",
                inferUs, maxInferUs, pipelineUs, (unsigned)ESP.getFreeHeap());
}

// ================ WIFI =================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (lastWiFiAttempt != 0 && now - lastWiFiAttempt < WIFI_RETRY_INTERVAL_MS) return;
  lastWiFiAttempt = now;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// ================ OLED =================
void updateOLED(const char* status) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("T %.1fC  H %.0f%%  %s", lastTemp, lastHum, WiFi.status() == WL_CONNECTED ? "W" : "-");

  display.setCursor(0, 12);
  display.print("Air now:");
  display.setTextSize(2);
  display.setCursor(0, 22);
  if (currentClass >= 0) display.print(CLASS_SHORT[currentClass]);
  else { display.setTextSize(1); display.print("Warming up (1 h)"); }

  display.setTextSize(1);
  display.setCursor(0, 42);
  display.print("Index: "); display.print(num(pollutionIndex, 0));
  display.setCursor(0, 52);
  display.print("+10m: ");
  if (forecastClass >= 0) { display.print(CLASS_SHORT[forecastClass]); display.print(" "); display.print(num(forecastIndex, 0)); }
  else display.print(status);
  display.display();
}

// ================ GOOGLE SHEETS =================
bool uploadData() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;

  double gasDelta  = (prevGasADC >= 0) ? gasADC - prevGasADC : NAN;
  double humDelta  = (isfinite(humidityRH) && isfinite(prevHum)) ? humidityRH - prevHum : NAN;
  double tempDelta = (isfinite(temperatureC) && isfinite(prevTemp)) ? temperatureC - prevTemp : NAN;
  double rmean = NAN, rstd = NAN;
  if (gasBuf.count >= 10) {
    double s = 0; for (int k = 0; k < 10; k++) s += gasBuf.ago(k); rmean = s / 10;
    double q = 0; for (int k = 0; k < 10; k++) { double d = gasBuf.ago(k) - rmean; q += d * d; } rstd = sqrt(q / 10);
  }

  String url = String(SCRIPT_URL);
  url += "?temp=" + num(temperatureC) + "&hum=" + num(humidityRH);
  url += "&co2=&nh3=&co=&alcohol=&benzene=&h2=&nox=&smoke=&voc=";
  url += "&index=" + num(pollutionIndex);
  url += "&air=";  if (currentClass >= 0) { String a = CLASS_NAME[currentClass]; a.replace(" ", "%20"); url += a; }
  url += "&gas_adc=" + String(gasADC);
  url += "&rs=" + num(rsK, 3) + "&r0=" + num(R0_KOHM, 3) + "&ratio=" + num(ratio, 4);
  url += "&gas_delta=" + num(gasDelta) + "&humidity_delta=" + num(humDelta) + "&temp_delta=" + num(tempDelta);
  url += "&rolling_mean=" + num(rmean) + "&rolling_std=" + num(rstd);
  url += "&target_label=";                                  // true future label is filled later in Python
  url += "&forecast_index=" + num(forecastIndex);           // ignored unless the Apps Script stores it
  url += "&forecast_class=" + (forecastClass >= 0 ? String(forecastClass) : String(""));

  if (!https.begin(client, url)) return false;
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setConnectTimeout(10000);
  https.setTimeout(15000);
  int code = https.GET();
  String resp = https.getString();
  https.end();
  return code >= 200 && code < 400 && resp.indexOf("OK") >= 0;
}

// ================ SETUP / LOOP =================
void setup() {
  Serial.begin(115200);
  delay(300);
  analogReadResolution(12);
  pinMode(GAS_PIN, INPUT);
  dht.begin();

  invBuf.init(BASE_N); idxBuf.init(IDX_N);
  gasBuf.init(SHORT_N); humBuf.init(SHORT_N); rcompBuf.init(SHORT_N);

  // TinyML footprint report (copy these numbers into the paper)
  {
    double probe[23] = {1,1,1,1,1,1,0,0,1,0,0.01,31,65,0,0,1,0,1,1,1,1,1,1};
    unsigned long ti = micros();
    volatile double p = 0;
    for (int i = 0; i < 1000; i++) p += aq_model_delta(probe);
    unsigned long avgUs1000 = micros() - ti;
    Serial.printf("[TinyML] model: LightGBM 60 trees x 8 leaves, 23 features\n");
    Serial.printf("[TinyML] avg inference over 1000 runs: %.2f us\n", avgUs1000 / 1000.0);
    Serial.printf("[TinyML] sketch size %u bytes | free flash %u bytes | free heap %u bytes | buffers %u bytes\n",
                  (unsigned)ESP.getSketchSize(), (unsigned)ESP.getFreeSketchSpace(), (unsigned)ESP.getFreeHeap(),
                  (unsigned)((BASE_N + IDX_N + 3 * SHORT_N + BASE_N) * sizeof(double)));
  }

  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0); display.println("Air Monitor Starting");
    display.println("Sensor warm-up..."); display.display();
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttempt = millis();
  configTime(6 * 3600, 0, "pool.ntp.org", "time.google.com");   // Asia/Dhaka (UTC+6) for hour features

  delay(STARTUP_DELAY_MS);
  startupComplete = true;
  lastSampleTime = millis() - SAMPLE_INTERVAL_MS;
}

void loop() {
  connectWiFi();
  if (!startupComplete) return;
  unsigned long now = millis();
  if (now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;
    processSample();
    updateOLED("collecting");
    bool ok = uploadData();
    updateOLED(ok ? "upload OK" : "upload fail");
    prevGasADC = gasADC;
    if (isfinite(humidityRH))   prevHum  = humidityRH;
    if (isfinite(temperatureC)) prevTemp = temperatureC;
  }
  delay(20);
}
