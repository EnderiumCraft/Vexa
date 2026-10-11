#!/usr/bin/env python3
"""Generates libc's declarations for Vexa from libvexa's C headers.

    gen-vexa-libc.py libvexa/include OUT_DIR

writes three Rust files into OUT_DIR (src/unix/vexa in libc's source tree):

  consts.rs   the integer #defines (octal, hex and decimal), as `pub const`
  types.rs    the typedefs and structs (repr(C)), as libvexa lays them out
  fns.rs      the extern "C" prototypes

The hand-written parts (the pthread, semaphore and stat layouts that libc needs
in a particular shape, and the names libc defines itself) are in
src/unix/vexa/mod.rs and are skipped here. Anything the generator cannot read
is reported on stderr and left out, so the output is never a guess.
"""
import os
import re
import sys

SKIP_DIRS = {"c++", "GL", "KHR", "SDL2", "mbedtls", "psa", "vexa"}

# Names declared by hand in src/unix/vexa/mod.rs. Never emitted again.
HAND = {
    "time_t", "suseconds_t", "clock_t", "clockid_t", "off_t", "mode_t", "dev_t",
    "ino_t", "nlink_t", "blksize_t", "blkcnt_t", "fsblkcnt_t", "fsfilcnt_t", "nfds_t",
    "rlim_t", "sigset_t", "socklen_t", "sa_family_t", "wchar_t", "speed_t", "tcflag_t",
    "cc_t", "sem_t", "pthread_t", "pthread_key_t", "pthread_once_t", "pthread_spinlock_t",
    "pthread_attr_t", "pthread_mutex_t", "pthread_mutexattr_t", "pthread_cond_t",
    "pthread_condattr_t", "pthread_rwlock_t", "pthread_rwlockattr_t", "pthread_barrier_t",
    "pthread_barrierattr_t", "stat", "dirent", "passwd", "tm", "lconv", "sockaddr",
    "sockaddr_storage", "addrinfo", "fd_set", "sigaction", "termios", "Dl_info", "statvfs",
}

C_BASE = {
    "void": "c_void", "char": "c_char", "signed char": "c_schar", "unsigned char": "c_uchar",
    "short": "c_short", "short int": "c_short", "unsigned short": "c_ushort",
    "unsigned short int": "c_ushort", "int": "c_int", "signed": "c_int", "signed int": "c_int",
    "unsigned": "c_uint", "unsigned int": "c_uint", "long": "c_long", "long int": "c_long",
    "signed long": "c_long", "unsigned long": "c_ulong", "unsigned long int": "c_ulong",
    "long long": "c_longlong", "unsigned long long": "c_ulonglong", "double": "c_double",
    "float": "c_float",
}

RUST_KEYWORDS = {"type", "fn", "ref", "match", "loop", "move", "self", "in", "as", "use",
                 "mod", "impl", "where", "let", "mut", "const", "static", "struct", "enum",
                 "trait", "pub", "return", "crate", "super", "box", "dyn", "else", "if",
                 "while", "for", "break", "continue", "unsafe", "extern", "true", "false"}

NAME_KEYWORDS = {"int", "char", "long", "short", "unsigned", "signed", "void", "double",
                 "float", "const", "volatile", "struct", "union", "enum"}


def warn(msg):
    sys.stderr.write("gen-vexa-libc: " + msg + "\n")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def read_headers(root):
    """All the headers libc needs, with C++ wrappers and #defines removed."""
    texts = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            if not name.endswith(".h"):
                continue
            path = os.path.join(dirpath, name)
            lines = []
            skipping = 0
            for line in open(path, errors="replace"):
                s = line.strip()
                if s.startswith("#"):
                    if re.match(r"#\s*if(def)?\s+__cplusplus|#\s*if\s+defined\s*\(\s*__cplusplus", s):
                        skipping += 1
                        continue
                    if skipping and re.match(r"#\s*(if|ifdef|ifndef)", s):
                        skipping += 1
                    elif skipping and re.match(r"#\s*endif", s):
                        skipping -= 1
                    continue
                if skipping:
                    continue
                if 'extern "C"' in s:
                    continue
                lines.append(line)
            texts.append(strip_comments("".join(lines)))
    return "\n".join(texts)


