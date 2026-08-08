"use strict";

// ---------------------------------------------------------------------------
// WebView2 bridge
// ---------------------------------------------------------------------------
let seq = 0;
const pending = new Map();
let installed = false;

function call(cmd, args, timeoutMs = 30000) {
  return new Promise((resolve) => {
    const id = ++seq;
    const timer = setTimeout(() => {
      pending.delete(id);
      resolve({ ok: false, data: "", error: cmd + " 响应超时（GUI 与驱动控制进程通信异常）" });
    }, timeoutMs);
    pending.set(id, (m) => {
      clearTimeout(timer);
      resolve(m);
    });
    const payload = { id, cmd, args: args || [] };
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(payload);
    } else {
      // Fallback (non-WebView2 preview): resolve with failure.
      setTimeout(() => resolve({ ok: false, data: "", error: "no webview bridge" }), 50);
    }
  });
}

window.chrome?.webview?.addEventListener("message", (e) => {
  const m = e.data;
  if (m && m.id !== undefined && pending.has(m.id)) {
    const resolve = pending.get(m.id);
    pending.delete(m.id);
    resolve(m);
  } else if (m && m.event === "refresh") {
    refreshAll();
  }
});

// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------
let toastTimer = null;
function showToast(msg, ok = true) {
  const el = document.getElementById("toast");
  el.textContent = msg;
  el.classList.toggle("error", !ok);
  el.classList.remove("hidden");
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.add("hidden"), 3000);
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
function switchPage(name) {
  document.querySelectorAll(".nav-item").forEach((b) =>
    b.classList.toggle("active", b.dataset.page === name));
  document.querySelectorAll(".page").forEach((p) =>
    p.classList.toggle("active", p.id === "page-" + name));
  if (name === "dashboard") refreshDashboard();
  if (name === "monitors") refreshMonitors();
  if (name === "gpu") refreshGpu();
  if (name === "persist") refreshPersist();
  if (name === "settings") refreshSettings();
}

async function refreshSettings() {
  const vEl = document.getElementById("set-version");
  const caps = await call("caps");
  try {
    const c = JSON.parse(caps.data);
    vEl.textContent = `VirtualDisplay v1.0.0 · IddCx ${c.runtime_major}.${c.runtime_minor} · WebView2 · 驱动版本 1.0.0`;
  } catch (e) {
    vEl.textContent = "VirtualDisplay v1.0.0 · WebView2";
  }
}

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------
function setDriverState(ok) {
  installed = ok;
  const el = document.getElementById("dash-driver-status");
  const btn = document.getElementById("dash-install-btn");
  if (ok) {
    el.textContent = "驱动已安装，一切就绪";
    el.style.color = "var(--success)";
    btn.textContent = "卸载驱动";
  } else {
    el.textContent = "驱动未安装，请先安装";
    el.style.color = "var(--error)";
    btn.textContent = "安装驱动";
  }
  document.querySelectorAll("[data-quick]").forEach((b) => (b.disabled = !ok));
  document.getElementById("dash-custom-btn").disabled = !ok;
}

async function refreshDashboard() {
  const caps = await call("caps");
  setDriverState(caps.ok);
  if (!caps.ok) {
    document.getElementById("dash-mon-count").textContent = "-";
    document.getElementById("dash-mode").textContent = "-";
    document.getElementById("dash-gpu").textContent = "-";
    document.getElementById("dash-hdr").textContent = "-";
    return;
  }
  const list = await call("list");
  let count = "-";
  try { count = JSON.parse(list.data).monitor_count; } catch (e) {}
  document.getElementById("dash-mon-count").textContent = count;
  document.getElementById("dash-mode").textContent = "查看显示器页";
  document.getElementById("dash-mode").style.color = "var(--text-dim)";

  const render = await call("render-get");
  const gpuEl = document.getElementById("dash-gpu");
  try {
    const r = JSON.parse(render.data);
    const luid = r.actual_luid || "";
    if (luid && luid !== "0:0") {
      const rl = await call("render-list");
      try {
        const adapters = JSON.parse(rl.data).adapters || [];
        const match = adapters.find((x) => x.luid === luid ||
          (x.vendor_id === r.vendor_id && x.device_id === r.device_id));
        gpuEl.textContent = match ? match.description : ("LUID " + luid);
      } catch (e) { gpuEl.textContent = "LUID " + luid; }
    } else {
      gpuEl.textContent = "自动（尚未分配）";
    }
  } catch (e) {
    gpuEl.textContent = "自动";
  }

  const hdr = await call("advancedcolor", ["status"]);
  const hdrEl = document.getElementById("dash-hdr");
  try {
    const h = JSON.parse(hdr.data);
    if (h.hdr_enabled) { hdrEl.textContent = "HDR 开"; hdrEl.style.color = "var(--success)"; }
    else if (h.hdr_supported) { hdrEl.textContent = "HDR 关"; hdrEl.style.color = "var(--text-dim)"; }
    else { hdrEl.textContent = "不支持"; hdrEl.style.color = "var(--error)"; }
  } catch (e) { hdrEl.textContent = "-"; }
}

