/*
  AMG8833 열화상 + 3× NTC 서미스터 — Nextion HMI 통합 스케치
  Target : Arduino UNO R4 WiFi

  페이지 구성:
    Thermal 페이지  : AMG8833 8×8 히트맵 + t11(전체 상태 표시)
    메인(NTC) 페이지: t1~t3(온도값), t4~t6(상태색),
                     t7(최고온 상태색), t8(최고온 CELL명),
                     t9(최고온도), t10(CELL2 온도), t12("CELL2" 고정 라벨),
                     n0~n3(상태값 0/1/2)

  하드웨어 연결:
    AMG8833  → I2C : SDA(A4), SCL(A5), VCC=3.3V, GND
    NTC×3    → A0, A1, A2 (10kΩ 분압, VCC=3.3V)
    Nextion  → Serial1 : D1(TX→RX), D0(RX←TX), VCC=5V, GND
    USB      → Serial (PC 디버그)
*/

#include <Wire.h>
#include <Adafruit_AMG88xx.h>

// ── AMG8833 열화상 설정 ────────────────────────────────────────────────────
Adafruit_AMG88xx amg;

#define T0_X   200
#define T0_Y    75
#define T0_W   400
#define T0_H   320
#define GRID_W  8
#define GRID_H  8
#define CELL_W (T0_W / GRID_W)
#define CELL_H (T0_H / GRID_H)

// ── NTC 서미스터 설정 ──────────────────────────────────────────────────────
const int   NTC_PIN1           = A0;
const int   NTC_PIN2           = A1;
const int   NTC_PIN3           = A2;
const float SERIES_RESISTOR    = 10000.0f;
const float NOMINAL_RESISTANCE = 10000.0f;
const float NOMINAL_TEMPERATURE = 25.0f;
const float BETA_COEFFICIENT   = 3950.0f;
const float ADC_MAX            = 4095.0f;

// ── 상태 변수 ─────────────────────────────────────────────────────────────
bool          isThermalPage   = true;
bool          ntcNeedsRefresh = true;    // 부팅 직후 NTC 즉시 1회 갱신
int           prevStatus      = -1;
unsigned long previousMillis  = 0;
const unsigned long UPDATE_INTERVAL = 1000;

// ═══════════════════════════════════════════════════════════════════════════
// Nextion HMI 통신 (Serial1)
// ═══════════════════════════════════════════════════════════════════════════

void resetDisplayCache();   // 아래에 정의 (checkSerial에서 먼저 호출)

void sendCmd(const String& cmd) {
  Serial1.print(cmd);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
  delay(5);
}

