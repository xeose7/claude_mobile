/*
  AMG8833 Thermal Heatmap — Nextion HMI
  Target : Arduino UNO R4 WiFi

  수정 사항 (원본 → v1.0):
  1. [Serial → Serial1] Nextion HMI 통신 전체를 Serial1(D0/D1 핀)으로 변경
     - UNO R4 WiFi에서 Serial은 USB(PC 디버그)이고,
       외부 장치(Nextion) 연결은 반드시 Serial1을 사용해야 함
  2. [setup] Serial1.begin(115200) 추가, Serial.begin은 USB 디버그용으로 분리 유지
  3. [amg.begin] begin(0x69, &Wire) 형식 유지 (Adafruit_AMG88xx 최신 API 호환)
  4. [Wire.begin] I2C 초기화 유지 — amg.begin() 전에 호출 필수
  5. [그 외 로직] 변경 없음

  하드웨어 연결:
    AMG8833  → I2C : SDA(A4), SCL(A5), VCC=3.3V, GND
    Nextion  → Serial1 : TX(D1) → Nextion RX, RX(D0) ← Nextion TX, VCC=5V, GND
*/

#include <Wire.h>
#include <Adafruit_AMG88xx.h>

Adafruit_AMG88xx amg;

#define T0_X   200
#define T0_Y    75
#define T0_W   400
#define T0_H   320
#define GRID_W  8
#define GRID_H  8
#define CELL_W (T0_W / GRID_W)
#define CELL_H (T0_H / GRID_H)

bool isThermalPage = true;
int prevStatus = -1; // 0=정상, 1=경고, 2=위험

// ── Nextion HMI 명령 전송 (Serial1 사용) ──────────────────────────────────
void endCmd() {
  Serial1.write(0xFF);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
}

void sendCmd(const String& cmd) {
  Serial1.print(cmd);
  endCmd();
  delay(5);
}

// ── 온도 → RGB565 색상 변환 ────────────────────────────────────────────────
uint16_t tempToColor(float temp) {
  float r, g, b;
  if (temp < 35.0f) {
    float t = constrain((temp - 20.0f) / 15.0f, 0.0f, 1.0f);
    r = 0.0f; g = t; b = 1.0f;
  } else if (temp < 40.0f) {
    float t = (temp - 35.0f) / 5.0f;
    r = 0.0f; g = 1.0f; b = 1.0f - t;
  } else if (temp < 45.0f) {
    float t = (temp - 40.0f) / 5.0f;
    r = t; g = 1.0f; b = 0.0f;
  } else if (temp < 50.0f) {
    float t = (temp - 45.0f) / 5.0f;
    r = 1.0f; g = 1.0f - t; b = 0.0f;
  } else {
    r = 1.0f; g = 0.0f; b = 0.0f;
  }
  uint8_t r5 = (uint8_t)(r * 31);
  uint8_t g6 = (uint8_t)(g * 63);
  uint8_t b5 = (uint8_t)(b * 31);
  return (r5 << 11) | (g6 << 5) | b5;
}

// ── Nextion fill 명령 ─────────────────────────────────────────────────────
void fillRect(int x, int y, int w, int h, uint16_t color) {
  String cmd = "fill " + String(x) + "," + String(y) + "," +
               String(w) + "," + String(h) + "," + String(color);
  sendCmd(cmd);
}

// ── 쌍선형 보간 ────────────────────────────────────────────────────────────
float bilinear(float grid[8][8], float gx, float gy) {
  int x0 = (int)gx,        y0 = (int)gy;
  int x1 = min(x0 + 1, 7), y1 = min(y0 + 1, 7);
  float fx = gx - x0,      fy = gy - y0;
  return grid[y0][x0] * (1-fx) * (1-fy)
       + grid[y0][x1] *    fx  * (1-fy)
       + grid[y1][x0] * (1-fx) *    fy
       + grid[y1][x1] *    fx  *    fy;
}

float getMaxTemp(float pixels[64]) {
  float maxTemp = pixels[0];
  for (int i = 1; i < 64; i++)
    if (pixels[i] > maxTemp) maxTemp = pixels[i];
  return maxTemp;
}

// ── 상태 변화 시에만 t11 업데이트 ─────────────────────────────────────────
void updateStatus(float maxTemp) {
  int status;
  if (maxTemp >= 50.0f)      status = 2;
  else if (maxTemp >= 40.0f) status = 1;
  else                       status = 0;

  if (status == prevStatus) return;
  prevStatus = status;

  if (status == 2) {
    sendCmd("t11.pco=63488");   // 0xF800 빨강
    sendCmd("t11.txt=\"위험\"");
  } else if (status == 1) {
    sendCmd("t11.pco=65504");   // 0xFFE0 노랑
    sendCmd("t11.txt=\"경고\"");
  } else {
    sendCmd("t11.pco=2016");    // 0x07E0 초록
    sendCmd("t11.txt=\"정상\"");
  }
}

// ── Nextion → Arduino 이벤트 수신 (Serial1) ───────────────────────────────
void checkSerial() {
  while (Serial1.available() > 0) {
    byte data = Serial1.read();
    if (data == 0x00) {
      isThermalPage = false;
      prevStatus = -1; // 페이지 복귀 시 t11 강제 재갱신
    }
    if (data == 0x01) isThermalPage = true;
  }
}

// ── 8x8 열화상 렌더링 ─────────────────────────────────────────────────────
void renderHeatmap(float pixels[8][8]) {
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) {
      checkSerial();
      if (!isThermalPage) return;

      float gx   = col * 7.0f / (GRID_W - 1);
      float gy   = row * 7.0f / (GRID_H - 1);
      float temp = bilinear(pixels, gx, gy);
      uint16_t c = tempToColor(temp);
      fillRect(T0_X + col * CELL_W, T0_Y + row * CELL_H, CELL_W, CELL_H, c);
    }
  }
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);   // USB 디버그 (PC 모니터)
  Serial1.begin(115200);  // Nextion HMI (D0:RX, D1:TX)
  Wire.begin();           // I2C 초기화 (amg.begin 전 필수)

  // AMG8833 센서 초기화 — 연결될 때까지 대기
  while (!amg.begin(0x69, &Wire)) {
    Serial.println("AMG8833 not found. Retrying...");
    delay(1000);
  }
  Serial.println("AMG8833 ready.");

  delay(1000);
  sendCmd("page Thermal");
  delay(500);
  sendCmd("t0.txt=\"\"");
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
  checkSerial();

  if (isThermalPage) {
    float pixels[64];
    amg.readPixels(pixels);

    float grid[8][8];
    for (int i = 0; i < 8; i++)
      for (int j = 0; j < 8; j++)
        grid[i][j] = pixels[i * 8 + j];

    renderHeatmap(grid);

    if (isThermalPage) {
      float maxTemp = getMaxTemp(pixels);
      updateStatus(maxTemp);
    }
  }

  delay(50);
}
