/* 关系: reference graph between sections/commands/疏漏/需求. Also the
   requirement -> command -> code coverage roll-up. Rendered as a layered
   matrix rather than a force graph so it stays readable at any width. */
(function () {
  'use strict';
  const { $, esc, inl, chip } = window.UI;
  const D = window.DOC;

  function card(title, body, sub) {
    return `<div class="card"><h3>${esc(title)}</h3>${sub ? `<div class="sub">${esc(sub)}</div>` : ''}${body}</div>`;
  }

  /** Compact a list of sections into links; collapse the 12 command subsections
   *  into "§5 命令详表" because that is what a reader is actually after. */
  function links(secs, max) {
    max = max || 6;
    const top = [];
    const seen = new Set();
    for (const s of secs) {
      const root = D.secs[s.id].parent != null && D.secs[D.secs[s.id].parent].key === '§5'
        ? D.secs[D.secs[s.id].parent] : s;
      if (seen.has(root.id)) continue;
      seen.add(root.id);
      top.push(root);
    }
    if (!top.length) return '<span style="color:var(--fg-mute)">孤立</span>';
    let html = top.slice(0, max).map(s =>
      `<a class="lnk" data-nav="sec:${s.id}">${esc(window.secLabel(s))}</a>`).join(' ');
    if (top.length > max) html += ` <span style="color:var(--fg-mute)">+${top.length - max}</span>`;
    return html;
  }

  function ovMatrix() {
    const ovs = D.ovs;
    if (!ovs.length) return '<p class="empty">没有疏漏小节。</p>';
    let html = `<table><thead><tr><th>条目</th><th>标签</th><th>字</th>
      <th>被引用</th><th>指向</th></tr></thead><tbody>`;
    for (const o of ovs) {
      const back = o.back.map(id => D.secs[id]).filter(Boolean);
      const fwd = o.fwd.map(id => D.secs[id]).filter(Boolean);
      html += `<tr><td><a class="lnk" data-nav="sec:${o.sec}">${esc(o.key)}</a></td>
        <td>${o.tag ? chip(o.tag, o.sev >= 3 ? 'err' : o.tag.includes('待决') ? 'info' : 'warn') : '—'}</td>
        <td>${o.chars}</td>
        <td style="white-space:normal">${links(back)}</td>
        <td style="white-space:normal">${links(fwd)}</td></tr>`;
    }
    return html + '</tbody></table>';
  }

  function orphanSections() {
    const referenced = new Set(D.refs.filter(r => r.tid != null).map(r => r.tid));
    const leaves = D.secs.filter(s => s.level >= 2 && s.kind !== 'title');
    const orphans = leaves.filter(s => !referenced.has(s.id) && !s.children.length);
    return card('没被任何地方引用的叶子章节（' + orphans.length + '）',
      orphans.length ? `<div style="display:flex;flex-wrap:wrap;gap:5px">
        ${orphans.map(s => `<a class="lnk chip row" data-nav="sec:${s.id}">${esc(window.secLabel(s))}</a>`).join('')}</div>`
        : '<p class="empty">无</p>',
      '这些节只在自己的位置出现，结构上不会再被读者跳回来。');
  }

  function unresolved() {
    const bad = D.refs.filter(r => r.tid == null && ['sec', 'ovs', 'sug'].includes(r.kind));
    const groups = {};
    bad.forEach(r => { (groups[r.target] = groups[r.target] || []).push(r); });
    const keys = Object.keys(groups);
    return card('悬空引用（' + keys.length + '）',
      keys.length ? keys.map(k => `<div style="font-size:12.5px;margin:3px 0">
        <code>${esc(k)}</code> ← L${groups[k].map(r => r.line).join(', L')}</div>`).join('')
        : '<p class="empty">全部引用都能解析。</p>',
      '§x / 疏漏 n / 建议 X 在文中出现但目标章节不存在。');
  }

  function reqMap() {
    let html = `<table><thead><tr><th>#</th><th>需求原文</th><th>命令</th><th>疏漏</th><th>状态</th></tr></thead><tbody>`;
    for (const r of D.reqs) {
      html += `<tr><td style="white-space:nowrap;color:var(--fg-mute)">${r.req}</td>
        <td style="white-space:normal">${inl(r.text)}</td>
        <td style="white-space:normal">${r.cmds.map(c => `<a class="lnk refcmd" data-nav="cmd:${c}">${c}</a>`).join(' ') || '—'}</td>
        <td style="white-space:normal">${r.ovs.map(o => {
          const ov = D.ovs.find(x => x.key === o);
          return ov ? `<a class="lnk" data-nav="sec:${ov.sec}">${esc(o)}</a>` : esc(o);
        }).join(' ') || '—'}</td>
        <td style="white-space:normal">${inl(r.status)}</td></tr>`;
    }
    return html + '</tbody></table>';
  }

  function codeRoll() {
    const byStatus = {};
    D.code.forEach(c => (byStatus[c.status] = byStatus[c.status] || []).push(c));
    let html = `<table><thead><tr><th>状态</th><th>数量</th><th>符号</th></tr></thead><tbody>`;
    for (const k of ['code', 'sdk', 'proto', 'planned', 'comment', 'missing']) {
      const list = byStatus[k] || [];
      if (!list.length) continue;
      html += `<tr><td>${chip(window.UI.codeLabel[k] || k, window.UI.codeCls[k])}</td><td>${list.length}</td>
        <td style="white-space:normal">${list.slice(0, 24).map(c => `<code style="cursor:pointer" onclick="App.nav('code','${esc(c.tok)}')">${esc(c.tok)}</code>`).join(' ')}${list.length > 24 ? ' …' : ''}</td></tr>`;
    }
    return html + '</tbody></table>';
  }

  function render(el) {
    el.innerHTML = `<div class="doc"><h1 style="font-size:22px">关系</h1>
      <p class="note-inline">命令、疏漏、需求、代码符号之间的相互引用。孤立项与悬空引用比正文更容易暴露结构问题。</p></div>
      ${card('疏漏 ↔ 引用', ovMatrix(), D.ovs.length + ' 条疏漏/建议')}
      ${card('需求 → 命令', reqMap(), D.reqs.length + ' 条需求')}
      ${card('代码符号落点', codeRoll(), D.code.length + ' 个符号')}
      <div class="grid2">${orphanSections()}${unresolved()}</div>`;
  }

  App.register('graph', { render });
})();
