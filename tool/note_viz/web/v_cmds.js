/* 命令: the review view for the protocol itself. Each command is shown as a
   request/response chain: frame lengths, field tables, wire bytes, mode rule,
   timeouts, status codes referenced — with the doc's own numbers checked. */
(function () {
  'use strict';
  const { $, esc, inl, chip, secAt } = window.UI;
  const D = window.DOC;
  const { renderSection } = window.MD;

  const FRAME_DATA = 16, HDR = 4;
  const modeLabel = m => ({ any: '任意模式', cal: '须在标定模式', not_cal: '须不在标定模式' }[m] || m || '—');

  function fmtReq(r) {
    if (!r) return null;
    return r.kind === 'range' ? `${r.lo}~${r.hi} 字节` : r.vals.map(v => v + ' 字节').join(' 或 ');
  }

  function fieldTable(c, fields, label) {
    if (!fields || !fields.length) return '';
    let html = `<table><thead><tr><th>偏移</th><th>长</th><th>字段</th><th>说明</th></tr></thead><tbody>`;
    for (const f of fields) {
      const bad = f.off == null || f.len == null;
      html += `<tr><td>${bad ? '?' : f.off}</td><td>${f.len_raw || f.len || '…'}</td>
        <td><code>${esc(f.name)}</code></td>
        <td style="white-space:normal;color:var(--fg-dim)">${inl(f.note || '')}</td></tr>`;
    }
    html += `</tbody></table>`;
    const sum = fields.reduce((a, f) => a + (f.len || 0), 0);
    return `<div class="sub">${label} · 字段合计 ${sum} 字节 ${fields.some(f => f.len == null) ? '（含变长）' : ''}</div>` + html;
  }

  function bitTables(c) {
    if (!c.bits.length) return '';
    return c.bits.map(b => {
      const label = b.label === 'flags' || b.label === 'mask' ? b.label : b.label;
      let html = `<div class="sub" style="margin-top:14px">${esc(label)} 位定义</div>
        <table><thead>${b.head.map(x => `<th>${inl(x)}</th>`).join('')}</thead><tbody>`;
      for (const r of b.rows) {
        html += `<tr>${b.head.map(k => `<td style="white-space:normal">${inl(r[k] || '')}</td>`).join('')}</tr>`;
      }
      return html + `</tbody></table>`;
    }).join('');
  }

  function wirePreview(c) {
    // request header bytes
    let req = `<span class="byte hdr">${c.code.slice(2).toUpperCase()}<small>CMD</small></span>
      <span class="byte tag">xx<small>TAG</small></span>`;
    for (const f of c.req_fields) {
      if (f.len == null) { req += `<span class="byte data">n×${f.len_raw || '?'}<small>${esc(f.name)}</small></span>`; continue; }
      for (let i = 0; i < f.len; i++)
        req += `<span class="byte data">··<small>${i === 0 ? esc(f.name) : ''}</small></span>`;
    }
    // response header
    let resp = `<span class="byte hdr">${(c.num | 0x80).toString(16).toUpperCase()}<small>CMD|0x80</small></span>
      <span class="byte tag">xx<small>TAG</small></span>
      <span class="byte st">00<small>STATUS</small></span>
      <span class="byte frag">8x<small>FRAG</small></span>`;
    const sz = c.resp.max_size != null ? c.resp.max_size : c.resp.size;
    if (sz) {
      const n = Math.min(sz, FRAME_DATA);
      for (let i = 0; i < n; i++) resp += `<span class="byte data">··<small></small></span>`;
      if (sz > FRAME_DATA) resp += `<span class="byte gap">…共 ${sz} B<small>${c.resp.frames || c.resp.calc_frames} 帧</small></span>`;
    }
    return `<div class="chain">
      <div class="stage"><h4>请求帧 · ${fmtReq(c.req_calc) || '?'}</h4><div class="bytes">${req}</div></div>
      <div class="stage ok"><h4>响应帧 · 头 4 B${sz ? ` + DATA ${sz} B` : ' + 无 DATA'}</h4><div class="bytes">${resp}</div></div>
    </div>`;
  }

  function verdicts(c) {
    const out = [];
    const decl = c.req_decl, calc = c.req_calc;
    const same = decl && calc && decl.kind === calc.kind &&
      (decl.kind === 'range' ? decl.lo === calc.lo && decl.hi === calc.hi
        : JSON.stringify(decl.vals.slice().sort()) === JSON.stringify(calc.vals.slice().sort()));
    if (!decl) out.push(chip('请求帧长未写明', 'err'));
    else if (!same) out.push(chip(`请求帧长 ${fmtReq(decl)} ≠ 推算 ${fmtReq(calc)}`, 'err'));
    else out.push(chip('请求帧长一致', 'ok'));

    const sz = c.resp.size, sum = c.resp.field_sum;
    if (sz != null && sum != null && sz !== sum) out.push(chip(`响应声明 ${sz} B ≠ 字段合计 ${sum} B`, 'err'));
    if (c.resp.frames && (sz != null)) {
      const want = Math.max(1, Math.ceil(sz / FRAME_DATA));
      if (want !== c.resp.frames) out.push(chip(`${sz} B 应为 ${want} 帧，文中 ${c.resp.frames} 帧`, 'err'));
    }
    if (!c.resp_fields.length && c.resp.size && c.resp.size > 0 && !c.resp.para.includes('= 生效'))
      out.push(chip('响应无字段表', 'info'));
    if (c.timeout && c.timeout.default) out.push(chip('超时按普通命令默认值', 'sdk'));
    return out.join(' ');
  }

  function ownParas(c) {
    const sec = D.secs[c.sec];
    const blocks = D.blocks.filter(b => b.line > c.line && b.line <= sec.own_end);
    return blocks.map(b => {
      if (b.t === 'p') return `<p>${inl(b.text)}</p>`;
      if (b.t === 'quote') return `<blockquote>${b.children.map(x => x.t === 'p' ? `<p>${inl(x.text)}</p>` : '').join('')}</blockquote>`;
      if (b.t === 'list') return `<ul>${b.items.map(it => `<li>${inl(it.text)}</li>`).join('')}</ul>`;
      return '';
    }).join('');
  }

  function relatedFlags(c) {
    // commands that mention APP_PROTO_FLAG_ / flags bits in their own text
    return '';
  }

  function progressBar(c) {
    const ck = D.checklist.filter(x => new RegExp(`\\b${c.name}\\b`).test(x.text));
    if (!ck.length) return '';
    const done = ck.filter(x => x.done).length;
    return `<div class="sub">§12 待办：${done}/${ck.length} 完成</div>` +
      ck.map(x => `<div style="font-size:12.5px;color:${x.done ? 'var(--fg-mute)' : 'var(--fg-dim)'}">
        ${x.done ? '☑' : '☐'} ${inl(x.text)}</div>`).join('');
  }

  function detail(c) {
    const sec = D.secs[c.sec];
    const r = c.resp;
    let html = `<div class="doc cmd-detail">
      <h2><span class="c">${c.code}</span> ${esc(c.name)}
        <span style="font-size:14px;font-weight:400;color:var(--fg-dim)">${esc(c.title)}</span></h2>
      <p style="color:var(--fg-mute);font-size:12px">${esc(sec.key || '')} · L${c.line}–${c.end}
        · ${chip(modeLabel(c.mode), c.mode === 'cal' ? 'proto' : '')}</p>
      <div style="margin:8px 0">${verdicts(c)}</div>
    </div>`;

    html += `<div class="card">${wirePreview(c)}</div>`;

    html += `<div class="grid2">
      <div class="card"><h3>参数</h3>
        <div class="sub">${fmtReq(c.req_calc) || '?'}（帧头 2 + 参数 ${(c.req_calc && (c.req_calc.kind === 'range' ? c.req_calc.hi - 2 : Math.max.apply(null, c.req_calc.vals) - 2)) || 0}）</div>
        ${c.req_fields.length ? fieldTable(c, c.req_fields, '参数表') : '<p class="empty">无参数</p>'}
        ${bitTables(c)}
      </div>
      <div class="card"><h3>响应 DATA</h3>
        <div class="sub">${r.size != null ? r.size + ' 字节' : r.max_size != null ? `最大 ${r.max_size} 字节` : '无 DATA'}
          ${(r.frames || r.calc_frames) ? ` · ${r.frames || r.calc_frames} 帧` : ''}</div>
        ${c.resp_fields.length ? fieldTable(c, c.resp_fields, '响应表') : `<p class="note-inline">${inl(r.para || '')}</p>`}
      </div>
    </div>`;

    html += `<div class="grid2">
      <div class="card"><h3>时序与约束</h3>
        <table><tbody>
          <tr><th>标定模式</th><td style="white-space:normal">${esc(c.mode_raw || modeLabel(c.mode))}</td></tr>
          <tr><th>客户端超时</th><td>${c.timeout ? esc(c.timeout.value) + (c.timeout.default ? ' <span style="color:var(--fg-mute)">(默认)</span>' : '') : '—'}</td></tr>
          <tr><th>写 flash</th><td>${c.writes_flash ? chip('会占用 flash 寿命', 'warn') : '否'}</td></tr>
          <tr><th>可能状态码</th><td style="white-space:normal">${c.statuses.map(s => stChip(s)).join(' ') || '仅 OK'}</td></tr>
          ${c.reqs.length ? `<tr><th>对应需求</th><td>${c.reqs.map(n => '需求 ' + n).join(' · ')}</td></tr>` : ''}
        </tbody></table>
        ${progressBar(c)}
      </div>
      <div class="card"><h3>正文说明</h3><div class="doc">${ownParas(c)}</div></div>
    </div>`;

    html += `<div class="doc"><p style="font-size:12px">
      <a class="lnk" data-nav="sec:${c.sec}">在原文中查看 ${esc(sec.key || '')}</a>
      · 被其它章节引用 ${D.refs.filter(x => x.kind === 'cmd' && x.tid === c.sec).length} 次</p></div>`;
    return html;
  }

  function stChip(name) {
    const s = D.status.find(x => x.name === name);
    return chip(name + (s ? ' ' + s.val : ''), name === 'ERR_INTERNAL' ? 'sdk' : 'warn');
  }

  function render(el, arg) {
    const code = arg ? (arg.toLowerCase().startsWith('0x') ? arg.toLowerCase() : '0x' + arg.toLowerCase()) : D.cmds[0].code;
    const c = D.cmds.find(x => x.code === code) || D.cmds[0];
    el.innerHTML = detail(c);
    // sync sidebar selection
    document.querySelectorAll('#side-body .tree-item').forEach(x =>
      x.classList.toggle('on', x.dataset.nav === 'cmd:' + c.code));
  }

  App.register('commands', { render });
})();
