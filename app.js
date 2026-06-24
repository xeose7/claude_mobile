/* app.js — WireDraw 배선도 에디터 핵심 로직 */
(function () {
  'use strict';

  const SVGNS = 'http://www.w3.org/2000/svg';
  const COMPONENTS = window.WireDrawComponents;
  const GRID = 20;

  /* ---------------- 상태 ---------------- */
  const state = {
    components: [],          // { id, type, x, y, rot, label, value }
    wires: [],               // { id, from:{comp,term}|null, to:{comp,term}|null, points:[{x,y}], color }
    selection: new Set(),    // 선택된 id 집합
    selKind: null,           // 'component' | 'wire'
    tool: 'select',
    view: { x: 0, y: 0, scale: 1 },
    snap: true,
    showGrid: true,
    nextId: 1,
  };

  let history = [];
  let historyIdx = -1;

  /* ---------------- DOM 참조 ---------------- */
  const svg = document.getElementById('canvas');
  const canvasWrap = document.querySelector('.canvas-wrap');
  const hintEl = document.getElementById('hint');
  const statusCoords = document.getElementById('status-coords');
  const statusInfo = document.getElementById('status-info');
  const statusCount = document.getElementById('status-count');
  const zoomLevelEl = document.getElementById('zoom-level');

  // SVG 레이어 구성
  const gGrid = el('g', { id: 'layer-grid' });
  const gWires = el('g', { id: 'layer-wires' });
  const gComponents = el('g', { id: 'layer-components' });
  const gOverlay = el('g', { id: 'layer-overlay' }); // 미리보기, 핸들
  const gWorld = el('g', { id: 'world' });
  gWorld.append(gGrid, gWires, gComponents, gOverlay);
  svg.append(gWorld);

  function el(tag, attrs) {
    const e = document.createElementNS(SVGNS, tag);
    if (attrs) for (const k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  }

  function uid(prefix) { return prefix + (state.nextId++); }

  /* ---------------- 좌표 변환 ---------------- */
  function screenToWorld(sx, sy) {
    const rect = svg.getBoundingClientRect();
    return {
      x: (sx - rect.left - state.view.x) / state.view.scale,
      y: (sy - rect.top - state.view.y) / state.view.scale,
    };
  }
  function snap(v) { return state.snap ? Math.round(v / GRID) * GRID : v; }
  function snapPt(p) { return { x: snap(p.x), y: snap(p.y) }; }

  function applyView() {
    gWorld.setAttribute('transform',
      `translate(${state.view.x},${state.view.y}) scale(${state.view.scale})`);
    zoomLevelEl.textContent = Math.round(state.view.scale * 100) + '%';
    renderGrid();
  }

  /* ---------------- 부품 / 단자 기하 ---------------- */
  // 회전된 단자의 월드 좌표
  function termWorld(comp, term) {
    const a = comp.rot * Math.PI / 180;
    const cos = Math.cos(a), sin = Math.sin(a);
    return {
      x: comp.x + term.x * cos - term.y * sin,
      y: comp.y + term.x * sin + term.y * cos,
    };
  }
  function getComp(id) { return state.components.find(c => c.id === id); }
  function getWire(id) { return state.wires.find(w => w.id === id); }

  // 특정 단자에 연결된 배선 끝점 좌표 반환
  function endpointWorld(ref) {
    const comp = getComp(ref.comp);
    if (!comp) return null;
    const def = COMPONENTS[comp.type];
    const term = def.terminals.find(t => t.id === ref.term);
    if (!term) return null;
    return termWorld(comp, term);
  }

  /* ---------------- 그리드 ---------------- */
  function renderGrid() {
    gGrid.textContent = '';
    if (!state.showGrid) return;
    const rect = svg.getBoundingClientRect();
    const tl = screenToWorld(rect.left, rect.top);
    const br = screenToWorld(rect.right, rect.bottom);
    const startX = Math.floor(tl.x / GRID) * GRID;
    const startY = Math.floor(tl.y / GRID) * GRID;
    // 너무 촘촘하면 생략
    if ((br.x - tl.x) / GRID > 400) return;
    for (let x = startX; x <= br.x; x += GRID) {
      const strong = Math.round(x / GRID) % 5 === 0;
      const line = el('line', { x1: x, y1: tl.y, x2: x, y2: br.y,
        class: 'grid-line' + (strong ? ' strong' : '') });
      gGrid.appendChild(line);
    }
    for (let y = startY; y <= br.y; y += GRID) {
      const strong = Math.round(y / GRID) % 5 === 0;
      const line = el('line', { x1: tl.x, y1: y, x2: br.x, y2: y,
        class: 'grid-line' + (strong ? ' strong' : '') });
      gGrid.appendChild(line);
    }
  }

  /* ---------------- 렌더링 ---------------- */
  function render() {
    renderComponents();
    renderWires();
    renderOverlay();
    updateStatus();
  }

  function renderComponents() {
    gComponents.textContent = '';
    for (const comp of state.components) {
      const def = COMPONENTS[comp.type];
      const g = el('g', {
        class: 'component' + (state.selection.has(comp.id) ? ' selected' : ''),
        transform: `translate(${comp.x},${comp.y}) rotate(${comp.rot})`,
        'data-id': comp.id,
      });
      g.style.setProperty('--symbol', comp.color || 'var(--symbol)');

      // 선택 박스
      const pad = 8;
      g.appendChild(el('rect', {
        class: 'selbox',
        x: -def.size.w / 2 - pad, y: -def.size.h / 2 - pad,
        width: def.size.w + pad * 2, height: def.size.h + pad * 2,
        rx: 6,
      }));

      // 심볼 본체
      const body = el('g', {});
      body.innerHTML = def.draw();
      g.appendChild(body);

      // 히트박스 (드래그/선택 영역)
      g.appendChild(el('rect', {
        class: 'hitbox',
        x: -def.size.w / 2 - pad, y: -def.size.h / 2 - pad,
        width: def.size.w + pad * 2, height: def.size.h + pad * 2,
      }));

      // 라벨 / 값 (회전 보정: 항상 본체 아래에 수평 유지)
      if (comp.label || comp.value) {
        const lg = el('g', { transform: `rotate(${-comp.rot})` });
        const baseY = def.size.h / 2 + 16;
        if (comp.label)
          lg.appendChild(text(0, baseY, comp.label, 'comp-label'));
        if (comp.value)
          lg.appendChild(text(0, baseY + (comp.label ? 14 : 0), comp.value, 'comp-value'));
        g.appendChild(lg);
      }

      // 단자 점
      for (const t of def.terminals) {
        g.appendChild(el('circle', {
          class: 'terminal-pt', cx: t.x, cy: t.y, r: 5,
          'data-comp': comp.id, 'data-term': t.id,
        }));
      }
      gComponents.appendChild(g);
    }
  }

  function text(x, y, str, cls) {
    const t = el('text', { x, y, class: cls });
    t.textContent = str;
    return t;
  }

  // 두 점 사이 직교(맨해튼) 경로 — 자동 라우팅
  function orthPath(a, b) {
    const midX = (a.x + b.x) / 2;
    return [a, { x: midX, y: a.y }, { x: midX, y: b.y }, b];
  }

  function wirePoints(w) {
    let pts = [];
    const start = w.from ? endpointWorld(w.from) : (w.points[0] || null);
    const end = w.to ? endpointWorld(w.to) : (w.points[w.points.length - 1] || null);
    if (w.manual && w.points && w.points.length >= 2) {
      pts = w.points.slice();
      if (start) pts[0] = start;
      if (end) pts[pts.length - 1] = end;
    } else if (start && end) {
      pts = orthPath(start, end);
    } else {
      pts = w.points.slice();
    }
    return pts;
  }

  function ptsToPath(pts) {
    if (!pts.length) return '';
    return 'M ' + pts.map(p => `${p.x} ${p.y}`).join(' L ');
  }

  function renderWires() {
    gWires.textContent = '';
    for (const w of state.wires) {
      const pts = wirePoints(w);
      if (pts.length < 2) continue;
      const d = ptsToPath(pts);
      // 넓은 히트 영역
      const hit = el('path', { class: 'wire-hit', d, 'data-id': w.id });
      const path = el('path', {
        class: 'wire' + (state.selection.has(w.id) ? ' selected' : ''),
        d, 'data-id': w.id,
      });
      if (w.color) path.style.stroke = w.color;
      gWires.appendChild(hit);
      gWires.appendChild(path);
    }
  }

  function renderOverlay() {
    gOverlay.textContent = '';
    // 선택된 단일 배선의 꼭짓점 핸들
    if (state.selKind === 'wire' && state.selection.size === 1) {
      const w = getWire([...state.selection][0]);
      if (w) {
        const pts = wirePoints(w);
        pts.forEach((p, i) => {
          // 양 끝이 부품에 연결돼 있으면 그 핸들은 표시하지 않음
          if (i === 0 && w.from) return;
          if (i === pts.length - 1 && w.to) return;
          gOverlay.appendChild(el('circle', {
            class: 'wire-vertex', cx: p.x, cy: p.y, r: 5,
            'data-wire': w.id, 'data-vidx': i,
          }));
        });
      }
    }
  }

  /* ---------------- 상태바 ---------------- */
  function updateStatus() {
    statusCount.textContent = `요소 ${state.components.length} · 배선 ${state.wires.length}`;
  }
  let hintTimer;
  function hint(msg) {
    hintEl.textContent = msg;
    hintEl.classList.add('show');
    clearTimeout(hintTimer);
    hintTimer = setTimeout(() => hintEl.classList.remove('show'), 1800);
  }

  /* ---------------- 히스토리 ---------------- */
  function snapshot() {
    return JSON.stringify({
      components: state.components, wires: state.wires, nextId: state.nextId,
    });
  }
  function pushHistory() {
    history = history.slice(0, historyIdx + 1);
    history.push(snapshot());
    if (history.length > 100) history.shift();
    historyIdx = history.length - 1;
  }
  function restore(snap) {
    const data = JSON.parse(snap);
    state.components = data.components;
    state.wires = data.wires;
    state.nextId = data.nextId;
    state.selection.clear();
    state.selKind = null;
    render();
    refreshInspector();
  }
  function undo() {
    if (historyIdx > 0) { historyIdx--; restore(history[historyIdx]); hint('실행 취소'); }
  }
  function redo() {
    if (historyIdx < history.length - 1) { historyIdx++; restore(history[historyIdx]); hint('다시 실행'); }
  }

  /* ---------------- 부품 추가 ---------------- */
  function addComponent(type, x, y) {
    const comp = {
      id: uid('c'), type,
      x: snap(x), y: snap(y),
      rot: 0, label: '', value: '', color: '',
    };
    state.components.push(comp);
    pushHistory();
    selectOnly('component', comp.id);
    render();
    refreshInspector();
    return comp;
  }

  /* ---------------- 선택 ---------------- */
  function selectOnly(kind, id) {
    state.selection.clear();
    state.selKind = kind;
    if (id != null) state.selection.add(id);
    render();
    refreshInspector();
  }
  function toggleSelect(kind, id) {
    if (state.selKind !== kind) { state.selection.clear(); state.selKind = kind; }
    if (state.selection.has(id)) state.selection.delete(id);
    else state.selection.add(id);
    if (!state.selection.size) state.selKind = null;
    render();
    refreshInspector();
  }
  function clearSelection() {
    state.selection.clear(); state.selKind = null;
    render(); refreshInspector();
  }

  function deleteSelection() {
    if (!state.selection.size) return;
    if (state.selKind === 'component') {
      for (const id of state.selection) {
        state.components = state.components.filter(c => c.id !== id);
        // 연결된 배선도 분리(끝점을 좌표로 고정)
        for (const w of state.wires) {
          if (w.from && w.from.comp === id) w.from = null;
          if (w.to && w.to.comp === id) w.to = null;
        }
      }
      // 양끝 모두 끊긴 떠도는 배선 제거
      state.wires = state.wires.filter(w => !(w.from === null && w.to === null && !w.manual));
    } else if (state.selKind === 'wire') {
      for (const id of state.selection) state.wires = state.wires.filter(w => w.id !== id);
    }
    clearSelection();
    pushHistory();
    hint('삭제됨');
  }

  function duplicateSelection() {
    if (state.selKind !== 'component' || !state.selection.size) return;
    const newIds = [];
    for (const id of [...state.selection]) {
      const c = getComp(id);
      if (!c) continue;
      const copy = { ...c, id: uid('c'), x: c.x + GRID, y: c.y + GRID };
      state.components.push(copy);
      newIds.push(copy.id);
    }
    state.selection = new Set(newIds);
    pushHistory();
    render(); refreshInspector();
    hint('복제됨');
  }

  function rotateSelection() {
    if (state.selKind !== 'component') return;
    for (const id of state.selection) {
      const c = getComp(id);
      if (c) c.rot = (c.rot + 90) % 360;
    }
    pushHistory();
    render(); refreshInspector();
  }

  /* ---------------- 도구 전환 ---------------- */
  function setTool(tool) {
    state.tool = tool;
    document.querySelectorAll('.tool-btn').forEach(b =>
      b.classList.toggle('active', b.dataset.tool === tool));
    canvasWrap.classList.toggle('show-terminals', tool === 'wire');
    svg.style.cursor = tool === 'pan' ? 'grab' : (tool === 'wire' ? 'crosshair' : 'default');
    const msgs = {
      select: '요소를 클릭해 선택하고 드래그로 이동합니다.',
      wire: '단자(초록 점)에서 다른 단자로 드래그해 배선합니다.',
      pan: '드래그하여 화면을 이동합니다.',
      delete: '요소나 배선을 클릭하면 삭제됩니다.',
    };
    statusInfo.textContent = msgs[tool] || '';
  }

  /* ---------------- 포인터 인터랙션 ---------------- */
  let drag = null; // 진행 중 동작 상태

  function findTerminalAt(target) {
    if (target && target.classList && target.classList.contains('terminal-pt'))
      return { comp: target.getAttribute('data-comp'), term: target.getAttribute('data-term') };
    return null;
  }
  // 포인터 캡처 중에는 e.target이 svg로 고정되므로 실제 좌표 아래 요소를 직접 조회한다.
  function elementUnder(clientX, clientY) {
    return document.elementFromPoint(clientX, clientY);
  }

  svg.addEventListener('pointerdown', onPointerDown);
  svg.addEventListener('pointermove', onPointerMove);
  svg.addEventListener('pointerup', onPointerUp);
  svg.addEventListener('pointercancel', onPointerUp);

  // 핀치 줌용 멀티터치 추적
  const activePointers = new Map();
  let pinch = null;

  function onPointerDown(e) {
    svg.setPointerCapture(e.pointerId);
    activePointers.set(e.pointerId, { x: e.clientX, y: e.clientY });

    if (activePointers.size === 2) {
      const [a, b] = [...activePointers.values()];
      pinch = {
        dist: Math.hypot(a.x - b.x, a.y - b.y),
        scale: state.view.scale,
        cx: (a.x + b.x) / 2, cy: (a.y + b.y) / 2,
      };
      drag = null;
      return;
    }

    const world = screenToWorld(e.clientX, e.clientY);
    const target = e.target;
    const tool = state.tool;

    // 배선 꼭짓점 핸들 드래그 (어느 도구에서나)
    if (target.classList.contains('wire-vertex')) {
      drag = { type: 'vertex', wireId: target.getAttribute('data-wire'),
        vidx: +target.getAttribute('data-vidx') };
      return;
    }

    // 팬 도구 또는 빈 공간 미들/스페이스
    if (tool === 'pan') {
      drag = { type: 'pan', startX: e.clientX, startY: e.clientY,
        vx: state.view.x, vy: state.view.y };
      svg.style.cursor = 'grabbing';
      return;
    }

    const term = findTerminalAt(target);

    // 배선 도구: 단자에서 시작
    if (tool === 'wire') {
      if (term) {
        const sp = endpointWorld(term);
        drag = { type: 'wire-new', from: term, cur: sp };
        const prev = el('path', { class: 'wire-preview', d: ptsToPath([sp, sp]) });
        gOverlay.appendChild(prev);
        drag.previewEl = prev;
      } else {
        // 빈 곳이면 화면 이동으로 대체
        drag = { type: 'pan', startX: e.clientX, startY: e.clientY,
          vx: state.view.x, vy: state.view.y };
      }
      return;
    }

    const compG = target.closest('.component');
    const wireEl = target.classList.contains('wire-hit') || target.classList.contains('wire')
      ? target : null;

    // 삭제 도구
    if (tool === 'delete') {
      if (compG) {
        const id = compG.getAttribute('data-id');
        selectOnly('component', id); deleteSelection();
      } else if (wireEl) {
        selectOnly('wire', wireEl.getAttribute('data-id')); deleteSelection();
      }
      return;
    }

    // 선택 도구
    if (tool === 'select') {
      if (compG) {
        const id = compG.getAttribute('data-id');
        if (e.shiftKey) toggleSelect('component', id);
        else if (!state.selection.has(id)) selectOnly('component', id);
        // 이동 준비 (선택된 모든 부품)
        const moving = [...state.selection].map(cid => {
          const c = getComp(cid);
          return c ? { id: cid, ox: c.x, oy: c.y } : null;
        }).filter(Boolean);
        drag = { type: 'move', startWorld: world, moving, moved: false };
      } else if (wireEl) {
        const id = wireEl.getAttribute('data-id');
        if (e.shiftKey) toggleSelect('wire', id);
        else selectOnly('wire', id);
        drag = { type: 'maybe-pan', startX: e.clientX, startY: e.clientY,
          vx: state.view.x, vy: state.view.y };
      } else {
        // 빈 공간: 선택 해제 + 박스 선택 시작
        if (!e.shiftKey) clearSelection();
        drag = { type: 'select-box', start: world,
          rect: el('rect', { class: 'selbox', style: 'stroke:var(--select-box);stroke-width:1;stroke-dasharray:4 2;fill:rgba(77,171,247,.08)' }) };
        gOverlay.appendChild(drag.rect);
      }
    }
  }

  function onPointerMove(e) {
    if (activePointers.has(e.pointerId))
      activePointers.set(e.pointerId, { x: e.clientX, y: e.clientY });

    // 핀치 줌
    if (pinch && activePointers.size >= 2) {
      const [a, b] = [...activePointers.values()];
      const dist = Math.hypot(a.x - b.x, a.y - b.y);
      const newScale = clamp(pinch.scale * (dist / pinch.dist), 0.2, 4);
      zoomAt(pinch.cx, pinch.cy, newScale);
      return;
    }

    const world = screenToWorld(e.clientX, e.clientY);
    statusCoords.textContent = `x: ${Math.round(world.x)}, y: ${Math.round(world.y)}`;

    // 배선 도구: 단자 하이라이트
    if (state.tool === 'wire' && !drag) {
      highlightTerminal(e.target);
    }

    if (!drag) return;

    if (drag.type === 'pan' || drag.type === 'maybe-pan') {
      const dx = e.clientX - drag.startX, dy = e.clientY - drag.startY;
      if (drag.type === 'maybe-pan' && Math.hypot(dx, dy) < 4) return;
      state.view.x = drag.vx + dx;
      state.view.y = drag.vy + dy;
      applyView();
      return;
    }

    if (drag.type === 'move') {
      const dx = world.x - drag.startWorld.x;
      const dy = world.y - drag.startWorld.y;
      if (Math.abs(dx) > 1 || Math.abs(dy) > 1) drag.moved = true;
      for (const m of drag.moving) {
        const c = getComp(m.id);
        if (c) { c.x = snap(m.ox + dx); c.y = snap(m.oy + dy); }
      }
      renderComponents(); renderWires(); renderOverlay();
      return;
    }

    if (drag.type === 'wire-new') {
      const hot = highlightTerminal(elementUnder(e.clientX, e.clientY));
      drag.cur = hot ? endpointWorld(hot) : snapPt(world);
      drag.snapTerm = hot || null;
      const start = endpointWorld(drag.from);
      drag.previewEl.setAttribute('d', ptsToPath(orthPath(start, drag.cur)));
      return;
    }

    if (drag.type === 'vertex') {
      const w = getWire(drag.wireId);
      if (w) {
        if (!w.manual) { w.manual = true; w.points = wirePoints(w); }
        w.points[drag.vidx] = snapPt(world);
        renderWires(); renderOverlay();
      }
      return;
    }

    if (drag.type === 'select-box') {
      const a = drag.start, b = world;
      const x = Math.min(a.x, b.x), y = Math.min(a.y, b.y);
      const wdt = Math.abs(a.x - b.x), hgt = Math.abs(a.y - b.y);
      drag.rect.setAttribute('x', x); drag.rect.setAttribute('y', y);
      drag.rect.setAttribute('width', wdt); drag.rect.setAttribute('height', hgt);
      drag.box = { x, y, w: wdt, h: hgt };
      return;
    }
  }

  function onPointerUp(e) {
    activePointers.delete(e.pointerId);
    if (activePointers.size < 2) pinch = null;
    try { svg.releasePointerCapture(e.pointerId); } catch (_) {}

    if (state.tool === 'pan') svg.style.cursor = 'grab';

    if (!drag) return;

    if (drag.type === 'move' && drag.moved) {
      pushHistory();
    }

    if (drag.type === 'wire-new') {
      drag.previewEl.remove();
      const endTerm = drag.snapTerm || findTerminalAt(elementUnder(e.clientX, e.clientY));
      const start = endpointWorld(drag.from);
      if (endTerm && !(endTerm.comp === drag.from.comp && endTerm.term === drag.from.term)) {
        state.wires.push({ id: uid('w'), from: drag.from, to: endTerm, points: [], color: '' });
        pushHistory();
      } else {
        // 빈 공간에서 끝나면 자유 끝점 배선 생성
        const endPt = snapPt(screenToWorld(e.clientX, e.clientY));
        if (Math.hypot(endPt.x - start.x, endPt.y - start.y) > GRID) {
          state.wires.push({ id: uid('w'), from: drag.from, to: null,
            manual: true, points: [start, endPt], color: '' });
          pushHistory();
        }
      }
      render();
    }

    if (drag.type === 'vertex') pushHistory();

    if (drag.type === 'select-box' && drag.box) {
      drag.rect.remove();
      selectInBox(drag.box, e.shiftKey);
    } else if (drag.type === 'select-box') {
      drag.rect.remove();
    }

    drag = null;
  }

  function selectInBox(box, additive) {
    const ids = [];
    for (const c of state.components) {
      if (c.x >= box.x && c.x <= box.x + box.w && c.y >= box.y && c.y <= box.y + box.h)
        ids.push(c.id);
    }
    if (ids.length) {
      if (!additive) state.selection.clear();
      state.selKind = 'component';
      ids.forEach(id => state.selection.add(id));
    }
    render(); refreshInspector();
  }

  let lastHot = null;
  function highlightTerminal(target) {
    const term = findTerminalAt(target);
    if (lastHot && lastHot !== target) lastHot.classList.remove('hot');
    if (term) { target.classList.add('hot'); lastHot = target; }
    return term;
  }

  /* ---------------- 줌 ---------------- */
  function clamp(v, a, b) { return Math.max(a, Math.min(b, v)); }
  function zoomAt(sx, sy, newScale) {
    const rect = svg.getBoundingClientRect();
    const wx = (sx - rect.left - state.view.x) / state.view.scale;
    const wy = (sy - rect.top - state.view.y) / state.view.scale;
    state.view.scale = newScale;
    state.view.x = sx - rect.left - wx * newScale;
    state.view.y = sy - rect.top - wy * newScale;
    applyView();
  }
  svg.addEventListener('wheel', (e) => {
    e.preventDefault();
    const factor = e.deltaY < 0 ? 1.1 : 1 / 1.1;
    zoomAt(e.clientX, e.clientY, clamp(state.view.scale * factor, 0.2, 4));
  }, { passive: false });

  function zoomFit() {
    if (!state.components.length && !state.wires.length) {
      state.view = { x: svg.clientWidth / 2, y: svg.clientHeight / 2, scale: 1 };
      applyView(); render(); return;
    }
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const c of state.components) {
      const def = COMPONENTS[c.type], r = Math.max(def.size.w, def.size.h) / 2 + 20;
      minX = Math.min(minX, c.x - r); minY = Math.min(minY, c.y - r);
      maxX = Math.max(maxX, c.x + r); maxY = Math.max(maxY, c.y + r);
    }
    for (const w of state.wires) for (const p of wirePoints(w)) {
      minX = Math.min(minX, p.x); minY = Math.min(minY, p.y);
      maxX = Math.max(maxX, p.x); maxY = Math.max(maxY, p.y);
    }
    const w = maxX - minX, h = maxY - minY;
    const scale = clamp(Math.min(svg.clientWidth / w, svg.clientHeight / h) * 0.85, 0.2, 2);
    state.view.scale = scale;
    state.view.x = svg.clientWidth / 2 - (minX + w / 2) * scale;
    state.view.y = svg.clientHeight / 2 - (minY + h / 2) * scale;
    applyView(); render();
  }

  /* ---------------- 팔레트 ---------------- */
  function buildPalette(filter) {
    const list = document.getElementById('palette-list');
    list.textContent = '';
    const cats = {};
    for (const [type, def] of Object.entries(COMPONENTS)) {
      if (filter) {
        const hay = (def.name + ' ' + def.keywords).toLowerCase();
        if (!hay.includes(filter.toLowerCase())) continue;
      }
      (cats[def.category] = cats[def.category] || []).push([type, def]);
    }
    for (const [cat, items] of Object.entries(cats)) {
      const h = document.createElement('div');
      h.className = 'palette-cat'; h.textContent = cat;
      list.appendChild(h);
      for (const [type, def] of items) {
        const item = document.createElement('div');
        item.className = 'palette-item';
        item.draggable = true;
        item.dataset.type = type;
        item.innerHTML =
          `<div class="thumb">${thumbSvg(def)}</div><div class="name">${def.name}</div>`;
        item.addEventListener('dragstart', (e) => {
          e.dataTransfer.setData('text/wiredraw', type);
          e.dataTransfer.effectAllowed = 'copy';
        });
        // 클릭 배치 (모바일 대응)
        item.addEventListener('click', () => {
          const c = screenToWorld(svg.clientWidth / 2 + svg.getBoundingClientRect().left,
            svg.clientHeight / 2 + svg.getBoundingClientRect().top);
          addComponent(type, c.x, c.y);
          hint(def.name + ' 추가됨');
        });
        list.appendChild(item);
      }
    }
  }

  function thumbSvg(def) {
    const s = Math.max(def.size.w, def.size.h) + 24;
    return `<svg viewBox="${-s/2} ${-s/2} ${s} ${s}" style="--symbol:#1a2733">${def.draw()}</svg>`;
  }

  // 캔버스로 드롭
  svg.addEventListener('dragover', (e) => { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; });
  svg.addEventListener('drop', (e) => {
    e.preventDefault();
    const type = e.dataTransfer.getData('text/wiredraw');
    if (type && COMPONENTS[type]) {
      const w = screenToWorld(e.clientX, e.clientY);
      addComponent(type, w.x, w.y);
    }
  });

  document.getElementById('palette-search').addEventListener('input', (e) =>
    buildPalette(e.target.value.trim()));

  /* ---------------- 속성 패널 ---------------- */
  const WIRE_COLORS = ['#2563c7', '#dc2626', '#16a34a', '#000000', '#ca8a04', '#9333ea'];

  function refreshInspector() {
    const empty = document.getElementById('inspector-empty');
    const body = document.getElementById('inspector-body');
    if (!state.selection.size) {
      empty.hidden = false; body.hidden = true; return;
    }
    empty.hidden = true; body.hidden = false;
    body.innerHTML = '';

    if (state.selKind === 'component') {
      if (state.selection.size > 1) {
        body.appendChild(titleEl(`부품 ${state.selection.size}개 선택됨`));
        body.appendChild(actionBtn('⟳ 90° 회전', rotateSelection));
        body.appendChild(actionBtn('⧉ 복제', duplicateSelection));
        body.appendChild(actionBtn('🗑 삭제', deleteSelection, true));
        return;
      }
      const c = getComp([...state.selection][0]);
      if (!c) return;
      const def = COMPONENTS[c.type];
      body.appendChild(titleEl(def.name));
      body.appendChild(fieldText('이름표 (예: R1, SW1)', c.label, (v) => { c.label = v; render(); }, true));
      body.appendChild(fieldText('값 (예: 10kΩ, 12V)', c.value, (v) => { c.value = v; render(); }, true));

      const rotField = document.createElement('div');
      rotField.className = 'insp-field';
      rotField.innerHTML = `<label>회전각</label>`;
      const sel = document.createElement('select');
      [0, 90, 180, 270].forEach(deg => {
        const o = document.createElement('option');
        o.value = deg; o.textContent = deg + '°';
        if (c.rot === deg) o.selected = true;
        sel.appendChild(o);
      });
      sel.addEventListener('change', () => { c.rot = +sel.value; pushHistory(); render(); });
      rotField.appendChild(sel);
      body.appendChild(rotField);

      body.appendChild(colorRow('심볼 색상', c.color || '#1a2733', (col) => {
        c.color = col; render();
      }, () => pushHistory()));

      body.appendChild(actionBtn('⟳ 회전', rotateSelection));
      body.appendChild(actionBtn('⧉ 복제', duplicateSelection));
      body.appendChild(actionBtn('🗑 삭제', deleteSelection, true));
    } else if (state.selKind === 'wire') {
      body.appendChild(titleEl(state.selection.size > 1 ? `배선 ${state.selection.size}개` : '배선'));
      const ids = [...state.selection];
      const first = getWire(ids[0]);
      body.appendChild(colorRow('배선 색상', (first && first.color) || '#2563c7', (col) => {
        ids.forEach(id => { const w = getWire(id); if (w) w.color = col; }); render();
      }, () => pushHistory()));
      if (ids.length === 1 && first && first.manual) {
        body.appendChild(actionBtn('↺ 자동 경로로', () => {
          first.manual = false; first.points = []; pushHistory(); render();
        }));
      }
      body.appendChild(actionBtn('🗑 삭제', deleteSelection, true));
    }
  }

  function titleEl(t) {
    const d = document.createElement('div');
    d.className = 'insp-title'; d.textContent = t; return d;
  }
  function fieldText(label, val, onInput, commitOnChange) {
    const f = document.createElement('div');
    f.className = 'insp-field';
    f.innerHTML = `<label>${label}</label>`;
    const inp = document.createElement('input');
    inp.type = 'text'; inp.value = val || '';
    inp.addEventListener('input', () => onInput(inp.value));
    if (commitOnChange) inp.addEventListener('change', () => pushHistory());
    f.appendChild(inp);
    return f;
  }
  function colorRow(label, current, onPick, onCommit) {
    const f = document.createElement('div');
    f.className = 'insp-field';
    f.innerHTML = `<label>${label}</label>`;
    const row = document.createElement('div');
    row.className = 'insp-color-row';
    WIRE_COLORS.concat(['#1a2733']).filter((v, i, a) => a.indexOf(v) === i).forEach(col => {
      const sw = document.createElement('div');
      sw.className = 'insp-swatch' + (col.toLowerCase() === (current || '').toLowerCase() ? ' active' : '');
      sw.style.background = col;
      sw.addEventListener('click', () => {
        onPick(col);
        row.querySelectorAll('.insp-swatch').forEach(s => s.classList.remove('active'));
        sw.classList.add('active');
        if (onCommit) onCommit();
      });
      row.appendChild(sw);
    });
    f.appendChild(row);
    return f;
  }
  function actionBtn(label, fn, danger) {
    const b = document.createElement('button');
    b.className = 'flat-btn';
    b.style.cssText = 'width:100%;margin-top:6px;text-align:center' +
      (danger ? ';color:var(--danger)' : '');
    b.textContent = label;
    b.addEventListener('click', fn);
    return b;
  }

  /* ---------------- 저장 / 불러오기 ---------------- */
  const STORAGE_KEY = 'wiredraw.diagram';
  function serialize() {
    return JSON.stringify({
      version: 1,
      components: state.components,
      wires: state.wires,
      nextId: state.nextId,
      view: state.view,
    }, null, 2);
  }
  function loadData(data) {
    state.components = data.components || [];
    state.wires = data.wires || [];
    state.nextId = data.nextId || (state.components.length + state.wires.length + 1);
    if (data.view) state.view = data.view;
    state.selection.clear(); state.selKind = null;
    applyView(); render(); refreshInspector();
    history = [snapshot()]; historyIdx = 0;
  }

  function saveLocal() {
    localStorage.setItem(STORAGE_KEY, serialize());
    hint('브라우저에 저장됨');
  }
  function loadLocal() {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) { try { loadData(JSON.parse(raw)); return true; } catch (_) {} }
    return false;
  }

  function download(filename, content, mime) {
    const blob = new Blob([content], { type: mime });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = filename;
    document.body.appendChild(a); a.click(); a.remove();
    URL.revokeObjectURL(url);
  }

  /* ---------------- 내보내기 ---------------- */
  function computeBounds(margin = 30) {
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const c of state.components) {
      const def = COMPONENTS[c.type], r = Math.max(def.size.w, def.size.h) / 2 + 24;
      minX = Math.min(minX, c.x - r); minY = Math.min(minY, c.y - r);
      maxX = Math.max(maxX, c.x + r); maxY = Math.max(maxY, c.y + r);
    }
    for (const w of state.wires) for (const p of wirePoints(w)) {
      minX = Math.min(minX, p.x); minY = Math.min(minY, p.y);
      maxX = Math.max(maxX, p.x); maxY = Math.max(maxY, p.y);
    }
    if (!isFinite(minX)) { minX = 0; minY = 0; maxX = 400; maxY = 300; }
    return { x: minX - margin, y: minY - margin,
      w: (maxX - minX) + margin * 2, h: (maxY - minY) + margin * 2 };
  }

  function buildExportSVG() {
    const b = computeBounds();
    const parts = [`<svg xmlns="${SVGNS}" viewBox="${b.x} ${b.y} ${b.w} ${b.h}" width="${b.w}" height="${b.h}">`];
    parts.push(`<rect x="${b.x}" y="${b.y}" width="${b.w}" height="${b.h}" fill="#ffffff"/>`);
    // 배선
    for (const w of state.wires) {
      const pts = wirePoints(w);
      if (pts.length < 2) continue;
      parts.push(`<path d="${ptsToPath(pts)}" fill="none" stroke="${w.color || '#2563c7'}" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>`);
    }
    // 부품
    for (const c of state.components) {
      const def = COMPONENTS[c.type];
      const col = c.color || '#1a2733';
      let inner = def.draw().replace(/var\(--symbol\)/g, col).replace(/var\(--canvas-bg\)/g, '#ffffff');
      parts.push(`<g transform="translate(${c.x},${c.y}) rotate(${c.rot})" style="--symbol:${col}">`);
      parts.push(inner);
      if (c.label || c.value) {
        parts.push(`<g transform="rotate(${-c.rot})">`);
        const ly = def.size.h / 2 + 16;
        if (c.label) parts.push(`<text x="0" y="${def.size.h/2 + 16}" font-size="12" fill="${col}" text-anchor="middle" font-family="sans-serif">${escapeXml(c.label)}</text>`);
        if (c.value) parts.push(`<text x="0" y="${def.size.h/2 + 16 + (c.label?14:0)}" font-size="11" fill="#1971c2" text-anchor="middle" font-family="sans-serif">${escapeXml(c.value)}</text>`);
        parts.push(`</g>`);
      }
      parts.push(`</g>`);
    }
    parts.push('</svg>');
    return parts.join('\n');
  }
  function escapeXml(s) {
    return String(s).replace(/[<>&"']/g, ch =>
      ({ '<': '&lt;', '>': '&gt;', '&': '&amp;', '"': '&quot;', "'": '&apos;' }[ch]));
  }

  function exportSVG() {
    download('wiring-diagram.svg', buildExportSVG(), 'image/svg+xml');
    hint('SVG 내보내기 완료');
  }
  function exportPNG() {
    const svgStr = buildExportSVG();
    const b = computeBounds();
    const scale = 2;
    const img = new Image();
    const blob = new Blob([svgStr], { type: 'image/svg+xml;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    img.onload = () => {
      const canvas = document.createElement('canvas');
      canvas.width = b.w * scale; canvas.height = b.h * scale;
      const ctx = canvas.getContext('2d');
      ctx.fillStyle = '#ffffff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
      URL.revokeObjectURL(url);
      canvas.toBlob((png) => {
        const u = URL.createObjectURL(png);
        const a = document.createElement('a');
        a.href = u; a.download = 'wiring-diagram.png';
        document.body.appendChild(a); a.click(); a.remove();
        URL.revokeObjectURL(u);
        hint('PNG 내보내기 완료');
      });
    };
    img.onerror = () => { hint('PNG 변환 실패'); URL.revokeObjectURL(url); };
    img.src = url;
  }

  /* ---------------- 툴바 이벤트 바인딩 ---------------- */
  document.querySelectorAll('.tool-btn').forEach(b =>
    b.addEventListener('click', () => setTool(b.dataset.tool)));

  document.getElementById('btn-rotate').addEventListener('click', rotateSelection);
  document.getElementById('btn-duplicate').addEventListener('click', duplicateSelection);
  document.getElementById('btn-deleteSel').addEventListener('click', deleteSelection);
  document.getElementById('btn-undo').addEventListener('click', undo);
  document.getElementById('btn-redo').addEventListener('click', redo);

  document.getElementById('snap-toggle').addEventListener('change', (e) => { state.snap = e.target.checked; });
  document.getElementById('grid-toggle').addEventListener('change', (e) => { state.showGrid = e.target.checked; renderGrid(); });

  document.getElementById('btn-zoom-in').addEventListener('click', () =>
    zoomAt(svg.getBoundingClientRect().left + svg.clientWidth / 2,
      svg.getBoundingClientRect().top + svg.clientHeight / 2, clamp(state.view.scale * 1.2, 0.2, 4)));
  document.getElementById('btn-zoom-out').addEventListener('click', () =>
    zoomAt(svg.getBoundingClientRect().left + svg.clientWidth / 2,
      svg.getBoundingClientRect().top + svg.clientHeight / 2, clamp(state.view.scale / 1.2, 0.2, 4)));
  document.getElementById('btn-zoom-fit').addEventListener('click', zoomFit);

  document.getElementById('btn-new').addEventListener('click', () => {
    if (state.components.length || state.wires.length) {
      if (!confirm('현재 도면을 지우고 새로 시작할까요?')) return;
    }
    loadData({ components: [], wires: [], nextId: 1 });
    state.view = { x: svg.clientWidth / 2, y: svg.clientHeight / 2, scale: 1 };
    applyView(); render();
    hint('새 도면');
  });

  document.getElementById('btn-save').addEventListener('click', saveLocal);
  document.getElementById('btn-load').addEventListener('click', () =>
    document.getElementById('file-input').click());
  document.getElementById('file-input').addEventListener('change', (e) => {
    const file = e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
      try { loadData(JSON.parse(reader.result)); hint('불러오기 완료'); }
      catch (_) { hint('파일을 읽을 수 없습니다'); }
    };
    reader.readAsText(file);
    e.target.value = '';
  });

  // 내보내기 메뉴
  const exportMenu = document.getElementById('export-menu');
  document.getElementById('btn-export').addEventListener('click', (e) => {
    e.stopPropagation(); exportMenu.classList.toggle('open');
  });
  document.addEventListener('click', () => exportMenu.classList.remove('open'));
  exportMenu.addEventListener('click', (e) => {
    const kind = e.target.dataset.export;
    if (kind === 'svg') exportSVG();
    else if (kind === 'png') exportPNG();
    else if (kind === 'json') { download('wiring-diagram.json', serialize(), 'application/json'); hint('JSON 다운로드'); }
  });

  /* ---------------- 키보드 단축키 ---------------- */
  window.addEventListener('keydown', (e) => {
    if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA' || e.target.tagName === 'SELECT') return;
    const meta = e.ctrlKey || e.metaKey;
    if (meta && e.key.toLowerCase() === 'z') { e.preventDefault(); e.shiftKey ? redo() : undo(); return; }
    if (meta && e.key.toLowerCase() === 'y') { e.preventDefault(); redo(); return; }
    if (meta && e.key.toLowerCase() === 'd') { e.preventDefault(); duplicateSelection(); return; }
    if (meta && e.key.toLowerCase() === 's') { e.preventDefault(); saveLocal(); return; }
    switch (e.key.toLowerCase()) {
      case 'v': setTool('select'); break;
      case 'w': setTool('wire'); break;
      case 'h': case ' ': setTool('pan'); break;
      case 'x': setTool('delete'); break;
      case 'r': rotateSelection(); break;
      case 'delete': case 'backspace': deleteSelection(); break;
      case 'escape': clearSelection(); break;
    }
  });

  window.addEventListener('resize', () => { applyView(); });

  /* ---------------- 초기화 ---------------- */
  function init() {
    buildPalette('');
    setTool('select');
    state.view = { x: svg.clientWidth / 2, y: svg.clientHeight / 2, scale: 1 };
    applyView();
    if (!loadLocal()) {
      seedDemo();
    }
    render();
    history = [snapshot()]; historyIdx = 0;
  }

  // 첫 실행용 예시 도면 (간단한 LED 점등 회로)
  function seedDemo() {
    state.components = [
      { id: 'c1', type: 'battery', x: 0, y: 120, rot: 0, label: 'BAT1', value: '9V', color: '' },
      { id: 'c2', type: 'switch_spst', x: 160, y: 0, rot: 0, label: 'SW1', value: '', color: '' },
      { id: 'c3', type: 'resistor', x: 320, y: 0, rot: 0, label: 'R1', value: '330Ω', color: '' },
      { id: 'c4', type: 'led', x: 480, y: 120, rot: 90, label: 'LED1', value: '', color: '' },
    ];
    state.wires = [
      { id: 'w1', from: { comp: 'c1', term: 'p' }, to: { comp: 'c2', term: 'a' }, points: [], color: '' },
      { id: 'w2', from: { comp: 'c2', term: 'b' }, to: { comp: 'c3', term: 'a' }, points: [], color: '' },
      { id: 'w3', from: { comp: 'c3', term: 'b' }, to: { comp: 'c4', term: 'a' }, points: [], color: '' },
      { id: 'w4', from: { comp: 'c4', term: 'b' }, to: { comp: 'c1', term: 'n' }, points: [], color: '' },
    ];
    state.nextId = 10;
    setTimeout(zoomFit, 0);
  }

  init();
})();
