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
        step = re.match(r"    \d+\. (.*)$|       (\S.*)$", line)
        if not step:
            break
        acts.append(step.group(1) or step.group(2))
    return int(m.group(1)), acts


def spec_value(v, itf):
    """A trace value written the way cmc prints a state: 5 or [0:1,1:2]."""
    if itf and "#bigint" in v:
        return v["#bigint"]
    if itf:
        return "[" + ",".join(k["#bigint"] + ":" + x["#bigint"] for k, x in v["#map"]) + "]"
    if isinstance(v, int):
        return str(v)
    return "[" + ",".join(k + ":" + str(x) for k, x in v.items()) + "]"


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
    line = "  state:" + "".join(" %s=%s" % (x, spec_value(last[x], itf)) for x in names)
    if line not in out.splitlines():
        fail("final state %r is not the one cmc printed" % line)


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
    if "  replies:" + (reps or " none") not in out.splitlines():
        fail("final replies %r are not the ones cmc printed" % reps)


def shape(v):
    """A value's ITF form with the numbers taken out."""
    if isinstance(v, dict) and "#map" in v:
        return ["#map"] + sorted({json.dumps([shape(k), shape(x)]) for k, x in v["#map"]})
    if isinstance(v, dict):
        return sorted(v)
    return type(v).__name__


def same_shape(mine, theirs):
    """cmc's ITF has the structure of Apalache's for the same module: the
    same header, variables, parameters and types, as many states, and the
    same keys and value forms in each state (the values may differ, as
    each tool picks its own counterexample)."""
    for key in ("vars", "params"):
        if sorted(mine.get(key, [])) != sorted(theirs.get(key, [])):
            fail("%s %s, Apalache wrote %s" % (key, mine.get(key), theirs.get(key)))
    for key in ("format", "varTypes"):
        if mine["#meta"].get(key) != theirs["#meta"].get(key):
            fail("#meta %s differs from Apalache's" % key)
    if len(mine["states"]) != len(theirs["states"]):
        fail("%d states, Apalache wrote %d" % (len(mine["states"]), len(theirs["states"])))
    for a, b in zip(mine["states"], theirs["states"]):
        if a["#meta"]["index"] != b["#meta"]["index"] or sorted(a) != sorted(b):
            fail("state %d keys %s, Apalache wrote %s" % (a["#meta"]["index"], sorted(a), sorted(b)))
        for k in a:
            if k != "#meta" and shape(a[k]) != shape(b[k]):
                fail("state %d: %s has another form than Apalache's" % (a["#meta"]["index"], k))


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
