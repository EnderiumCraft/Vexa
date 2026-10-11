#!/usr/bin/env python3
"""Adds Vexa to the places in Rust's std that select per-OS code.

    std-vexa-glue.py LIBRARY_DIR

LIBRARY_DIR is the rust-src `library` directory (the one with std/ in it). Each edit
is a single exact replacement, and the script stops if the text it expects isn't
there exactly once, so a different toolchain fails loudly instead of being patched
in the wrong place. An edit whose new text is already present is skipped, so the
script can run again on a tree it has changed.

What each edit does, and why Vexa can take the Linux or generic Unix path:

  os/mod.rs, os/unix/mod.rs   os::linux is the module with the Unix struct layouts and
                              the MetadataExt and raw types; Vexa's libc gives the same
                              names, so the Linux module serves Vexa too.
  os/linux/mod.rs             process (pidfds, Linux's own process support) and net
                              (Linux's socket extensions) stay Linux-only.
  sys/fs/unix.rs              directory entries carry d_ino and d_name, read through
                              readdir (as Linux's do), not readdir_r. The lists that pick
                              that path get Vexa.
  sys/args/unix.rs            argc and argv are stored at startup (as on Linux), not
                              read on demand.
  sys/random/mod.rs           Vexa has no getrandom; /dev/urandom exists, so random
                              bytes come from it, as on the other targets without
                              getrandom.
  sys/pal/unix/os.rs          current_exe reads /proc/self/exe; Vexa has procfs.
  sys/thread/unix.rs          Vexa has no thread names, so setting one does nothing.
"""
import os
import sys

EDITS_BUILD = [
    # std/build.rs marks std "restricted" (unstable, for platforms it doesn't know) unless
    # the target OS is on its list. Vexa is on it: its libc is the one this build patches in.
    ("../build.rs",
     '        || target_os == "vexos"\n',
     '        || target_os == "vexos"\n        || target_os == "vexa"\n'),
]

# The directory-entry lists: each names "wasi" as its last OS, and Vexa goes next to it.
WASI_LIST_EDITS = [
    ('        target_os = "wasi",\n    ))]\n    fn next(&mut self) -> Option<io::Result<DirEntry>> {\n'
     '        use crate::sys::io::{errno, set_errno};\n'),
    ('        target_os = "wasi",\n    )))]\n    fn next(&mut self) -> Option<io::Result<DirEntry>> {\n'
     '        if self.end_of_stream {\n'),
    ('        target_os = "wasi",\n        target_vendor = "apple",\n    ))]\n    pub fn ino(&self) -> u64 {\n'),
    ('        target_os = "wasi",\n    )))]\n    fn name_cstr(&self) -> &CStr {\n'),
    ('        target_os = "wasi",\n    ))]\n    fn name_cstr(&self) -> &CStr {\n'),
]

EDITS = [
    ("os/mod.rs",
     '#[cfg(any(target_os = "linux", doc))]\npub mod linux;\n',
     '#[cfg(any(target_os = "linux", target_os = "vexa", doc))]\npub mod linux;\n'),
    ("os/unix/mod.rs",
     '    #[cfg(target_os = "linux")]\n    pub use crate::os::linux::*;\n',
     '    #[cfg(target_os = "linux")]\n    pub use crate::os::linux::*;\n'
     '    #[cfg(target_os = "vexa")]\n    pub use crate::os::linux::*;\n'),
    ("os/linux/mod.rs",
     'pub mod net;\n',
     '#[cfg(target_os = "linux")]\npub mod net;\n'),
    ("os/linux/mod.rs",
     'pub mod process;\n',
     '#[cfg(target_os = "linux")]\npub mod process;\n'),
    ("sys/fs/unix.rs",
     '    target_os = "wasi",\n))]\nstruct dirent64_min {\n',
     '    target_os = "wasi",\n    target_os = "vexa",\n))]\nstruct dirent64_min {\n'),
    ("sys/fs/unix.rs",
     '    target_os = "wasi",\n)))]\npub struct DirEntry {\n',
     '    target_os = "wasi",\n    target_os = "vexa",\n)))]\npub struct DirEntry {\n'),
    ("sys/fs/unix.rs",
     '    target_os = "wasi",\n))]\npub struct DirEntry {\n',
     '    target_os = "wasi",\n    target_os = "vexa",\n))]\npub struct DirEntry {\n'),
    ("sys/fs/unix.rs",
     '    target_os = "wasi",\n    all(target_os = "linux", target_env = "musl"),\n))]\nuse libc::readdir as readdir64;\n',
     '    target_os = "wasi",\n    target_os = "vexa",\n    all(target_os = "linux", target_env = "musl"),\n))]\nuse libc::readdir as readdir64;\n'),
    ("sys/fs/unix.rs",
     '    target_os = "wasi",\n)))]\nuse libc::readdir_r as readdir64_r;\n',
     '    target_os = "wasi",\n    target_os = "vexa",\n)))]\nuse libc::readdir_r as readdir64_r;\n'),
    ("sys/args/unix.rs",
     '    target_os = "nuttx",\n))]\nmod imp {\n',
     '    target_os = "nuttx",\n    target_os = "vexa",\n))]\nmod imp {\n'),
    ("sys/random/mod.rs",
     '        target_os = "nto",\n    ) => {\n        mod unix_legacy;\n',
     '        target_os = "nto",\n        target_os = "vexa",\n    ) => {\n        mod unix_legacy;\n'),
    ("sys/pal/unix/os.rs",
     '    target_os = "emscripten"\n))]\npub fn current_exe() -> io::Result<PathBuf> {\n',
     '    target_os = "emscripten",\n    target_os = "vexa"\n))]\npub fn current_exe() -> io::Result<PathBuf> {\n'),
    ("sys/thread/unix.rs",
     '#[cfg(any(\n    target_os = "linux",\n    target_os = "freebsd",\n    target_os = "dragonfly",\n'
     '    target_os = "nuttx",\n    target_os = "cygwin"\n))]\npub fn set_name(name: &CStr) {\n',
     '#[cfg(target_os = "vexa")]\npub fn set_name(_name: &CStr) {\n'
     '    // Vexa has no thread names: there is nothing to set.\n}\n\n'
     '#[cfg(any(\n    target_os = "linux",\n    target_os = "freebsd",\n    target_os = "dragonfly",\n'
     '    target_os = "nuttx",\n    target_os = "cygwin"\n))]\npub fn set_name(name: &CStr) {\n'),
]

# Each directory-entry edit: Vexa goes right after the "wasi" line, in the list that
# ends with the given text.
for old in WASI_LIST_EDITS:
    new = old.replace('        target_os = "wasi",\n', '        target_os = "wasi",\n        target_os = "vexa",\n', 1)
    EDITS.append(("sys/fs/unix.rs", old, new))


def main():
    library = sys.argv[1]
    for rel, old, new in EDITS + EDITS_BUILD:
        path = os.path.join(library, "std", "src", rel)
        text = open(path).read()
        if text.count(new) >= 1:
            # Checked first: some new texts contain their old text.
            print("already applied:", rel)
            continue
        count = text.count(old)
        if count != 1:
            sys.exit("std-vexa-glue: %s: expected the text once, found %d times" % (rel, count))
        open(path, "w").write(text.replace(old, new, 1))
        print("edited", rel)


if __name__ == "__main__":
    main()
