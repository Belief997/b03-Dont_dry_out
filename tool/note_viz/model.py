"""Turn note 06 into a structured model: section tree, references,
commands, status codes, oversights, requirements, checklist, decisions.

Generic parts (sections, refs) work on any note; the protocol-specific
extractors simply return empty lists when their sections are absent.
"""
import io
import math
import os
import re
import time

import mdparse

CMD_HEAD_RE = re.compile(r"^(0x[0-9A-Fa-f]{2})\s+`CMD_(\w+)`\s*[—-]+\s*(.*)$")
SEC_RE = re.compile(r"^(\d+(?:\.\d+)?)\.?\s+(.*)$")
OVS_RE = re.compile(r"^疏漏\s*(\d+)\s*[—-]+\s*(.*)$")
SUG_RE = re.compile(r"^建议\s*([A-Z])\s*[—-]+\s*(.*)$")
TAG_RE = re.compile(r"【([^】]+)】")
TICK_RE = re.compile(r"`([^`]+)`")
TYPE_SIZE = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4, "f32": 4}


def plain(s):
    """Strip inline markup for titles / search."""
    return re.sub(r"[*`]", "", s).strip()


def nchars(lines):
    return sum(len(re.sub(r"\s", "", x)) for x in lines)


# ---------------------------------------------------------------- sections
def build_sections(lines, blocks):
    heads = [b for b in blocks if b["t"] == "h"]
    secs = []
    for i, h in enumerate(heads):
        end = len(lines)
        for h2 in heads[i + 1:]:
            if h2["level"] <= h["level"]:
                end = h2["line"] - 1
                break
        own_end = heads[i + 1]["line"] - 1 if i + 1 < len(heads) else len(lines)
        secs.append({"id": i, "level": h["level"], "title": plain(h["text"]),
                     "raw": h["text"], "line": h["line"], "end": end,
                     "own_end": min(own_end, end), "parent": None, "children": [],
                     "kind": "sec", "key": None, "aliases": [], "tag": None})
    stack = []
    for s in secs:
        while stack and stack[-1]["level"] >= s["level"]:
            stack.pop()
        if stack:
            s["parent"] = stack[-1]["id"]
            stack[-1]["children"].append(s["id"])
        stack.append(s)

    for s in secs:
        t = s["title"]
        m = TAG_RE.search(t)
        if m:
            s["tag"] = m.group(1)
            t = TAG_RE.sub("", t).strip()
        if s["level"] == 1:
            s["kind"], s["key"] = "title", "title"
        elif CMD_HEAD_RE.match(s["raw"]):
            m = CMD_HEAD_RE.match(s["raw"])
            s["kind"], s["key"] = "cmd", m.group(1).lower()
            s["aliases"] = ["CMD_" + m.group(2), m.group(2)]
        elif OVS_RE.match(t):
            s["kind"], s["key"] = "ovs", "疏漏" + OVS_RE.match(t).group(1)
        elif SUG_RE.match(t):
            s["kind"], s["key"] = "sug", "建议" + SUG_RE.match(t).group(1)
        elif SEC_RE.match(t) and s["level"] in (2, 3):
            s["key"] = "§" + SEC_RE.match(t).group(1)
        else:
            p = secs[s["parent"]] if s["parent"] is not None else None
            s["key"] = (p["key"] + "/" if p and p["level"] > 1 else "") + t
            if s["level"] == 2:
                s["kind"] = "quick"
        span = lines[s["line"] - 1:s["end"]]
        own = lines[s["line"] - 1:s["own_end"]]
        s["chars"], s["own_chars"] = nchars(span), nchars(own)
        s["warn"] = sum(x.count("⚠") for x in own)
        s["why"] = sum(1 for x in own if re.search(r"为什么", x))
    return secs


def section_at(secs, line):
    """Deepest section containing the line."""
    best = None
    for s in secs:
        if s["line"] <= line <= s["end"]:
            if best is None or s["level"] > best["level"]:
                best = s
    return best


