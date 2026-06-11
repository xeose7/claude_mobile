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
const int   NTC_PIN1        = A0;
const int   NTC_PIN2        = A1;
const int   NTC_PIN3        = A2;
const float SERIES_RESISTOR = 10000.0f;
const float ADC_MAX         = 4095.0f;

// ── 2-포인트 캘리브레이션 (실측값) ────────────────────────────────────────
// 각 채널별로 0°C 및 50°C에서 측정한 NTC 저항값
const float NTC_R_0C[3]  = {33624.2f, 33614.6f, 33562.3f};  // Ω at 0°C
const float NTC_R_50C[3] = {3588.0f,  3590.2f,  3587.1f};   // Ω at 50°C
float ntcBeta[3];  // setup()에서 실측값으로 계산

// ── 상태 변수 ─────────────────────────────────────────────────────────────
bool          isThermalPage      = true;
bool          ntcNeedsRefresh    = true;   // 부팅 직후 NTC 즉시 1회 갱신
unsigned long t10t12ForceUntil   = 0;      // 이 시각까지 t10/t12 캐시 무효화 → 강제 재전송
int           prevStatus         = -1;
unsigned long previousMillis     = 0;
const unsigned long UPDATE_INTERVAL = 1000;

// ═══════════════════════════════════════════════════════════════════════════
// Nextion HMI 통신 (Serial1)
// ═══════════════════════════════════════════════════════════════════════════

