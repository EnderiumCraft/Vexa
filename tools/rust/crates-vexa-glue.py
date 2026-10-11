#!/usr/bin/env python3
"""Adds Vexa to the crates Servo needs that select code per OS, and nothing else.

    crates-vexa-glue.py CRATES_DIR

CRATES_DIR holds copies of mio-1.2.4, getrandom-0.2.17 and getrandom-0.3.4 (the
directories cargo unpacks them into). Each edit is one exact replacement; the script
stops if the text it expects isn't there exactly once. An edit whose new text is
already present is skipped, so the script can run again on a tree it has changed.

What each edit does, and why Vexa can take the generic path:

  mio-1.2.4               Vexa has no epoll or kqueue. mio uses its poll(2) selector
                          (the one for the other Unixes) and the pipe waker. libvexa has
                          poll and pipe. The Poll and Registry raw-fd impls exist only
                          for selectors that have an fd, so Vexa is left out of them, as
                          the other poll(2) systems are. The libc requirement is lowered
                          to the 0.2.178 this build patches in: mio's selector uses only
                          names that version has.
  socket2-0.6.5           Its IovLen (the msg_iovlen type) is defined only for some OSes;
                          Vexa's is size_t, as on glibc, so it takes the usize branch.
  getrandom-0.2.17        Vexa has no getrandom(2). It has /dev/urandom, and the kernel
  getrandom-0.3.4         seeds it from RDRAND, so the crates read it, as they do on the
                          other systems in the use_file list. getrandom 0.3 declares its
                          libc dependency only for that list, so Vexa is added there too.
                          The errno accessor is __errno_location, as on Linux.
"""
import os
import sys