// ---------------------------------------------------------------------------
// Monitors
// ---------------------------------------------------------------------------
async function refreshMonitors() {
  const listEl = document.getElementById("mon-list");
  const emptyEl = document.getElementById("mon-empty");
  const list = await call("list");
  let count = 0;
  let monIdx = [];
  try {
    const j = JSON.parse(list.data);
    count = j.monitor_count || 0;
    monIdx = j.monitors || [];
  } catch (e) {}
  if (!monIdx.length) for (let k = 1; k <= count; k++) monIdx.push(k);
  emptyEl.style.display = count > 0 ? "none" : "block";
  listEl.innerHTML = "";
  if (count <= 0) return;

  // Collect available modes from displays output
  const displays = await call("displays");
  const res = new Set();
  const freqs = new Set();
  let disp = [];
  try { disp = JSON.parse(displays.data).displays || []; } catch (e) {}
  const curByIndex = new Map();
  let virtSeq = 0;
  for (const d of disp) {
    if (d.current && d.virtual) {
      const slotIndex = monIdx[virtSeq++];
      if (slotIndex) curByIndex.set(slotIndex, d.current);
    }
    for (const m of (d.modes || [])) {
      res.add(m.w + "x" + m.h);
      freqs.add(m.rate);
    }
  }
  const resArr = [...res];
  const freqArr = [...freqs].sort((a, b) => a - b);

  const hdr = await call("advancedcolor", ["status"]);
  let hdrOk = false;
  try { hdrOk = !!JSON.parse(hdr.data).hdr_supported; } catch (e) {}

  for (const i of monIdx) {
    const cur = curByIndex.get(i);
    const card = document.createElement("div");
    card.className = "mon-card";
    card.innerHTML = `
      <div class="mon-head">
        <span class="mon-title">显示器 ${i}</span>
        <span class="badge active">● 活动</span>
        <span class="badge hdr" id="hdrbadge-${i}">HDR: 关</span>
      </div>
      <div class="mon-info">当前：${cur ? `${cur.w}×${cur.h} @${cur.rate} Hz（位置 ${cur.x},${cur.y}）` : "未激活"}</div>
      <div class="mon-body">
        <select id="res-${i}"></select>
        <select id="freq-${i}"></select>
        <button class="btn" id="primary-${i}">设为主屏</button>
        <button class="btn" id="open-here-${i}">在此屏打开程序</button>
        <button class="btn primary" id="apply-${i}">应用</button>
        <button class="btn danger" id="remove-${i}">移除</button>
      </div>`;
    listEl.appendChild(card);

    const resSel = card.querySelector(`#res-${i}`);
    resArr.forEach((r) => resSel.add(new Option(r, r)));
    const freqSel = card.querySelector(`#freq-${i}`);
    freqArr.forEach((f) => freqSel.add(new Option(f + " Hz", f)));
    if (cur) {
      const curRes = cur.w + "x" + cur.h;
      if (resSel.querySelector(`option[value="${curRes}"]`)) resSel.value = curRes;
      if (freqSel.querySelector(`option[value="${cur.rate}"]`)) freqSel.value = String(cur.rate);
    }

    card.querySelector(`#apply-${i}`).addEventListener("click", async () => {
      const [w, h] = resSel.value.split("x");
      const vsync = parseInt(freqSel.value, 10) * 1000;
      const r = await call("update", [String(i), w, h, String(vsync)]);
      showToast(r.ok ? `显示器 ${i} 模式已应用` : "应用失败：" + (r.error || "模式可能不在列表中").slice(0, 200), r.ok);
      if (r.ok) setTimeout(refreshAll, 800);
    });
    card.querySelector(`#remove-${i}`).addEventListener("click", async () => {
      if (!confirm(`确定移除显示器 ${i} 吗？`)) return;
      const r = await call("remove", [String(i)]);
      showToast(r.ok ? `显示器 ${i} 已移除` : "移除失败：" + (r.error || "").slice(0, 200), r.ok);
      refreshAll();
    });
    // "在此屏打开程序": pick an exe and launch it on this virtual display.
    const openBtn = card.querySelector(`#open-here-${i}`);
    openBtn.addEventListener("click", () => pickAndRun("run-display", i));
    card.querySelector(`#primary-${i}`).addEventListener("click", async () => {
      if (!confirm("把虚拟显示器设为系统主屏后：\n\n· 新打开的程序窗口将默认显示在虚拟显示器上\n· 物理显示器将变成扩展屏\n· 在物理屏前操作时窗口可能“看不见”\n\n确定要继续吗？")) return;
      const r = await call("primary", [String(i)]);
      if (r.ok) {
        showToast(`显示器 ${i} 已设为主屏（可用物理屏上的「恢复物理主屏」撤销）`);
      } else {
        showToast("设为主屏失败：主屏位置 (0,0) 可能被占用。请先到布局面板或 Windows 显示设置把其他显示器移开后再试。", false);
      }
      if (r.ok) setTimeout(refreshAll, 800);
    });

    // HDR toggle
    const badge = card.querySelector(`#hdrbadge-${i}`);
    const hdrBtn = document.createElement("button");
    hdrBtn.className = "btn";
    hdrBtn.textContent = hdrOk ? "HDR: 关" : "HDR: 不支持";
    hdrBtn.disabled = !hdrOk;
    hdrBtn.style.marginLeft = "auto";
    if (!hdrOk) badge.textContent = "HDR: 不支持";
    card.querySelector(".mon-body").appendChild(hdrBtn);
    hdrBtn.addEventListener("click", async () => {
      const on = hdrBtn.textContent.includes("开");
      const r = await call("advancedcolor", [on ? "off" : "on", String(i)]);
      if (r.ok) {
        const next = !on;
        hdrBtn.textContent = next ? "HDR: 开" : "HDR: 关";
        badge.textContent = next ? "HDR: 开" : "HDR: 关";
      } else {
        showToast("HDR 切换失败", false);
      }
    });
  }

  renderLayoutPanel(monIdx, curByIndex, disp);
}

