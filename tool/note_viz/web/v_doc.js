/* 原文: the markdown as written, with line numbers per block so any review
   comment can point at an exact line. Double-click a block to annotate it. */
(function () {
  'use strict';
  const { $, esc, inl, renderBlock } = window.UI;
  const D = window.DOC;

  function lineRows() {
    // render block by block, each block carrying its own line gutter
    return D.blocks.map(b => {
      const end = b.end || b.line;
      const n = Math.max(1, end - b.line + 1);
      const notes = D.review.filter(x => x.line >= b.line && x.line <= end);
      const badge = notes.length ? ` <span class="chip warn">${notes.length} 批注</span>` : '';
      return `<div class="blk" data-line="${b.line}" data-end="${end}"
          style="display:grid;grid-template-columns:58px 1fr;gap:0;position:relative">
        <div class="gutter" style="font:11px var(--mono);color:var(--fg-mute);text-align:right;
            padding:2px 10px 0 0;user-select:none;white-space:pre">${b.line}${end > b.line ? '\n' + end : ''}</div>
        <div class="blkbody" style="min-width:0" title="双击添加批注">${renderBlock(b)}${badge}</div>
      </div>`;
    }).join('');
  }

  function render(el, arg) {
    let html = `<div class="card" style="display:flex;gap:10px;align-items:center;flex-wrap:wrap">
      <b style="font-size:13px">原文</b>
      <span style="color:var(--fg-mute);font-size:12px">${D.meta.lines} 行 ·
        ${D.blocks.length} 个块 · 双击任意段落添加批注</span>
      <span style="margin-left:auto;display:flex;gap:6px">
        <button id="doc-expand">展开全部</button>
        <button id="doc-fold">只看标题</button>
      </span>
    </div>
    <div class="doc" id="doc-body">${lineRows()}</div>`;

    if (D.review.length) {
      html += `<h2>批注 <span class="chip">${D.review.length}</span></h2>`;
      html += D.review.map(n => `<div class="card ${n.stale ? 'review-stale' : ''}">
        <div class="sub">L${n.line || '?'} · <code>${esc(n.match)}</code>${n.stale ? ' · 锚点已失效' : ''}</div>
        <div>${inl(n.text)}</div></div>`).join('');
    }
    el.innerHTML = html;

    // per-block double-click -> editor anchored at that line
    el.querySelectorAll('.blk').forEach(b => {
      b.addEventListener('dblclick', e => {
        if (window.getSelection().toString()) return;
        const ln = parseInt(b.dataset.line, 10);
        window.App.openNoteEditor(ln, D.lines[ln - 1].trim().slice(0, 40));
      });
    });

    $('#doc-expand').onclick = () => {
      el.querySelectorAll('.blkbody').forEach(x => x.style.display = '');
    };
    $('#doc-fold').onclick = () => {
      el.querySelectorAll('.blk').forEach(b => {
        const isH = b.querySelector('h1,h2,h3');
        b.querySelector('.blkbody').style.display = isH ? '' : 'none';
      });
    };

    if (arg) {
      const ln = parseInt(arg, 10);
      const t = el.querySelector(`#doc-body .blk[data-line="${ln}"]`) ||
        Array.from(el.querySelectorAll('#doc-body .blk'))
          .find(x => +x.dataset.line <= ln && ln <= +x.dataset.end);
      if (t) window.UI.flash(t);
    }
  }

  App.register('doc', { render });
})();
