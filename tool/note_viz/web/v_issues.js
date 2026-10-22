/* 问题: everything checks.py produced, plus repeated passages and the
   TODO checklist. This is the tab a reviewer actually works from. */
(function () {
  'use strict';
  const { $, esc, inl, chip, sevChip } = window.UI;
  const D = window.DOC;

  const CAT_HINT = {
    '字段布局': '偏移或长度前后对不上',
    '帧长': '文中写的请求/响应长度与参数表推算不一致',
    '帧数': '多帧响应的帧数与长度不匹配',
    '状态码': '定义了没用到、或者用了没定义',
    '命名': '同一命令在总表和小节标题里名字不同',
    '编号': '章节号 / 疏漏号不连续',
    '引用': '§x / 疏漏 n 指向不存在的目标',
    '覆盖': '疏漏没有对应的待办、待决没进待决清单',
    '需求': '需求没完全落地',
    '进度': '§12 待办完成度',
    '代码锚点': '文档引用的代码符号 / 行号与工程对不上'
  };

  function issueCard(i) {
    const sec = window.UI.secAt(i.line);
    return `<div class="card" style="border-left:3px solid var(--${i.sev === 'error' ? 'err' : i.sev === 'warn' ? 'warn' : 'info'})">
      <div style="display:flex;gap:8px;align-items:baseline;flex-wrap:wrap">
        ${sevChip(i.sev)}
        <b style="font-size:13.5px">${inl(i.title)}</b>
        <span class="chip">${esc(i.cat)}</span>
        <a class="lnk" style="font-size:11.5px" data-nav="line:${i.line}">L${i.line}${sec ? ' · ' + esc(sec.key || sec.title) : ''}</a>
      </div>
      ${i.detail ? `<div style="margin-top:7px;font-size:12.5px;color:var(--fg-dim);white-space:pre-wrap">${inl(i.detail)}</div>` : ''}
    </div>`;
  }

  function dupes() {
    if (!D.dupes.length) return '';
    return `<h2>重复表述 <span class="chip">${D.dupes.length}</span></h2>
      <p class="note-inline">不同章节里说了同一件事。有时是必要的呼应，有时是两处要一起改的隐患。</p>
      <table><thead><tr><th>位置</th><th>重合度</th><th>最长同句</th><th>内容</th></tr></thead><tbody>
      ${D.dupes.slice(0, 30).map(d => `<tr>
        <td><a class="lnk" data-nav="line:${d.a}">L${d.a}</a> ↔ <a class="lnk" data-nav="line:${d.b}">L${d.b}</a></td>
        <td>${d.cover.toFixed(2)}</td>
        <td style="white-space:normal"><code>${esc(d.lcs)}</code></td>
        <td style="white-space:normal;font-size:12px;color:var(--fg-dim)">
          <div title="${esc(d.ta)}">${inl(d.ta.slice(0, 100))}${d.ta.length > 100 ? '…' : ''}</div>
          <div title="${esc(d.tb)}" style="color:var(--fg-mute)">${inl(d.tb.slice(0, 100))}${d.tb.length > 100 ? '…' : ''}</div>
        </td></tr>`).join('')}
      </tbody></table>`;
  }

  function checklist() {
    const groups = {};
    D.checklist.forEach(c => (groups[c.group] = groups[c.group] || []).push(c));
    const done = D.checklist.filter(c => c.done).length;
    let html = `<h2>§12 固件待实现清单 <span class="chip ${done === D.checklist.length ? 'ok' : 'warn'}">${done}/${D.checklist.length}</span></h2>`;
    for (const g of Object.keys(groups)) {
      html += `<div class="card"><h3>${esc(g || '未分组')}</h3>
        <div class="detail-list">${groups[g].map(c => `<div style="font-size:12.5px;
          color:${c.done ? 'var(--fg-mute)' : 'var(--fg-dim)'}">
          <span style="font:11px var(--mono);color:${c.done ? 'var(--acc2)' : 'var(--fg-mute)'}">${c.done ? '☑' : '☐'}</span>
          ${inl(c.text)}
          <a class="lnk" style="font-size:10.5px;color:var(--fg-mute)" data-nav="line:${c.line}">L${c.line}</a>
        </div>`).join('')}</div></div>`;
    }
    return html;
  }

  function render(el) {
    const cats = {};
    D.issues.forEach(i => (cats[i.cat] = (cats[i.cat] || 0) + 1));
    const order = { error: 0, warn: 1, info: 2 };
    const sorted = D.issues.slice().sort((a, b) =>
      order[a.sev] - order[b.sev] || a.line - b.line);

    let html = `<div class="doc"><h1 style="font-size:22px">问题 <span class="chip ${sorted.some(i => i.sev === 'error') ? 'err' : 'ok'}">${D.issues.length}</span></h1>
      <p class="note-inline">来自 checks.py 的机器检查：字段偏移、帧长/帧数、状态码、编号、引用、覆盖、代码锚点。</p></div>`;

    html += `<div class="stats">${Object.keys(cats).map(c =>
      `<div class="stat"><b>${cats[c]}</b><span>${esc(c)}</span></div>`).join('')}</div>`;

    html += `<h2>清单</h2>` +
      (sorted.length ? `<div style="display:flex;flex-direction:column;gap:2px">${sorted.map(i =>
        `<div class="issue" data-nav="line:${i.line}">${sevChip(i.sev)}
          <div class="body"><div class="t">${inl(i.title)}<span class="cat">${esc(i.cat)} · L${i.line}</span></div></div></div>`
      ).join('')}</div>` : '<p class="empty">没有检查出问题。</p>');

    html += `<h2>分类说明</h2><div class="card"><table><thead><tr><th>类别</th><th>数量</th><th>含义</th></tr></thead><tbody>
      ${Object.keys(cats).map(c => `<tr><td>${esc(c)}</td><td>${cats[c]}</td>
        <td style="white-space:normal;color:var(--fg-dim)">${esc(CAT_HINT[c] || '')}</td></tr>`).join('')}
      </tbody></table></div>`;

    html += `<h2>详情</h2>${sorted.map(issueCard).join('')}`;
    html += dupes();
    html += checklist();
    el.innerHTML = html;
  }

  App.register('issues', { render });
})();
