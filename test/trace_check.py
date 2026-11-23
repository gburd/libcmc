#!/usr/bin/env python3
"""trace_check.py KIND OUT TRACE: check a --trace-out file against cmc's output.

KIND is itf or json (cmc --spec) or proc (cmc --model-file).  OUT holds
what cmc printed for the run, TRACE the file it wrote.  The file must
parse, hold one state per action plus the initial one, name the actions
cmc printed in the order it printed them, and end in the state cmc
printed.  The checks use only what cmc printed, never a value worked out
here.  Exit 0 if the file agrees, 1 with a reason if not.

trace_check.py apalache MINE THEIRS: MINE, an ITF file cmc wrote, has
the structure of THEIRS, the one Apalache wrote for the same module.
"""

import json
import re
import sys


def fail(msg):
    print("trace_check: " + msg)
    sys.exit(1)


def printed(out):
    """The actions cmc listed and its final-state lines."""
    m = re.search(r"is \((\d+) actions\):\n", out)
    if not m:
        fail("no counterexample in the output")
    acts = []
    for line in out[m.end():].splitlines():
        if line.startswith("         spec: "):
            continue
        step = re.match(r"    \d+\. (.*)$|       (\S.*)$", line)
        if not step:
            break
        acts.append(step.group(1) or step.group(2))
    return int(m.group(1)), acts


def parse_printed(text):
    """A value as cmc prints it (5, [0:1,1:2], [a |-> 1], <<1, 2>>) as a
    Python value: an int, a dict keyed by int (a function) or by name (a
    record), or a list (a sequence)."""
    pos = 0

    def value():
        nonlocal pos
        if text.startswith("<<", pos):
            pos += 2
            items = []
            while not text.startswith(">>", pos):
                items.append(value())
                if text.startswith(", ", pos):
                    pos += 2
            pos += 2
            return items
        if text[pos] == "[":
            pos += 1
            items = {}
            while text[pos] != "]":
                m = re.compile(r"(-?\d+):|(\w+) \|-> ").match(text, pos)
                if not m:
                    fail("cannot read the printed value %r" % text)
                pos = m.end()
                items[int(m.group(1)) if m.group(1) else m.group(2)] = value()
                if text[pos] == ",":
                    pos += 2 if text.startswith(", ", pos) else 1
            pos += 1
            return items
        m = re.compile(r"-?\d+").match(text, pos)
        if not m:
            fail("cannot read the printed value %r" % text)
        pos = m.end()
        return int(m.group())

    v = value()
    if pos != len(text):
        fail("trailing text in the printed value %r" % text)
    return v


def trace_value(v, itf):
    """A trace file's value as parse_printed() would read cmc's print of it."""
    if itf and isinstance(v, dict) and "#bigint" in v:
        return int(v["#bigint"])
    if itf and isinstance(v, dict) and "#map" in v:
        return {int(k["#bigint"]): trace_value(x, itf) for k, x in v["#map"]}
    if isinstance(v, list):
        return [trace_value(x, itf) for x in v]
    if isinstance(v, dict):
        return {int(k) if re.fullmatch(r"-?\d+", k) else k: trace_value(x, itf) for k, x in v.items()}
    return v


def printed_state(out, names):
    """The variable values of cmc's printed final state, by name."""
    lines = [x for x in out.splitlines() if x.startswith("  state: ")]
    if not lines:
        fail("no final state in the output")
    line = lines[-1][len("  state:"):]
    marks = [line.find(" %s=" % x) for x in names]
    if any(m < 0 for m in marks) or marks != sorted(marks):
        fail("the printed state %r does not list the variables %s" % (line, names))
    ends = marks[1:] + [len(line)]
    return {x: parse_printed(line[m + len(x) + 2:e]) for x, m, e in zip(names, marks, ends)}


def check_spec(kind, out, tr):
    n, acts = printed(out)
    itf = kind == "itf"
    if itf:
        meta = tr["#meta"]
        if meta.get("format") != "ITF" or not isinstance(tr.get("vars"), list):
            fail("not an ITF trace: " + str(meta))
        states, names = tr["states"], tr["vars"]
        got = [s["#meta"].get("action") for s in states]
        if [s["#meta"]["index"] for s in states] != list(range(len(states))):
            fail("state indices are not 0, 1, ...")
    else:
        states = tr
        names = list(states[0]["vars"])
        got = [s["action"] for s in states]
    if len(states) != n + 1:
        fail("%d states for %d actions" % (len(states), n))
    if got != [None] + acts:
        fail("actions %s, cmc printed %s" % (got, acts))
    last = states[-1] if itf else states[-1]["vars"]
    want = printed_state(out, names)
    for x in names:
        if trace_value(last[x], itf) != want[x]:
            fail("final %s %r is not the one cmc printed, %r" % (x, last[x], want[x]))


def check_proc(out, tr):
    n, acts = printed(out)
    if len(tr) != len(acts) + 1:
        fail("%d worlds for %d listed steps" % (len(tr), len(acts)))
    if [w["action"] for w in tr] != [None] + acts:
        fail("actions differ from the ones cmc printed")
    if [w["number"] for w in tr if w["number"] is not None] != list(range(1, n + 1)):
        fail("numbered actions are not 1 .. %d" % n)
    last = tr[-1]
    nodes = " ".join("node[%d]=%s" % (x["node"], ",".join(map(str, x["state"])) or "()")
                     for x in last["nodes"])
    if "  node states: " + nodes not in out.splitlines():
        fail("final node states %r are not the ones cmc printed" % nodes)
    reps = "".join(" (n%d,%d)" % (r["node"], r["value"]) for r in last["replies"])
    # a refinement report prints no replies line; every other one does
    refine = any("spec" in w for w in tr)
    if not refine and "  replies:" + (reps or " none") not in out.splitlines():
        fail("final replies %r are not the ones cmc printed" % reps)
    check_spec_terms(out, tr)


