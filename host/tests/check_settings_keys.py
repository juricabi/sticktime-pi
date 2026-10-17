"""Checks that image/rootfs/usr/lib/sticktime/settings.py carries exactly the key table and the
template of host/src/settings_keys.h (key, kind, default, choices, live, reboot, help; order too).

    python host/tests/check_settings_keys.py        (from anywhere; exit 0 when they match)

image/tests/test_settings_py.py runs it as well."""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HEADER = os.path.join(REPO, "host", "src", "settings_keys.h")
MODULE = os.path.join(REPO, "image", "rootfs", "usr", "lib", "sticktime", "settings.py")

KINDS = {"SK_STRING": "string", "SK_ENUM": "enum", "SK_INT": "int", "SK_NUM": "num", "SK_BOOL": "bool", "SK_SECRET": "secret"}
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "0": "\0", "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?"}


def macro_body(text, name):
    """the text of #define NAME, continuation lines joined"""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("#define " + name) and line[len("#define " + name):][:1] in (" ", "\t", "\\", ""):
            body = [line[len("#define " + name):]]
            while body[-1].rstrip().endswith("\\"):
                body[-1] = body[-1].rstrip()[:-1]
                i += 1
                body.append(lines[i])
            return "\n".join(body)
    raise ValueError(f"no #define {name} in {HEADER}")


def tokens(src):
    """C tokens of a macro body: ("str", text), ("id", name) or ("p", char)"""
    i, n, out = 0, len(src), []
    while i < n:
        c = src[i]
        if c.isspace():
            i += 1
        elif src.startswith("/*", i):
            i = src.index("*/", i) + 2
        elif src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
        elif c == '"':
            i += 1
            s = []
            while src[i] != '"':
                if src[i] == "\n":
                    raise ValueError("newline in a string literal")
                if src[i] == "\\":
                    e = src[i + 1]
                    if e == "x":
                        j = i + 2
                        while j < n and src[j] in "0123456789abcdefABCDEF":
                            j += 1
                        s.append(chr(int(src[i + 2:j], 16)))
                        i = j
                        continue
                    if e not in ESCAPES:
                        raise ValueError(f"unsupported escape \\{e}")
                    s.append(ESCAPES[e])
                    i += 2
                else:
                    s.append(src[i])
                    i += 1
            i += 1
            out.append(("str", "".join(s)))
        elif c.isalnum() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            out.append(("id", src[i:j]))
            i = j
        else:
            out.append(("p", c))
            i += 1
    return out


def header_keys(text):
    """[(key, kind, default, choices, live, reboot, help)] from SETTINGS_KEY_LIST"""
    toks, keys, i = tokens(macro_body(text, "SETTINGS_KEY_LIST")), [], 0
    while i < len(toks):
        if toks[i] != ("id", "X") or toks[i + 1] != ("p", "("):
            raise ValueError(f"unexpected {toks[i]!r} in SETTINGS_KEY_LIST")
        i += 2
        args, cur = [], []
        while toks[i] != ("p", ")"):
            if toks[i] == ("p", ","):
                args.append(cur)
                cur = []
            else:
                cur.append(toks[i])
            i += 1
        args.append(cur)
        i += 1

        def one_str(a):
            if not a or any(t[0] != "str" for t in a):
                raise ValueError(f"expected a string, got {a!r}")
            return "".join(t[1] for t in a)       # adjacent literals concatenate

        def one_id(a):
            if len(a) != 1 or a[0][0] != "id":
                raise ValueError(f"expected a name, got {a!r}")
            return a[0][1]

        if len(args) != 7:
            raise ValueError(f"X() with {len(args)} arguments: {args!r}")
        bools = {"true": True, "false": False}
        kind, live, reboot = one_id(args[1]), one_id(args[4]), one_id(args[5])
        if kind not in KINDS or live not in bools or reboot not in bools:
            raise ValueError(f"bad kind or flag in {args!r}")
        keys.append((one_str(args[0]), KINDS[kind], one_str(args[2]), one_str(args[3]), bools[live], bools[reboot],
                     one_str(args[6])))
    return keys


def header_template(text):
    toks = tokens(macro_body(text, "SETTINGS_TEMPLATE_TEXT"))
    if not toks or any(t[0] != "str" for t in toks):
        raise ValueError("SETTINGS_TEMPLATE_TEXT is not only string literals")
    return "".join(t[1] for t in toks)


def load_module():
    spec = importlib.util.spec_from_file_location("sticktime_settings_check", MODULE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def problems():
    """[str]: what differs, empty when settings.py matches settings_keys.h"""
    with open(HEADER, encoding="utf-8") as f:
        text = f.read()
    mod = load_module()
    hk, out = header_keys(text), []
    pk = [tuple(k) for k in mod.KEYS]
    fields = ("key", "kind", "default", "choices", "live", "reboot", "help")
    for i in range(max(len(hk), len(pk))):
        h = hk[i] if i < len(hk) else None
        p = pk[i] if i < len(pk) else None
        if h == p:
            continue
        if h is None or p is None:
            out.append(f"key #{i + 1}: header {h!r}, settings.py {p!r}")
            continue
        for f, a, b in zip(fields, h, p):
            if a != b:
                out.append(f"key #{i + 1} ({h[0]}): {f} is {a!r} in the header, {b!r} in settings.py")
    ht, pt = header_template(text), mod.TEMPLATE
    if ht != pt:
        hl, pl = ht.split("\n"), pt.split("\n")
        for n, (a, b) in enumerate(zip(hl, pl), 1):
            if a != b:
                out.append(f"template line {n}: header {a!r}, settings.py {b!r}")
                break
        else:
            out.append(f"template: header has {len(hl)} lines, settings.py {len(pl)}")
    return out


def main():
    p = problems()
    for line in p:
        print(line, file=sys.stderr)
    if not p:
        print(f"settings.py matches settings_keys.h ({len(load_module().KEYS)} keys and the template)")
    return 1 if p else 0


if __name__ == "__main__":
    sys.exit(main())