def parse_statements(text):
    """Yields ('stmt', text), ('struct', head, body, trailing) for the top level."""
    i, n = 0, len(text)
    buf = ""
    while i < n:
        c = text[i]
        if c == "{":
            depth, j = 0, i
            while j < n:
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            body = text[i + 1:j]
            head = buf.strip()
            buf = ""
            k = text.find(";", j)
            trailing = text[j + 1:k] if k != -1 else text[j + 1:]
            if "(" in head or head == "":
                # A function body (or an unnamed block): nothing to declare.
                i = j + 1
                if text[i:i + 1] == ";":
                    i += 1
                continue
            yield ("struct", head, body, trailing.strip())
            i = (k + 1) if k != -1 else n
            continue
        if c == ";":
            yield ("stmt", buf.strip())
            buf = ""
        else:
            buf += c
        i += 1


def base_type(t):
    t = re.sub(r"\b(const|volatile|restrict|register|static|extern|inline)\b", " ", t)
    t = re.sub(r"\s+", " ", t).strip()
    if t.startswith("struct "):
        return t[len("struct "):].strip()
    if t in C_BASE:
        return C_BASE[t]
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", t):
        return t
    raise ValueError("unknown type " + repr(t))


def map_type(t):
    t = t.strip()
    if t.endswith("*"):
        inner = t[:-1].strip()
        pointee_const = bool(re.search(r"\bconst\b", inner))
        inner_clean = re.sub(r"\bconst\b", " ", inner)
        return ("*const " if pointee_const else "*mut ") + map_type(inner_clean)
    return base_type(t)


def fix_name(name):
    return name + "_" if name in RUST_KEYWORDS else name


def eval_dim(dim, consts):
    dim = dim.strip()
    if re.fullmatch(r"\d+", dim):
        return int(dim)
    if dim in consts:
        return consts[dim]
    raise ValueError("array size " + dim)


def parse_struct(head, body, trailing, consts):
    m = re.fullmatch(r"(?:typedef\s+)?struct\s*([A-Za-z_]\w*)?", head)
    if not m:
        raise ValueError("struct head " + repr(head))
    name = m.group(1) or trailing
    if not name:
        raise ValueError("anonymous struct")
    fields = []
    for decl in re.split(r";", body):
        decl = decl.strip()
        if not decl:
            continue
        if ":" in decl or "(" in decl or "{" in decl:
            raise ValueError("unsupported field " + repr(decl))
        parts = decl.split(",")
        first = parts[0].strip()
        mm = re.match(r"^(.*?)(\**)\s*([A-Za-z_]\w*)\s*((?:\[[^\]]*\])*)$", first)
        if not mm:
            raise ValueError("field " + repr(first))
        base, stars, fname, dims = mm.group(1), mm.group(2), mm.group(3), mm.group(4)
        decls = [(base, stars, fname, dims)]
        for extra in parts[1:]:
            em = re.match(r"^\s*(\**)\s*([A-Za-z_]\w*)\s*((?:\[[^\]]*\])*)\s*$", extra)
            if not em:
                raise ValueError("field " + repr(extra))
            decls.append((base, em.group(1), em.group(2), em.group(3)))
        for base, stars, fname, dims in decls:
            ty = map_type(base + " " + stars)
            for dim in re.findall(r"\[([^\]]*)\]", dims)[::-1]:
                ty = "[%s; %d]" % (ty, eval_dim(dim, consts))
            fields.append("    pub %s: %s," % (fix_name(fname), ty))
    return name, "#[repr(C)]\npub struct %s {\n%s\n}\n" % (name, "\n".join(fields))