def spec_text(v):
    """A spec value of a proc trace (an int, or a function as an object
    keyed by its arguments) as cmc prints it."""
    if isinstance(v, dict):
        return "[" + ",".join("%s:%s" % (k, spec_text(x)) for k, x in v.items()) + "]"
    return str(v)


def check_spec_terms(out, tr):
    """With --refine, every world the check judged carries "spec", and
    the spec lines cmc printed are those worlds' actions and states, in
    order.  Without it, no world carries one and none is printed."""
    lines = [x[len("         spec: "):] for x in out.splitlines() if x.startswith("         spec: ")]
    if not any("spec" in w for w in tr):
        if lines:
            fail("cmc printed spec lines but the worlds carry no spec")
        return
    want = []
    for w in tr:
        if "spec" not in w:
            fail("world %d has no spec" % w["index"])
        sp = w["spec"]
        if sp is None:
            continue
        vals = " ".join("%s=%s" % (k, spec_text(x)) for k, x in sp["state"].items())
        want.append(vals if sp["action"] is None else "%s -> %s" % (sp["action"], vals))
    if want != lines:
        fail("spec lines %r, the worlds say %r" % (lines, want))


def parse_type(text):
    """An ITF varTypes string (Int, (Int -> T), Seq(T), { a: T, ... }) as
    a tree: "Int", ("fun", T), ("seq", T) or ("rec", {name: T})."""
    toks = re.findall(r"->|\w+|[(){}:,]", text)
    pos = 0

    def take(want=None):
        nonlocal pos
        if pos >= len(toks) or (want and toks[pos] != want):
            fail("cannot read the type %r" % text)
        pos += 1
        return toks[pos - 1]

    def one():
        t = take()
        if t == "Int":
            return "Int"
        if t == "Seq":
            take("(")
            el = one()
            take(")")
            return ("seq", el)
        if t == "(":
            if take() != "Int":
                fail("a function type with a key other than Int: %r" % text)
            take("->")
            rng = one()
            take(")")
            return ("fun", rng)
        if t == "{":
            fields = {}
            while True:
                name = take()
                take(":")
                fields[name] = one()
                if take() == "}":
                    return ("rec", fields)
        fail("cannot read the type %r" % text)

    ty = one()
    if pos != len(toks):
        fail("trailing text in the type %r" % text)
    return ty


def conforms(v, ty):
    """Is v written as ITF writes a value of type ty?"""
    if ty == "Int":
        return isinstance(v, dict) and list(v) == ["#bigint"] and isinstance(v["#bigint"], str)
    kind, sub = ty
    if kind == "seq":
        return isinstance(v, list) and all(conforms(x, sub) for x in v)
    if kind == "fun":
        return (isinstance(v, dict) and list(v) == ["#map"]
                and all(len(p) == 2 and conforms(p[0], "Int") and conforms(p[1], sub)
                        for p in v["#map"]))
    return (isinstance(v, dict) and list(v) == sorted(sub)
            and all(conforms(v[k], sub[k]) for k in sub))


def same_shape(mine, theirs):
    """cmc's ITF has the structure of Apalache's for the same module: the
    same header, variables, parameters and types, as many states, and the
    same keys in each state, every value written as ITF writes a value of
    its type: a parameter an integer, a variable of its varTypes entry,
    a record's fields in the order of their names.  The values may
    differ, as each tool picks its own counterexample."""
    for key in ("vars", "params"):
        if sorted(mine.get(key, [])) != sorted(theirs.get(key, [])):
            fail("%s %s, Apalache wrote %s" % (key, mine.get(key), theirs.get(key)))
    for key in ("format", "varTypes"):
        if mine["#meta"].get(key) != theirs["#meta"].get(key):
            fail("#meta %s differs from Apalache's" % key)
    types = {k: parse_type(t) for k, t in theirs["#meta"]["varTypes"].items()}
    if len(mine["states"]) != len(theirs["states"]):
        fail("%d states, Apalache wrote %d" % (len(mine["states"]), len(theirs["states"])))
    for a, b in zip(mine["states"], theirs["states"]):
        if a["#meta"]["index"] != b["#meta"]["index"] or sorted(a) != sorted(b):
            fail("state %d keys %s, Apalache wrote %s" % (a["#meta"]["index"], sorted(a), sorted(b)))
        for k in a:
            if k == "#meta":
                continue
            ty = types.get(k, "Int")
            if not conforms(a[k], ty) or not conforms(b[k], ty):
                fail("state %d: %s is not written as Apalache writes a value of type %s"
                     % (a["#meta"]["index"], k, theirs["#meta"]["varTypes"].get(k, "Int")))


def main():
    kind, out_path, trace_path = sys.argv[1:4]
    if kind == "apalache":
        with open(out_path) as f, open(trace_path) as g:
            same_shape(json.load(f), json.load(g))
        print("trace_check: ok")
        return
    with open(out_path) as f:
        out = f.read()
    try:
        with open(trace_path) as f:
            tr = json.load(f)
    except (OSError, ValueError) as e:
        fail("%s does not parse: %s" % (trace_path, e))
    if kind == "proc":
        check_proc(out, tr)
    else:
        check_spec(kind, out, tr)
    print("trace_check: ok")


main()
