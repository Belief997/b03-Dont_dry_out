"""Doc-internal consistency checks. Each issue:
{sev: error|warn|info, cat, title, line, detail, refs: [line...]}"""
import math
import re

from extract import FRAME_DATA
from model import plain

MAX_FRAME = 20
MAX_RESP = 128 * FRAME_DATA


def issue(sev, cat, title, line, detail="", refs=None):
    return {"sev": sev, "cat": cat, "title": title, "line": line,
            "detail": detail, "refs": refs or []}


def _offsets(c, key, label, out):
    prev = None
    for f in c[key]:
        if prev and None not in (prev["off"], prev["len"], f["off"]):
            want = prev["off"] + prev["len"]
            if f["off"] != want:
                out.append(issue(
                    "error", "字段布局",
                    "%s %s：`%s` 偏移写成 %d，应为 %d" % (c["name"], label, f["name"], f["off"], want),
                    f["line"],
                    "`%s` 占 %d..%d（%d 字节），下一个字段只能从 %d 开始。"
                    % (prev["name"], prev["off"], want - 1, prev["len"], want),
                    [prev["line"]]))
        prev = f


def _fmt_req(d):
    if d is None:
        return "未写明"
    if d["kind"] == "range":
        return "%d~%d" % (d["lo"], d["hi"])
    return " 或 ".join(map(str, d["vals"]))


def check_commands(cmds):
    out = []
    for c in cmds:
        _offsets(c, "req_fields", "参数", out)
        _offsets(c, "resp_fields", "响应", out)
        r = c["resp"]
        if r.get("size") is not None and r.get("field_sum") is not None \
                and r["size"] != r["field_sum"]:
            parts = " + ".join("%s(%d)" % (f["name"], f["len"]) for f in c["resp_fields"])
            out.append(issue("error", "字段布局",
                             "%s 响应声明 %d 字节，字段表合计只有 %d" % (c["name"], r["size"], r["field_sum"]),
                             c["resp_line"], parts + " = %d" % r["field_sum"],
                             [c["resp_fields_line"]]))
        if r.get("frames") and r.get("size") is not None:
            for sz, what in ((r["size"], "声明长度"), (r.get("field_sum"), "字段合计")):
                if sz is None:
                    continue
                want = max(1, math.ceil(sz / FRAME_DATA))
                if want != r["frames"]:
                    out.append(issue("error", "帧数", "%s：%s %d 字节应为 %d 帧，文中写 %d 帧"
                                     % (c["name"], what, sz, want, r["frames"]), c["resp_line"]))
        sz = r.get("max_size", r.get("size"))
        if sz is not None and sz > MAX_RESP:
            out.append(issue("error", "帧数", "%s 响应最大 %d 字节，超过 128 帧上限 %d"
                             % (c["name"], sz, MAX_RESP), c["resp_line"]))
        d, calc = c["req_decl"], c["req_calc"]
        if d is None:
            out.append(issue("warn", "帧长", "%s 未写明请求帧长" % c["name"], c["line"],
                             "按参数表推算应为 %s。" % _fmt_req(calc)))
        elif d["kind"] != calc["kind"] or \
                (d["kind"] == "set" and sorted(d["vals"]) != sorted(calc["vals"])) or \
                (d["kind"] == "range" and (d["lo"], d["hi"]) != (calc["lo"], calc["hi"])):
            det = "参数表推算：%s。" % _fmt_req(calc)
            if calc["kind"] == "range":
                det += " 最大 = 帧头 2 + mask 2 + 位表各字段之和；最小 %d 意味着 mask=0（什么都不改）" \
                       "也合法——文中没说 mask=0 该回 OK 还是 BAD_PARAM。" % calc["lo"]
            out.append(issue("error", "帧长", "%s 请求帧长写 %s，推算为 %s"
                             % (c["name"], _fmt_req(d), _fmt_req(calc)), c["req_line"], det))
        hi = calc["hi"] if calc["kind"] == "range" else max(calc["vals"])
        if hi > MAX_FRAME:
            out.append(issue("error", "帧长", "%s 请求最长 %d 字节，超过单帧 20" % (c["name"], hi),
                             c["req_line"]))
        if c.get("summary_name") and c["summary_name"] != c["name"]:
            out.append(issue("warn", "命名", "命令表写 `%s`，小节标题写 `%s`"
                             % (c["summary_name"], c["name"]), c["summary_line"], "", [c["line"]]))
    return out


