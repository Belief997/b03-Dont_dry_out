"""Second half of the note-06 extractors: oversights, requirement map,
TODO checklist, open decisions, code-symbol index, repeated passages."""
import re

import mdparse
from model import find_key, own_blocks, plain, table_dicts

SEVERITY = [("必修", 4), ("必做", 3), ("重要", 3), ("待决", 2), ("建议", 1)]


def _sev(tag):
    for k, v in SEVERITY:
        if tag and k in tag:
            return v
    return 0


def extract_oversights(secs, refs):
    out = []
    for s in secs:
        if s["kind"] not in ("ovs", "sug"):
            continue
        back = sorted({r["src"] for r in refs if r["tid"] == s["id"]})
        fwd = sorted({r["tid"] for r in refs if r["src"] == s["id"] and r["tid"] is not None})
        out.append({"key": s["key"], "sec": s["id"], "title": s["title"],
                    "tag": s["tag"] or "", "sev": _sev(s["tag"]), "line": s["line"],
                    "end": s["end"], "chars": s["chars"], "back": back, "fwd": fwd})
    return out


def extract_reqs(blocks, secs, cmds):
    s = find_key(secs, "§9")
    out = []
    if not s:
        return out
    names = {c["name"]: c["code"] for c in cmds}
    for b in own_blocks(blocks, s):
        if b["t"] != "table":
            continue
        for r in table_dicts(b):
            impl = r.get("落地方式", "")
            hit = set()
            for tok in re.findall(r"`([^`]+)`", impl):
                head = re.split(r"[.(\[]", tok.replace("CMD_", ""))[0]
                if head in names:
                    hit.add(names[head])
            st = plain(r.get("状态", ""))
            both = impl + " " + r.get("状态", "")
            out.append({"req": int(plain(r.get("#", "0")) or 0), "text": plain(r.get("需求原文", "")),
                        "impl": impl, "status": st, "ok": st.startswith("✅"),
                        "cmds": sorted(hit), "line": r["_line"],
                        "secs": ["§" + x for x in re.findall(r"§\s*(\d+(?:\.\d+)?)", both)],
                        "ovs": ["疏漏" + x for x in re.findall(r"疏漏\s*(\d+)", both)]})
    return out


def extract_checklist(blocks, secs):
    s = find_key(secs, "§12")
    out, group = [], ""
    if not s:
        return out
    for b in own_blocks(blocks, s):
        if b["t"] == "p" and b["text"].startswith("**"):
            group = plain(b["text"])
        elif b["t"] == "list":
            for it in b["items"]:
                if it["check"] is None:
                    continue
                out.append({"group": group, "text": it["text"], "done": it["check"],
                            "line": it["line"]})
    return out


def extract_decisions(blocks, secs):
    s = find_key(secs, "§13")
    out = []
    if not s:
        return out
    for b in own_blocks(blocks, s):
        if b["t"] == "table":
            for r in table_dicts(b):
                q = r.get("问题", "")
                out.append({"n": plain(r.get("#", "")), "q": q,
                            "advice": r.get("我的建议", ""), "line": r["_line"],
                            "ovs": ["疏漏" + x for x in re.findall(r"疏漏\s*(\d+)", q)]})
    return out


def _norm(t):
    return re.sub(r"[\s*`>\-|，。、；：:,.()（）“”\"'—→⚠【】]+", "", t)


def _grams(t, n=3):
    return {t[i:i + n] for i in range(max(0, len(t) - n + 1))}


def _lcs(a, b):
    from difflib import SequenceMatcher
    m = SequenceMatcher(None, a, b, autojunk=False).find_longest_match(0, len(a), 0, len(b))
    return a[m.a:m.a + m.size]


def _units(blocks, secs, section_at):
    """Paragraph-sized text units; list items count individually."""
    out = []
    for b, _ in mdparse.walk(blocks):
        if b["t"] == "p":
            items = [(b["line"], b["text"])]
        elif b["t"] == "list":
            items = [(i["line"], i["text"]) for i in b["items"]]
        else:
            continue
        for ln, txt in items:
            nt = _norm(txt)
            if len(nt) >= 24:
                sec = section_at(secs, ln)
                out.append({"line": ln, "sec": sec["id"] if sec else None, "text": txt,
                            "norm": nt, "g": _grams(nt)})
    return out


def extract_dupes(blocks, secs, section_at, min_cover=0.42, min_lcs=14):
    """Text units in different sections that say the same thing.

    Overlap is IDF-weighted over character 3-grams, so shared domain terms
    (`REC_READ`, 标定模式 ...) that appear everywhere count for little and
    genuinely restated sentences dominate. `lcs` = longest verbatim run."""
    import math
    units = _units(blocks, secs, section_at)
    df = {}
    for u in units:
        for g in u["g"]:
            df[g] = df.get(g, 0) + 1
    n = float(len(units))
    idf = {g: math.log(n / c) for g, c in df.items()}
    for u in units:
        u["w"] = sum(idf[g] for g in u["g"])
    out = []
    for i in range(len(units)):
        a = units[i]
        for j in range(i + 1, len(units)):
            b = units[j]
            if a["sec"] == b["sec"]:
                continue
            common = a["g"] & b["g"]
            if len(common) < 6:
                continue
            cover = sum(idf[g] for g in common) / min(a["w"], b["w"])
            if cover < min_cover * 0.7:
                continue
            lcs = _lcs(a["norm"], b["norm"])
            if cover >= min_cover or len(lcs) >= min_lcs:
                out.append({"a": a["line"], "b": b["line"], "sa": a["sec"], "sb": b["sec"],
                            "cover": round(cover, 2), "lcs": lcs,
                            "ta": a["text"], "tb": b["text"]})
    out.sort(key=lambda d: -(d["cover"] + len(d["lcs"]) / 60.0))
    return out