// ---------------------------------------------------------------------------
// Layout panel: drag virtual displays to reposition the desktop layout.
// Physical displays are rendered as a fixed core reference.
// ---------------------------------------------------------------------------
let layoutDrag = null;

function renderLayoutPanel(monIdx, curByIndex, allDisplays) {
  const stage = document.getElementById("layout-stage");
  const meta = document.getElementById("layout-meta");
  if (!stage) return;
  stage.innerHTML = "";
  if (!meta) return;
  meta.textContent = "";

  // Collect active displays: physical ones come first (the layout core).
  const physical = [];
  const virtual = [];
  for (const d of (allDisplays || [])) {
    if (!d.current) continue;
    if (d.virtual) virtual.push(d);
    else physical.push(d);
  }

  // Map virtual display index -> position (aligned to monIdx order).
  const virtByIndex = new Map();
  let virtSeq = 0;
  for (const d of allDisplays || []) {
    if (!d.current || !d.virtual) continue;
    const slotIndex = monIdx[virtSeq++];
    if (slotIndex) virtByIndex.set(slotIndex, d);
  }

  if (virtual.length === 0) {
    if (physical.length > 0) {
      const p = physical[0];
      meta.textContent = `物理${p.primary ? "主屏" : "显示器"} ${p.w}×${p.h} 位于 (${p.x},${p.y})。添加虚拟显示器后可在此拖拽排版。`;
    } else {
      meta.textContent = "暂无已激活的显示器，先添加并应用模式后即可拖拽排版。";
    }
    return;
  }

  // Compute bounding box in desktop coordinates covering everything.
  let minX = 0, minY = 0, maxX = 0, maxY = 0;
  for (const c of physical.map((d) => d.current).concat(virtual.map((d) => d.current))) {
    minX = Math.min(minX, c.x);
    minY = Math.min(minY, c.y);
    maxX = Math.max(maxX, c.x + c.w);
    maxY = Math.max(maxY, c.y + c.h);
  }
  const bw = maxX - minX || 1920;
  const bh = maxY - minY || 1080;

  // Scale to fit: keep the display blocks at the same absolute size as the
  // original layout panel (600x260 logical area, 0.5 max). The larger stage
  // simply gives more room so wide layouts are not clipped.
  const availW = Math.max(stage.clientWidth - 30, 400);
  const availH = 380;
  const scale = Math.min(600 / bw, 260 / bh, 0.5);
  const offX = 15 + (availW - bw * scale) / 2 - minX * scale;
  const offY = 15 + (availH - bh * scale) / 2 - minY * scale;

  // Snap edges of the dragged monitor to the edges of all other monitors.
  // Returns the position with the closest edge snap applied (within 24px).
  const snapPositions = (dragged, x, y) => {
    const SNAP = 24;
    const rects = [];
    const w = dragged.current.w, h = dragged.current.h;
    for (const d of physical.concat(virtual)) {
      if (d === dragged) continue;
      rects.push(d.current);
    }
    let best = null;
    // Candidate x: align my left/right to their left/right
    for (const r of rects) {
      const cand = [
        { v: r.x - w, d: Math.abs(x - (r.x - w)) },   // my right = their left
        { v: r.x + r.w, d: Math.abs(x - (r.x + r.w)) }, // my left = their right
        { v: r.x, d: Math.abs(x - r.x) },               // my left = their left
        { v: r.x + r.w - w, d: Math.abs(x - (r.x + r.w - w)) } // my right = their right
      ];
      for (const c of cand) {
        if (c.d <= SNAP && (!best || c.d < best.d)) best = { x: c.v, y: null, d: c.d };
      }
    }
    // Candidate y: align my top/bottom to their top/bottom
    for (const r of rects) {
      const cand = [
        { v: r.y - h, d: Math.abs(y - (r.y - h)) },
        { v: r.y + r.h, d: Math.abs(y - (r.y + r.h)) },
        { v: r.y, d: Math.abs(y - r.y) },
        { v: r.y + r.h - h, d: Math.abs(y - (r.y + r.h - h)) }
      ];
      for (const c of cand) {
        if (c.d <= SNAP && (!best || c.d < best.d)) best = { x: null, y: c.v, d: c.d };
      }
    }
    if (best && best.x !== null) x = best.x;
    if (best && best.y !== null) y = best.y;
    return { x, y };
  };

  // Physical displays: fixed core, highlighted, not draggable.
  physical.forEach((d, k) => {
    const c = d.current;
    const el = document.createElement("div");
    el.className = "layout-physical";
    el.style.left = (c.x * scale + offX) + "px";
    el.style.top = (c.y * scale + offY) + "px";
    el.style.width = (c.w * scale) + "px";
    el.style.height = (c.h * scale) + "px";
    el.innerHTML = `<div class="layout-mon-title">物理${d.primary ? "主屏" : "显示器"} ${k + 1}${d.primary ? " ★" : ""}</div>
      <div class="layout-mon-size">${c.w}×${c.h}</div>
      ${!d.primary && physical.some((p) => !p.primary) ? '<button class="layout-primary-btn layout-restore-btn" data-physical="1">恢复物理主屏</button>' : ""}`;
    const restoreBtn = el.querySelector(".layout-restore-btn");
    if (restoreBtn) {
      restoreBtn.addEventListener("click", async (e) => {
        e.stopPropagation();
        const r = await call("physical-primary");
        showToast(r.ok ? "已恢复物理显示器为主屏" : "恢复失败：" + (r.error || "").slice(0, 120), r.ok);
        if (r.ok) setTimeout(refreshAll, 800);
      });
    }
    stage.appendChild(el);
  });

  for (const i of monIdx) {
    const d = virtByIndex.get(i);
    if (!d) continue;
    const c = d.current;
    const el = document.createElement("div");
    el.className = "layout-mon" + (d.primary ? " primary" : "");
    el.dataset.index = String(i);
    el.style.left = (c.x * scale + offX) + "px";
    el.style.top = (c.y * scale + offY) + "px";
    el.style.width = (c.w * scale) + "px";
    el.style.height = (c.h * scale) + "px";
    el.innerHTML = `<div class="layout-mon-title">显示器 ${i}${d.primary ? '<span class="primary-mark">★ 主屏</span>' : ""}</div>
      <div class="layout-mon-size">${c.w}×${c.h}</div>
      <button class="layout-primary-btn" data-idx="${i}">设为主屏</button>`;

    el.querySelector(".layout-primary-btn").addEventListener("click", async (e) => {
      e.stopPropagation();
      const isPrimaryAlready = d.primary;
      if (!isPrimaryAlready && !confirm("把虚拟显示器设为系统主屏后：\n\n· 新打开的程序窗口将默认显示在虚拟显示器上\n· 你的物理显示器将变成扩展屏（任务栏移到虚拟屏）\n· 若你在物理屏前操作，程序窗口可能“看不见”\n\n确定要继续吗？")) return;
      const r = await call("primary", [String(i)]);
      if (r.ok) {
        showToast(`显示器 ${i} 已设为主屏（可用物理屏上的「恢复物理主屏」撤销）`);
      } else {
        showToast("设为主屏失败：主屏位置 (0,0) 可能被其他显示器占用。请先在布局面板把显示器拖离 (0,0)，或到 Windows 显示设置中调整后再试。", false);
      }
      if (r.ok) setTimeout(refreshAll, 800);
    });

    // Drag to reposition (Pointer Events + capture for smooth tracking).
    el.addEventListener("pointerdown", (e) => {
      if (e.target.classList.contains("layout-primary-btn")) return;
      if (d.primary) {
        showToast("该显示器是当前主屏。Windows 不允许直接拖动主屏——请先点击物理屏上的「恢复物理主屏」，或到 Windows 显示设置中把物理屏设为主屏后再拖动。", false);
        return;
      }
      e.preventDefault();
      el.setPointerCapture(e.pointerId);
      const startX = e.clientX, startY = e.clientY;
      const origLeft = parseFloat(el.style.left);
      const origTop = parseFloat(el.style.top);
      layoutDrag = { el, i, d, startX, startY, origLeft, origTop, moved: false, pointerId: e.pointerId };
    });

    stage.appendChild(el);
  }

  const onMove = (e) => {
    if (!layoutDrag) return;
    const dx = e.clientX - layoutDrag.startX;
    const dy = e.clientY - layoutDrag.startY;
    if (Math.abs(dx) + Math.abs(dy) > 3) layoutDrag.moved = true;
    let newLeft = layoutDrag.origLeft + dx;
    let newTop = layoutDrag.origTop + dy;
    // Snap while dragging (convert stage px -> desktop px).
    const cand = snapPositions(layoutDrag.d,
      (newLeft - offX) / scale, (newTop - offY) / scale);
    newLeft = cand.x * scale + offX;
    newTop = cand.y * scale + offY;
    layoutDrag.el.style.left = newLeft + "px";
    layoutDrag.el.style.top = newTop + "px";
  };

  const onUp = async (e) => {
    if (!layoutDrag) return;
    const drag = layoutDrag;
    layoutDrag = null;
    try { drag.el.releasePointerCapture(e.pointerId); } catch (err) {}
    if (!drag.moved) return;

    const newLeft = parseFloat(drag.el.style.left);
    const newTop = parseFloat(drag.el.style.top);
    const idx = Number(drag.el.dataset.index);
    const newX = Math.round((newLeft - offX) / scale);
    const newY = Math.round((newTop - offY) / scale);

    const args = [`${idx}:${newX},${newY}`];
    const r = await call("layout", args);
    showToast(r.ok ? `显示器 ${idx} 已移动到 ${newX},${newY}` : "布局失败：" + (r.error || "").slice(0, 120), r.ok);
    if (r.ok) setTimeout(refreshAll, 800);
    else renderLayoutPanel(monIdx, curByIndex, allDisplays);
  };

  stage.addEventListener("pointermove", onMove);
  stage.addEventListener("pointerup", onUp);
  stage.addEventListener("pointercancel", onUp);
}

