"""Build a self-contained, offline HTML review view of a dev/note document.

Run:  python build.py                  # note 06 -> out/06-ble-command-protocol.html
      python build.py --open           # ...and open it in the browser
      python build.py --serve          # live view: rebuilds + reloads on save
      python build.py --dump           # print the model summary, no HTML
      python build.py --note 05        # any other note (generic views only)
"""
import argparse
import io
import json
import os
import re
import sys
import time

import mdparse
import codeidx
from checks import (check_commands, check_coverage, check_numbering, check_refs,
                    check_status, issue)
from extract import extract_commands, extract_status, extract_unsolicited
from extract2 import (extract_checklist, extract_decisions, extract_dupes,
                      extract_oversights, extract_reqs)
from model import build_refs, build_sections, plain, section_at

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = os.path.dirname(os.path.dirname(HERE))
NOTE_DIR = os.path.join(DEV, "note")
WEB = os.path.join(HERE, "web")
OUT = os.path.join(HERE, "out")
REVIEW = os.path.join(HERE, "review")
JS_ORDER = ["core.js", "md.js", "v_overview.js", "v_outline.js", "v_graph.js",
            "v_cmds.js", "v_trace.js", "v_code.js", "v_issues.js", "v_doc.js",
            "live.js"]


def find_note(sel):
    for f in sorted(os.listdir(NOTE_DIR)):
        if f.endswith(".md") and (f.startswith(sel) or f == sel):
            return os.path.join(NOTE_DIR, f)
    sys.exit("no note matching %r in %s" % (sel, NOTE_DIR))


def load_review(stem, lines):
    """Hand-written review notes, anchored by a text snippet rather than a line
    number so they survive edits; a note whose snippet vanished is 'stale'."""
    p = os.path.join(REVIEW, stem[:2] + ".json")
    if not os.path.exists(p):
        return []
    notes = json.load(io.open(p, encoding="utf-8"))
    for n in notes:
        n["line"], n["stale"] = 0, True
        for i, t in enumerate(lines, 1):
            if n["match"] in t:
                n["line"], n["stale"] = i, False
                break
    return notes


def proto_fields(blocks):
    """Names this doc itself defines in field / bit tables (wire fields, flag
    bits) - they are not expected to exist in code yet."""
    out = set()
    for b, _ in mdparse.walk(blocks):
        if b["t"] != "table":
            continue
        cols = [i for i, h in enumerate(b["head"]) if h in ("字段", "名称", "新字段")]
        for r in b["rows"]:
            for i in cols:
                if i < len(r):
                    out.update(re.findall(r"`([A-Za-z_]\w*)`", r[i]))
    return out


def build_model(path):
    text = io.open(path, encoding="utf-8").read().replace("\r", "")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    blocks = mdparse.parse(lines)
    secs = build_sections(lines, blocks)
    refs = build_refs(lines, secs)
    at = section_at
    cmds = extract_commands(lines, blocks, secs)
    status = extract_status(blocks, secs)
    ovs = extract_oversights(secs, refs)
    reqs = extract_reqs(blocks, secs, cmds)
    checklist = extract_checklist(blocks, secs)
    decisions = extract_decisions(blocks, secs)
    exclude = {c["name"] for c in cmds} | {"CMD_" + c["name"] for c in cmds} | \
              {s["name"] for s in status}
    code = codeidx.build_code_index(lines, secs, at, exclude, proto_fields(blocks))
    cites = codeidx.check_line_cites(lines, secs, at, code)

    issues = []
    issues += check_commands(cmds)
    issues += check_status(status, cmds, lines, secs, at)
    issues += check_refs(refs, secs)
    issues += check_numbering(secs)
    issues += check_coverage(ovs, checklist, decisions, reqs)
    for c in cites:
        if not c["ok"]:
            near = [x["line"] for x in c["cands"]]
            issues.append(issue("error", "代码锚点",
                                "`%s` 已过时：那一行现在是别的代码" % c["cite"], c["doc_line"],
                                "现在第 %d 行：%s\n本节提到的标识符出现在：%s"
                                % (c["n"], c["now"], ", ".join("%s:%d" % (c["file"], n) for n in near) or "（无）")))
    for e in code:
        if e["status"] == "missing":
            issues.append(issue("warn", "代码锚点", "`%s` 在代码里找不到，文中也没说是待新增" % e["tok"],
                                e["lines"][0], "出现在第 %s 行" % ", ".join(map(str, e["lines"][:6]))))
    dupes = extract_dupes(blocks, secs, at)
    review = load_review(os.path.basename(path), lines)

    return {"meta": {"file": os.path.relpath(path, DEV).replace("\\", "/"),
                     "title": plain(lines[0].lstrip("# ")) if lines else "",
                     "built": time.strftime("%Y-%m-%d %H:%M:%S"),
                     "mtime": os.path.getmtime(path),
                     "lines": len(lines), "bytes": len(text.encode("utf-8")),
                     "chars": sum(len(re.sub(r"\s", "", x)) for x in lines)},
            "lines": lines, "blocks": blocks, "secs": secs, "refs": refs,
            "cmds": cmds, "unsol": extract_unsolicited(blocks, secs, cmds),
            "status": status, "ovs": ovs, "reqs": reqs, "checklist": checklist,
            "decisions": decisions, "code": code, "cites": cites,
            "issues": issues, "dupes": dupes, "review": review}


