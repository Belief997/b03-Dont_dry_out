/* 代码: every code-ish token the doc cites, and where it actually lives.
   The point is to catch drift between the spec (this note) and the firmware. */
(function () {
  'use strict';
  const { $, esc, inl, chip, codeCls, codeLabel } = window.UI;
  const D = window.DOC;

  const GROUP_ORDER = ['code', 'missing', 'planned', 'proto', 'comment', 'sdk', 'dir', 'file'];
  const GROUP_HINT = {
    code: '文档提到的标识符，在工程代码里确实存在（含缩写展开，如 RECS_PER_PAGE → APP_STORAGE_RECS_PER_PAGE）',
    missing: '文档提到了，但工程里找不到，文中也没说是待新增 —— 最容易是文档漂移或笔误',
    planned: '文档明说「待新增 / 待建」，属于实现清单里的事，不算漂移',
    proto: '本文自己定义的线格式字段（帧里的名字），本来就不该出现在代码里',
    comment: '只在注释里出现，没有任何实际代码使用',
    sdk: 'Nordic SDK 里的符号，不是本工程代码',
    dir: '目录',
    file: '文件，在 dev/ 下能找到'
  };

  function statusChip(s) { return chip(codeLabel[s] || s, codeCls[s] || ''); }

  function row(c) {
    let info = '';
    if (c.loc) {
      const m = /^(.*?):(\d+)$/.exec(c.loc);
      if (m && c.status !== 'dir' && c.status !== 'file') {
        info += `<span class="loc" onclick="App.nav('line','${m[2]}')" title="跳到原文引用该行的位置">${esc(m[1])}:${m[2]}</span>`;
      } else {
        info += `<span class="loc" style="color:var(--fg-dim);cursor:default">${esc(c.loc)}</span>`;
      }
    }
    if (c.full) info += ` <span class="full">= ${esc(c.full)}</span>`;
    if (c.value) info += ` <span class="full">= ${esc(c.value)}</span>`;
    if (c.count > 1) info += ` <span class="full">· ${c.count} 处</span>`;
    if (c.code) info += `<code>${esc(c.code)}</code>`;
    const lines = c.lines.slice(0, 6).map(n => `L${n}`).join(' ');
    return `<div class="code-row" data-tok="${esc(c.tok)}">
      <span class="tok" title="${esc(c.tok)}">${esc(c.tok)}</span>
      <span class="info">${info}
        <span class="full">文中 ${lines}${c.lines.length > 6 ? ' …' : ''}</span>
        <a class="lnk" style="font-size:11px" data-nav="line:${c.lines[0]}">跳到首次出现</a>
      </span></div>`;
  }

  function citaSection() {
    if (!D.cites.length) return '';
    return `<h2>行号引用校验 <span class="chip ${D.cites.some(c => !c.ok) ? 'err' : 'ok'}">${D.cites.length}</span></h2>
      <p class="note-inline"><code>file.c:NNN</code> 形式的引用：那一行现在还是不是文中说的东西。</p>
      ${D.cites.map(c => `<div class="card" style="border-color:${c.ok ? 'var(--line)' : 'var(--err)'}">
        <div class="sub">L${c.doc_line} · <code>${esc(c.cite)}</code> ${c.ok ? chip('仍然对得上', 'ok') : chip('已过时', 'err')}</div>
        <div style="font:12px var(--mono)"><span style="color:var(--fg-mute)">第 ${c.n} 行现在是：</span> ${esc(c.now)}</div>
        ${c.ok ? '' : `<div class="sub" style="margin-top:8px">文中这一节提到的标识符出现在：</div>
          ${c.cands.map(x => `<div style="font:11.5px var(--mono);color:var(--fg-dim)">${esc(c.file)}:${x.line} — ${esc(x.code)}</div>`).join('')}`}
      </div>`).join('')}`;
  }

  function render(el, arg) {
    const q = (arg || '').toLowerCase();
    const groups = {};
    D.code.forEach(c => (groups[c.status] = groups[c.status] || []).push(c));

    let html = `<div class="doc"><h1 style="font-size:22px">代码锚点
      <span class="chip">${D.code.length}</span></h1>
      <p class="note-inline">文档里每个反引号包起来的标识符/文件名，都被拿去工程里找了一圈。
      分类看：<b>未找到</b> 与 <b>已过时行号</b> 是真正要处理的。</p></div>`;

    // filter box
    html += `<div class="card" style="display:flex;gap:8px;align-items:center">
      <input id="code-q" placeholder="过滤符号…" value="${esc(arg || '')}"
        style="flex:1;background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:5px 9px;outline:none">
      ${GROUP_ORDER.filter(k => groups[k]).map(k =>
        `<span class="chip ${codeCls[k]}">${esc(codeLabel[k] || k)} ${groups[k].length}</span>`).join(' ')}
    </div>`;

    html += citaSection();

    for (const k of GROUP_ORDER) {
      let list = groups[k] || [];
      if (!list.length) continue;
      if (q) list = list.filter(c => c.tok.toLowerCase().includes(q));
      if (!list.length) continue;
      html += `<div class="code-grp">
        <h3>${statusChip(k)} <span class="cnt">${list.length}</span></h3>
        <div class="sub" style="color:var(--fg-mute);font-size:11.5px;margin-bottom:4px">${esc(GROUP_HINT[k] || '')}</div>
        ${list.map(row).join('')}
      </div>`;
    }

    if (!html.includes('code-grp')) html += '<p class="empty">没有匹配的符号。</p>';
    el.innerHTML = html;

    const inp = $('#code-q');
    if (inp) {
      inp.oninput = () => { App.nav('code', inp.value, true); };
      inp.onkeydown = e => { if (e.key === 'Enter') inp.blur(); };
    }
  }

  App.register('code', { render });
})();