def find_key(secs, key):
    for s in secs:
        if s["key"] == key:
            return s
    return None


def own_blocks(blocks, sec):
    return [b for b in blocks if sec["line"] < b["line"] <= sec["own_end"]]


# ---------------------------------------------------------------- refs
def cmd_regex(names):
    alt = "|".join(sorted(map(re.escape, names), key=len, reverse=True))
    return re.compile(r"(?<![A-Za-z0-9_])(?:CMD_)?(" + alt + r")(?![A-Za-z0-9_])")


def is_ancestor(secs, a, b):
    """True if section a is b or an ancestor of b."""
    while b is not None:
        if b == a:
            return True
        b = secs[b]["parent"]
    return False


def build_refs(lines, secs):
    by_key = {s["key"]: s["id"] for s in secs}
    cmds = {}
    for s in secs:
        if s["kind"] == "cmd":
            cmds[s["aliases"][1]] = s["id"]
    cre = cmd_regex(cmds) if cmds else None
    in_code, refs = False, []
    for ln, text in enumerate(lines, 1):
        if text.lstrip().startswith(("```", "~~~")):
            in_code = not in_code
            continue
        src = section_at(secs, ln)
        if src is None:
            continue
        intro = src["kind"] == "title"
        found = []
        for m in re.finditer(r"§\s*(\d+(?:\.\d+)?)", text):
            found.append(("sec", "§" + m.group(1), m.group(0)))
        for m in re.finditer(r"§\s*`(?:CMD_)?([A-Z_]+)`", text):
            found.append(("cmd", m.group(1), m.group(0)))
        for m in re.finditer(r"疏漏\s*(\d+)", text):
            found.append(("ovs", "疏漏" + m.group(1), m.group(0)))
        for m in re.finditer(r"建议\s*([A-Z])(?![A-Za-z])", text):
            found.append(("sug", "建议" + m.group(1), m.group(0)))
        for m in re.finditer(r"需求\s*(\d+)", text):
            found.append(("req", "需求" + m.group(1), m.group(0)))
        for m in re.finditer(r"note\s*0?(\d)\b|\b(0\d)-[\w-]+\.md", text):
            found.append(("note", "note " + (m.group(1) or m.group(2)[1]), m.group(0)))
        if cre:
            for m in cre.finditer(text):
                found.append(("cmd", m.group(1), m.group(0)))
        seen = set()
        for kind, tgt, raw in found:
            if kind == "cmd":
                tid = cmds.get(tgt)
            elif kind in ("req", "note"):
                tid = None
            else:
                tid = by_key.get(tgt)
            k = (kind, tgt)
            if k in seen:
                continue
            seen.add(k)
            if tid is not None and not intro and is_ancestor(secs, src["id"], tid):
                continue        # TOC rows / self mention, not a real link
            if tid is not None and is_ancestor(secs, tid, src["id"]):
                continue        # mentioning the enclosing section
            refs.append({"src": src["id"], "line": ln, "kind": kind,
                         "target": tgt, "tid": tid, "raw": raw})
    return refs


# ---------------------------------------------------------------- tables
def table_dicts(tb):
    return [dict(zip(tb["head"], r), _line=l) for r, l in zip(tb["rows"], tb["rlines"])]


def to_int(s):
    s = plain(s)
    try:
        return int(s, 0)
    except ValueError:
        return None


def field_rows(tb):
    out = []
    for r in table_dicts(tb):
        name = plain(r.get("字段", ""))
        out.append({"off": to_int(r.get("偏移", "")), "len": to_int(r.get("长", "")),
                    "len_raw": plain(r.get("长", "")),
                    "name": name.split("—")[0].strip(),
                    "note": r.get("说明", r.get("来源 / 说明", "")),
                    "line": r["_line"]})
    return out


def is_field_table(tb):
    h = "".join(tb["head"])
    return "偏移" in h and "长" in h and "字段" in h


def is_bit_table(tb):
    return tb["head"] and tb["head"][0] == "位"