def render(model):
    rd = lambda n: io.open(os.path.join(WEB, n), encoding="utf-8").read()
    data = json.dumps(model, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    js = "\n".join("/* ---- %s ---- */\n%s" % (n, rd(n)) for n in JS_ORDER)
    html = rd("index.html")
    for k, v in (("/*__CSS__*/", rd("style.css")), ("/*__DATA__*/", "window.DOC = " + data + ";"),
                 ("/*__JS__*/", js), ("__TITLE__", model["meta"]["title"])):
        html = html.replace(k, v)
    return html


def build(path):
    model = build_model(path)
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    out = os.path.join(OUT, os.path.splitext(os.path.basename(path))[0] + ".html")
    io.open(out, "w", encoding="utf-8", newline="\n").write(render(model))
    return out, model


def dump(m):
    p = lambda *a: print(*a)
    p("sections %d, refs %d (unresolved %d)" % (len(m["secs"]), len(m["refs"]),
                                                sum(1 for r in m["refs"] if r["tid"] is None)))
    for c in m["cmds"]:
        r = c["resp"]
        p("  %s %-14s mode=%-7s req=%s calc=%s resp=%s sum=%s frames=%s/%s st=%s" % (
            c["code"], c["name"], c.get("mode"), c["req_decl"], c["req_calc"],
            r.get("size", r.get("max_size")), r.get("field_sum"), r.get("frames"),
            r.get("calc_frames"), ",".join(x[4:] for x in c["statuses"])))
    p("status %d, ovs %d, reqs %d, checklist %d, decisions %d, code %d, cites %d, dupes %d" % (
        len(m["status"]), len(m["ovs"]), len(m["reqs"]), len(m["checklist"]),
        len(m["decisions"]), len(m["code"]), len(m["cites"]), len(m["dupes"])))
    for i in m["issues"]:
        p("  [%s] L%-5d %s | %s" % (i["sev"][0].upper(), i["line"], i["cat"], i["title"]))
    for e in m["code"]:
        if e["status"] != "code" or e.get("full"):
            p("  code %-8s %-30s %s %s" % (e["status"], e["tok"], e.get("loc") or "",
                                           "(= %s)" % e["full"] if e.get("full") else ""))
    for d in m["dupes"][:30]:
        p("  dupe L%-4d~L%-4d cover=%.2f lcs=%2d %s" % (d["a"], d["b"], d["cover"], len(d["lcs"]),
                                                        d["lcs"][:40]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--note", default="06")
    ap.add_argument("--open", action="store_true")
    ap.add_argument("--serve", action="store_true")
    ap.add_argument("--port", type=int, default=8706)
    ap.add_argument("--dump", action="store_true")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    path = find_note(a.note)
    if a.dump:
        return dump(build_model(path))
    out, m = build(path)
    print("wrote %s  (%d sections, %d refs, %d issues)" % (
        os.path.relpath(out, DEV), len(m["secs"]), len(m["refs"]), len(m["issues"])))
    if a.serve:
        import serve
        serve.run(path, out, a.port, lambda: build(path))
    elif a.open:
        import webbrowser
        webbrowser.open("file:///" + out.replace("\\", "/"))


if __name__ == "__main__":
    main()
