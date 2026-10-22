/* 总览: dashboard — what this doc is, what's broken, what's decided, what's left. */
(function () {
  'use strict';
  const { $, esc, inl, chip, sevChip } = window.UI;
  const D = window.DOC;

  const modeLabel = m => ({ any: '任意', cal: '须在标定', not_cal: '须不在标定' }[m] || m || '—');

  function stat(v, label, cls) {
    return `<div class="stat ${cls || ''}"><b>${v}</b><span>${label}</span></div>`;
  }

  function issueRow(i, limit) {
    return `<div class="issue" data-nav="line:${i.line}">
      ${sevChip(i.sev)}
      <div class="body">
        <div class="t">${inl(i.title)}<span class="cat">${esc(i.cat)} · L${i.line}</span></div>
        ${i.detail && !limit ? `<div class="d">${inl(i.detail)}</div>` : ''}
      </div></div>`;
  }

  function render(el) {
    const errs = D.issues.filter(i => i.sev === 'error');
    const warns = D.issues.filter(i => i.sev === 'warn');
    const infos = D.issues.filter(i => i.sev === 'info');
    const done = D.checklist.filter(c => c.done).length;
    const reqOk = D.reqs.filter(r => r.ok).length;
    const codeLive = D.code.filter(c => c.status === 'code').length;
    const codeMissing = D.code.filter(c => c.status === 'missing').length;

    let html = `<div class="doc ov-hero">
      <h1>${esc(D.meta.title)} <span class="chip">review</span></h1>
      <p style="color:var(--fg-mute);font:12px var(--mono)">${esc(D.meta.file)} · ${D.meta.lines} 行 ·
        ${D.meta.chars} 字 · 构建于 ${esc(D.meta.built)}</p>
    </div>`;

    html += `<div class="stats">
      ${stat(D.secs.filter(s => s.level <= 2 && s.level > 1).length, '正文章节')}
      ${stat(D.cmds.length, '命令')}
      ${stat(D.status.length, '状态码')}
      ${stat(errs.length, '错误', errs.length ? 'bad' : 'good')}
      ${stat(warns.length, '警告', warns.length ? 'warn' : '')}
      ${stat(infos.length, '提示')}
      ${stat(reqOk + '/' + D.reqs.length, '需求已落地')}
      ${stat(done + '/' + D.checklist.length, '固件待办')}
      ${stat(codeLive, '已存在的符号')}
      ${stat(codeMissing, '文中有、代码无', codeMissing ? 'warn' : '')}
    </div>`;

    /* ---- 需要先看的 */
    const urgent = [].concat(errs, warns).slice(0, 8);
    html += `<h2>需要先看的</h2>`;
    if (!urgent.length) html += `<p class="empty">没有错误或警告。</p>`;
    else {
      html += `<div class="issue-list">${urgent.map(i => issueRow(i, true)).join('')}</div>`;
      if (errs.length + warns.length > urgent.length)
        html += `<p><a class="lnk" data-nav="tab:issues">查看全部 ${errs.length + warns.length} 条 →</a></p>`;
    }

    /* ---- 命令一览 */
    html += `<h2>命令一览 <span class="chip">${D.cmds.length}</span></h2>
      <table><thead><tr><th>码</th><th>名称</th><th>标定模式</th><th>请求帧长</th><th>响应</th>
      <th>帧数</th><th>超时</th><th>可能状态码</th></tr></thead><tbody>`;
    for (const c of D.cmds) {
      const req = c.req_calc;
      const reqTxt = c.req_decl
        ? (c.req_decl.kind === 'range' ? `${c.req_decl.lo}~${c.req_decl.hi}` : c.req_decl.vals.join('/'))
        : `<span style="color:var(--err)">未写明</span>`;
      const calcTxt = req && (req.kind === 'range' ? `${req.lo}~${req.hi}` : req.vals.join('/'));
      const mismatch = req && c.req_decl && reqTxt && calcTxt && !reqTxt.includes('未写明') &&
        String(reqTxt).replace(/[~]/g, '-') !== String(calcTxt).replace(/[~]/g, '-');
      const sz = c.resp.max_size != null ? c.resp.max_size : c.resp.size;
      const frames = c.resp.frames || c.resp.calc_frames;
      html += `<tr class="rowclick" data-nav="cmd:${c.code}">
        <td><code>${c.code}</code></td>
        <td><b>${esc(c.name)}</b></td>
        <td style="color:${c.mode === 'cal' ? 'var(--purple)' : 'var(--fg-dim)'}">${modeLabel(c.mode)}</td>
        <td>${reqTxt}${mismatch ? ` <span class="chip err">推算 ${calcTxt}</span>` : ''}</td>
        <td>${sz == null ? '—' : sz + ' B'}</td>
        <td>${frames == null ? '—' : frames}</td>
        <td>${c.timeout ? esc(c.timeout.value) : '—'}</td>
        <td style="white-space:normal">${c.statuses.map(s => chip(s, s === 'ERR_INTERNAL' ? 'sdk' : 'warn')).join(' ')}</td>
      </tr>`;
    }
    html += `</tbody></table>`;

    /* ---- 疏漏 + 待决 */
    const ovs = D.ovs.filter(o => o.key.startsWith('疏漏'));
    const sugs = D.ovs.filter(o => o.key.startsWith('建议'));
    html += `<div class="grid2">
      <div><h2>疏漏清单 <span class="chip">${ovs.length}</span></h2>
        <div class="detail-list">${ovs.map(o => {
          const cls = o.sev >= 3 ? 'err' : o.tag && o.tag.includes('待决') ? 'info' : 'warn';
          return `<div class="issue" data-nav="sec:${o.sec}">${chip(o.key, cls)}
            <div class="body"><div class="t">${esc((D.secs[o.sec] || {}).title || '').replace(/^.*?——\s*/, '')}</div>
            <div class="d">${esc(o.tag || '')} · ${o.chars} 字 · 被引用 ${o.back.length} 处</div></div></div>`;
        }).join('')}</div>
        ${sugs.length ? `<p style="margin-top:10px">${sugs.map(o => chip(o.key, 'sdk')).join(' ')}</p>` : ''}
      </div>
      <div><h2>待决清单 <span class="chip">${D.decisions.length}</span></h2>
        <table><thead><tr><th>#</th><th>问题</th><th>建议</th></tr></thead><tbody>
        ${D.decisions.map(d => `<tr><td>${esc(d.n)}</td>
          <td style="white-space:normal">${inl(d.q)}</td>
          <td style="white-space:normal;color:var(--fg-dim)">${inl(d.advice)}</td></tr>`).join('')}
        </tbody></table>
      </div>
    </div>`;

    /* ---- 需求覆盖 */
    html += `<h2>需求覆盖 <span class="chip">${reqOk}/${D.reqs.length}</span></h2>
      <table><thead><tr><th>#</th><th>需求</th><th>落地</th><th>状态</th></tr></thead><tbody>
      ${D.reqs.map(r => `<tr><td>${r.req}</td>
        <td style="white-space:normal">${inl(r.text)}</td>
        <td style="white-space:normal">${inl(r.impl)}</td>
        <td style="white-space:normal">${inl(r.status)}</td></tr>`).join('')}
      </tbody></table>`;

    /* ---- 章节地图 */
    const top = D.secs.filter(s => s.level === 2);
    html += `<h2>章节地图</h2><div class="tocgrid">` + top.map(s => {
      const kids = s.children.length;
      return `<a class="sec-link" data-nav="sec:${s.id}"><b>${esc(window.secLabel(s))}</b>
        <span style="font-size:11px;color:var(--fg-mute)">${kids} 个子节 · ${s.chars} 字</span></a>`;
    }).join('') + `</div>`;

    el.innerHTML = html;
  }

  App.register('overview', { render });
})();
