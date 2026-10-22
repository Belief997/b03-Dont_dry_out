"""Protocol-specific extraction for note 06: commands, status codes,
oversights, requirements, checklist, decisions, near-duplicate passages.
Each extractor returns [] when its section is missing.
"""
import math
import re

import mdparse
from model import (TYPE_SIZE, cmd_regex, field_rows, find_key, is_bit_table,
                   is_field_table, own_blocks, plain, section_at, table_dicts)

FRAME_DATA = 16          # DATA bytes per response frame (§2.2)
REQ_HDR = 2              # CMD + TAG
RESP_HDR = 4             # CMD|0x80 + TAG + STATUS + FRAG


def _blocks_in(blocks, sec):
    return [b for b, _ in mdparse.walk(blocks) if sec["line"] < b["line"] <= sec["end"]]


def _text(lines, sec):
    return "\n".join(lines[sec["line"] - 1:sec["end"]])


def _mode(s):
    s = plain(s)
    if "不在" in s:
        return "not_cal"
    if "在" in s and "须" in s:
        return "cal"
    return "any"


def _req_decl(p):
    m = re.search(r"帧长\s*(\d+)\s*[~～]\s*(\d+)", p)
    if m:
        return {"kind": "range", "lo": int(m.group(1)), "hi": int(m.group(2))}
    m = re.search(r"帧长\s*(\d+)(?:\s*或\s*(\d+))?", p)
    if m:
        v = [int(m.group(1))] + ([int(m.group(2))] if m.group(2) else [])
        return {"kind": "set", "vals": v}
    if re.search(r"参数\s*[：:]\s*无", p):
        return {"kind": "set", "vals": [REQ_HDR], "implied": True}
    return None


def _resp_decl(p):
    d = {"para": p}
    m = re.search(r"共\s*\**(\d+)\s*字节\**\s*[（(](\d+)\s*帧", p)
    if m:
        d.update(size=int(m.group(1)), frames=int(m.group(2)))
    elif re.search(r"(\d+)\s*\+\s*n\s*[×x*]\s*(\d+)", p):
        m = re.search(r"(\d+)\s*\+\s*n\s*[×x*]\s*(\d+)", p)
        d.update(base=int(m.group(1)), per=int(m.group(2)))
    elif re.search(r"DATA\s*[：:]\s*(\d+)\s*字节", p):
        d.update(size=int(re.search(r"DATA\s*[：:]\s*(\d+)\s*字节", p).group(1)))
    elif re.search(r"DATA\s*[：:]\s*无", p):
        d.update(size=0)
    m = re.search(r"帧长\s*(\d+)", p)
    if m:
        d["frame_len"] = int(m.group(1))
    return d


def _req_sizes(fields, bits):
    """Possible request frame lengths implied by the argument table."""
    fixed, opt, var = 0, 0, False
    for f in fields:
        if f["len"] is None:
            var = True
        elif re.search(r"仅.*时存在", f["note"]):
            opt += f["len"]
        else:
            fixed += f["len"]
    if var:
        tsum = sum(TYPE_SIZE.get(plain(b.get("类型", "")), 0) for b in bits)
        smallest = min([TYPE_SIZE.get(plain(b.get("类型", "")), 99) for b in bits] or [0])
        return {"kind": "range", "lo": REQ_HDR + fixed, "hi": REQ_HDR + fixed + tsum,
                "lo1": REQ_HDR + fixed + smallest}
    v = [REQ_HDR + fixed] + ([REQ_HDR + fixed + opt] if opt else [])
    return {"kind": "set", "vals": v}


