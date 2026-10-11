use mio::net::{TcpListener, TcpStream};
use mio::{Events, Interest, Poll, Token, Waker};
use std::io::{ErrorKind, Read, Write};
use std::time::{Duration, Instant};

const WAKER: Token = Token(0);
const LISTENER: Token = Token(1);
const CLIENT: Token = Token(2);
const SERVER: Token = Token(3);

fn main() {
    let mut poll = Poll::new().expect("poll");
    let waker = Waker::new(poll.registry(), WAKER).expect("waker");
    println!("step 1: waker");
    waker.wake().expect("wake");
    let mut events = Events::with_capacity(16);
    poll.poll(&mut events, Some(Duration::from_millis(100))).expect("poll wait");
    for event in events.iter() {
        println!("event token {}", event.token().0);
    }
    println!("step 2: waker polled");
    let mut buf = [0u8; 16];
    getrandom02::getrandom(&mut buf).expect("getrandom 0.2");
    getrandom03::fill(&mut buf).expect("getrandom 0.3");
    println!("random bytes ok");

    let mut listener = TcpListener::bind("127.0.0.1:0".parse().unwrap()).expect("bind");
    let addr = listener.local_addr().expect("local_addr");
    println!("step 3: listening on {addr}");
    poll.registry().register(&mut listener, LISTENER, Interest::READABLE).expect("register listener");
    let mut client = TcpStream::connect(addr).expect("connect");
    poll.registry().register(&mut client, CLIENT, Interest::READABLE | Interest::WRITABLE).expect("register client");
    println!("step 4: connecting");

    let mut server: Option<TcpStream> = None;
    let mut sent = false;
    let mut received = Vec::new();
    let deadline = Instant::now() + Duration::from_secs(5);
    while received.len() < 5 && Instant::now() < deadline {
        poll.poll(&mut events, Some(Duration::from_millis(100))).expect("poll tcp");
        for event in events.iter() {
            match event.token() {
                LISTENER => {
                    let (mut stream, peer) = listener.accept().expect("accept");
                    println!("accepted from {peer}");
                    poll.registry().register(&mut stream, SERVER, Interest::READABLE).expect("register server");
                    server = Some(stream);
                }
                CLIENT if event.is_writable() && !sent => {
                    client.write_all(b"hello").expect("write");
                    sent = true;
                    println!("step 5: client wrote");
                }
                SERVER => {
                    if let Some(stream) = server.as_mut() {
                        let mut chunk = [0u8; 16];
                        loop {
                            match stream.read(&mut chunk) {
                                Ok(0) => break,
                                Ok(n) => received.extend_from_slice(&chunk[..n]),
                                Err(e) if e.kind() == ErrorKind::WouldBlock => break,
                                Err(e) => panic!("server read: {e}"),
                            }
                        }
                    }
                }
                _ => {}
            }
        }
    }
    println!("tcp got: {}", String::from_utf8_lossy(&received));
}
