/* Core: data accessors, inline markdown, routing, sidebar, search, theme. */
(function () {
  'use strict';

  const D = window.DOC;
  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));

  const esc = s => String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');

  /* ---------------------------------------------------------------- model */
  const secById = {};
  const secByKey = {};
  const cmdByCode = {};
  const cmdByName = {};
  D.secs.forEach(s => { secById[s.id] = s; if (s.key) secByKey[s.key] = s; });
  D.cmds.forEach(c => { cmdByCode[c.code] = c; cmdByName[c.name] = c; });
  D.ovs.forEach(o => { o.sec_obj = secById[o.sec]; });

  function secAt(line) {
    let best = null;
    for (const s of D.secs) {
      if (s.line <= line && line <= s.end && (!best || s.level > best.level)) best = s;
    }
    return best;
  }
  function blockAt(line) {
    let best = null;
    for (const b of D.blocks) {
      if (b.line <= line && line <= b.end && (!best || b.line >= best.line)) best = b;
    }
    return best;
  }
  const cmdKey = code => String(code).toLowerCase();
  function cmdByNum(n) { return D.cmds.find(c => c.num === (n & 0x7F)) || null; }

  const statusCls = { error: 'err', warn: 'warn', info: 'info' };
  const codeCls = {
    code: 'ok', sdk: 'sdk', missing: 'missing', proto: 'proto',
    planned: 'info', comment: 'sdk', dir: 'ok', file: 'ok'
  };
  const codeLabel = {
    code: '代码', sdk: 'SDK', missing: '未找到', proto: '线格式字段',
    planned: '待新增', comment: '仅注释', dir: '目录', file: '文件'
  };

  /* ---------------------------------------------------------------- inline md */
  const CMD_RE = /\bCMD_([A-Z][A-Z0-9_]+)\b/g;

  function linkify(html) {
    // §4.2 / §5  -> section links
    html = html.replace(/§\s*(\d+(?:\.\d+)?)/g, (m, n) => {
      const s = secByKey['§' + n];
      return s ? `<a class="lnk" data-nav="sec:${s.id}">${m}</a>` : m;
    });
    html = html.replace(/§\s*`(?:CMD_)?([A-Z_]+)`/g, (m, n) => {
      const c = cmdByName[n];
      return c ? `<a class="lnk refcmd" data-nav="cmd:${c.code}">§${esc(n)}</a>` : m;
    });
    html = html.replace(/疏漏\s*(\d+)/g, (m, n) => {
      const o = D.ovs.find(x => x.key === '疏漏' + n);
      return o ? `<a class="lnk" data-nav="sec:${o.sec}">${m}</a>` : m;
    });
    html = html.replace(/建议\s*([A-Z])(?![A-Za-z])/g, (m, l) => {
      const o = D.ovs.find(x => x.key === '建议' + l);
      return o ? `<a class="lnk" data-nav="sec:${o.sec}">${m}</a>` : m;
    });
    html = html.replace(/\b(0x[0-9A-Fa-f]{2})\s+`?(CMD_[A-Z_]+)?`?/g, (m, hex, nm) => {
      const c = cmdByCode[cmdKey('0x' + hex.slice(2).toLowerCase())];
      if (!c) return m;
      if (nm) return m; // the backticked name is linked separately
      return `<a class="lnk refcmd" data-nav="cmd:${c.code}">${hex}</a>`;
    });
    html = html.replace(CMD_RE, (m, n) => {
      const c = cmdByName[n];
      return c ? `<a class="lnk refcmd" data-nav="cmd:${c.code}">${m}</a>` : m;
    });
    return html;
  }

  /** Code spans -> <code>, still linkified because the doc writes every command
   *  as `CMD_X`. Italics are only applied outside code spans. */
  function codeSpans(t) {
    const parts = String(t).split(/`([^`]+)`/);
    let out = '';
    for (let i = 0; i < parts.length; i++) {
      if (i % 2) {
        out += `<code>${linkify(esc(parts[i]))}</code>`;
      } else {
        out += linkify(esc(parts[i]).replace(/(^|[\s(（])\*([^*\n]+)\*/g, '$1<i>$2</i>'));
      }
    }
    return out;
  }

  /** Inline markdown -> HTML. Bold is matched first so that `**`…`cmd`…**`
   *  (bold spanning a code span, which this doc uses a lot) still works. */
  function inl(src) {
    const re = /\*\*([^*]+)\*\*/g;
    let out = '', last = 0, m;
    const s = String(src);
    while ((m = re.exec(s))) {
      out += codeSpans(s.slice(last, m.index));
      out += `<b>${codeSpans(m[1])}</b>`;
      last = m.index + m[0].length;
    }
    return out + codeSpans(s.slice(last));
  }

  /* ---------------------------------------------------------------- elements */
  function h(html) {
    const t = document.createElement('template');
    t.innerHTML = html.trim();
    return t.content.firstElementChild;
  }
  function chip(text, cls) {
    return `<span class="chip ${cls || ''}">${esc(text)}</span>`;
  }
  function sevChip(sev) {
    const t = { error: '错', warn: '警', info: '注' }[sev] || sev;
    return `<span class="sev ${sev}">${t}</span>`;
  }

  /* ---------------------------------------------------------------- doc render */
  function renderBlock(b, opts) {
    opts = opts || {};
    switch (b.t) {
      case 'h': {
        const lv = Math.min(6, Math.max(1, b.level));
        const sid = D.secs.find(s => s.line === b.line);
        const id = sid ? ` id="sec-${sid.id}" data-sec="${sid.id}"` : '';
        return `<h${lv}${id} data-line="${b.line}">${inl(b.text)}</h${lv}>`;
      }
      case 'p':
        return `<p data-line="${b.line}">${inl(b.text)}</p>`;
      case 'hr':
        return `<hr data-line="${b.line}">`;
      case 'code':
        return `<pre data-line="${b.line}"><code>${esc(b.text)}</code></pre>`;
      case 'quote':
        return `<blockquote data-line="${b.line}">${b.children.map(c => renderBlock(c)).join('')}</blockquote>`;
      case 'list': {
        const tag = b.ordered ? 'ol' : 'ul';
        const items = b.items.map(it => {
          const cb = it.check === null ? '' :
            `<span class="cb">${it.check ? '☑' : '☐'}</span>`;
          return `<li class="${it.check === true ? 'done' : ''}" data-line="${it.line}">${cb}${inl(it.text)}</li>`;
        }).join('');
        return `<${tag} data-line="${b.line}">${items}</${tag}>`;
      }
      case 'table': {
        const head = b.head.map(c => `<th>${inl(c)}</th>`).join('');
        const rows = b.rows.map((r, i) => {
          const ln = b.rlines[i];
          const cls = r[0] && /^-+$/.test(r[0].trim()) ? '' : '';
          return `<tr class="${cls}" data-line="${ln}">${r.map(c => `<td>${inl(c)}</td>`).join('')}</tr>`;
        }).join('');
        return `<table data-line="${b.line}"><thead><tr>${head}</tr></thead><tbody>${rows}</tbody></table>`;
      }
      default:
        return '';
    }
  }

  function renderDoc(container, onlyLine) {
    container.innerHTML = `<div class="doc">${D.blocks.map(b => renderBlock(b)).join('')}</div>`;
    if (onlyLine) {
      const el = container.querySelector(`[data-line="${onlyLine}"]`);
      if (el) flash(el);
    }
  }

  function flash(el) {
    el.scrollIntoView({ block: 'center', behavior: 'smooth' });
    el.classList.add('hl');
    setTimeout(() => el.classList.remove('hl'), 1400);
  }

  /* ---------------------------------------------------------------- review notes */
  function reviewFor(line) {
    return D.review.filter(n => n.line === line);
  }
  function noteCard(n) {
    const stale = n.stale ? ' review-stale' : '';
    return `<div class="card${stale}" data-note="${esc(n.match)}">
      <div class="sub">批注 · ${esc(n.author || 'reviewer')}${n.stale ? ' · 锚点已失效' : ''}</div>
      <div>${inl(n.text)}</div>
    </div>`;
  }

  /* ---------------------------------------------------------------- routing */
  const TABS = [
    ['overview', '总览'],
    ['outline', '结构'],
    ['commands', '命令'],
    ['trace', '报文'],
    ['code', '代码'],
    ['graph', '关系'],
    ['issues', '问题'],
    ['doc', '原文']
  ];
  const V = {};           // tab -> {render(el, arg), side(arg), count()}
  const App = {
    tab: null, arg: null, ctx: {},
    register(name, mod) { V[name] = mod; }
  };
  window.App = App;
  window.UI = { $, $$, esc, inl, h, chip, sevChip, renderDoc, renderBlock, flash,
                secById, secByKey, cmdByCode, cmdByName, secAt, blockAt, cmdByNum,
                cmdKey, statusCls, codeCls, codeLabel, noteCard, reviewFor, linkify };

  function parseHash() {
    const raw = location.hash.replace(/^#\/?/, '');
    if (!raw) return ['overview', null];
    const [t, a] = raw.split('/');
    return [V[t] ? t : 'overview', a ? decodeURIComponent(a) : null];
  }

  function nav(tab, arg, replace) {
    const url = '#/' + tab + (arg != null && arg !== '' ? '/' + encodeURIComponent(arg) : '');
    if (location.hash === url) { route(); return; }
    if (replace) location.replace(url); else location.hash = url;
  }
  App.nav = nav;

  function route() {
    const [tab, arg] = parseHash();
    const changed = tab !== App.tab;
    App.tab = tab; App.arg = arg;
    $$('#tabs button').forEach(b => b.classList.toggle('on', b.dataset.tab === tab));
    buildSide();
    const view = $('#view');
    if (changed) view.scrollTop = 0;
    V[tab].render(view, arg);
    updateStatus();
  }

  /* ---------------------------------------------------------------- sidebar */
  const sideState = { filter: '', open: {} };

  function sideItem(html, cls, nav) {
    return `<div class="tree-item ${cls || ''}" ${nav ? `data-nav="${nav}"` : ''}>${html}</div>`;
  }

  // "1.1 走哪条通道" / "§1.1 走哪条通道" -> "走哪条通道"
  const stripNum = t => t.replace(/^§?\s*[\d.]+\s+/, '');
  // short human label for a section: "§1.4 固件侧的两个坑" / "0x20 CMD_CFG_SET"
  function secLabel(s) {
    if (!s) return '';
    if (s.kind === 'cmd') return `${s.key} ${s.aliases[1]}`;
    const t = stripNum(s.title).replace(/^0x[0-9A-Fa-f]{2}\s*/, '');
    return s.key && /^§/.test(s.key) ? `${s.key} ${t}` : t;
  }
  window.secLabel = secLabel;

  function sectionTree() {
    const out = [];
    for (const s of D.secs) {
      if (s.level === 1) continue;
      const nmatch = sideState.filter &&
        !(s.title + ' ' + (s.key || '')).toLowerCase().includes(sideState.filter);
      if (nmatch) continue;
      const cls = 'lvl' + s.level + (App.arg === String(s.id) && App.tab === 'outline' ? ' on' : '');
      let label = '', t = stripNum(s.title);
      if (s.kind === 'cmd') {
        label = `<span class="code">${esc(s.key)}</span>`;
        t = s.title.replace(/^0x[0-9A-Fa-f]{2}\s*`?CMD_[A-Z_]+`?\s*[—-]+\s*/, '');
      } else if (s.key && /^§/.test(s.key)) {
        label = `<span class="code">${esc(s.key)}</span>`;
      }
      let n = '';
      if (s.kind === 'cmd') {
        const c = cmdByCode[cmdKey(s.key)];
        n = c ? `<span class="n">${c.req_decl ? (c.req_decl.kind === 'range' ? c.req_decl.lo + '~' + c.req_decl.hi : c.req_decl.vals.join('/')) : '?'}B</span>` : '';
      } else if (s.kind === 'ovs' || s.kind === 'sug') {
        const o = D.ovs.find(x => x.sec === s.id);
        if (o && o.tag) {
          const c = o.sev >= 3 ? 'warn' : (o.tag.includes('待决') ? 'info' : 'sdk');
          n = `<span class="n" style="color:var(--${c === 'warn' ? 'warn' : c === 'info' ? 'info' : 'fg-mute'})">${esc(o.tag)}</span>`;
        }
      }
      out.push(sideItem(label + `<span class="t">${esc(t)}</span>` + n, cls, 'sec:' + s.id));
    }
    return out.join('');
  }

  function commandList() {
    const groups = [['0x0', '只读信息'], ['0x1', '标定'], ['0x2', '配置'], ['0x3', '存储']];
    let out = '';
    for (const [pre, label] of groups) {
      const cs = D.cmds.filter(c => c.code.startsWith(pre));
      if (!cs.length) continue;
      out += `<div class="tree-item lvl2" style="cursor:default"><span class="t" style="color:var(--fg-mute)">${label}</span></div>`;
      for (const c of cs) {
        if (sideState.filter && !(c.name + ' ' + c.title).toLowerCase().includes(sideState.filter)) continue;
        const on = App.arg === c.code ? ' on' : '';
        out += sideItem(`<span class="code">${c.code}</span><span class="t">${esc(c.name)}</span>`,
          'lvl3' + on, 'cmd:' + c.code);
      }
    }
    return out;
  }

  function buildSide() {
    const body = $('#side-body');
    if (App.tab === 'commands') body.innerHTML = commandList();
    else body.innerHTML = sectionTree();
    $$('.tree-item', body).forEach(el => {
      if (el.dataset.nav) el.onclick = () => doNav(el.dataset.nav);
    });
  }

  function updateStatus() {
    const left = $('#status-left');
    const m = D.meta;
    const bad = D.issues.filter(i => i.sev === 'error').length;
    const warn = D.issues.filter(i => i.sev === 'warn').length;
    if (left) left.textContent = `${D.meta.lines} 行 · ${D.meta.chars} 字 · ${D.secs.length} 节 · ` +
      `${D.cmds.length} 命令 · ${bad} 错误 / ${warn} 警告`;
    const right = $('#status-right');
    if (right) right.textContent = `${m.file} · 构建于 ${m.built}`;
  }

  /* ---------------------------------------------------------------- nav dispatch */
  function doNav(key) {
    const [kind, val] = key.split(':');
    if (kind === 'sec') {
      const s = secById[val];
      if (!s) return;
      if (App.tab === 'doc' || App.tab === 'outline') {
        const el = $(`#sec-${s.id}`);
        if (el) { flash(el); App.ctx.focusSec = s.id; return; }
      }
      nav('outline', val);
    } else if (kind === 'cmd') {
      nav('commands', val);
    } else if (kind === 'line') {
      nav('doc', val);
      requestAnimationFrame(() => {
        const el = $(`#view [data-line="${val}"]`);
        if (el) flash(el);
      });
    } else if (kind === 'tab') {
      nav(val);
    }
  }
  App.doNav = doNav;

  document.addEventListener('click', e => {
    const a = e.target.closest('[data-nav]');
    if (a) { e.preventDefault(); doNav(a.dataset.nav); return; }
    const l = e.target.closest('[data-line]');
    if (l && App.tab === 'doc' && e.detail === 2) {
      // double-click a rendered line in 原文 view -> anchor a review note there
      openNoteEditor(parseInt(l.dataset.line, 10), null);
    }
  });

  /* ---------------------------------------------------------------- search */
  let searchTimer = null;
  function runSearch(q) {
    const panel = $('#search-panel');
    q = q.trim().toLowerCase();
    if (q.length < 1) { panel.hidden = true; return; }
    const hits = [];
    for (let i = 0; i < D.lines.length; i++) {
      const t = D.lines[i];
      if (!t || !t.toLowerCase().includes(q)) continue;
      const s = secAt(i + 1);
      hits.push({ line: i + 1, text: t, sec: s });
      if (hits.length >= 60) break;
    }
    const cmds = D.cmds.filter(c => (c.name + ' ' + c.code + ' ' + c.title).toLowerCase().includes(q));
    const secs = D.secs.filter(s => (s.title + ' ' + (s.key || '')).toLowerCase().includes(q)).slice(0, 12);
    let html = '';
    if (cmds.length) {
      html += '<div class="hdr">命令</div>' + cmds.map(c =>
        `<div class="hit" data-nav="cmd:${c.code}"><div class="where">${c.code} · ${esc(c.title)}</div>
         <div class="txt">${esc(c.name)} — ${esc(c.summary || '')}</div></div>`).join('');
    }
    if (secs.length) {
      html += '<div class="hdr">章节</div>' + secs.map(s =>
        `<div class="hit" data-nav="sec:${s.id}"><div class="where">${esc(s.key || '')}</div>
         <div class="txt">${esc(s.title)}</div></div>`).join('');
    }
    html += `<div class="hdr">正文 · ${hits.length} 处</div>` + hits.map(x =>
      `<div class="hit" data-nav="line:${x.line}"><div class="where">L${x.line} · ${esc(x.sec ? (x.sec.key || x.sec.title) : '')}</div>
       <div class="txt">${hl(x.text, q)}</div></div>`).join('');
    if (!html) html = '<div class="hdr">无匹配</div>';
    panel.innerHTML = html;
    panel.hidden = false;
  }
  function hl(text, q) {
    const i = text.toLowerCase().indexOf(q);
    if (i < 0) return esc(text);
    return esc(text.slice(0, i)) + '<mark>' + esc(text.slice(i, i + q.length)) + '</mark>' + esc(text.slice(i + q.length));
  }

  /* ---------------------------------------------------------------- review editor */
  const ne = { line: null, match: null, existing: null };
  function openNoteEditor(line, match) {
    const box = $('#note-editor');
    ne.line = line; ne.match = match || (D.lines[line - 1] || '').trim().slice(0, 40);
    ne.existing = D.review.find(n => n.line === line && n.match === ne.match);
    $('#ne-anchor').textContent = `L${line} · ${ne.match}`;
    $('#ne-text').value = ne.existing ? ne.existing.text : '';
    $('#ne-del').hidden = !ne.existing;
    box.hidden = false;
    $('#ne-text').focus();
  }
  function closeNoteEditor() { $('#note-editor').hidden = true; ne.line = null; }
  App.openNoteEditor = openNoteEditor;

  function saveNote() {
    const text = $('#ne-text').value.trim();
    if (!text) return closeNoteEditor();
    if (ne.existing) {
      ne.existing.text = text; ne.existing.stale = false;
    } else {
      const n = { match: ne.match, text, author: 'reviewer', stale: false, line: ne.line };
      D.review.push(n);
    }
    const payload = D.review.map(({ match, text, author }) => ({ match, text, author }));
    fetch('api/review', {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify(payload)
    }).then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      toast(`已写入 review/${String(D.meta.file.split('/').pop()).slice(0, 2)}.json`);
    }).catch(err => {
      toast('保存失败：' + err.message + '（需用 --serve 启动，且 review 目录可写）', true);
    });
    closeNoteEditor();
    if (App.tab === 'doc') V.doc.render($('#view'), App.arg);
  }

  function deleteNote() {
    if (!ne.existing) return;
    D.review.splice(D.review.indexOf(ne.existing), 1);
    const payload = D.review.map(({ match, text, author }) => ({ match, text, author }));
    fetch('api/review', { method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify(payload) }).catch(() => {});
    closeNoteEditor();
    if (App.tab === 'doc') V.doc.render($('#view'), App.arg);
  }

  let toastTimer = null;
  function toast(msg, bad) {
    let t = $('#toast');
    if (!t) { t = document.createElement('div'); t.id = 'toast'; document.body.appendChild(t); }
    t.textContent = msg;
    t.className = bad ? 'bad' : '';
    t.style.cssText = 'position:fixed;left:50%;bottom:52px;transform:translateX(-50%);z-index:99;' +
      'padding:7px 14px;border-radius:7px;font-size:12.5px;background:var(--bg3);' +
      'border:1px solid ' + (bad ? 'var(--err)' : 'var(--acc2)') + ';box-shadow:var(--shadow);';
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.remove(), 2600);
  }
  App.toast = toast;

  /* ---------------------------------------------------------------- live reload */
  function connectLive() {
    if (!('EventSource' in window)) return;
    try {
      const es = new EventSource('api/events');
      es.addEventListener('open', () => {
        const el = $('#live'); el.classList.add('on'); el.querySelector('em').textContent = 'live';
      });
      es.addEventListener('reload', e => {
        loadModel(JSON.parse(e.data));
      });
      es.addEventListener('error', () => {
        const el = $('#live'); el.classList.remove('on'); el.querySelector('em').textContent = 'off';
      });
    } catch (e) { /* static file: no live channel */ }
  }

  function loadModel(m) {
    window.DOC = D; // views read window.DOC; merge in place
    Object.keys(m).forEach(k => { D[k] = m[k]; });
    D.cmds.forEach(c => { cmdByCode[c.code] = c; cmdByName[c.name] = c; });
    D.secs.forEach(s => { secById[s.id] = s; if (s.key) secByKey[s.key] = s; });
    route();
    toast('已重新构建 · ' + m.meta.built);
  }

  /* ---------------------------------------------------------------- boot */
  function init() {
    // tabs
    const counts = {
      commands: D.cmds.length, issues: D.issues.length, code: D.code.length,
      trace: D.unsol.length + 2
    };
    $('#tabs').innerHTML = TABS.map(([k, label]) => {
      const n = counts[k];
      return `<button data-tab="${k}">${label}${n ? `<span class="cnt${k === 'issues' && D.issues.some(i => i.sev === 'error') ? ' bad' : ''}">${n}</span>` : ''}</button>`;
    }).join('');
    $$('#tabs button').forEach(b => b.onclick = () => nav(b.dataset.tab));

    $('#doc-title').textContent = D.meta.title;
    $('#doc-meta').textContent = `${D.meta.lines} 行 · ${D.meta.file}`;

    // search
    const si = $('#search');
    si.oninput = () => { clearTimeout(searchTimer); searchTimer = setTimeout(() => runSearch(si.value), 90); };
    si.onkeydown = e => {
      if (e.key === 'Escape') { si.value = ''; $('#search-panel').hidden = true; si.blur(); }
      if (e.key === 'Enter') {
        const first = $('#search-panel .hit');
        if (first) { doNav(first.dataset.nav); $('#search-panel').hidden = true; }
      }
    };
    document.addEventListener('keydown', e => {
      if (e.key === '/' && document.activeElement.tagName !== 'INPUT' &&
          document.activeElement.tagName !== 'TEXTAREA') {
        e.preventDefault(); si.focus();
      }
      if (e.key === 'Escape') { $('#search-panel').hidden = true; closeNoteEditor(); }
    });
    document.addEventListener('click', e => {
      if (!e.target.closest('#search-panel') && !e.target.closest('.searchbox'))
        $('#search-panel').hidden = true;
    });

    // theme
    const saved = localStorage.getItem('noteviz.theme');
    if (saved) document.documentElement.dataset.theme = saved;
    $('#theme-btn').onclick = () => {
      const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
      document.documentElement.dataset.theme = next;
      localStorage.setItem('noteviz.theme', next);
    };

    // sidebar
    $('#side-toggle').onclick = () => {
      const s = $('#side'); s.classList.toggle('collapsed');
      $('#side-toggle').textContent = s.classList.contains('collapsed') ? '⟩' : '⟨';
    };
    $('#side-filter').oninput = e => { sideState.filter = e.target.value.trim().toLowerCase(); buildSide(); };

    // review editor buttons
    $('#ne-save').onclick = saveNote;
    $('#ne-cancel').onclick = closeNoteEditor;
    $('#ne-del').onclick = deleteNote;

    window.addEventListener('hashchange', route);
    if (!location.hash) location.replace('#/overview');
    route();
    connectLive();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
