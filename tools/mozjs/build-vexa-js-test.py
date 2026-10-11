#!/usr/bin/env python3
"""Builds and links SpiderMonkey's embedding test for x86_64-unknown-vexa.

  build-vexa-js-test.py MOZJS_WORKDIR

MOZJS_WORKDIR is the directory tools/mozjs/build-mozjs-vexa.sh made; its obj/ holds the
built library. The test (vexa-js-test.cpp) creates a runtime and a global, runs a
script that allocates enough to need a GC and calls String.prototype.normalize (which
goes through the Rust glue in glue/), and checks the result. It writes MOZJS_WORKDIR/
vexa-js-test, a static Vexa binary to run in the VM.

Needs: the vexa-std toolchain (tools/rust/build-std-vexa.sh), the SDK built
(build/sdk/vexa-sdk), zlib's library (build/zlib-native/libz.a), and cargo.
"""
import os
import shlex
import shutil
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.abspath(os.path.join(here, "..", ".."))
sdk_bin = os.path.join(repo, "build", "sdk", "vexa-sdk", "bin")
target = os.path.join(repo, "tools", "rust", "x86_64-unknown-vexa.json")
zlib = os.path.join(repo, "build", "zlib-native", "libz.a")
env = dict(os.environ, PATH=f"{sdk_bin}:{os.environ['PATH']}")


def run(cmd, what, cwd=None, extra_env=None):
    result = subprocess.run(cmd, cwd=cwd, env=dict(env, **(extra_env or {})),
                            capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-4000:])
        sys.exit(f"{what} failed (exit {result.returncode})")
    return result


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    work = os.path.abspath(sys.argv[1])
    obj = os.path.join(work, "obj")
    library = os.path.join(obj, "js", "src", "build", "libjs_static.a")
    for needed in (library, os.path.join(obj, "dist", "include", "jsapi.h"), zlib):
        if not os.path.exists(needed):
            sys.exit(f"missing {needed}: build the library and zlib first")

    print("== the Rust glue (normalizer and encoding), as a static archive for Vexa")
    glue = os.path.join(work, "glue")
    shutil.rmtree(glue, ignore_errors=True)
    shutil.copytree(os.path.join(here, "glue"), glue)
    run(["cargo", "+vexa-std", "build", "--release", "--target", target,
         "-Zbuild-std=core,alloc,std,panic_abort"], "the Rust glue", cwd=glue,
        extra_env={"RUSTC_BOOTSTRAP": "1", "CC_x86_64_unknown_vexa": "vexa-cc"})
    archive = os.path.join(glue, "target", "x86_64-unknown-vexa", "release",
                           "libvexa_js_glue.a")

    print("== the test program, with the library's own flags")
    acdefs = None
    with open(os.path.join(obj, "config", "autoconf.mk")) as f:
        for line in f:
            if line.startswith("ACDEFINES = "):
                acdefs = shlex.split(line[len("ACDEFINES = "):])
    if acdefs is None:
        sys.exit("no ACDEFINES in autoconf.mk")
    compile_flags = ["vexa-c++", "-std=gnu++20", "-O1", "-fno-rtti", "-fno-exceptions",
                     "-fno-sized-deallocation", "-fno-aligned-new", "-fno-math-errno",
                     "-fPIC", "-DMOZILLA_CLIENT",
                     "-include", os.path.join(obj, "js", "src", "js-confdefs.h"),
                     f"-I{os.path.join(obj, 'dist', 'include')}",
                     f"-I{os.path.join(obj, 'js', 'src')}",
                     f"-I{os.path.join(obj, 'dist', 'system_wrappers')}"] + acdefs
    test_object = os.path.join(work, "vexa-js-test.o")
    run(compile_flags + ["-c", os.path.join(here, "vexa-js-test.cpp"), "-o", test_object],
        "compiling the test")

    print("== link")
    output = os.path.join(work, "vexa-js-test")
    run(["vexa-c++", "-static", "-o", output, test_object,
         "-Wl,--start-group", library, archive, zlib, "-Wl,--end-group"], "linking")
    print(f"built {output}")


if __name__ == "__main__":
    main()
