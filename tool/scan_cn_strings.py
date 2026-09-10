"""Find Chinese inside real string literals (C / Rust / Dart), ignoring comments.

Usage: python scan_cn_strings.py <file> [<file> ...]
       python scan_cn_strings.py --fix-list <file> ...   (one "path:line" per hit)
"""
import io
import sys

BS = chr(92)


def cn(ch):
    return "一" <= ch <= "鿿"


def strip_comments_keep_strings(text, single_quote_is_string=False):
    """Return list of (line_no, literal_text) for every string literal.

    Walks the file as a tiny state machine so that // and /* */ comments are
    skipped and quotes appearing inside comments never open a literal.
    """
    out = []
    i = 0
    n = len(text)
    line = 1
    state = "code"          # code | line_comment | block_comment | string | char
    lit_start_line = 0
    lit = []
    quote = '"'

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if c == "\n":
            line += 1

        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block_comment"
                i += 2
                continue
            if c == '"':
                state = "string"
                quote = '"'
                lit_start_line = line
                lit = []
                i += 1
                continue
            if c == "'":
                # Dart/Python use '' for strings; C/Rust use it for chars.
                if single_quote_is_string:
                    state = "string"
                    quote = "'"
                    lit_start_line = line
                    lit = []
                    i += 1
                    continue
                # Rust lifetimes ('static, '_, 'a) also start with a quote and
                # have no closing one. Treating them as char literals swallows
                # every string up to the next lifetime, silently hiding hits.
                # A real char literal is 'x' or '\x' -- check for the closer.
                nn = text[i + 2] if i + 2 < n else ""
                if nxt == BS or nn == "'":
                    state = "char"
                i += 1
                continue

        elif state == "line_comment":
            if c == "\n":
                state = "code"

        elif state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"
                i += 2
                continue

        elif state == "string":
            if c == BS:
                lit.append(c)
                if nxt:
                    lit.append(nxt)
                i += 2
                continue
            if c == quote:
                out.append((lit_start_line, "".join(lit)))
                state = "code"
                i += 1
                continue
            lit.append(c)

        elif state == "char":
            if c == BS:
                i += 2
                continue
            if c == "'":
                state = "code"

        i += 1

    return out


def main():
    args = sys.argv[1:]
    fix_list = False
    if args and args[0] == "--fix-list":
        fix_list = True
        args = args[1:]

    total = 0
    for path in args:
        try:
            text = io.open(path, encoding="utf-8").read()
        except (IOError, UnicodeDecodeError):
            continue
        sq = path.endswith(".dart")
        hits = [(ln, s) for ln, s in strip_comments_keep_strings(text, sq)
                if any(cn(ch) for ch in s)]
        if not hits:
            continue
        if fix_list:
            for ln, _ in hits:
                print("%s:%d" % (path, ln))
        else:
            print("--- %s : %d ---" % (path, len(hits)))
            for ln, s in hits:
                print("  %5d  %s" % (ln, s[:200]))
        total += len(hits)
    print("TOTAL = %d" % total)


main()