// Nextion 페이지 전환 이벤트 수신
//   0x01 → Thermal 페이지 진입
//   0x00 → Thermal 페이지 이탈
void checkSerial() {
  while (Serial1.available() > 0) {
    byte data = Serial1.read();
    if (data == 0x00) {
      if (isThermalPage) {
        Serial.println("[PAGE] Thermal → NTC");
        isThermalPage   = false;
        ntcNeedsRefresh = true;
        prevStatus      = -1;
        resetDisplayCache();   // 페이지 리셋되었으므로 전체 강제 재전송
      }
    }
    if (data == 0x01) {
      if (!isThermalPage) {
        Serial.println("[PAGE] NTC → Thermal");
        isThermalPage = true;
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// NTC 서미스터
// ═══════════════════════════════════════════════════════════════════════════

float readTemperature(int pin) {
  // ADC 노이즈로 소수점 끝자리가 흔들리면 표시가 깜빡이므로
  // 16회 평균으로 안정화한다.
  long sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(pin);
  int adcValue = sum / 16;
  if (adcValue <= 0)    adcValue = 1;
  if (adcValue >= 4095) adcValue = 4094;

  float R    = SERIES_RESISTOR * ((ADC_MAX / adcValue) - 1.0f);
  float tempK = 1.0f / (log(R / NOMINAL_RESISTANCE) / BETA_COEFFICIENT
                        + 1.0f / (NOMINAL_TEMPERATURE + 273.15f));
  return tempK - 273.15f;
}

int getStatusValue(float temp) {
  if (temp < 40.0f) return 0;
  if (temp < 50.0f) return 1;
  return 2;
}

int getStatusColor(float temp) {
  if (temp < 40.0f) return 2016;    // 초록 (RGB565 #07E0)
  if (temp < 50.0f) return 65504;   // 노랑 (RGB565 #FFE0)
  return 63488;                     // 빨강 (RGB565 #F800)
}

// ─── 깜빡임 방지 캐시 ───────────────────────────────────────────────────────
// 직전 전송값을 기억해두고, 값이 바뀔 때만 Nextion에 재전송한다.
// (매초 동일 값을 다시 쓰면 텍스트 박스가 지워졌다 그려지며 깜빡임 발생)
String cTxt[13];   // t0~t12 직전 텍스트
int    cPco[13];   // t0~t12 직전 글자색
int    cNum[4];    // n0~n3 직전 값

void resetDisplayCache() {
  for (int i = 0; i < 13; i++) { cTxt[i] = "\x01"; cPco[i] = -1; }
  for (int i = 0; i < 4;  i++)   cNum[i] = -1;
}

// 텍스트가 직전과 다를 때만 전송
void setTxt(int id, const String& val) {
  if (cTxt[id] == val) return;
  cTxt[id] = val;
  sendCmd("t" + String(id) + ".txt=\"" + val + "\"");
}

// 글자색이 직전과 다를 때만 전송
void setPco(int id, int color) {
  if (cPco[id] == color) return;
  cPco[id] = color;
  sendCmd("t" + String(id) + ".pco=" + String(color));
}

// 숫자값이 직전과 다를 때만 전송
void setNum(int id, int val) {
  if (cNum[id] == val) return;
  cNum[id] = val;
  sendCmd("n" + String(id) + ".val=" + String(val));
}

// 메인(NTC) 페이지 전체 갱신 (변경분만 전송 → 깜빡임 없음)
void updateNtcDisplay(float t1, float t2, float t3) {
  // 온도값 + 색상
  setTxt(1, String(t1, 1) + " C"); setPco(1, getStatusColor(t1));
  setTxt(2, String(t2, 1) + " C"); setPco(2, getStatusColor(t2));
  setTxt(3, String(t3, 1) + " C"); setPco(3, getStatusColor(t3));

  // 상태값 (Nextion Timer가 읽어 한글 텍스트 표시)
  setNum(0, getStatusValue(t1));
  setNum(1, getStatusValue(t2));
  setNum(2, getStatusValue(t3));

  // 상태 색상
  setPco(4, getStatusColor(t1));
  setPco(5, getStatusColor(t2));
  setPco(6, getStatusColor(t3));

  // 최고온도 CELL 찾기
  float maxTemp  = t1;
  String maxCell = "CELL1";
  if (t2 > maxTemp) { maxTemp = t2; maxCell = "CELL2"; }
  if (t3 > maxTemp) { maxTemp = t3; maxCell = "CELL3"; }

  setNum(3, getStatusValue(maxTemp));
  setPco(7, getStatusColor(maxTemp));
  setTxt(8, maxCell);                 setPco(8, getStatusColor(maxTemp));
  setTxt(9, String(maxTemp, 1) + " C"); setPco(9, getStatusColor(maxTemp));

  // t10 : CELL2(NTC2/A1) 온도
  setTxt(10, String(t2, 1) + " C");
  setPco(10, getStatusColor(t2));

  // t12 : "CELL2" 고정 라벨 — 글자색(흰색)을 함께 지정해야 보임
  setTxt(12, "CELL2");
  setPco(12, 65535);   // 흰색. 배경이 밝으면 0(검정)으로 변경
}

// ═══════════════════════════════════════════════════════════════════════════
// AMG8833 열화상 히트맵
// ═══════════════════════════════════════════════════════════════════════════

void fillRect(int x, int y, int w, int h, uint16_t color) {
  sendCmd("fill " + String(x) + "," + String(y) + "," +
          String(w) + "," + String(h) + "," + String(color));
}

uint16_t tempToColor(float temp) {
  float r, g, b;
  if (temp < 35.0f) {
    float t = constrain((temp - 20.0f) / 15.0f, 0.0f, 1.0f);
    r = 0.0f; g = t;    b = 1.0f;
  } else if (temp < 40.0f) {
    float t = (temp - 35.0f) / 5.0f;
    r = 0.0f; g = 1.0f; b = 1.0f - t;
  } else if (temp < 45.0f) {
    float t = (temp - 40.0f) / 5.0f;
    r = t;    g = 1.0f; b = 0.0f;
  } else if (temp < 50.0f) {
    float t = (temp - 45.0f) / 5.0f;
    r = 1.0f; g = 1.0f - t; b = 0.0f;
  } else {
    r = 1.0f; g = 0.0f; b = 0.0f;
  }
  return ((uint8_t)(r * 31) << 11)
       | ((uint8_t)(g * 63) << 5)
       |  (uint8_t)(b * 31);
}

float bilinear(float grid[8][8], float gx, float gy) {
  int x0 = (int)gx,        y0 = (int)gy;
  int x1 = min(x0 + 1, 7), y1 = min(y0 + 1, 7);
  float fx = gx - x0,      fy = gy - y0;
  return grid[y0][x0]*(1-fx)*(1-fy) + grid[y0][x1]*fx*(1-fy)
       + grid[y1][x0]*(1-fx)*fy     + grid[y1][x1]*fx*fy;
}

float getMaxTemp(float pixels[64]) {
  float m = pixels[0];
  for (int i = 1; i < 64; i++) if (pixels[i] > m) m = pixels[i];
  return m;
}

// Thermal 페이지 t11 상태 표시 (IR 최고온 기준, 변화 시에만 갱신)
void updateIrStatus(float maxTemp) {
  int status = (maxTemp >= 50.0f) ? 2 : (maxTemp >= 40.0f) ? 1 : 0;
  if (status == prevStatus) return;
  prevStatus = status;

  if (status == 2) {
    sendCmd("t11.pco=63488");
    sendCmd("t11.txt=\"위험\"");
  } else if (status == 1) {
    sendCmd("t11.pco=65504");
    sendCmd("t11.txt=\"경고\"");
  } else {
    sendCmd("t11.pco=2016");
    sendCmd("t11.txt=\"정상\"");
  }
}

void renderHeatmap(float pixels[8][8]) {
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) {
      checkSerial();
      if (!isThermalPage) return;   // 페이지 이탈 시 중단

      float gx = col * 7.0f / (GRID_W - 1);
      float gy = row * 7.0f / (GRID_H - 1);
      fillRect(T0_X + col*CELL_W, T0_Y + row*CELL_H, CELL_W, CELL_H,
               tempToColor(bilinear(pixels, gx, gy)));
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// setup / loop
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);   // USB 디버그
  Serial1.begin(115200);  // Nextion HMI
  Wire.begin();
  analogReadResolution(12);

  while (!amg.begin(0x69, &Wire)) {
    Serial.println("AMG8833 not found. Retrying...");
    delay(1000);
  }
  Serial.println("AMG8833 ready.");

  delay(1000);

  resetDisplayCache();   // 캐시 초기화 → 첫 갱신 시 전체 1회 전송

  // Thermal 페이지로 이동
  sendCmd("page Thermal");
  delay(500);
  sendCmd("t0.txt=\"\"");
}

void loop() {
  checkSerial();

  // ── NTC 읽기 (1초 주기 또는 페이지 진입 즉시) ────────────────────────────
  unsigned long now = millis();
  bool intervalElapsed = (now - previousMillis >= UPDATE_INTERVAL);

  if (intervalElapsed || ntcNeedsRefresh) {
    if (intervalElapsed) previousMillis = now;
    ntcNeedsRefresh = false;

    float t1 = readTemperature(NTC_PIN1);
    float t2 = readTemperature(NTC_PIN2);
    float t3 = readTemperature(NTC_PIN3);

    Serial.print("NTC1:"); Serial.print(t1, 1);
    Serial.print(" NTC2:"); Serial.print(t2, 1);
    Serial.print(" NTC3:"); Serial.print(t3, 1);
    Serial.print(" | Page:"); Serial.println(isThermalPage ? "THERMAL" : "NTC");

    // 페이지 무관 항상 갱신 — Nextion이 값을 내부 저장, 페이지 전환 시 즉시 표시
    // (이전에 !isThermalPage 조건이 있었으나 page 감지 오류 시 t10/t12 미출력됨)
    updateNtcDisplay(t1, t2, t3);
  }

  // ── AMG8833 열화상 (Thermal 페이지 전용) ──────────────────────────────
  if (isThermalPage) {
    float pixels[64];
    amg.readPixels(pixels);

    float grid[8][8];
    for (int i = 0; i < 8; i++)
      for (int j = 0; j < 8; j++)
        grid[i][j] = pixels[i * 8 + j];

    renderHeatmap(grid);

    if (isThermalPage) {
      updateIrStatus(getMaxTemp(pixels));
    }
  }

  delay(50);
}
