#!/usr/bin/env python3
"""Writes the Linux items that Rust crates use, but libvexa does not have, for the Vexa libc.

  gen-vexa-libc-extra.py <libc src dir> <out file>

Constants are copied from libc's own Linux definitions (linux_like/), so their values and
types are libc's. Vexa has no call behind most of the functions and no record for the types
below, so the functions fail with ENOSYS (or return what their Linux counterpart returns for
no work, as malloc_usable_size does), and the types have Linux's layout. The file is included
into the Vexa module as `extra`, and is generated, not part of the libc patch."""
import re
import sys
from pathlib import Path

# Constants libvexa's headers do not define, that crates use. Looked up in this order.
CONSTANTS = [
    "AT_EACCESS", "ENOTBLK", "EADV", "EBADE", "EBADFD", "EBADMSG", "EBADRQC", "EBADR", "EBADSLT",
    "EBFONT", "ECHRNG", "ECOMM", "EDEADLOCK", "EDOTDOT", "EHOSTDOWN", "EHWPOISON", "EIDRM",
    "EISNAM", "EKEYEXPIRED", "EKEYREJECTED", "EKEYREVOKED", "EL2HLT", "EL2NSYNC", "EL3HLT",
    "EL3RST", "ELIBACC", "ELIBBAD", "ELIBEXEC", "ELIBMAX", "ELIBSCN", "ELNRNG", "EMEDIUMTYPE",
    "EMULTIHOP", "ENAVAIL", "ENETRESET", "ENOANO", "ENOBUFS", "ENOCSI", "ENODATA", "ENOKEY",
    "ENOLINK", "ENOMEDIUM", "ENOMSG", "ENONET", "ENOPKG", "ENOSR", "ENOSTR", "ENOTNAM",
    "ENOTRECOVERABLE", "ENOTUNIQ", "EOWNERDEAD", "EPFNOSUPPORT", "EPROTO", "EREMCHG",
    "EREMOTEIO", "EREMOTE", "ERESTART", "ERFKILL", "ESHUTDOWN", "ESOCKTNOSUPPORT", "ESRMNT",
    "ESTRPIPE", "ETIME", "ETOOMANYREFS", "EUCLEAN", "EUNATCH", "EUSERS", "EXFULL",
    "FALLOC_FL_COLLAPSE_RANGE", "FALLOC_FL_INSERT_RANGE", "FALLOC_FL_KEEP_SIZE",
    "FALLOC_FL_NO_HIDE_STALE", "FALLOC_FL_PUNCH_HOLE", "FALLOC_FL_UNSHARE_RANGE",
    "FALLOC_FL_ZERO_RANGE", "MADV_DONTNEED", "MADV_NORMAL", "MADV_RANDOM", "MADV_SEQUENTIAL",
    "MADV_WILLNEED", "MS_ASYNC", "MS_SYNC",
    "POSIX_FADV_DONTNEED", "POSIX_FADV_NOREUSE", "POSIX_FADV_NORMAL", "POSIX_FADV_RANDOM",
    "POSIX_FADV_SEQUENTIAL", "POSIX_FADV_WILLNEED", "ST_NOSUID", "ST_RDONLY", "UTIME_NOW",
]
# Where Linux's values live, in the order they are looked for.
SOURCES = ["linux/mod.rs", "mod.rs", "android/mod.rs", "emscripten/mod.rs"]

SPECIAL = {
    # EDEADLOCK is EDEADLK in Linux, and libvexa has no EDEADLK.
    "EDEADLOCK": "pub const EDEADLOCK: c_int = 35;",
    # libvexa's MAP_FAILED and MAP_ANON are macros, which the generator does not evaluate.
    "MAP_FAILED": "pub const MAP_FAILED: *mut c_void = !0 as *mut c_void;",
    "MAP_ANON": "pub const MAP_ANON: c_int = 0x20;",
}

