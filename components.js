/* components.js
 * 배선도 부품 정의 라이브러리.
 * 각 부품은 SVG 심볼 그리기 함수와 단자(terminal) 좌표를 제공한다.
 * 좌표계는 부품 로컬 기준(0,0 = 중심). 단자는 로컬 좌표로 정의하고,
 * 회전/이동은 앱에서 변환(transform)으로 처리한다.
 *
 * draw(): 심볼 본체에 들어갈 SVG 마크업 문자열을 반환.
 * terminals: [{ id, x, y }] — 배선이 붙는 연결점.
 * size: { w, h } — 바운딩 박스(선택 영역/충돌 판정용).
 */
(function (global) {
  'use strict';

  // 공통 스타일 — 심볼 본체 선
  const S = 'fill="none" stroke="var(--symbol)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"';
  const FILL = 'fill="var(--symbol)" stroke="none"';

  // 단자에서 본체로 이어지는 짧은 리드선
  function lead(x1, y1, x2, y2) {
    return `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" ${S}/>`;
  }

  const COMPONENTS = {
    /* ---------- 전원 ---------- */
    battery: {
      name: '전지', category: '전원', keywords: 'battery cell 전지 배터리 전원',
      size: { w: 80, h: 40 },
      terminals: [{ id: 'p', x: -40, y: 0 }, { id: 'n', x: 40, y: 0 }],
      draw() {
        return `
          ${lead(-40, 0, -8, 0)}
          ${lead(8, 0, 40, 0)}
          <line x1="-8" y1="-16" x2="-8" y2="16" ${S}/>
          <line x1="8" y1="-9" x2="8" y2="9" ${S}/>
          <text x="-14" y="-20" class="sym-pole">+</text>
          <text x="14" y="-20" class="sym-pole">−</text>`;
      }
    },
    dcsource: {
      name: 'DC 전원', category: '전원', keywords: 'dc source 직류 전원 vcc',
      size: { w: 56, h: 56 },
      terminals: [{ id: 'p', x: 0, y: -28 }, { id: 'n', x: 0, y: 28 }],
      draw() {
        return `
          ${lead(0, -28, 0, -20)}
          ${lead(0, 20, 0, 28)}
          <circle cx="0" cy="0" r="20" ${S}/>
          <line x1="-10" y1="-7" x2="10" y2="-7" ${S}/>
          <text x="0" y="13" class="sym-pole">−</text>`;
      }
    },
    acsource: {
      name: 'AC 전원', category: '전원', keywords: 'ac source 교류 전원',
      size: { w: 56, h: 56 },
      terminals: [{ id: 'a', x: 0, y: -28 }, { id: 'b', x: 0, y: 28 }],
      draw() {
        return `
          ${lead(0, -28, 0, -20)}
          ${lead(0, 20, 0, 28)}
          <circle cx="0" cy="0" r="20" ${S}/>
          <path d="M -11 0 q 5.5 -11 11 0 q 5.5 11 11 0" ${S}/>`;
      }
    },
    ground: {
      name: '접지', category: '전원', keywords: 'ground gnd 접지 어스',
      size: { w: 36, h: 40 },
      terminals: [{ id: 'g', x: 0, y: -20 }],
      draw() {
        return `
          ${lead(0, -20, 0, 4)}
          <line x1="-14" y1="4" x2="14" y2="4" ${S}/>
          <line x1="-9" y1="11" x2="9" y2="11" ${S}/>
          <line x1="-4" y1="18" x2="4" y2="18" ${S}/>`;
      }
    },

    /* ---------- 수동 소자 ---------- */
    resistor: {
      name: '저항', category: '수동소자', keywords: 'resistor 저항 r',
      size: { w: 80, h: 24 },
      terminals: [{ id: 'a', x: -40, y: 0 }, { id: 'b', x: 40, y: 0 }],
      draw() {
        return `
          ${lead(-40, 0, -24, 0)}
          ${lead(24, 0, 40, 0)}
          <rect x="-24" y="-9" width="48" height="18" rx="2" ${S}/>`;
      }
    },
    resistor_zig: {
      name: '저항(지그재그)', category: '수동소자', keywords: 'resistor zigzag 저항',
      size: { w: 80, h: 24 },
      terminals: [{ id: 'a', x: -40, y: 0 }, { id: 'b', x: 40, y: 0 }],
      draw() {
        return `
          ${lead(-40, 0, -24, 0)}
          ${lead(24, 0, 40, 0)}
          <polyline points="-24,0 -20,-9 -12,9 -4,-9 4,9 12,-9 20,9 24,0" ${S}/>`;
      }
    },
    capacitor: {
      name: '커패시터', category: '수동소자', keywords: 'capacitor 커패시터 콘덴서 c',
      size: { w: 60, h: 30 },
      terminals: [{ id: 'a', x: -30, y: 0 }, { id: 'b', x: 30, y: 0 }],
      draw() {
        return `
          ${lead(-30, 0, -5, 0)}
          ${lead(5, 0, 30, 0)}
          <line x1="-5" y1="-13" x2="-5" y2="13" ${S}/>
          <line x1="5" y1="-13" x2="5" y2="13" ${S}/>`;
      }
    },
    inductor: {
      name: '인덕터', category: '수동소자', keywords: 'inductor coil 인덕터 코일 l',
      size: { w: 80, h: 24 },
      terminals: [{ id: 'a', x: -40, y: 0 }, { id: 'b', x: 40, y: 0 }],
      draw() {
        return `
          ${lead(-40, 0, -24, 0)}
          ${lead(24, 0, 40, 0)}
          <path d="M -24 0 a 6 6 0 0 1 12 0 a 6 6 0 0 1 12 0 a 6 6 0 0 1 12 0 a 6 6 0 0 1 12 0" ${S}/>`;
      }
    },

    /* ---------- 스위치 / 보호 ---------- */
    switch_spst: {
      name: '스위치(SPST)', category: '스위치', keywords: 'switch spst 스위치',
      size: { w: 80, h: 30 },
      terminals: [{ id: 'a', x: -40, y: 0 }, { id: 'b', x: 40, y: 0 }],
      draw() {
        return `
          ${lead(-40, 0, -18, 0)}
          ${lead(18, 0, 40, 0)}
          <circle cx="-18" cy="0" r="3" ${FILL}/>
          <circle cx="18" cy="0" r="3" ${FILL}/>
          <line x1="-18" y1="0" x2="14" y2="-14" ${S}/>`;
      }
    },
    pushbutton: {
      name: '푸시버튼', category: '스위치', keywords: 'push button 버튼 푸시버튼',
      size: { w: 80, h: 40 },
      terminals: [{ id: 'a', x: -40, y: 0 }, { id: 'b', x: 40, y: 0 }],
      draw() {
        return `
          ${lead(-40, 0, -16, 0)}
          ${lead(16, 0, 40, 0)}
          <line x1="-16" y1="0" x2="-16" y2="-2" ${S}/>
          <line x1="-18" y1="-8" x2="18" y2="-8" ${S}/>
          <line x1="0" y1="-8" x2="0" y2="-18" ${S}/>
          <line x1="-10" y1="-18" x2="10" y2="-18" ${S}/>`;
      }
    },
    fuse: {
      name: '퓨즈', category: '스위치', keywords: 'fuse 퓨즈 보호',
      size: { w: 80, h: 24 },
      terminals: [{ id: 'a', x: -40, y: 0 }, { id: 'b', x: 40, y: 0 }],
      draw() {
        return `
          ${lead(-40, 0, -22, 0)}
          ${lead(22, 0, 40, 0)}
          <rect x="-22" y="-8" width="44" height="16" rx="2" ${S}/>
          <line x1="-22" y1="0" x2="22" y2="0" ${S}/>`;
      }
    },

    /* ---------- 출력 / 부하 ---------- */
    lamp: {
      name: '램프', category: '부하', keywords: 'lamp light bulb 램프 전구 조명',
      size: { w: 56, h: 56 },
      terminals: [{ id: 'a', x: -28, y: 0 }, { id: 'b', x: 28, y: 0 }],
      draw() {
        return `
          ${lead(-28, 0, -16, 0)}
          ${lead(16, 0, 28, 0)}
          <circle cx="0" cy="0" r="16" ${S}/>
          <line x1="-11" y1="-11" x2="11" y2="11" ${S}/>
          <line x1="-11" y1="11" x2="11" y2="-11" ${S}/>`;
      }
    },
    led: {
      name: 'LED', category: '부하', keywords: 'led 다이오드 발광',
      size: { w: 60, h: 44 },
      terminals: [{ id: 'a', x: -30, y: 0 }, { id: 'b', x: 30, y: 0 }],
      draw() {
        return `
          ${lead(-30, 0, -12, 0)}
          ${lead(12, 0, 30, 0)}
          <polygon points="-12,-12 -12,12 12,0" ${S}/>
          <line x1="12" y1="-12" x2="12" y2="12" ${S}/>
          <line x1="6" y1="-16" x2="14" y2="-24" ${S}/>
          <polygon points="14,-24 10,-22 13,-19" ${FILL}/>
          <line x1="0" y1="-14" x2="8" y2="-22" ${S}/>
          <polygon points="8,-22 4,-20 7,-17" ${FILL}/>`;
      }
    },
    motor: {
      name: '모터', category: '부하', keywords: 'motor 모터 m',
      size: { w: 56, h: 56 },
      terminals: [{ id: 'a', x: -28, y: 0 }, { id: 'b', x: 28, y: 0 }],
      draw() {
        return `
          ${lead(-28, 0, -18, 0)}
          ${lead(18, 0, 28, 0)}
          <circle cx="0" cy="0" r="18" ${S}/>
          <text x="0" y="6" class="sym-text">M</text>`;
      }
    },
    buzzer: {
      name: '부저', category: '부하', keywords: 'buzzer 부저 스피커 알람',
      size: { w: 60, h: 44 },
      terminals: [{ id: 'a', x: -30, y: 0 }, { id: 'b', x: 30, y: 0 }],
      draw() {
        return `
          ${lead(-30, 0, -16, 0)}
          ${lead(16, 0, 30, 0)}
          <path d="M -16 -14 L -16 14 A 16 16 0 0 0 -16 -14 Z" ${S}/>`;
      }
    },

    /* ---------- 반도체 ---------- */
    diode: {
      name: '다이오드', category: '반도체', keywords: 'diode 다이오드 d',
      size: { w: 64, h: 30 },
      terminals: [{ id: 'a', x: -32, y: 0 }, { id: 'k', x: 32, y: 0 }],
      draw() {
        return `
          ${lead(-32, 0, -12, 0)}
          ${lead(12, 0, 32, 0)}
          <polygon points="-12,-12 -12,12 12,0" ${S}/>
          <line x1="12" y1="-12" x2="12" y2="12" ${S}/>`;
      }
    },
    transistor_npn: {
      name: '트랜지스터(NPN)', category: '반도체', keywords: 'transistor npn bjt 트랜지스터',
      size: { w: 60, h: 64 },
      terminals: [{ id: 'b', x: -30, y: 0 }, { id: 'c', x: 16, y: -32 }, { id: 'e', x: 16, y: 32 }],
      draw() {
        return `
          ${lead(-30, 0, -8, 0)}
          <line x1="-8" y1="-16" x2="-8" y2="16" ${S}/>
          <line x1="-8" y1="-9" x2="14" y2="-22" ${S}/>
          <line x1="-8" y1="9" x2="14" y2="22" ${S}/>
          ${lead(14, -22, 16, -32)}
          ${lead(14, 22, 16, 32)}
          <polygon points="14,22 6,18 11,13" ${FILL}/>`;
      }
    },

    /* ---------- 측정 / 커넥터 ---------- */
    voltmeter: {
      name: '전압계', category: '계측', keywords: 'voltmeter 전압계 v',
      size: { w: 56, h: 56 },
      terminals: [{ id: 'a', x: -28, y: 0 }, { id: 'b', x: 28, y: 0 }],
      draw() {
        return `
          ${lead(-28, 0, -18, 0)}
          ${lead(18, 0, 28, 0)}
          <circle cx="0" cy="0" r="18" ${S}/>
          <text x="0" y="6" class="sym-text">V</text>`;
      }
    },
    ammeter: {
      name: '전류계', category: '계측', keywords: 'ammeter 전류계 a',
      size: { w: 56, h: 56 },
      terminals: [{ id: 'a', x: -28, y: 0 }, { id: 'b', x: 28, y: 0 }],
      draw() {
        return `
          ${lead(-28, 0, -18, 0)}
          ${lead(18, 0, 28, 0)}
          <circle cx="0" cy="0" r="18" ${S}/>
          <text x="0" y="6" class="sym-text">A</text>`;
      }
    },
    terminal: {
      name: '단자', category: '커넥터', keywords: 'terminal node 단자 노드 점',
      size: { w: 24, h: 24 },
      terminals: [{ id: 't', x: 0, y: 0 }],
      draw() {
        return `<circle cx="0" cy="0" r="5" fill="var(--canvas-bg)" stroke="var(--symbol)" stroke-width="2"/>`;
      }
    },
    junction: {
      name: '접속점', category: '커넥터', keywords: 'junction dot 접속점 분기',
      size: { w: 20, h: 20 },
      terminals: [
        { id: 'n', x: 0, y: -10 }, { id: 'e', x: 10, y: 0 },
        { id: 's', x: 0, y: 10 }, { id: 'w', x: -10, y: 0 }
      ],
      draw() {
        return `
          ${lead(0, -10, 0, 10)}
          ${lead(-10, 0, 10, 0)}
          <circle cx="0" cy="0" r="4" ${FILL}/>`;
      }
    },
    connector2: {
      name: '커넥터(2핀)', category: '커넥터', keywords: 'connector plug 커넥터 핀',
      size: { w: 40, h: 50 },
      terminals: [{ id: 'p1', x: -20, y: -12 }, { id: 'p2', x: -20, y: 12 }],
      draw() {
        return `
          ${lead(-20, -12, -6, -12)}
          ${lead(-20, 12, -6, 12)}
          <rect x="-6" y="-22" width="16" height="44" rx="2" ${S}/>
          <circle cx="-2" cy="-12" r="2.5" ${FILL}/>
          <circle cx="-2" cy="12" r="2.5" ${FILL}/>`;
      }
    }
  };

  global.WireDrawComponents = COMPONENTS;
})(window);
