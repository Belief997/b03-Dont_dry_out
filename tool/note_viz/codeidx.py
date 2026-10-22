"""Anchor the note to real code: where each cited identifier/file lives,
and whether `file.c:NNN` line citations still point at the right code."""
import io
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
DEV = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.dirname(DEV)
ROOTS = ["app/sensor_beacon", "sensor_tool/rust/src", "sensor_tool/lib", "desktop", "tool"]
SKIP_DIRS = {"_build", "target", ".dart_tool", "build", "out", "__pycache__", "note_viz"}
CODE_EXT = (".c", ".h", ".rs", ".dart", ".py", ".html")
FILE_TOKEN = re.compile(r"^[\w./-]+\.(c|h|rs|py|dart|html|md)$")
IDENT = re.compile(r"^[A-Za-z_]\w*(\(\))?$")
PLANNED = re.compile(r"待建|新增|需要新增|加一个|可加|要加|将来|v2|建议新增|新的")


def _read(p):
    return io.open(p, encoding="utf-8", errors="replace").read().replace("\r", "")


def rel(p):
    return os.path.relpath(p, DEV).replace("\\", "/")


def _walk(base, exts):
    for dp, dns, fns in os.walk(base):
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        for f in fns:
            if f.endswith(exts):
                yield os.path.join(dp, f)


def project_files():
    out = []
    for r in ROOTS:
        out.extend(_walk(os.path.join(DEV, r), CODE_EXT))
    return out


def all_dev_files():
    return list(_walk(DEV, CODE_EXT + (".md",)))


_cache = {}


def lines_of(p):
    if p not in _cache:
        _cache[p] = _read(p).split("\n")
    return _cache[p]


def resolve_file(tok, files):
    tok = tok.lstrip("./")
    if tok.startswith("dev/"):
        tok = tok[4:]
    hits = [f for f in files if rel(f) == tok or rel(f).endswith("/" + tok)]
    return sorted(hits, key=lambda f: len(rel(f)))


def _in_comment(line, pos):
    s = line.lstrip()
    if s.startswith(("*", "/*", "//", "#!", "///")) or (s.startswith("#") and not s.startswith("#define")
                                                         and not s.startswith("#include")):
        return True
    head = line[:pos]
    return "/*" in head and "*/" not in head[head.rfind("/*"):] or "//" in head


def _def_rank(line, name, pos):
    """0 #define / 1 declaration / 2 code use / 3 comment. name = full identifier."""
    if _in_comment(line, pos):
        return 3
    s, n = line.strip(), re.escape(name)
    if re.match(r"#define\s+%s\b" % n, s):
        return 0
    if re.match(r"\}\s*%s\s*;" % n, s) or re.search(r"\b(fn|struct|enum|const|class|def)\s+%s\b" % n, s):
        return 1
    if re.match(r"(static\s+|const\s+|volatile\s+)*[A-Za-z_]\w*[\s\*]+%s\s*(\[[^\]]*\])?\s*[;=(]" % n, s) \
            and not re.match(r"(return|case|else|if|while)\b", s) and not s.endswith((");", ") ;")) \
            or (re.match(r"(static\s+|const\s+|volatile\s+)*[A-Za-z_]\w*[\s\*]+%s\s*(\[[^\]]*\])?\s*;" % n, s)):
        return 1
    return 2


def find_symbols(syms, files):
    """-> (exact, suffix): sym -> {loc, rank, count, value, full, comment_only}.
    `suffix` catches the doc's abbreviations (`RECS_PER_PAGE` ->
    APP_STORAGE_RECS_PER_PAGE, `PAIRING_NOT_SUPP` -> BLE_GAP_SEC_STATUS_...)."""
    exact, suffix = {}, {}
    if not syms:
        return exact, suffix
    alt = "|".join(sorted(map(re.escape, syms), key=len, reverse=True))
    # two passes: in one alternation a longer doc symbol (app_storage_cfg_load)
    # would swallow the shorter one it ends with (cfg_load)
    passes = (re.compile(r"\b()(" + alt + r")\b"), re.compile(r"\b(\w+?_)(" + alt + r")\b"))
    for f in files:
        text = _read(f)
        if not passes[0].search(text):
            continue
        for i, ln in enumerate(text.split("\n"), 1):
            for m in (x for p in passes for x in p.finditer(ln)):
                pre, sym = m.group(1), m.group(2)
                full = pre + sym
                rk = _def_rank(ln, full, m.start())
                e = (suffix if pre else exact).setdefault(sym, {"count": 0, "rank": 9, "code_hits": 0})
                e["count"] += 1
                e["code_hits"] += rk < 3
                if rk < e["rank"]:
                    e.update(rank=rk, loc="%s:%d" % (rel(f), i), code=ln.strip()[:160],
                             full=full if pre else None)
                    v = re.match(r"\s*#define\s+\w+\s+(.+?)\s*(/\*.*)?$", ln)
                    e["value"] = v.group(1) if (v and rk == 0) else None
    for d in (exact, suffix):
        for e in d.values():
            e["comment_only"] = e["code_hits"] == 0
    return exact, suffix