# Functions: (signature, body). Errors set errno to ENOSYS and return -1.
ENOSYS_FNS = [
    "pub unsafe fn dup3(oldfd: c_int, newfd: c_int, flags: c_int) -> c_int",
    "pub unsafe fn faccessat(dirfd: c_int, pathname: *const c_char, mode: c_int, flags: c_int) -> c_int",
    "pub unsafe fn fdatasync(fd: c_int) -> c_int",
    "pub unsafe fn fstatfs(fd: c_int, buf: *mut statfs) -> c_int",
    "pub unsafe fn statfs(path: *const c_char, buf: *mut statfs) -> c_int",
    "pub unsafe fn madvise(addr: *mut c_void, len: size_t, advice: c_int) -> c_int",
    "pub unsafe fn msync(addr: *mut c_void, len: size_t, flags: c_int) -> c_int",
    "pub unsafe fn posix_fadvise(fd: c_int, offset: off_t, len: off_t, advise: c_int) -> c_int",
    "pub unsafe fn posix_fallocate(fd: c_int, offset: off_t, len: off_t) -> c_int",
    "pub unsafe fn preadv(fd: c_int, iov: *const iovec, iovcnt: c_int, offset: off_t) -> ssize_t",
    "pub unsafe fn pwritev(fd: c_int, iov: *const iovec, iovcnt: c_int, offset: off_t) -> ssize_t",
    "pub unsafe fn mknodat(dirfd: c_int, pathname: *const c_char, mode: mode_t, dev: dev_t) -> c_int",
]

TYPES = """
#[repr(C)]
#[derive(Copy, Clone)]
pub struct fsid_t {
    pub __val: [c_int; 2],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct statfs {
    pub f_type: c_long,
    pub f_bsize: c_long,
    pub f_blocks: fsblkcnt_t,
    pub f_bfree: fsblkcnt_t,
    pub f_bavail: fsblkcnt_t,
    pub f_files: fsfilcnt_t,
    pub f_ffree: fsfilcnt_t,
    pub f_fsid: fsid_t,
    pub f_namelen: c_long,
    pub f_frsize: c_long,
    pub f_flags: c_long,
    pub f_spare: [c_long; 4],
}
"""

FUNCTIONS = """
// Vexa has none of these calls yet: they fail with ENOSYS, as an unsupported call does.
unsafe fn enosys() {
    *__errno_location() = ENOSYS;
}

""" + "\n".join(
    "%s {\n    enosys();\n    -1\n}\n" % sig if "-> c_int" in sig or "-> ssize_t" in sig else "%s {\n    enosys();\n    -1\n}\n" % sig
    for sig in ENOSYS_FNS
) + """
pub unsafe fn malloc_usable_size(_ptr: *const c_void) -> size_t {
    0
}

pub unsafe fn seekdir(_dirp: *mut DIR, _loc: c_long) {
    enosys();
}

// The device numbers' layout is glibc's and Linux's, so these match the other platforms.
pub const fn major(dev: dev_t) -> c_uint {
    (((dev >> 32) & 0xffff_f000) | ((dev >> 8) & 0xfff)) as c_uint
}

pub const fn minor(dev: dev_t) -> c_uint {
    (((dev >> 12) & 0xffff_ff00) | (dev & 0xff)) as c_uint
}

pub const fn makedev(major: c_uint, minor: c_uint) -> dev_t {
    let (major, minor) = (major as dev_t, minor as dev_t);
    ((major & 0xffff_f000) << 32)
        | ((major & 0xfff) << 8)
        | ((minor & 0xffff_ff00) << 12)
        | (minor & 0xff)
}
"""


def linux_constant(src, name):
    if name in SPECIAL:
        return SPECIAL[name]
    for rel in SOURCES:
        path = src / "unix" / "linux_like" / rel
        if not path.exists():
            continue
        m = re.search(r"^pub const %s: ([A-Za-z0-9_]+) = (.+);$" % re.escape(name),
                      path.read_text(), re.M)
        if m:
            return "pub const %s: %s = %s;" % (name, m.group(1), m.group(2))
    raise SystemExit("gen-vexa-libc-extra: no Linux value for %s" % name)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    lines = ["//! Generated by tools/rust/gen-vexa-libc-extra.py: Linux items libvexa lacks.",
             "#![allow(dead_code)]",
             "use crate::prelude::*;",
             "use crate::DIR;",
             "use super::*;",
             ""]
    for name in CONSTANTS + ["MAP_FAILED", "MAP_ANON"]:
        lines.append(linux_constant(src, name))
    lines.append(TYPES)
    lines.append(FUNCTIONS)
    out.write_text("\n".join(lines) + "\n")
    print("gen-vexa-libc-extra: %d constants, %d functions" % (
        len(CONSTANTS) + 2, len(ENOSYS_FNS) + 3))


if __name__ == "__main__":
    main()