// ---------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------
async function refreshGpu() {
  const list = await call("render-list");
  const listEl = document.getElementById("gpu-list");
  listEl.innerHTML = "";
  let adapters = [];
  try { adapters = JSON.parse(list.data).adapters || []; } catch (e) {}

  // IDD virtual-display drivers register their own DXGI adapter whose
  // description mirrors the render GPU, so identical descriptions appear
  // multiple times. Merge them into one entry per GPU.
  const seen = new Set();
  const merged = [];
  for (const a of adapters) {
    const key = (a.software ? "sw:" : "hw:") + a.description;
    if (seen.has(key)) continue;
    seen.add(key);
    merged.push(a);
  }

  for (const a of merged) {
    const row = document.createElement("div");
    row.className = "gpu-item";
    const label = document.createElement("span");
    label.className = "gpu-label";
    label.textContent = a.description + (a.software ? "  [软件]" : "");
    const badge = document.createElement("span");
    badge.className = "gpu-badge";
    badge.textContent = `0x${a.vendor_id.toString(16)} / 0x${a.device_id.toString(16)}`;
    const btn = document.createElement("button");
    btn.className = "btn";
    btn.textContent = "选择";
    btn.addEventListener("click", async () => {
      const r = await call("render-set", ["id", "0x" + a.vendor_id.toString(16), "0x" + a.device_id.toString(16)]);
      showToast(r.ok ? "渲染 GPU 已设置" : "设置失败（该 GPU 不可用）", r.ok);
      refreshGpu();
    });
    row.append(label, badge, btn);
    listEl.appendChild(row);
  }

  const render = await call("render-get");
  try {
    const r = JSON.parse(render.data);
    const luid = r.actual_luid || "";
    let actualText = "";
    if (luid && luid !== "0:0") {
      const [h, l] = luid.split(":").map(Number);
      const match = adapters.find((x) => x.luid === luid ||
        (x.vendor_id === r.vendor_id && x.device_id === r.device_id));
      actualText = match ? match.description : ("LUID " + luid);
    } else {
      actualText = "（尚未分配）";
    }
    document.getElementById("gpu-actual").textContent = "当前实际分配: " + actualText;
  } catch (e) {}
}