def sdk_symbols(syms):
    found = {}
    if not syms:
        return found
    big = re.compile(r"\b(" + "|".join(sorted(map(re.escape, syms), key=len, reverse=True)) + r")\b")
    for sub in ("components", "modules"):
        for f in _walk(os.path.join(SDK, sub), (".h",)):
            text = _read(f)
            for m in big.finditer(text):
                if m.group(1) not in found:
                    ln = text.count("\n", 0, m.start()) + 1
                    found[m.group(1)] = os.path.relpath(f, SDK).replace("\\", "/") + ":%d" % ln
            if len(found) == len(syms):
                return found
    return found


def index_doc(lines, secs, section_at, exclude):
    """Collect code-like backtick tokens from the doc (outside fences)."""
    toks, in_code = {}, False
    for ln, t in enumerate(lines, 1):
        if t.lstrip().startswith(("```", "~~~")):
            in_code = not in_code
            continue
        if in_code:
            continue
        for m in re.finditer(r"`([^`]+)`", t):
            tok = m.group(1).strip()
            kind = None
            if FILE_TOKEN.match(tok):
                kind = "file"
            elif IDENT.match(tok):
                name = tok.rstrip("()")
                if name in exclude or len(name) < 5 or re.fullmatch(r"u?int\d+_t", name) \
                        or "_" not in name and not tok.endswith("()") and not re.search(r"[a-z][A-Z]", name):
                    continue
                tok, kind = name, "sym"
            if kind:
                sec = section_at(secs, ln)
                e = toks.setdefault(tok, {"tok": tok, "kind": kind, "lines": [], "secs": [],
                                          "planned": False})
                e["lines"].append(ln)
                if sec and sec["id"] not in e["secs"]:
                    e["secs"].append(sec["id"])
                if PLANNED.search(t) or (sec and sec["key"] in ("§12", "§13")):
                    e["planned"] = True
    return toks


STATUS_ORDER = {"missing": 0, "comment": 1, "planned": 2, "proto": 3, "code": 4, "sdk": 5}


def build_code_index(lines, secs, section_at, exclude, proto_fields):
    """status: code / sdk / comment (only in code comments) / planned (doc says
    it's to be added) / proto (a wire field this doc itself defines) / missing."""
    toks = index_doc(lines, secs, section_at, exclude)
    pfiles, dfiles = project_files(), all_dev_files()
    syms = [k for k, v in toks.items() if v["kind"] == "sym"]
    exact, suffix = find_symbols(syms, pfiles)
    live = lambda d, k: k in d and not d[k]["comment_only"]
    sdk = sdk_symbols([s for s in syms if not live(exact, s) and not live(suffix, s)])
    dirs = {}
    for dp, dns, _ in os.walk(DEV):
        dns[:] = [d for d in dns if d not in SKIP_DIRS and not d.startswith(".")]
        if dp.count(os.sep) - DEV.count(os.sep) < 2:
            for d in dns:
                dirs.setdefault(d, rel(os.path.join(dp, d)))
    out = []
    for k, e in toks.items():
        fallback = "planned" if e["planned"] else ("proto" if k in proto_fields else "missing")
        hit = exact[k] if live(exact, k) else None
        if hit is None and k not in proto_fields and live(suffix, k):
            hit = suffix[k]
        if k in dirs:
            e.update(kind="dir", loc=dirs[k], status="code")
        elif e["kind"] == "file":
            hits = resolve_file(k, dfiles)
            e["loc"] = rel(hits[0]) if hits else None
            e["status"] = "code" if hits else fallback
        elif hit:
            e.update(loc=hit["loc"], code=hit.get("code"), value=hit.get("value"),
                     count=hit["count"], full=hit.get("full"), status="code")
        elif k in sdk:
            e.update(loc=sdk[k], status="sdk")
        elif k in proto_fields or e["planned"]:
            e["status"] = fallback
        elif k in exact or k in suffix:
            f = exact.get(k) or suffix[k]
            e.update(loc=f["loc"], code=f.get("code"), count=f["count"], status="comment")
        else:
            e["status"] = "missing"
        out.append(e)
    out.sort(key=lambda e: (STATUS_ORDER[e["status"]], e["tok"]))
    return out


def check_line_cites(lines, secs, section_at, code_index):
    """`main.c:857` style citations: does that line still hold what the section talks about?"""
    files = all_dev_files()
    out = []
    for ln, t in enumerate(lines, 1):
        for m in re.finditer(r"([\w./-]+\.(?:c|h|rs|py|dart)):(\d+)", t):
            hits = resolve_file(m.group(1), files)
            if not hits:
                continue
            sec = section_at(secs, ln)
            span = "\n".join(lines[sec["line"] - 1:sec["end"]]) if sec else t
            words = {w.rstrip("()") for w in re.findall(r"`([A-Za-z_]\w*(?:\(\))?)`", span)}
            src, n = lines_of(hits[0]), int(m.group(2))
            window = "\n".join(src[max(0, n - 4):n + 3])
            ok = [w for w in words if re.search(r"\b%s\b" % re.escape(w), window)]
            cands = []
            for i, s in enumerate(src, 1):
                if any(re.search(r"\b%s\b" % re.escape(w), s) for w in words):
                    cands.append({"line": i, "code": s.strip()[:140]})
            out.append({"doc_line": ln, "cite": m.group(0), "file": rel(hits[0]), "n": n,
                        "now": src[n - 1].strip() if n <= len(src) else "(超出文件末尾)",
                        "words": sorted(words), "ok": bool(ok), "cands": cands[:8]})
    return out
