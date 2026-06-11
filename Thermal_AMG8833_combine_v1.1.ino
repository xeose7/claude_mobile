/*
  Integrated Battery Charging Fire Monitor
  Target board: Arduino UNO R4 WiFi

  실행 과정 요약:
  1. setup()에서 USB Serial 로그, Nextion HMI용 Serial1, AMG8833 센서를 초기화한다.
  2. loop()에서 Nextion 페이지 이벤트를 읽고, 주기적으로 NTC 3채널과 AMG8833 8x8 픽셀을 측정한다.
  3. NTC 평균값을 기준으로 AMG8833 최고온에 느린 보정값을 적용해 대표 온도를 만든다.
  4. 대표 온도 기준으로 NORMAL/WARN/DANGER/ERROR 상태를 판단하고 Serial 로그와 HMI에 표시한다.
  5. Thermal 페이지가 활성화되어 있으면 8x8 열화상 블록을 Nextion draw 명령으로 출력한다.
  6. 센서 오류가 생기면 오류 코드와 원인을 Serial 및 HMI의 tErr/tErrReason에 기록한다.

  Required Arduino library:
  - Adafruit AMG88xx

  Safety default:
  - ENABLE_DANGER_OUTPUT is false by default. Keep it disabled until LED-only tests are complete.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AMG88xx.h>
#include <math.h>

Adafruit_AMG88xx amg;

// -----------------------------
// User configuration
// -----------------------------
const uint32_t SERIAL_BAUD = 115200;
const uint32_t HMI_BAUD = 115200;

const uint8_t NTC_PINS[3] = {A0, A1, A2};
const uint8_t NTC_COUNT = 3;

// If your voltage divider is 5V -> fixed resistor -> analog pin -> NTC -> GND, keep true.
// If it is 5V -> NTC -> analog pin -> fixed resistor -> GND, set false.
const bool NTC_TO_GND = true;

const double SERIES_RESISTOR_OHM = 10000.0;
const double NTC_NOMINAL_OHM = 10000.0;
const double NTC_NOMINAL_TEMP_C = 25.0;
const double NTC_BETA = 3950.0;
double NTC_OFFSET_C[NTC_COUNT] = {0.0, 0.0, 0.0};

// Enable only after calibration coefficients are known.
const bool USE_STEINHART_HART = false;
double SH_A[NTC_COUNT] = {0.001129148, 0.001129148, 0.001129148};
double SH_B[NTC_COUNT] = {0.000234125, 0.000234125, 0.000234125};
double SH_C[NTC_COUNT] = {0.0000000876741, 0.0000000876741, 0.0000000876741};

const uint32_t SENSOR_INTERVAL_MS = 1000;
const uint32_t HMI_THERMAL_INTERVAL_MS = 1200;
const uint16_t ADC_MIN_VALID = 8;
const uint16_t ADC_MAX_VALID = 4087;

const double NTC_MIN_VALID_C = -20.0;
const double NTC_MAX_VALID_C = 120.0;
const double NTC_SPREAD_LIMIT_C = 12.0;
const double SENSOR_MISMATCH_LIMIT_C = 5.0;
const double THERMAL_MIN_VALID_C = -20.0;
const double THERMAL_MAX_VALID_C = 120.0;

const double WARN_TEMP_C = 40.0;
const double DANGER_TEMP_C = 50.0;

// HMI thermal area from the review document.
const int THERMAL_X = 200;
const int THERMAL_Y = 75;
const int THERMAL_W = 400;
const int THERMAL_H = 320;
const int THERMAL_COLS = 8;
const int THERMAL_ROWS = 8;
const int8_t THERMAL_PAGE_ID = -1;  // Set to the real Nextion page ID if you want to use sendme page detection.

const bool ENABLE_DANGER_OUTPUT = false;
const uint8_t DANGER_OUTPUT_PIN = 8;

// Nextion text colors in RGB565.
const uint16_t COLOR_GREEN = 2016;
const uint16_t COLOR_YELLOW = 65504;
const uint16_t COLOR_RED = 63488;
const uint16_t COLOR_WHITE = 65535;
const uint16_t COLOR_BLACK = 0;

enum SystemStatus {
  STATUS_WAIT = 0,
  STATUS_NORMAL,
  STATUS_WARN,
  STATUS_DANGER,
  STATUS_ERROR
};

struct ErrorInfo {
  const char* code;
  const char* reason;
};

struct NtcResult {
  double values[NTC_COUNT];
  bool valid[NTC_COUNT];
  uint8_t validCount;
  double averageC;
  double minC;
  double maxC;
  ErrorInfo error;
};

struct ThermalResult {
  float pixels[AMG88xx_PIXEL_ARRAY_SIZE];
  bool valid;
  double rawMaxC;
  ErrorInfo error;
};

struct SensorSnapshot {
  NtcResult ntc;
  ThermalResult thermal;
  double thermalOffsetC;
  double thermalAdjustedMaxC;
  double fusedTempC;
  SystemStatus status;
  ErrorInfo error;
};

double thermalOffsetC = 0.0;
bool amgReady = false;
bool thermalPageActive = false;
uint32_t lastSensorMs = 0;
uint32_t lastThermalHmiMs = 0;
SensorSnapshot lastSnapshot;

const ErrorInfo NO_ERROR = {"", ""};

void resetSnapshot(SensorSnapshot& snapshot) {
  snapshot.ntc.validCount = 0;
  snapshot.ntc.averageC = NAN;
  snapshot.ntc.minC = NAN;
  snapshot.ntc.maxC = NAN;
  snapshot.ntc.error = NO_ERROR;

  for (uint8_t i = 0; i < NTC_COUNT; i++) {
    snapshot.ntc.values[i] = NAN;
    snapshot.ntc.valid[i] = false;
  }

  for (uint8_t i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
    snapshot.thermal.pixels[i] = NAN;
  }

  snapshot.thermal.valid = false;
  snapshot.thermal.rawMaxC = NAN;
  snapshot.thermal.error = NO_ERROR;
  snapshot.thermalOffsetC = 0.0;
  snapshot.thermalAdjustedMaxC = NAN;
  snapshot.fusedTempC = NAN;
  snapshot.status = STATUS_WAIT;
  snapshot.error = NO_ERROR;
}

void sendHmiCommand(const String& command) {
  Serial1.print(command);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
}

String hmiSafeText(const String& value) {
  String safe = value;
  safe.replace("\\", "\\\\");
  safe.replace("\"", "'");
  return safe;
}

void setHmiText(const char* component, const String& value) {
  sendHmiCommand(String(component) + ".txt=\"" + hmiSafeText(value) + "\"");
}

void setHmiTextColor(const char* component, uint16_t color) {
  sendHmiCommand(String(component) + ".pco=" + String(color));
}

void clearThermalArea() {
  sendHmiCommand("fill " + String(THERMAL_X) + "," + String(THERMAL_Y) + "," +
                 String(THERMAL_W) + "," + String(THERMAL_H) + "," + String(COLOR_BLACK));
}

void refreshPageBase() {
  sendHmiCommand("cls 0");
  clearThermalArea();
}

uint16_t colorForTemperature(double tempC) {
  if (!isfinite(tempC)) {
    return COLOR_BLACK;
  }

  if (tempC < 30.0) return 31;       // blue
  if (tempC < 40.0) return 2016;     // green
  if (tempC < 50.0) return 65504;    // yellow
  if (tempC < 60.0) return 64512;    // orange
  return 63488;                      // red
}

const char* statusText(SystemStatus status) {
  switch (status) {
    case STATUS_NORMAL: return "NORMAL";
    case STATUS_WARN: return "WARN";
    case STATUS_DANGER: return "DANGER";
    case STATUS_ERROR: return "ERROR";
    case STATUS_WAIT:
    default: return "WAIT";
  }
}

uint16_t statusColor(SystemStatus status) {
  switch (status) {
    case STATUS_NORMAL: return COLOR_GREEN;
    case STATUS_WARN: return COLOR_YELLOW;
    case STATUS_DANGER:
    case STATUS_ERROR: return COLOR_RED;
    case STATUS_WAIT:
    default: return COLOR_WHITE;
  }
}

void updateDangerOutput(SystemStatus status) {
  if (!ENABLE_DANGER_OUTPUT) {
    digitalWrite(DANGER_OUTPUT_PIN, LOW);
    return;
  }

  digitalWrite(DANGER_OUTPUT_PIN, status == STATUS_DANGER ? HIGH : LOW);
}

double readNtcTemperatureC(uint8_t pin, uint8_t channel, bool& valid, ErrorInfo& error) {
  const uint16_t adc = analogRead(pin);

  if (adc <= ADC_MIN_VALID) {
    valid = false;
    error = {"NTC_ADC_LOW", "NTC channel may be disconnected or pulled to GND"};
    return NAN;
  }

  if (adc >= ADC_MAX_VALID) {
    valid = false;
    error = {"NTC_ADC_HIGH", "NTC channel may be shorted or pulled to 5V"};
    return NAN;
  }

  const double adcMax = 4095.0;
  double ntcResistance = NAN;

  if (NTC_TO_GND) {
    ntcResistance = SERIES_RESISTOR_OHM * adc / (adcMax - adc);
  } else {
    ntcResistance = SERIES_RESISTOR_OHM * (adcMax - adc) / adc;
  }

  if (!isfinite(ntcResistance) || ntcResistance <= 0.0) {
    valid = false;
    error = {"NTC_TEMP_RANGE", "NTC resistance calculation is invalid"};
    return NAN;
  }

  double tempC = NAN;

  if (USE_STEINHART_HART) {
    const double lnR = log(ntcResistance);
    const double invT = SH_A[channel] + SH_B[channel] * lnR + SH_C[channel] * lnR * lnR * lnR;
    tempC = (1.0 / invT) - 273.15;
  } else {
    double steinhart = ntcResistance / NTC_NOMINAL_OHM;
    steinhart = log(steinhart);
    steinhart /= NTC_BETA;
    steinhart += 1.0 / (NTC_NOMINAL_TEMP_C + 273.15);
    tempC = (1.0 / steinhart) - 273.15;
  }

  tempC += NTC_OFFSET_C[channel];

  if (!isfinite(tempC) || tempC < NTC_MIN_VALID_C || tempC > NTC_MAX_VALID_C) {
    valid = false;
    error = {"NTC_TEMP_RANGE", "Calculated NTC temperature is outside valid range"};
    return NAN;
  }

  valid = true;
  error = NO_ERROR;
  return tempC;
}

NtcResult readNtcSensors() {
  NtcResult result;
  result.validCount = 0;
  result.averageC = NAN;
  result.minC = 999.0;
  result.maxC = -999.0;
  result.error = NO_ERROR;

  double sum = 0.0;

  for (uint8_t i = 0; i < NTC_COUNT; i++) {
    ErrorInfo channelError = NO_ERROR;
    bool valid = false;
    result.values[i] = readNtcTemperatureC(NTC_PINS[i], i, valid, channelError);
    result.valid[i] = valid;

    if (valid) {
      sum += result.values[i];
      result.validCount++;
      result.minC = min(result.minC, result.values[i]);
      result.maxC = max(result.maxC, result.values[i]);
    } else if (result.error.code[0] == '\0') {
      result.error = channelError;
    }
  }

  if (result.validCount == 0) {
    result.error = {"NTC_ALL_INVALID", "All three NTC channels are invalid"};
    return result;
  }

  result.averageC = sum / result.validCount;

  if (result.validCount >= 2 && (result.maxC - result.minC) > NTC_SPREAD_LIMIT_C) {
    result.error = {"NTC_SPREAD", "NTC channel temperatures differ too much"};
  }

  return result;
}

ThermalResult readThermalSensor() {
  ThermalResult result;
  result.valid = false;
  result.rawMaxC = NAN;
  result.error = NO_ERROR;

  for (uint8_t i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
    result.pixels[i] = NAN;
  }

  if (!amgReady) {
    result.error = {"AMG_INIT", "AMG8833 was not detected during setup"};
    return result;
  }

  amg.readPixels(result.pixels);

  double maxC = -999.0;
  uint8_t validPixels = 0;

  for (uint8_t i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
    const float pixel = result.pixels[i];
    if (isfinite(pixel) && pixel >= THERMAL_MIN_VALID_C && pixel <= THERMAL_MAX_VALID_C) {
      maxC = max(maxC, (double)pixel);
      validPixels++;
    }
  }

  if (validPixels == 0) {
    result.error = {"AMG_READ", "AMG8833 pixel values are invalid"};
    return result;
  }

  result.valid = true;
  result.rawMaxC = maxC;
  return result;
}

SystemStatus classifyStatus(double fusedTempC, bool hasError) {
  if (hasError || !isfinite(fusedTempC)) {
    return STATUS_ERROR;
  }

  if (fusedTempC >= DANGER_TEMP_C) return STATUS_DANGER;
  if (fusedTempC >= WARN_TEMP_C) return STATUS_WARN;
  return STATUS_NORMAL;
}

SensorSnapshot buildSnapshot() {
  SensorSnapshot snapshot;
  snapshot.ntc = readNtcSensors();
  snapshot.thermal = readThermalSensor();
  snapshot.thermalOffsetC = thermalOffsetC;
  snapshot.thermalAdjustedMaxC = NAN;
  snapshot.fusedTempC = NAN;
  snapshot.status = STATUS_ERROR;
  snapshot.error = NO_ERROR;

  const bool ntcOk = snapshot.ntc.validCount > 0;
  const bool thermalOk = snapshot.thermal.valid;

  if (!ntcOk) {
    snapshot.error = snapshot.ntc.error;
  } else if (!thermalOk) {
    snapshot.error = snapshot.thermal.error;
  } else {
    if (snapshot.ntc.error.code[0] != '\0') {
      snapshot.error = snapshot.ntc.error;
    }

    const double rawDifferenceC = snapshot.ntc.averageC - snapshot.thermal.rawMaxC;
    if (snapshot.error.code[0] == '\0' && fabs(rawDifferenceC) >= SENSOR_MISMATCH_LIMIT_C) {
      snapshot.error = {"SENSOR_MISMATCH", "Raw NTC and thermal values differ before alignment"};
    }

    // A small gain prevents the representative temperature from jumping abruptly.
    thermalOffsetC = thermalOffsetC * 0.90 + rawDifferenceC * 0.10;
    snapshot.thermalOffsetC = thermalOffsetC;
    snapshot.thermalAdjustedMaxC = snapshot.thermal.rawMaxC + thermalOffsetC;
    snapshot.fusedTempC = (snapshot.ntc.averageC + snapshot.thermalAdjustedMaxC) / 2.0;
  }

  snapshot.status = classifyStatus(snapshot.fusedTempC, snapshot.error.code[0] != '\0');
  return snapshot;
}

String formatTemperature(double tempC) {
  if (!isfinite(tempC)) return "--.- C";
  return String(tempC, 1) + " C";
}

void updateHmiSummary(const SensorSnapshot& snapshot) {
  setHmiText("tMain", formatTemperature(snapshot.fusedTempC));
  setHmiText("tNtc", formatTemperature(snapshot.ntc.averageC));
  setHmiText("tIr", formatTemperature(isfinite(snapshot.thermalAdjustedMaxC)
                                        ? snapshot.thermalAdjustedMaxC
                                        : snapshot.thermal.rawMaxC));
  setHmiText("t11", statusText(snapshot.status));
  setHmiTextColor("t11", statusColor(snapshot.status));

  if (snapshot.error.code[0] == '\0') {
    setHmiText("tErr", "");
    setHmiText("tErrReason", "");
  } else {
    setHmiText("tErr", snapshot.error.code);
    setHmiText("tErrReason", snapshot.error.reason);
  }
}

void updateThermalHmi(const SensorSnapshot& snapshot) {
  if (!thermalPageActive || !snapshot.thermal.valid) {
    return;
  }

  const int cellW = THERMAL_W / THERMAL_COLS;
  const int cellH = THERMAL_H / THERMAL_ROWS;

  for (uint8_t row = 0; row < THERMAL_ROWS; row++) {
    for (uint8_t col = 0; col < THERMAL_COLS; col++) {
      const uint8_t index = row * THERMAL_COLS + col;
      const double adjustedTemp = snapshot.thermal.pixels[index] + snapshot.thermalOffsetC;
      const uint16_t color = colorForTemperature(adjustedTemp);
      const int x = THERMAL_X + col * cellW;
      const int y = THERMAL_Y + row * cellH;
      sendHmiCommand("fill " + String(x) + "," + String(y) + "," +
                     String(cellW) + "," + String(cellH) + "," + String(color));
    }
  }
}

void logSnapshot(const SensorSnapshot& snapshot) {
  if (snapshot.error.code[0] != '\0') {
    Serial.print("[ERROR] code=");
    Serial.print(snapshot.error.code);
    Serial.print(" reason=");
    Serial.println(snapshot.error.reason);
  }

  Serial.print("[DATA] ntcAvg=");
  Serial.print(snapshot.ntc.averageC, 2);
  Serial.print(" thermalRawMax=");
  Serial.print(snapshot.thermal.rawMaxC, 2);
  Serial.print(" thermalAdjMax=");
  Serial.print(snapshot.thermalAdjustedMaxC, 2);
  Serial.print(" thermalOffset=");
  Serial.print(snapshot.thermalOffsetC, 2);
  Serial.print(" fused=");
  Serial.print(snapshot.fusedTempC, 2);
  Serial.print(" status=");
  Serial.print(statusText(snapshot.status));
  Serial.print(" ntc=");
  for (uint8_t i = 0; i < NTC_COUNT; i++) {
    if (i > 0) Serial.print(",");
    if (snapshot.ntc.valid[i]) Serial.print(snapshot.ntc.values[i], 2);
    else Serial.print("INVALID");
  }
  Serial.println();
}

void handleHmiInput() {
  while (Serial1.available() > 0) {
    const uint8_t value = Serial1.read();

    // The Nextion page events in the document use "prints 01,1" and "prints 00,1".
    if (value == 0x01) {
      thermalPageActive = true;
      refreshPageBase();
      updateHmiSummary(lastSnapshot);
      lastThermalHmiMs = 0;
    } else if (value == 0x00) {
      thermalPageActive = false;
      refreshPageBase();
      updateHmiSummary(lastSnapshot);
    } else if (value == 0x66) {
      // Optional sendme page-id event. It is ignored unless THERMAL_PAGE_ID is configured.
      uint32_t started = millis();
      while (Serial1.available() < 4 && millis() - started < 20) {
        delay(1);
      }
      if (Serial1.available() >= 4) {
        const uint8_t pageId = Serial1.read();
        Serial1.read();
        Serial1.read();
        Serial1.read();
        if (THERMAL_PAGE_ID >= 0) {
          thermalPageActive = pageId == THERMAL_PAGE_ID;
        }
      }
    }
  }
}

void initializeHmi() {
  setHmiText("t11", "WAIT");
  setHmiTextColor("t11", COLOR_WHITE);
  setHmiText("tMain", "--.- C");
  setHmiText("tNtc", "--.- C");
  setHmiText("tIr", "--.- C");
  setHmiText("tErr", "");
  setHmiText("tErrReason", "");
}

void setup() {
  pinMode(DANGER_OUTPUT_PIN, OUTPUT);
  digitalWrite(DANGER_OUTPUT_PIN, LOW);

  Serial.begin(SERIAL_BAUD);
  Serial1.begin(HMI_BAUD);
  analogReadResolution(12);
  Wire.begin();

  delay(700);
  Serial.println("UNO R4 WiFi NTC + AMG8833 + Nextion integrated monitor started.");

  amgReady = amg.begin();
  if (!amgReady) {
    Serial.println("[ERROR] code=AMG_INIT reason=AMG8833 was not detected during setup");
  }

  initializeHmi();
  resetSnapshot(lastSnapshot);
}

void loop() {
  handleHmiInput();

  const uint32_t now = millis();
  if (now - lastSensorMs >= SENSOR_INTERVAL_MS) {
    lastSensorMs = now;
    lastSnapshot = buildSnapshot();
    updateDangerOutput(lastSnapshot.status);
    updateHmiSummary(lastSnapshot);
    logSnapshot(lastSnapshot);
  }

  if (thermalPageActive && now - lastThermalHmiMs >= HMI_THERMAL_INTERVAL_MS) {
    lastThermalHmiMs = now;
    updateThermalHmi(lastSnapshot);
  }
}