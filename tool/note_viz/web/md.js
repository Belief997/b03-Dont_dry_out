/* Shared document helpers: render one section's own blocks, breadcrumbs,
   prev/next, and the review-note strip used by 原文 / 结构. */
(function () {
  'use strict';
  const { $, esc, inl, h, renderBlock, secById } = window.UI;
  const D = window.DOC;

  /** Blocks that belong to `sec` itself (not its children). */
  function ownBlocks(sec) {
    return D.blocks.filter(b => b.line > sec.line && b.line <= sec.own_end);
  }

  function headingOf(sec) {
    return D.blocks.find(b => b.t === 'h' && b.line === sec.line);
  }

  function childrenOf(sec) {
    return sec.children.map(id => secById[id]);
  }

  /** "§1.4" for numbered sections, "" for the intro/overview ones. */
  const keyOf = s => (s && s.key && /^§/.test(s.key) ? s.key : '');

  function breadcrumb(sec) {
    if (!sec || sec.level <= 2) return '';      // level-2 already is the page title
    const chain = [];
    let p = sec.parent;
    while (p != null) { chain.unshift(secById[p]); p = secById[p].parent; }
    // chain[0] is the h1 title (= the doc itself); start from its child
    return '<a class="lnk" data-nav="sec:' + chain[0].id + '">' +
      esc(chain[0].title) + '</a>' +
      chain.slice(1).filter(s => s.level > 1)
        .map(s => ` › <a class="lnk" data-nav="sec:${s.id}">${esc(keyOf(s) || s.title)}</a>`).join('');
  }

  /** Section heading text with the key prefix stripped, markdown kept. */
  function headingText(sec) {
    const blk = headingOf(sec);
    if (!blk) return esc(sec.title);
    let raw = blk.text;
    if (sec.kind === 'cmd') raw = raw.replace(/^0x[0-9A-Fa-f]{2}\s*`?CMD_[A-Z_]+`?\s*[—-]+\s*/, '');
    else raw = raw.replace(/^§?\s*[\d.]+\s+/, '');
    return inl(raw);
  }

  /** Render section heading + own blocks + child cards + its review notes. */
  function renderSection(sec, opts) {
    opts = opts || {};
    const lv = Math.min(4, Math.max(2, sec.level));
    const tag = sec.tag ? `<span class="chip ${/待决/.test(sec.tag) ? 'info' : /必/.test(sec.tag) ? 'err' : 'warn'}">${esc(sec.tag)}</span>` : '';
    let html = `<div class="sec" id="sec-${sec.id}">`;
    html += `<h${lv} style="margin-top:${opts.top ? 0 : 18}px">
      ${keyOf(sec) ? `<span style="color:var(--fg-mute);font:14px var(--mono);margin-right:8px">${esc(keyOf(sec))}</span>` : ''}
      ${sec.kind === 'cmd' ? `<code style="font-size:.8em">${esc(sec.key.toUpperCase())}</code> ` : ''}
      ${headingText(sec)} ${tag}</h${lv}>`;
    if (opts.crumb) {
      const bc = breadcrumb(sec);
      if (bc) html += `<div class="sub" style="color:var(--fg-mute);font-size:11.5px">${bc}</div>`;
    }

    if (sec.kind === 'cmd') html += cmdHeadline(sec);

    const own = ownBlocks(sec);
    html += own.map(b => renderBlock(b)).join('');

    const kids = childrenOf(sec);
    if (kids.length && opts.children !== false) {
      html += kids.map(k => `<div class="card">${renderSection(k, { top: true, crumb: false, children: false })}</div>`).join('');
    }

    D.review.filter(n => n.line >= sec.line && n.line <= sec.end && !n.stale).forEach(n => {
      html += `<div class="card" style="border-color:var(--warn)"><div class="sub">批注 · L${n.line}</div><div>${inl(n.text)}</div></div>`;
    });
    html += '</div>';
    return html;
  }

  function cmdHeadline(sec) {
    const c = D.cmds.find(x => x.sec === sec.id);
    if (!c) return '';
    const r = c.req_calc;
    const req = r ? (r.kind === 'range' ? `${r.lo}~${r.hi} 字节` : r.vals.map(v => v + ' 字节').join(' 或 ')) : '?';
    const resp = c.resp.max_size || c.resp.size;
    return `<div class="bytes">
      <span class="byte hdr">0x${c.code.slice(2).toUpperCase()}<small>CMD</small></span>
      <span class="byte tag">TAG<small>回填</small></span>
      ${req === '2 字节' ? '' : `<span class="byte data">${req.split(' ')[0]}<small>ARGS</small></span>`}
      <span class="byte gap" style="align-self:center;border:none">→</span>
      <span class="byte st">STATUS<small>0x00=OK</small></span>
      <span class="byte frag">FRAG<small>多帧</small></span>
      ${resp != null ? `<span class="byte data">${resp}<small>响应 DATA</small></span>` : ''}
    </div>`;
  }

  function neighbours(sec) {
    // walk the document order, skipping the h1 title whenever it would show up
    const i = D.secs.indexOf(sec);
    let p = i - 1, n = i + 1;
    while (p >= 0 && D.secs[p].level === 1) p--;
    while (n < D.secs.length && D.secs[n].level === 1) n++;
    return { prev: p >= 0 ? D.secs[p] : null, next: n < D.secs.length ? D.secs[n] : null };
  }

  function pager(sec) {
    const { prev, next } = neighbours(sec);
    const name = s => esc(window.secLabel(s));
    const a = prev ? `<a class="lnk" data-nav="sec:${prev.id}">← ${name(prev)}</a>` : '';
    const b = next ? `<a class="lnk" data-nav="sec:${next.id}">${name(next)} →</a>` : '';
    return `<div style="display:flex;justify-content:space-between;gap:16px;margin:24px 0 0;padding-top:12px;border-top:1px solid var(--line)">${a}<span style="text-align:right">${b}</span></div>`;
  }

  const stripNum = t => t.replace(/^§?\s*[\d.]+\s+/, '');

  window.MD = { ownBlocks, headingOf, childrenOf, breadcrumb, headingText, keyOf,
                renderSection, pager, cmdHeadline, stripNum };
})();