// ---------------------------------------------------------------------------
// Persist
// ---------------------------------------------------------------------------
async function refreshPersist() {
  const st = document.getElementById("persist-task-state");
  const box = document.getElementById("persist-auto");
  st.textContent = "开机自恢复: 检查中…";
  st.style.color = "var(--text-dim)";
  const r = await call("task-status");
  try {
    const registered = !!JSON.parse(r.data).registered;
    box.checked = registered;
    st.textContent = registered ? "开机自恢复: 已启用（登录时自动恢复显示器）" : "开机自恢复: 未启用";
    st.style.color = registered ? "var(--success)" : "var(--text-dim)";
  } catch (e) {
    st.textContent = "开机自恢复: 检查失败";
  }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
async function runDiag(cmd) {
  const out = document.getElementById("diag-output");
  out.textContent = "运行 " + cmd + " …";
  const r = await call(cmd);
  out.textContent = r.ok ? r.data : ("命令失败 (exit=" + (r.ok ? 0 : "err") + ")\n" + (r.error || ""));
}

// ---------------------------------------------------------------------------
// Refresh all
// ---------------------------------------------------------------------------
function refreshAll() {
  if (document.getElementById("page-dashboard").classList.contains("active")) refreshDashboard();
  if (document.getElementById("page-monitors").classList.contains("active")) refreshMonitors();
  if (document.getElementById("page-gpu").classList.contains("active")) refreshGpu();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
document.addEventListener("DOMContentLoaded", () => {
  // Navigation
  document.querySelectorAll(".nav-item").forEach((b) =>
    b.addEventListener("click", () => switchPage(b.dataset.page)));

  // Install/uninstall
  document.getElementById("dash-install-btn").addEventListener("click", async () => {
    const installing = !installed;
    const btn = document.getElementById("dash-install-btn");
    btn.disabled = true;
    btn.textContent = installing ? "正在安装…" : "正在卸载…";
    let r;
    try {
      r = installing ? await call("install", ["--trust-certs"]) : await call("uninstall");
    } finally {
      btn.disabled = false;
    }
    const err = (r.error || r.data || "").toString().trim();
    if (r.ok) {
      showToast(installing ? "驱动已安装" : "驱动已卸载", true);
    } else {
      showToast(err ? (installing ? "安装失败：" : "卸载失败：") + err.slice(0, 200) : "操作失败", false);
    }
    refreshAll();
  });

  // Quick add
  document.querySelectorAll("[data-quick]").forEach((b) =>
    b.addEventListener("click", async () => {
      const idx = parseInt(b.dataset.quick, 10);
      const presets = [
        [1920,1080,60000],[1920,1080,120000],
        [2560,1440,60000],[2560,1440,120000],
        [3840,2160,60000],[3840,2160,120000]
      ];
      const [w,h,v] = presets[idx];
      const r = await call("add", [String(w), String(h), String(v)]);
      showToast(r.ok ? "显示器已添加" : "添加失败：" + (r.error || "").slice(0, 200), r.ok);
      refreshAll();
    }));
  // Follow-physical: add a virtual display matching the physical primary size.
  document.getElementById("dash-physical-btn").addEventListener("click", async () => {
    const d = await call("displays");
    let target = null;
    try {
      const arr = JSON.parse(d.data).displays || [];
      target = arr.find((x) => !x.virtual && x.primary && x.current) ||
               arr.find((x) => !x.virtual && x.current);
    } catch (e) {}
    if (!target || !target.current) {
      showToast("未检测到活动的物理显示器", false);
      return;
    }
    const c = target.current;
    const vsync = (c.rate && c.rate > 0) ? c.rate * 1000 : 60000;
    const r = await call("add", [String(c.w), String(c.h), String(vsync)]);
    showToast(r.ok ? `已添加 ${c.w}×${c.h}@${c.rate}Hz（跟随物理屏）` : "添加失败：" + (r.error || "").slice(0, 200), r.ok);
    refreshAll();
  });
  document.getElementById("dash-custom-btn").addEventListener("click", () =>
    document.getElementById("dialog-overlay").classList.remove("hidden"));
  // Run a program on the monitor the mouse is currently on.
  document.getElementById("dash-run-here-btn").addEventListener("click", () =>
    pickAndRun("run-here", null));

  // Monitors page buttons
  document.getElementById("mon-add-btn").addEventListener("click", () =>
    document.getElementById("dialog-overlay").classList.remove("hidden"));
  document.getElementById("mon-clear-btn").addEventListener("click", async () => {
    const r = await call("clear");
    showToast(r.ok ? "已清除全部显示器" : "清除失败：" + (r.error || "").slice(0, 200), r.ok);
    refreshAll();
  });

  // Dialog
  document.getElementById("dlg-cancel").addEventListener("click", closeDialog);
  document.getElementById("dlg-ok").addEventListener("click", async () => {
    const w = parseInt(document.getElementById("dlg-w").value, 10);
    const h = parseInt(document.getElementById("dlg-h").value, 10);
    const v = parseInt(document.getElementById("dlg-v").value, 10);
    if (!(w >= 320 && h >= 200 && v >= 24)) { showToast("分辨率无效（最小 320×200@24Hz）", false); return; }
    if (w > 16384 || h > 16384) { showToast("分辨率超出驱动上限（最大 16384×16384）", false); return; }
    if (v > 1000) { showToast("刷新率超出驱动上限（最大 1000Hz）", false); return; }
    closeDialog();
    const r = await call("add", [String(w), String(h), String(v * 1000)]);
    if (r.ok) {
      let applied = true;
      try { applied = JSON.parse(r.data).mode_applied !== false; } catch (e) {}
      showToast(applied ? `已添加 ${w}×${h}@${v}Hz` : `已添加，但 ${w}×${h}@${v}Hz 超出显示能力，已用默认分辨率`, applied);
    } else {
      showToast("添加失败：" + (r.error || "").slice(0, 200), false);
    }
    refreshAll();
  });
  function closeDialog() { document.getElementById("dialog-overlay").classList.add("hidden"); }
  document.getElementById("dialog-overlay").addEventListener("click", (e) => {
    if (e.target.id === "dialog-overlay") closeDialog();
  });

  // GPU page
  document.getElementById("gpu-auto-btn").addEventListener("click", async () => {
    const r = await call("render-set", ["auto"]);
    showToast(r.ok ? "已设置为自动选择" : "设置失败", r.ok);
    refreshGpu();
  });

  // Persist page
  document.getElementById("persist-save-btn").addEventListener("click", async () => {
    const r = await call("save-config");
    showToast(r.ok ? "配置已保存" : "保存失败", r.ok);
    if (r.ok) refreshPersist();
  });
  document.getElementById("persist-restore-btn").addEventListener("click", async () => {
    const r = await call("restore");
    showToast(r.ok ? "显示器已恢复" : "恢复失败", r.ok);
    if (r.ok) setTimeout(refreshAll, 1000);
  });
  document.getElementById("persist-clear-btn").addEventListener("click", async () => {
    const r = await call("clear");
    showToast(r.ok ? "已清除全部显示器" : "清除失败：" + (r.error || "").slice(0, 200), r.ok);
    refreshAll();
  });
  document.getElementById("persist-auto").addEventListener("change", async (e) => {
    const enable = e.target.checked;
    const r = await call("register-task", enable ? [] : ["off"]);
    showToast(r.ok ? (enable ? "已开启开机自恢复（登录时自动恢复显示器）" : "已关闭开机自恢复") : "设置失败：" + (r.error || "").slice(0, 120), r.ok);
    refreshPersist();
  });

  // Diag page
  document.querySelectorAll("[data-diag]").forEach((b) =>
    b.addEventListener("click", () => runDiag(b.dataset.diag)));
  document.getElementById("diag-log-btn").addEventListener("click", () =>
    window.chrome?.webview?.postMessage({ id: 0, cmd: "openlog", args: [] }));

  // Settings
  const trayBox = document.getElementById("set-tray");
  (async () => {
    const r = await call("tray-status");
    try { trayBox.checked = r.data === "1"; } catch (e) {}
  })();
  trayBox.addEventListener("change", async () => {
    const enable = trayBox.checked;
    const r = await call("settray", [enable ? "1" : "0"]);
    showToast(r.ok ? (enable ? "已启用托盘（关闭窗口时最小化到托盘）" : "已禁用托盘（关闭窗口将退出程序）") : "设置失败", r.ok);
  });

  // Custom title bar
  document.getElementById("tb-min").addEventListener("click", () =>
    window.chrome?.webview?.postMessage({ id: 0, cmd: "--minimize", args: [] }));
  document.getElementById("tb-close").addEventListener("click", () =>
    window.chrome?.webview?.postMessage({ id: 0, cmd: "--close", args: [] }));
  document.getElementById("titlebar").addEventListener("mousedown", (e) => {
    if (e.target.closest(".tb-btn")) return;
    if (e.button === 0) {
      call("--drag");
    }
  });

  // 在此屏打开程序（run-display <index>）或 在鼠标所在屏打开（run-here）
  const fileInput = document.createElement("input");
  fileInput.type = "file";
  fileInput.accept = ".exe,.bat,.cmd,.lnk";
  fileInput.style.display = "none";
  document.body.appendChild(fileInput);
  let pendingRun = null;
  fileInput.addEventListener("change", async () => {
    const path = fileInput.value;
    fileInput.value = "";
    if (!path || !pendingRun) return;
    const args = pendingRun.args.concat([path]);
    const r = await call(pendingRun.cmd, args);
    const label = pendingRun.cmd === "run-display" ? `显示器 ${pendingRun.idx}` : "鼠标所在屏";
    showToast(r.ok ? `已在${label}打开程序` : "打开失败：" + (r.error || "").slice(0, 160), r.ok);
  });
  function pickAndRun(cmd, idx) {
    pendingRun = { cmd, idx, args: idx ? [String(idx)] : [] };
    fileInput.click();
  }

  // Boot
  refreshDashboard();
  refreshGpu();
  refreshPersist();
});
