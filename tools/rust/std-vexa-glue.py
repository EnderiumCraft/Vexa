#!/usr/bin/env python3
"""Adds Vexa to the places in Rust's std that select per-OS code.

    std-vexa-glue.py LIBRARY_DIR

LIBRARY_DIR is the rust-src `library` directory (the one with std/ in it). Each edit
is a single exact replacement, and the script stops if the text it expects isn't
there exactly once, so a different toolchain fails loudly instead of being patched
in the wrong place.

What each edit does, and why Vexa can take the Linux or generic Unix path:

  os/mod.rs, os/unix/mod.rs   os::linux is the module with the Unix struct layouts and
                              the MetadataExt and raw types; Vexa's libc gives the same
                              names, so the Linux module serves Vexa too.
  sys/fs/unix.rs              directory entries carry d_ino and d_type, as Linux's do.
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

EDITS = [
    ("os/mod.rs",
     '#[cfg(any(target_os = "linux", doc))]\npub mod linux;\n',
     '#[cfg(any(target_os = "linux", target_os = "vexa", doc))]\npub mod linux;\n'),
    ("os/unix/mod.rs",
     '    #[cfg(target_os = "linux")]\n    pub use crate::os::linux::*;\n',
     '    #[cfg(target_os = "linux")]\n    pub use crate::os::linux::*;\n'
     '    #[cfg(target_os = "vexa")]\n    pub use crate::os::linux::*;\n'),
    ("sys/fs/unix.rs",
     '    target_os = "wasi",\n))]\nstruct dirent64_min {\n',
     '    target_os = "wasi",\n    target_os = "vexa",\n))]\nstruct dirent64_min {\n'),
    ("sys/fs/unix.rs",
     '    target_os = "wasi",\n)))]\npub struct DirEntry {\n',
     '    target_os = "wasi",\n    target_os = "vexa",\n)))]\npub struct DirEntry {\n'),
    ("sys/fs/unix.rs",
     '    target_os = "wasi",\n))]\npub struct DirEntry {\n',
     '    target_os = "wasi",\n    target_os = "vexa",\n))]\npub struct DirEntry {\n'),
    ("os/linux/mod.rs",
     'pub mod process;\n',
     '#[cfg(target_os = "linux")]\npub mod process;\n'),
    ("os/mod.rs",
     '#[cfg(any(target_os = "linux", target_os = "android", target_os = "cygwin", doc))]\nmod net;\n',
     '#[cfg(any(target_os = "linux", target_os = "vexa", target_os = "android", target_os = "cygwin", doc))]\nmod net;\n'),
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


def main():
    library = sys.argv[1]
    for rel, old, new in EDITS:
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