def check_status(status, cmds, lines, secs, section_at):
    out = []
    defined = {s["name"]: s for s in status}
    vals = {}
    for s in status:
        if s["val"] in vals:
            out.append(issue("error", "状态码", "状态码值 %s 重复：%s / %s"
                             % (s["val"], vals[s["val"]], s["name"]), s["line"]))
        vals[s["val"]] = s["name"]
    mentions = {}
    for ln, t in enumerate(lines, 1):
        sec = section_at(secs, ln)
        for m in re.findall(r"ERR_[A-Z_]+", t):
            mentions.setdefault(m, []).append((ln, sec["key"] if sec else ""))
    for name, ms in mentions.items():
        if name not in defined:
            out.append(issue("error", "状态码", "`%s` 被引用但 §3 没有定义" % name, ms[0][0]))
    for name, s in defined.items():
        if name == "OK":
            continue
        outside = [m for m in mentions.get(name, []) if m[0] != s["line"]]
        if not outside:
            sev = "info" if name == "ERR_INTERNAL" else "warn"
            det = ("兜底码，没有命令显式引用可以接受。" if sev == "info" else
                   "§3 定义了它，但没有任何命令、会话规则或流程说明会在什么情况下返回它。"
                   "要么在具体命令里写明触发条件，要么删掉。")
            out.append(issue(sev, "状态码", "`%s`（%s）是孤儿状态码：只在 §3 出现" % (name, s["val"]),
                             s["line"], det))
    return out


def check_refs(refs, secs):
    out = []
    for r in refs:
        if r["kind"] in ("sec", "ovs", "sug") and r["tid"] is None:
            out.append(issue("error", "引用", "引用了不存在的 %s" % r["target"], r["line"],
                             "原文：%s" % r["raw"]))
    return out


def check_numbering(secs):
    out = []
    top = sorted(int(s["key"][1:]) for s in secs if s["key"] and re.fullmatch(r"§\d+", s["key"]))
    for a, b in zip(top, top[1:]):
        if b != a + 1:
            out.append(issue("warn", "编号", "章节号从 §%d 跳到 §%d" % (a, b), 0))
    subs = {}
    for s in secs:
        m = s["key"] and re.fullmatch(r"§(\d+)\.(\d+)", s["key"])
        if m:
            subs.setdefault(int(m.group(1)), []).append((int(m.group(2)), s["line"]))
    for k, v in subs.items():
        nums = [x[0] for x in v]
        if nums != list(range(1, len(nums) + 1)):
            out.append(issue("warn", "编号", "§%d 的小节编号不连续：%s" % (k, nums), v[0][1]))
    ovs = [(int(s["key"][2:]), s["line"]) for s in secs if s["kind"] == "ovs"]
    if ovs and [o[0] for o in ovs] != list(range(1, len(ovs) + 1)):
        out.append(issue("warn", "编号", "疏漏编号不连续：%s" % [o[0] for o in ovs], ovs[0][1]))
    for s in secs:
        if s["kind"] == "sug" and s["key"][-1] != "A":
            first = s["key"][-1]
            out.append(issue("info", "编号", "只有「%s」，没有建议 A~%s" % (s["key"], chr(ord(first) - 1)),
                             s["line"], "像是旧稿编号残留。改成「建议 A」或并入疏漏编号，"
                             "否则读者会去找不存在的 A~E。"))
    return out


def check_coverage(ovs, checklist, decisions, reqs):
    out = []
    ck_text = " ".join(c["text"] for c in checklist)
    dc_ovs = {o for d in decisions for o in d["ovs"]}
    for o in ovs:
        if o["key"].startswith("疏漏"):
            n = o["key"][2:]
            in_ck = re.search(r"疏漏\s*%s(?!\d)" % n, ck_text)
            if o["sev"] >= 3 and not in_ck:
                out.append(issue("warn", "覆盖", "%s【%s】没有对应的 §12 待实现项" % (o["key"], o["tag"]),
                                 o["line"]))
            if "待决" in o["tag"] and o["key"] not in dc_ovs:
                out.append(issue("warn", "覆盖", "%s 标【待决】，但 §13 待决清单里没有它" % o["key"],
                                 o["line"]))
    for d in decisions:
        for k in d["ovs"]:
            o = next((x for x in ovs if x["key"] == k), None)
            if o and "待决" not in o["tag"]:
                out.append(issue("info", "覆盖", "§13 第 %s 条指向 %s，但它的标签是【%s】而不是【待决】"
                                 % (d["n"], k, o["tag"]), d["line"]))
    for r in reqs:
        if not r["ok"]:
            out.append(issue("info", "需求", "需求 %d「%s」未完全满足：%s" % (r["req"], r["text"], r["status"]),
                             r["line"]))
    done = sum(1 for c in checklist if c["done"])
    if checklist:
        out.append(issue("info", "进度", "§12 固件待实现清单：%d / %d 已完成" % (done, len(checklist)),
                         checklist[0]["line"]))
    return out
