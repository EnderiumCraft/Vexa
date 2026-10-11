fn main() {
    // Vexa: libc++ needs libc++abi (operator new/delete, the guards), and the C library
    // holds its unwind stubs. Rust emits native libs in its own order, which puts libc++abi
    // before libc++, so the three go last as one group that resolves them against each other.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("vexa") {
        println!("cargo:rustc-link-arg-bins=-Wl,--start-group");
        println!("cargo:rustc-link-arg-bins=-lc++");
        println!("cargo:rustc-link-arg-bins=-lc++abi");
        println!("cargo:rustc-link-arg-bins=-lc");
        println!("cargo:rustc-link-arg-bins=-Wl,--end-group");
    }
}
