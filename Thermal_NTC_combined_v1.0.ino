/*
  AMG8833 열화상 + 3× NTC 서미스터 — Nextion HMI 통합 스케치
  Target : Arduino UNO R4 WiFi

  페이지 구성:
    Thermal 페이지  : AMG8833 8×8 히트맵 + t11(전체 상태 표시)
    메인(NTC) 페이지: t1~t3(온도값), t4~t6(상태색),
                     t7(최고온 상태색), t8(최고온 CELL명),
                     t9(최고온도), t10(서미스터 최고온도), t12("서미스터2" 고정 라벨),
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
bool          isThermalPage  = true;
int           prevStatus     = -1;
unsigned long previousMillis = 0;
const unsigned long UPDATE_INTERVAL = 1000;

// ═══════════════════════════════════════════════════════════════════════════
// Nextion HMI 통신 (Serial1)
// ═══════════════════════════════════════════════════════════════════════════

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
      isThermalPage = false;
      prevStatus = -1;   // 복귀 시 t11 강제 재갱신
    }
    if (data == 0x01) isThermalPage = true;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// NTC 서미스터
// ═══════════════════════════════════════════════════════════════════════════

float readTemperature(int pin) {
  int adcValue = analogRead(pin);
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

// 메인(NTC) 페이지 전체 갱신
void updateNtcDisplay(float t1, float t2, float t3) {
  // 온도값
  sendCmd("t1.txt=\"" + String(t1, 1) + " C\"");
  sendCmd("t2.txt=\"" + String(t2, 1) + " C\"");
  sendCmd("t3.txt=\"" + String(t3, 1) + " C\"");

  // 온도 색상
  sendCmd("t1.pco=" + String(getStatusColor(t1)));
  sendCmd("t2.pco=" + String(getStatusColor(t2)));
  sendCmd("t3.pco=" + String(getStatusColor(t3)));

  // 상태값 (Nextion Timer가 이 값을 읽어 한글 텍스트 표시)
  sendCmd("n0.val=" + String(getStatusValue(t1)));
  sendCmd("n1.val=" + String(getStatusValue(t2)));
  sendCmd("n2.val=" + String(getStatusValue(t3)));

  // 상태 색상
  sendCmd("t4.pco=" + String(getStatusColor(t1)));
  sendCmd("t5.pco=" + String(getStatusColor(t2)));
  sendCmd("t6.pco=" + String(getStatusColor(t3)));

  // 최고온도 CELL 찾기
  float maxTemp  = t1;
  String maxCell = "CELL1";
  if (t2 > maxTemp) { maxTemp = t2; maxCell = "CELL2"; }
  if (t3 > maxTemp) { maxTemp = t3; maxCell = "CELL3"; }

  sendCmd("n3.val=" + String(getStatusValue(maxTemp)));
  sendCmd("t7.pco=" + String(getStatusColor(maxTemp)));
  sendCmd("t8.txt=\"" + maxCell + "\"");
  sendCmd("t8.pco=" + String(getStatusColor(maxTemp)));
  sendCmd("t9.txt=\"" + String(maxTemp, 1) + " C\"");
  sendCmd("t9.pco=" + String(getStatusColor(maxTemp)));

  // t10 : 서미스터 3채널 중 최고온도
  sendCmd("t10.txt=\"" + String(maxTemp, 1) + " C\"");
  sendCmd("t10.pco=" + String(getStatusColor(maxTemp)));
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

  // 메인(NTC) 페이지 컴포넌트 초기값 설정
  // (Thermal 페이지 전환 전에 미리 보내도 Nextion이 값을 저장함)
  sendCmd("t1.txt=\"--.- C\"");
  sendCmd("t2.txt=\"--.- C\"");
  sendCmd("t3.txt=\"--.- C\"");
  sendCmd("t4.txt=\"WAIT\""); sendCmd("t4.pco=2016");
  sendCmd("t5.txt=\"WAIT\""); sendCmd("t5.pco=2016");
  sendCmd("t6.txt=\"WAIT\""); sendCmd("t6.pco=2016");
  sendCmd("t7.txt=\"WAIT\""); sendCmd("t7.pco=2016");
  sendCmd("t8.txt=\"-\"");
  sendCmd("t9.txt=\"--.- C\"");
  sendCmd("t10.txt=\"--.- C\"");
  sendCmd("t12.txt=\"서미스터2\"");  // 고정 라벨 — 이후 변경 없음
  sendCmd("n0.val=0"); sendCmd("n1.val=0");
  sendCmd("n2.val=0"); sendCmd("n3.val=0");

  // Thermal 페이지로 이동
  sendCmd("page Thermal");
  delay(500);
  sendCmd("t0.txt=\"\"");
}

void loop() {
  checkSerial();

  // ── NTC 읽기 (1초 주기) ─────────────────────────────────────────────────
  unsigned long now = millis();
  if (now - previousMillis >= UPDATE_INTERVAL) {
    previousMillis = now;

    float t1 = readTemperature(NTC_PIN1);
    float t2 = readTemperature(NTC_PIN2);
    float t3 = readTemperature(NTC_PIN3);

    Serial.print("NTC1: "); Serial.print(t1, 1);
    Serial.print(" / NTC2: "); Serial.print(t2, 1);
    Serial.print(" / NTC3: "); Serial.print(t3, 1);
    Serial.println(" C");

    // 메인(NTC) 페이지에 있을 때만 NTC 디스플레이 갱신
    if (!isThermalPage) {
      updateNtcDisplay(t1, t2, t3);
    }
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