EDITS = [
    # The libc requirement: mio asks for 0.2.183, and the patched libc is 0.2.178.
    ("mio-1.2.4/Cargo.toml",
     '[target.\'cfg(any(unix, target_os = "hermit", target_os = "wasi"))\'.dependencies.libc]\nversion = "0.2.183"\n',
     '[target.\'cfg(any(unix, target_os = "hermit", target_os = "wasi"))\'.dependencies.libc]\nversion = "0.2.178"\n'),
    # Selector: poll(2), as for the other Unixes that have no epoll or kqueue.
    ("mio-1.2.4/src/sys/unix/mod.rs",
     '        target_os = "horizon"\n    ), path = "selector/poll.rs")]\n',
     '        target_os = "horizon",\n        target_os = "vexa",\n    ), path = "selector/poll.rs")]\n'),
    # Waker: a pipe, as for the other Unixes without eventfd.
    ("mio-1.2.4/src/sys/unix/mod.rs",
     '        target_os = "aix",\n        target_os = "haiku",\n        target_os = "hurd",\n',
     '        target_os = "aix",\n        target_os = "haiku",\n        target_os = "hurd",\n        target_os = "vexa",\n'),
    # The pipe module the waker uses: its cfg list is separate from the waker's.
    ("mio-1.2.4/src/sys/unix/mod.rs",
     '            target_os = "cygwin",\n        ),\n        not(target_os = "hermit"),\n',
     '            target_os = "cygwin",\n            target_os = "vexa",\n        ),\n        not(target_os = "hermit"),\n'),
    # Poll and Registry: the raw-fd impls, for the selectors with an fd (not poll(2)).
    ("mio-1.2.4/src/poll.rs",
     '        target_os = "horizon"\n    )),\n))]\nimpl AsRawFd for Poll {\n',
     '        target_os = "horizon",\n        target_os = "vexa"\n    )),\n))]\nimpl AsRawFd for Poll {\n'),
    ("mio-1.2.4/src/poll.rs",
     '        target_os = "horizon"\n    )),\n))]\nimpl AsFd for Registry {\n',
     '        target_os = "horizon",\n        target_os = "vexa"\n    )),\n))]\nimpl AsFd for Registry {\n'),
    ("mio-1.2.4/src/poll.rs",
     '        target_os = "horizon"\n    )),\n))]\nimpl AsRawFd for Registry {\n',
     '        target_os = "horizon",\n        target_os = "vexa"\n    )),\n))]\nimpl AsRawFd for Registry {\n'),
    # The fd imports are used only by those impls, so they follow them.
    ("mio-1.2.4/src/poll.rs",
     '        target_os = "horizon"\n    )),\n))]\nuse std::os::fd::{AsFd, AsRawFd, BorrowedFd, RawFd};\n',
     '        target_os = "horizon",\n        target_os = "vexa"\n    )),\n))]\nuse std::os::fd::{AsFd, AsRawFd, BorrowedFd, RawFd};\n'),
    # The pipe: libvexa has pipe and no pipe2, so the descriptors get their flags from
    # fcntl (F_SETFL, F_SETFD), as on the systems that have no pipe2 either. Without an
    # entry here mio makes no pipe at all and its descriptors stay -1.
    ("mio-1.2.4/src/sys/unix/pipe.rs",
     '        target_os = "nto",\n    ))]\n    unsafe {\n        // For platforms that don\'t have `pipe2(2)`',
     '        target_os = "nto",\n        target_os = "vexa",\n    ))]\n    unsafe {\n        // For platforms that don\'t have `pipe2(2)`'),
    # accept: libvexa has no accept4, so the accepted socket gets its flags from fcntl.
    # The explicit O_NONBLOCK is set too, since libvexa doesn't pass the listener's on.
    ("mio-1.2.4/src/sys/unix/tcp.rs",
     '        all(target_arch = "x86", target_os = "android"),\n    ))]\n    let stream = {\n        syscall!(accept(',
     '        all(target_arch = "x86", target_os = "android"),\n        target_os = "vexa",\n    ))]\n    let stream = {\n        syscall!(accept('),
    ("mio-1.2.4/src/sys/unix/tcp.rs",
     '                target_os = "wasi",\n            ))]\n            syscall!(fcntl(s.as_raw_fd(), libc::F_SETFL, libc::O_NONBLOCK))?;',
     '                target_os = "wasi",\n                target_os = "vexa",\n            ))]\n            syscall!(fcntl(s.as_raw_fd(), libc::F_SETFL, libc::O_NONBLOCK))?;'),
    # socket2: msg_iovlen is a size_t in libvexa's headers, as in glibc's, so IovLen is usize.
    ("socket2-0.6.5/src/sys/unix.rs",
     '    target_os = "android",\n))]\ntype IovLen = usize;\n',
     '    target_os = "android",\n    target_os = "vexa",\n))]\ntype IovLen = usize;\n'),
    # tokio: UnixStream::peer_cred calls the per-OS get_peer_cred, and only the listed OSes
    # have one. libvexa has no SO_PEERCRED, so Vexa's Unix sockets report it as unsupported.
    ("tokio-1.53.2/src/net/unix/ucred.rs",
     '#[cfg(any(\n    target_os = "linux",\n    target_os = "redox",\n    target_os = "android",\n    target_os = "openbsd",\n    target_os = "haiku",\n    target_os = "cygwin"\n))]\npub(crate) use self::impl_linux::get_peer_cred;\n',
     '#[cfg(target_os = "vexa")]\npub(crate) fn get_peer_cred(_sock: &crate::net::UnixStream) -> std::io::Result<UCred> {\n    Err(std::io::Error::new(\n        std::io::ErrorKind::Unsupported,\n        "Vexa\'s Unix sockets have no peer credentials",\n    ))\n}\n\n#[cfg(any(\n    target_os = "linux",\n    target_os = "redox",\n    target_os = "android",\n    target_os = "openbsd",\n    target_os = "haiku",\n    target_os = "cygwin"\n))]\npub(crate) use self::impl_linux::get_peer_cred;\n'),
    # getrandom 0.2: the use_file backend, which reads /dev/urandom.
    ("getrandom-0.2.17/src/lib.rs",
     '    if #[cfg(any(target_os = "haiku", target_os = "redox", target_os = "nto", target_os = "aix"))] {\n',
     '    if #[cfg(any(target_os = "haiku", target_os = "redox", target_os = "nto", target_os = "aix", target_os = "vexa"))] {\n'),
    # getrandom 0.2: errno is __errno_location, as on Linux.
    ("getrandom-0.2.17/src/util_libc.rs",
     '    } else if #[cfg(any(target_os = "linux", target_os = "emscripten", target_os = "hurd", target_os = "redox", target_os = "dragonfly"))] {\n',
     '    } else if #[cfg(any(target_os = "linux", target_os = "emscripten", target_os = "hurd", target_os = "redox", target_os = "dragonfly", target_os = "vexa"))] {\n'),
    # getrandom 0.3: the same use_file backend, and its libc dependency.
    ("getrandom-0.3.4/src/backends.rs",
     '        target_os = "haiku",\n        target_os = "redox",\n        target_os = "nto",\n        target_os = "aix",\n    ))] {\n        mod use_file;\n',
     '        target_os = "haiku",\n        target_os = "redox",\n        target_os = "nto",\n        target_os = "aix",\n        target_os = "vexa",\n    ))] {\n        mod use_file;\n'),
    ("getrandom-0.3.4/src/util_libc.rs",
     '    } else if #[cfg(any(target_os = "linux", target_os = "emscripten", target_os = "hurd", target_os = "redox", target_os = "dragonfly"))] {\n',
     '    } else if #[cfg(any(target_os = "linux", target_os = "emscripten", target_os = "hurd", target_os = "redox", target_os = "dragonfly", target_os = "vexa"))] {\n'),
    ("getrandom-0.3.4/Cargo.toml",
     'target_os = "aix"))\'.dependencies.libc]\n',
     'target_os = "aix", target_os = "vexa"))\'.dependencies.libc]\n'),
]


def main():
    crates = sys.argv[1]
    for rel, old, new in EDITS:
        path = os.path.join(crates, rel)
        text = open(path).read()
        if text.count(new) >= 1:
            print("already applied:", rel)
            continue
        count = text.count(old)
        if count != 1:
            sys.exit("crates-vexa-glue: %s: expected the text once, found %d times" % (rel, count))
        open(path, "w").write(text.replace(old, new, 1))
        print("edited", rel)


if __name__ == "__main__":
    main()
