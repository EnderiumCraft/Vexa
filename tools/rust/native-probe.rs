use std::ffi::{c_char, c_int, c_short, c_ulong, c_void, CStr, CString};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::time::{Duration, Instant};

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: c_short,
    revents: c_short,
}

extern "C" {
    fn poll(fds: *mut PollFd, count: c_ulong, timeout_ms: c_int) -> c_int;
    fn dlopen(file: *const c_char, flags: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, name: *const c_char) -> *mut c_void;
    fn dlerror() -> *mut c_char;
    fn dlclose(handle: *mut c_void) -> c_int;
}

const RTLD_NOW: c_int = 2;
const POLLIN: c_short = 1;

// poll with more descriptors than the old 64-handle limit.
fn poll_many() {
    let mut pairs: Vec<(UnixStream, UnixStream)> = (0..60)
        .map(|_| UnixStream::pair().expect("socket pair"))
        .collect();
    println!("step 1: {} descriptors", pairs.len() * 2);

    let mut fds: Vec<PollFd> = pairs
        .iter()
        .flat_map(|(a, b)| [a.as_raw_fd(), b.as_raw_fd()])
        .map(|fd| PollFd { fd, events: POLLIN, revents: 0 })
        .collect();
    let ready = unsafe { poll(fds.as_mut_ptr(), fds.len() as c_ulong, 50) };
    println!("poll over {} descriptors, no data: {ready} ready", fds.len());
    assert_eq!(ready, 0, "nothing was written, so nothing is ready");

    pairs[59].1.write_all(b"x").expect("write");
    for fd in fds.iter_mut() {
        fd.revents = 0;
    }
    let ready = unsafe { poll(fds.as_mut_ptr(), fds.len() as c_ulong, 1000) };
    let hit: Vec<i32> = fds.iter().filter(|fd| fd.revents & POLLIN != 0).map(|fd| fd.fd).collect();
    println!("poll after a write: {ready} ready, the readable descriptor is {}", hit.len() == 1 && hit[0] == pairs[59].0.as_raw_fd());
    assert!(ready == 1 && hit == [pairs[59].0.as_raw_fd()], "the written-to end is readable");
}

// dlopen and dlsym against libvexa.so, which every program has, and two failures.
fn load_libraries() {
    let name = CString::new("libvexa.so").unwrap();
    let handle = unsafe { dlopen(name.as_ptr(), RTLD_NOW) };
    assert!(!handle.is_null(), "dlopen libvexa.so");
    let strlen_name = CString::new("strlen").unwrap();
    let strlen = unsafe { dlsym(handle, strlen_name.as_ptr()) };
    assert!(!strlen.is_null(), "dlsym strlen");
    let strlen: extern "C" fn(*const c_char) -> usize = unsafe { std::mem::transmute(strlen) };
    let hello = CString::new("hello").unwrap();
    println!("dlopen libvexa.so, dlsym strlen: strlen(\"hello\") = {}", strlen(hello.as_ptr()));

    let missing_name = CString::new("libnosuchthing.so").unwrap();
    let missing = unsafe { dlopen(missing_name.as_ptr(), RTLD_NOW) };
    assert!(missing.is_null(), "a missing library must not load");
    let message = unsafe { CStr::from_ptr(dlerror()) }.to_string_lossy().into_owned();
    println!("dlopen of a missing library: null, dlerror: {message}");

    let absent = CString::new("no_such_symbol").unwrap();
    let none = unsafe { dlsym(handle, absent.as_ptr()) };
    println!("dlsym of a missing symbol: {}", if none.is_null() { "null" } else { "not null" });
    assert!(none.is_null());
    unsafe { dlclose(handle) };
}

// A receive timeout on a socket: a read with no data gives up, and the option reads back.
fn read_timeout() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("address");
    let mut client = TcpStream::connect(addr).expect("connect");
    let (_server, _) = listener.accept().expect("accept");

    client.set_read_timeout(Some(Duration::from_millis(200))).expect("set read timeout");
    let read_back = client.read_timeout().expect("read timeout");
    let started = Instant::now();
    let mut buf = [0u8; 16];
    let result = client.read(&mut buf);
    let waited = started.elapsed();
    let kind = result.as_ref().err().map(|error| error.kind());
    println!(
        "read timeout 200 ms: read_timeout() = {read_back:?}, the read gave up after {} ms with {kind:?}",
        waited.as_millis()
    );
    assert!(kind.is_some(), "a read with no data must not succeed");
    assert!(waited >= Duration::from_millis(150), "the read returned before its timeout");
    assert_eq!(read_back, Some(Duration::from_millis(200)));
}

fn main() {
    poll_many();
    load_libraries();
    read_timeout();
    println!("native probe: all checks passed");
}
