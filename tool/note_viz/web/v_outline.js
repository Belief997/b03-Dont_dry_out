/* 结构: section-by-section reading with structure metrics (chars, warnings,
   why-paragraphs, back/forward references) instead of a plain ToC. */
(function () {
  'use strict';
  const { $, esc, inl, chip } = window.UI;
  const D = window.DOC;
  const { renderSection } = window.MD;

  function backrefs(sec) {
    return D.refs.filter(r => r.tid === sec.id && r.kind !== 'note');
  }
  function fwdrefs(sec) {
    return D.refs.filter(r => r.src === sec.id && r.tid != null);
  }

  function metricBar(sec) {
    const all = D.secs.filter(s => s.level <= 3 && s.chars > 0);
    const max = Math.max.apply(null, all.map(s => s.chars)) || 1;
    const pct = Math.round(100 * sec.chars / max);
    const back = backrefs(sec), fwd = fwdrefs(sec);
    return `<div class="card" style="margin:10px 0">
      <div class="sub">结构度量</div>
      <div style="display:flex;gap:16px;flex-wrap:wrap;font-size:12.5px;color:var(--fg-dim)">
        <span>${sec.chars} 字 <span style="color:var(--fg-mute)">(本节+子节)</span></span>
        <span>${sec.own_chars} 字 <span style="color:var(--fg-mute)">(仅本节)</span></span>
        <span>${sec.warn} 处 ⚠</span>
        <span>${sec.why} 段「为什么」</span>
        <span>被引用 ${back.length} 次 · 引用出去 ${fwd.length} 次</span>
      </div>
      <div style="margin-top:8px;height:5px;background:var(--bg3);border-radius:3px;overflow:hidden">
        <div style="width:${pct}%;height:100%;background:var(--acc)"></div>
      </div>
      ${back.length ? `<div style="margin-top:9px;font-size:12px;color:var(--fg-mute)">被这些地方引用：
        ${back.slice(0, 10).map(r => {
          const s = D.secs.find(x => x.id === r.src);
          return `<a class="lnk" data-nav="sec:${r.src}">${esc(window.secLabel(s))}</a>`;
        }).join(' · ')}</div>` : ''}
    </div>`;
  }

  function render(el, arg) {
    const sec = arg != null ? D.secs[parseInt(arg, 10)] : D.secs.find(s => s.level === 2);
    if (!sec) { el.innerHTML = '<p class="empty">没有这个章节。</p>'; return; }

    const kids = sec.children.map(id => D.secs[id]);
    const bc = window.MD.breadcrumb(sec);
    let html = `<div class="doc">
      <div style="display:flex;align-items:baseline;gap:10px;flex-wrap:wrap">
        <h1 style="margin:0">${esc(window.MD.keyOf(sec) ? window.MD.keyOf(sec) + ' ' : '')}${window.MD.headingText(sec)}</h1>
        <span class="chip">L${sec.line}–${sec.end}</span>
        ${sec.tag ? chip(sec.tag, /待决/.test(sec.tag) ? 'info' : 'warn') : ''}
      </div>
      ${bc ? `<p style="color:var(--fg-mute);font-size:11.5px">${bc}</p>` : ''}
    </div>`;

    html += metricBar(sec);

    html += `<div class="doc">${renderSection(sec, { top: true, children: true })}</div>`;

    // children as a jump list
    if (kids.length) {
      html += `<div class="card"><div class="sub">子节</div><div class="tocgrid">
        ${kids.map(k => `<a class="sec-link" data-nav="sec:${k.id}"><b>${esc(window.MD.keyOf(k))} ${window.MD.headingText(k)}</b>
          <span style="font-size:11px;color:var(--fg-mute)">${k.chars} 字 · ${k.warn} ⚠</span></a>`).join('')}
      </div></div>`;
    }

    html += `<div class="doc">${window.MD.pager(sec)}</div>`;
    el.innerHTML = html;
  }

  App.register('outline', { render });
})();