void resetDisplayCache();    // 아래에 정의 (checkSerial에서 먼저 호출)
void resetHeatmapCache();    // 열화상 칸 색상 캐시 리셋 (페이지 진입 시 전체 재그리기)

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
      // NTC 페이지 진입 — isThermalPage 조건 없이 항상 처리.
      // 조건을 두면 Thermal→NTC 전환 신호(0x01)를 못 받았을 때 캐시가 리셋되지 않아
      // Nextion 페이지 초기화가 t10/t12를 지워도 재전송하지 못하는 문제 발생.
      Serial.println("[PAGE] → NTC");
      isThermalPage    = false;
      ntcNeedsRefresh  = true;
      prevStatus       = -1;
      resetDisplayCache();
      t10t12ForceUntil = millis() + 2000;  // 2초간 t10/t12 강제 재전송 (Nextion 초기화 덮어쓰기 방어)
    }
    if (data == 0x01) {
      // Thermal 페이지 진입 — 마찬가지로 조건 없이 항상 처리
      Serial.println("[PAGE] → Thermal");
      isThermalPage = true;
      prevStatus    = -1;
      resetHeatmapCache();
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// NTC 서미스터
// ═══════════════════════════════════════════════════════════════════════════

// ch: 0=CELL1(A0), 1=CELL2(A1), 2=CELL3(A2)
float readTemperature(int ch) {
  int pin = (ch == 0) ? NTC_PIN1 : (ch == 1) ? NTC_PIN2 : NTC_PIN3;

  // 16회 평균 → ADC 노이즈 감소 → 소수점 끝자리 안정화 → 깜빡임 방지
  long sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(pin);
  int adcValue = sum / 16;
  if (adcValue <= 0)    adcValue = 1;
  if (adcValue >= 4095) adcValue = 4094;

  float R = SERIES_RESISTOR * ((ADC_MAX / adcValue) - 1.0f);
  // 2-포인트 캘리브레이션 Beta 방정식 (0°C 기준점 사용)
  float tempK = 1.0f / (1.0f/273.15f + log(R / NTC_R_0C[ch]) / ntcBeta[ch]);
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

  // t10/t12 : 페이지 전환 후 2초간 캐시를 강제 무효화하여 반드시 재전송.
  // Nextion 페이지 초기화(Pre/Post-initialize)가 컴포넌트를 기본값으로 되돌려도
  // 이 기간 동안 매 업데이트마다 덮어씌우므로 값이 사라지지 않는다.
  if (millis() < t10t12ForceUntil) {
    cTxt[10] = "\x01"; cPco[10] = -1;
    cTxt[12] = "\x01"; cPco[12] = -1;
  }

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

// ─── 열화상 깜빡임 방지 캐시 ────────────────────────────────────────────────
// 각 칸의 직전 색상을 기억해두고, 색이 바뀐 칸만 다시 그린다.
// (매 루프 64칸 전체를 fill하면 화면(t0 영역)이 계속 깜빡임)
int heatCache[GRID_H][GRID_W];

void resetHeatmapCache() {
  for (int r = 0; r < GRID_H; r++)
    for (int c = 0; c < GRID_W; c++)
      heatCache[r][c] = -1;   // 다음 렌더 시 전체 강제 재그리기
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

// 한 번(=1 루프) 호출 시 최대 이 개수만큼만 fill 한다.
// 장면이 크게 바뀌어 많은 칸이 동시에 변해도 명령을 분산 전송하여
// Nextion 수신 버퍼 폭주(→ 사각형이 엉키는 화면 깨짐)를 방지한다.
// 못 그린 칸은 캐시(heatCache)에 반영되지 않으므로 다음 루프에서 이어 그려진다.
const int MAX_FILLS_PER_PASS = 12;

void renderHeatmap(float pixels[8][8]) {
  int budget = MAX_FILLS_PER_PASS;
  for (int row = 0; row < GRID_H; row++) {
    for (int col = 0; col < GRID_W; col++) {
      checkSerial();
      if (!isThermalPage) return;   // 페이지 이탈 시 중단

      float gx   = col * 7.0f / (GRID_W - 1);
      float gy   = row * 7.0f / (GRID_H - 1);
      // 0.5°C 단위 양자화: AMG8833 픽셀 노이즈(±0.3°C)로 인한 미세 색상 변화를 억제하여
      // heatCache 미스를 줄이고 t0 영역 깜빡임을 제거한다.
      float temp = roundf(bilinear(pixels, gx, gy) * 2.0f) / 2.0f;
      int color  = (int)tempToColor(temp);

      if (heatCache[row][col] == color) continue;   // 색 변화 없으면 건너뜀 → 깜빡임 없음
      heatCache[row][col] = color;
      fillRect(T0_X + col*CELL_W, T0_Y + row*CELL_H, CELL_W, CELL_H, color);

      if (--budget <= 0) return;   // 이번 패스 한도 도달 → 나머지는 다음 루프에서
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

  // 2-포인트 캘리브레이션 Beta 계산 (B = ln(R0/R50) / (1/T0 - 1/T50))
  for (int i = 0; i < 3; i++) {
    ntcBeta[i] = log(NTC_R_0C[i] / NTC_R_50C[i])
                 / (1.0f / 273.15f - 1.0f / 323.15f);
    Serial.print("ntcBeta["); Serial.print(i); Serial.print("]=");
    Serial.print(ntcBeta[i], 1); Serial.println();
  }

  resetDisplayCache();   // 텍스트/색상 캐시 초기화 → 첫 갱신 시 전체 1회 전송
  resetHeatmapCache();   // 열화상 칸 캐시 초기화 → 첫 렌더 시 전체 1회 그리기

  // bkcmd=0 : 명령 성공/실패 응답(0x01/0x00) 자동 회신 비활성화.
  // 이 응답이 페이지 전환 바이트(0x00/0x01)와 충돌해 t10/t12 미갱신을 유발하므로 반드시 꺼야 함.
  sendCmd("bkcmd=0");
  delay(50);

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

    float t1 = readTemperature(0);
    float t2 = readTemperature(1);
    float t3 = readTemperature(2);

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

  // 짧은 루프 주기 → fill 버짓 분산이 자주 이어져 열화상이 매끄럽게 갱신된다.
  // (명령 간 간격은 sendCmd의 delay(5)가 보장하므로 버퍼는 안전)
  delay(20);
}