def parse_function(stmt):
    stmt = re.sub(r"__attribute__\s*\(\([^()]*\)\)", " ", stmt).strip()
    stmt = re.sub(r"^extern\s+", "", stmt).strip()
    noreturn = "noreturn" in stmt
    m = re.match(r"^(.*?)([A-Za-z_]\w*)\s*\((.*)\)$", stmt, flags=re.S)
    if not m:
        return None
    ret, name, args = m.group(1).strip(), m.group(2), m.group(3).strip()
    if "(" in args or ")" in args or re.search(r"\b(typedef|struct\s*\{)", args):
        return None
    if re.search(r"\b(typedef|static)\b", ret) or not ret:
        return None
    if re.search(r"\binline\b", ret):
        return None
    rust_args = []
    if args and args != "void":
        for i, arg in enumerate(args.split(",")):
            arg = arg.strip()
            if arg == "...":
                rust_args.append("...")
                continue
            arg = re.sub(r"([A-Za-z_]\w*)\s*\[\s*\]", r"*\1", arg)
            am = re.match(r"^(.*?)([A-Za-z_]\w*)$", arg)
            if arg.endswith("*") or not am or am.group(2) in NAME_KEYWORDS or not am.group(1).strip() and am.group(2) not in C_BASE:
                ty = map_type(arg)
                rust_args.append("a%d: %s" % (i, ty))
            else:
                ty = map_type(am.group(1))
                rust_args.append("%s: %s" % (fix_name(am.group(2)), ty))
    if noreturn:
        rty = "!"
    elif ret == "void":
        rty = None
    else:
        rty = map_type(ret)
    sig = "pub fn %s(%s)" % (name, ", ".join(rust_args))
    if rty:
        sig += " -> " + rty
    return name, sig + ";"


def main():
    root, outdir = sys.argv[1], sys.argv[2]
    text = read_headers(root)

    consts = {}
    const_lines = []
    raw = open_all_defines(root)
    for name, val in raw:
        try:
            if re.fullmatch(r"-?0[0-7]+", val):
                n = int(val, 8)
            else:
                n = int(val, 0)
        except ValueError:
            continue
        if name in consts:
            continue
        consts[name] = n
        ty = "c_int" if -2**31 <= n < 2**31 else "c_ulong"
        const_lines.append("pub const %s: %s = %d;" % (name, ty, n))

    types, fns = [], []
    seen_types = set()
    for item in parse_statements(text):
        if item[0] == "struct":
            _, head, body, trailing = item
            try:
                name, code = parse_struct(head, body, trailing, consts)
            except ValueError as e:
                warn("skipped struct %s: %s" % (head or trailing, e))
                continue
            if name in HAND or name in seen_types:
                continue
            seen_types.add(name)
            types.append(code)
            continue
        stmt = item[1]
        if not stmt or stmt.startswith("#"):
            continue
        if stmt.startswith("typedef"):
            tm = re.match(r"^typedef\s+(.*?)\s+([A-Za-z_]\w*)$", stmt, flags=re.S)
            if not tm or "(" in stmt:
                continue
            name = tm.group(2)
            if name in HAND or name in seen_types:
                continue
            try:
                ty = map_type(tm.group(1))
            except ValueError as e:
                warn("skipped typedef %s: %s" % (name, e))
                continue
            seen_types.add(name)
            types.append("pub type %s = %s;" % (name, ty))
            continue
        if re.match(r"^(enum|union)\b", stmt):
            continue
        try:
            fp = parse_function(stmt)
        except ValueError as e:
            warn("skipped function: %s (%s)" % (stmt[:60].replace("\n", " "), e))
            continue
        if fp:
            name, decl = fp
            if name in HAND or name in consts:
                continue
            fns.append(decl)
        elif "(" in stmt and not stmt.startswith("extern \"C\""):
            warn("skipped declaration: %s" % stmt[:80].replace("\n", " "))

    header = ("// Generated by tools/rust/gen-vexa-libc.py from libvexa's headers. Do not edit.\n\n"
              "use crate::prelude::*;\nuse super::*;\n\n")
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "consts.rs"), "w") as f:
        f.write(header + "\n".join(const_lines) + "\n")
    with open(os.path.join(outdir, "types.rs"), "w") as f:
        f.write(header + "\n".join(types) + "\n")
    with open(os.path.join(outdir, "fns.rs"), "w") as f:
        f.write(header + 'extern "C" {\n' + "\n".join("    " + d for d in fns) + "\n}\n")
    print("%d constants, %d types, %d functions" % (len(const_lines), len(types), len(fns)))


def open_all_defines(root):
    pat = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s+\(?(-?(?:0x[0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*))[uUlL]*\)?\s*(?:/\*.*)?$")
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            if not name.endswith(".h"):
                continue
            for line in open(os.path.join(dirpath, name), errors="replace"):
                m = pat.match(line)
                if m:
                    out.append((m.group(1), m.group(2)))
    return out


if __name__ == "__main__":
    main()
