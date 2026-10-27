"""A minimal S-expression reader/writer for KiCad files."""
import re

TOKEN = re.compile(r'\s*(?:(\()|(\))|("(?:[^"\\]|\\.)*")|([^\s()"]+))')

class Sym(str):
    """A bare atom (not a quoted string)."""

def parse(text):
    stack = [[]]
    pos = 0
    while True:
        m = TOKEN.match(text, pos)
        if not m or m.end() == pos:
            break
        pos = m.end()
        op, cl, st, at = m.groups()
        if op:
            stack.append([])
        elif cl:
            node = stack.pop()
            stack[-1].append(node)
        elif st is not None:
            stack[-1].append(st[1:-1].replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\'))
        else:
            stack[-1].append(Sym(at))
    return stack[0][0]

def dump(node, indent=0):
    pad = "\t" * indent
    if isinstance(node, list):
        simple = all(not isinstance(c, list) for c in node)
        if simple:
            return pad + "(" + " ".join(atom(c) for c in node) + ")"
        head = [atom(c) for c in node if not isinstance(c, list)]
        parts = [pad + "(" + " ".join(head)]
        for c in node:
            if isinstance(c, list):
                parts.append(dump(c, indent + 1))
        return "\n".join(parts) + "\n" + pad + ")"
    return pad + atom(node)

def atom(x):
    if isinstance(x, Sym):
        return str(x)
    if isinstance(x, (int, float)):
        return fmt(x)
    return '"' + str(x).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'

def fmt(v):
    if isinstance(v, int):
        return str(v)
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s

def find(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]

def first(node, key):
    r = find(node, key)
    return r[0] if r else None
