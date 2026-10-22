/* 报文: the doc walked as an actual byte stream. Frame format, the two
   live examples in §速览/§2.3, and the unsolicited (TAG=0xFF) reports,
   decoded field by field so the numbers can be checked by eye. */
(function () {
  'use strict';
  const { $, esc, inl, chip } = window.UI;
  const D = window.DOC;

  const hex2 = n => n.toString(16).toUpperCase().padStart(2, '0');

  /* Slice one response's DATA into frames of 16 bytes and label each slice with
     the field(s) it covers. Bytes no field claims are shown as ? — that is how
     a declared-length/field-table mismatch becomes visible here. */
  function buildFrames(num, total, fields, tag) {
    const frames = [];
    for (let f = 0, off = 0; off < total; f++) {
      const n = Math.min(16, total - off);
      frames.push({ f, off, len: n, more: off + n < total });
      off += n;
    }
    return frames.map(fr => {
      const frag = (fr.more ? 0x80 : 0) | fr.f;
      const bytes = [
        { v: hex2(num | 0x80), k: 'hdr', t: 'CMD|0x80' },
        { v: tag, k: 'tag', t: 'TAG' },
        { v: '00', k: 'st', t: 'STATUS OK' },
        { v: hex2(frag), k: 'frag', t: `FRAG 帧${fr.f} ${fr.more ? 'MORE=1' : '末帧'}` }
      ];
      const lo = fr.off, hi = fr.off + fr.len;
      let cursor = lo;
      for (const fl of fields) {
        const start = fl.off, end = fl.off + (fl.len || 0);
        if (end <= lo || start >= hi) continue;
        if (start > cursor) {
          bytes.push({ v: '?'.repeat((start - cursor) * 2), k: 'gap',
            t: `${cursor}..${start - 1} 未被任何字段覆盖` });
          cursor = start;
        }
        const a = Math.max(start, lo), b = Math.min(end, hi);
        bytes.push({ v: '··'.repeat(b - a), k: 'data', t: `${fl.name} ${start}..${end - 1}` });
        cursor = b;
      }
      if (cursor < hi) {
        bytes.push({ v: '?'.repeat((hi - cursor) * 2), k: 'gap',
          t: `${cursor}..${hi - 1} 声明了长度但字段表没有对应字段` });
      }
      const covered = fields.reduce((s, fl) => s + (fl.len || 0), 0);
      return { bytes, len: 4 + fr.len, covered };
    });
  }

  /* ---- build the two concrete exchanges that the doc spells out ---- */
  function exSensor() {
    const c = D.cmds.find(x => x.name === 'SENSOR');
    const total = c.resp.size || 26;
    const sum = c.resp_fields.reduce((s, f) => s + (f.len || 0), 0);
    return {
      title: '读一次传感器（§速览 · 一次问答）',
      note: `请求 2 字节，响应声明 ${total} 字节拆 2 帧，字段表合计 ${sum} 字节` +
        (sum !== total ? ` —— 差 ${total - sum} 字节，下面是按声明长度切帧的结果，多出来的字节没有任何字段认领` : ''),
      warn: sum !== total,
      req: [{ len: 2, bytes: [
        { v: '02', k: 'hdr', t: 'CMD 0x02 SENSOR' },
        { v: '07', k: 'tag', t: 'TAG 0x07' }] }],
      resp: buildFrames(c.num, total, c.resp_fields, '07')
    };
  }

  function exInfo() {
    const c = D.cmds.find(x => x.name === 'INFO');
    const total = c.resp.size || 40;
    return {
      title: 'CMD_INFO 多帧（§2.3）',
      note: `${total} 字节 = 16+16+8，末帧可不满。客户端靠「非末帧必须满 16」判断丢帧。`,
      req: [{ len: 2, bytes: [
        { v: '01', k: 'hdr', t: 'CMD 0x01 INFO' },
        { v: '55', k: 'tag', t: 'TAG 0x55' }] }],
      resp: buildFrames(c.num, total, c.resp_fields, '55')
    };
  }

  function bytesH(b) {
    // label under the byte: the field name for a data run, otherwise the header name
    const m = /^([A-Za-z_][\w\[\]|]*)\s/.exec(b.t);
    const lab = b.k === 'data' && m ? m[1] : b.t.split(/[ ,]/)[0];
    return `<span class="byte ${b.k}" title="${esc(b.t)}">${esc(b.v)}<small>${esc(lab)}</small></span>`;
  }
  function frameRow(fr, label) {
    const total = fr.len != null ? fr.len : fr.bytes.length;
    const note = fr.covered != null && fr.covered < fr.len
      ? ` <span class="chip err">字段表缺 ${fr.len - fr.covered} B</span>` : '';
    return `<div style="display:flex;align-items:center;gap:10px;margin:5px 0;flex-wrap:wrap">
      <span style="font:11px var(--mono);color:var(--fg-mute);flex:0 0 82px">${label}</span>
      <div class="bytes" style="margin:0">${fr.bytes.map(bytesH).join('')}</div>
      <span style="font:11px var(--mono);color:var(--fg-mute)">${total} B</span>${note}
    </div>`;
  }

  function exchange(ex) {
    return `<div class="card" ${ex.warn ? 'style="border-color:var(--err)"' : ''}>
      <h3>${esc(ex.title)}</h3>
      <div class="sub">${esc(ex.note)}</div>
      ${ex.req.map((f, i) => frameRow(f, i ? '' : '请求')).join('')}
      <div style="margin:6px 0 2px;font:11px var(--mono);color:var(--fg-mute)">↓ 设备响应</div>
      ${ex.resp.map((f, i) => frameRow(f, i ? '' : '响应')).join('')}
    </div>`;
  }

  function frameSpecs() {
    const req = [['0', '1', 'CMD', 'hdr', '0x01..0x7F，最高位必须是 0'],
      ['1', '1', 'TAG', 'tag', '上位机流水号；0xFF 保留给主动上报'],
      ['2', 'N', 'ARGS', 'data', 'N = 0..18，长度由命令决定']];
    const resp = [['0', '1', 'CMD|0x80', 'hdr', '对应请求码最高位置 1'],
      ['1', '1', 'TAG', 'tag', '回填请求 TAG；主动上报填 0xFF'],
      ['2', '1', 'STATUS', 'st', '0x00 = 成功，非 0 见 §3'],
      ['3', '1', 'FRAG', 'frag', 'bit7 MORE · bit6..0 帧号 0..127'],
      ['4', 'M', 'DATA', 'data', 'M = 0..16']];
    const tbl = (rows, label) => `<div class="sub">${label}</div><table><thead><tr>
      <th>偏移</th><th>长</th><th>字段</th><th>说明</th></tr></thead><tbody>
      ${rows.map(r => `<tr><td>${r[0]}</td><td>${r[1]}</td>
        <td><span class="byte ${r[3]}" style="border:none;background:none">${esc(r[2])}</span></td>
        <td style="white-space:normal">${inl(r[4])}</td></tr>`).join('')}</tbody></table>`;
    return `<div class="grid2">
      <div class="card">${tbl(req, '请求帧 · 2 ~ 20 字节')}</div>
      <div class="card">${tbl(resp, '响应帧 · 4 ~ 20 字节')}</div>
    </div>`;
  }

  function fragTable() {
    const rows = [['0x00', '帧号 0', '最后一帧 → 单帧响应'], ['0x80', '帧号 0', '后面还有'],
      ['0x81', '帧号 1', '后面还有'], ['0x02', '帧号 2', '最后一帧']];
    return `<div class="card"><h3>FRAG 字节</h3><table><thead><tr><th>值</th><th>帧号</th><th>含义</th></tr></thead>
      <tbody>${rows.map(r => `<tr><td><code>${r[0]}</code></td><td>${r[1]}</td><td>${r[2]}</td></tr>`).join('')}</tbody>
    </table></div>`;
  }

  function unsolicited() {
    let html = `<h2>设备主动上报 · TAG = 0xFF <span class="chip">${D.unsol.length}</span></h2>
      <p class="note-inline">客户端必须能在任意时刻容忍这类帧——包括正在等某条请求应答的时候。
      推流帧与 <code>CMD_SENSOR</code> 应答长得一模一样，只有 TAG 能区分。</p>
      <table><thead><tr><th>场景</th><th>帧头</th><th>对应命令</th><th>说明</th></tr></thead><tbody>`;
    for (const u of D.unsol) {
      html += `<tr><td>${esc(u.scene)}</td><td><code>${esc(u.head)}</code></td>
        <td>${u.cmd ? `<a class="lnk refcmd" data-nav="cmd:${u.cmd}">${u.cmd}</a>` : '—'}</td>
        <td style="white-space:normal">${inl(u.note)}</td></tr>`;
    }
    html += `</tbody></table>`;
    return html;
  }

  function statusTable() {
    let html = `<h2>状态码只出现在一处 <span class="chip">${D.status.length}</span></h2>
      <p class="note-inline">客户端只需读 <code>STATUS</code> 一个字节，不必按命令分别判断。</p>
      <table><thead><tr><th>值</th><th>名称</th><th>含义</th><th>客户端应对</th><th>引用处</th></tr></thead><tbody>`;
    for (const s of D.status) {
      const uses = D.cmds.filter(c => c.statuses.includes(s.name));
      html += `<tr><td><code>${esc(s.val)}</code></td><td><code>${esc(s.name)}</code></td>
        <td style="white-space:normal">${inl(s.meaning)}</td>
        <td style="white-space:normal;color:var(--fg-dim)">${inl(s.action || '')}</td>
        <td style="white-space:normal">${uses.length ? uses.map(c => `<a class="lnk refcmd" data-nav="cmd:${c.code}">${c.code}</a>`).join(' ') : '<span style="color:var(--fg-mute)">无</span>'}</td></tr>`;
    }
    return html + `</tbody></table>`;
  }

  function render(el) {
    el.innerHTML = `<div class="doc"><h1 style="font-size:22px">报文</h1>
      <p class="note-inline">全协议小端。帧只负责搬运，拼完再按字段表解。</p></div>` +
      frameSpecs() +
      `<h2>两个真实往返</h2>` +
      exchange(exSensor()) + exchange(exInfo()) +
      fragTable() +
      unsolicited() + statusTable();
  }

  App.register('trace', { render });
})();