def extract_commands(lines, blocks, secs):
    s5 = find_key(secs, "§5")
    if not s5:
        return []
    summary = {}
    for b in own_blocks(blocks, s5):
        if b["t"] == "table" and "码" in b["head"][0]:
            for r in table_dicts(b):
                summary[plain(r["码"]).lower()] = r
    t44 = find_key(secs, "§4.4")
    timeouts = {}
    if t44:
        for b in own_blocks(blocks, t44):
            if b["t"] == "table":
                for r in table_dicts(b):
                    timeouts[plain(r[b["head"][0]])] = (plain(r[b["head"][1]]), r["_line"])
    out = []
    for s in secs:
        if s["kind"] != "cmd":
            continue
        code, name = s["key"], s["aliases"][1]
        text = _text(lines, s)
        c = {"code": code, "num": int(code, 16), "name": name, "sec": s["id"],
             "line": s["line"], "end": s["end"],
             "title": re.sub(r"^.*?[—-]+\s*", "", s["title"]),
             "req_fields": [], "resp_fields": [], "bits": [],
             "req_decl": None, "resp": {}, "req_line": None, "resp_line": None}
        row = summary.get(code)
        if row:
            c.update(mode=_mode(row.get("标定模式要求", "")), mode_raw=plain(row.get("标定模式要求", "")),
                     summary=plain(row.get("一句话", "")), summary_line=row["_line"],
                     summary_name=plain(row.get("名称", "")))
        phase, prev = None, None
        for b in _blocks_in(blocks, s):
            if b["t"] == "p":
                if phase is None and re.search(r"参数\s*[（(：:]", b["text"]):
                    c["req_decl"], c["req_line"], phase = _req_decl(b["text"]), b["line"], "req"
                if "响应 DATA" in b["text"] and not c["resp"]:
                    c["resp"], c["resp_line"], phase = _resp_decl(b["text"]), b["line"], "resp"
            elif b["t"] == "table" and is_field_table(b) and phase:
                key = "req_fields" if phase == "req" else "resp_fields"
                if not c[key]:
                    c[key] = field_rows(b)
                    c[key + "_line"] = b["line"]
            elif b["t"] == "table" and is_bit_table(b):
                label = "flags"
                if prev and prev["t"] == "p" and "位定义" in prev["text"]:
                    label = plain(prev["text"]).split()[0].rstrip("：:")
                elif any(f["name"] == "mask" for f in c["req_fields"]):
                    label = "mask"
                c["bits"].append({"label": label, "line": b["line"], "head": b["head"],
                                  "rows": table_dicts(b)})
            prev = b
        mask_bits = next((x["rows"] for x in c["bits"] if x["label"] == "mask"), [])
        c["req_calc"] = _req_sizes(c["req_fields"], mask_bits)
        r = c["resp"]
        if "base" in r:
            hi = max([int(x) for x in re.findall(r"1\.\.\**(\d+)", text)] or [0])
            r["n_max"] = hi
            r["max_size"] = r["base"] + r["per"] * hi
        sz = r.get("max_size", r.get("size"))
        r["calc_frames"] = max(1, math.ceil(sz / FRAME_DATA)) if sz is not None else None
        fsum = [f["len"] for f in c["resp_fields"]]
        r["field_sum"] = sum(fsum) if fsum and None not in fsum else None
        c["statuses"] = sorted(set(re.findall(r"ERR_[A-Z_]+", text)))
        c["reqs"] = sorted(set(int(x) for x in re.findall(r"需求\s*(\d+)", text)))
        c["writes_flash"] = bool(re.search(r"擦|提交到\s*flash|写了\s*flash|写入配置页", text))
        c["timeout"] = None
        for k, (v, ln) in timeouts.items():
            if name in k:
                c["timeout"] = {"value": v, "line": ln}
        if c["timeout"] is None and timeouts:
            k0 = next(iter(timeouts))
            c["timeout"] = {"value": timeouts[k0][0], "line": timeouts[k0][1], "default": True}
        out.append(c)
    return out


def extract_unsolicited(blocks, secs, cmds):
    s = find_key(secs, "§2.4")
    out = []
    if not s:
        return out
    by_num = {c["num"]: c for c in cmds}
    for b in own_blocks(blocks, s):
        if b["t"] == "table":
            for r in table_dicts(b):
                m = re.search(r"0x([0-9A-Fa-f]{2})", r.get("帧头", ""))
                num = int(m.group(1), 16) & 0x7F if m else None
                out.append({"scene": plain(r.get("场景", "")), "head": plain(r.get("帧头", "")),
                            "note": r.get("说明", ""), "line": r["_line"],
                            "cmd": by_num[num]["code"] if num in by_num else None})
    return out


def extract_status(blocks, secs):
    s = find_key(secs, "§3")
    out, group = [], ""
    if not s:
        return out
    for b in own_blocks(blocks, s):
        if b["t"] == "p" and b["text"].startswith("**"):
            group = plain(b["text"])
        elif b["t"] == "table":
            for r in table_dicts(b):
                out.append({"val": plain(r.get("值", "")), "name": plain(r.get("名称", "")),
                            "meaning": r.get("含义", ""), "action": r.get("客户端应对", ""),
                            "group": group, "line": r["_line"]})
    return out
