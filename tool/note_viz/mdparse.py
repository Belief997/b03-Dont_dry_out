"""Minimal Markdown block parser that keeps source line numbers.

Covers only what dev/note/*.md actually uses: ATX headings, pipe tables,
fenced code, blockquotes (re-parsed recursively), flat lists with indented
continuation lines, paragraphs and thematic breaks. Inline markup is left to
the browser.

Every block is a dict: {"t": type, "line": first, "end": last, ...}
(1-based, inclusive). Types: h, p, list, table, code, quote, hr.
"""
import re

H_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
LI_RE = re.compile(r"^(\s*)([-*+]|\d+[.)])\s+(?:\[([ xX])\]\s+)?(.*)$")
FENCE_RE = re.compile(r"^\s*(```|~~~)(.*)$")
HR_RE = re.compile(r"^\s*([-*_])(\s*\1){2,}\s*$")
TROW_RE = re.compile(r"^\s*\|.*\|\s*$")
TSEP_RE = re.compile(r"^\s*\|?(\s*:?-{2,}:?\s*\|)+\s*(:?-{2,}:?\s*)?$")


def split_row(line):
    """Split a pipe-table row; '\\|' is an escaped pipe even inside code."""
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith("\\|"):
        s = s[:-1]
    cells, cur, i = [], "", 0
    while i < len(s):
        if s[i] == "\\" and s[i + 1:i + 2] == "|":
            cur += "|"
            i += 2
            continue
        if s[i] == "|":
            cells.append(cur.strip())
            cur = ""
        else:
            cur += s[i]
        i += 1
    cells.append(cur.strip())
    return cells


def join_lines(parts):
    """Join soft-wrapped lines: no space between CJK, one space between ASCII."""
    s = ""
    for p in parts:
        p = p.strip()
        if s and p and s[-1].isascii() and p[0].isascii():
            s += " "
        s += p
    return s


def _starts_block(lines, k):
    ln = lines[k]
    return (H_RE.match(ln) or FENCE_RE.match(ln) or HR_RE.match(ln)
            or ln.lstrip().startswith(">") or LI_RE.match(ln)
            or (TROW_RE.match(ln) and k + 1 < len(lines)
                and TSEP_RE.match(lines[k + 1])))


def parse(lines, base=1):
    """lines: list of str without newlines; base: line number of lines[0]."""
    out, i, n = [], 0, len(lines)
    while i < n:
        ln = lines[i]
        if not ln.strip():
            i += 1
            continue

        m = FENCE_RE.match(ln)
        if m:
            fence, j = m.group(1), i + 1
            while j < n and not lines[j].strip().startswith(fence):
                j += 1
            out.append({"t": "code", "line": base + i, "end": base + min(j, n - 1),
                        "lang": m.group(2).strip(),
                        "text": "\n".join(lines[i + 1:j])})
            i = j + 1
            continue

        m = H_RE.match(ln)
        if m:
            out.append({"t": "h", "line": base + i, "end": base + i,
                        "level": len(m.group(1)), "text": m.group(2)})
            i += 1
            continue

        if HR_RE.match(ln):
            out.append({"t": "hr", "line": base + i, "end": base + i})
            i += 1
            continue

        if ln.lstrip().startswith(">"):
            j = i
            while j < n and lines[j].lstrip().startswith(">"):
                j += 1
            inner = [re.sub(r"^\s*>\s?", "", x) for x in lines[i:j]]
            out.append({"t": "quote", "line": base + i, "end": base + j - 1,
                        "children": parse(inner, base + i)})
            i = j
            continue

        if TROW_RE.match(ln) and i + 1 < n and TSEP_RE.match(lines[i + 1]):
            j = i + 2
            while j < n and TROW_RE.match(lines[j]):
                j += 1
            out.append({"t": "table", "line": base + i, "end": base + j - 1,
                        "head": split_row(ln),
                        "rows": [split_row(x) for x in lines[i + 2:j]],
                        "rlines": [base + k for k in range(i + 2, j)]})
            i = j
            continue

        m = LI_RE.match(ln)
        if m:
            items, j = [], i
            ordered = m.group(2)[0].isdigit()
            indent = len(m.group(1))
            while j < n and lines[j].strip():
                mm = LI_RE.match(lines[j])
                if mm and len(mm.group(1)) <= indent:
                    items.append({"line": base + j, "check": mm.group(3),
                                  "marker": mm.group(2), "parts": [mm.group(4)]})
                elif items and (lines[j][:1].isspace() or not _starts_block(lines, j)):
                    items[-1]["parts"].append(lines[j])
                else:
                    break
                j += 1
            for it in items:
                it["text"] = join_lines(it.pop("parts"))
                c = it["check"]
                it["check"] = None if c is None else (c.lower() == "x")
            out.append({"t": "list", "line": base + i, "end": base + j - 1,
                        "ordered": ordered, "items": items})
            i = j
            continue

        j = i
        while j < n and lines[j].strip() and (j == i or not _starts_block(lines, j)):
            j += 1
        out.append({"t": "p", "line": base + i, "end": base + j - 1,
                    "text": join_lines(lines[i:j])})
        i = j
    return out


def walk(blocks, depth=0):
    """Yield (block, depth) for every block, quote children included."""
    for b in blocks:
        yield b, depth
        if b["t"] == "quote":
            yield from walk(b["children"], depth + 1)
