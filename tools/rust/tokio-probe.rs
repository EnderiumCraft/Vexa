use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

#[tokio::main(flavor = "current_thread")]
async fn main() {
    println!("step 1: runtime");
    tokio::time::sleep(Duration::from_millis(10)).await;
    println!("step 2: slept");
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
    let addr = listener.local_addr().expect("local_addr");
    println!("step 3: listening on {addr}");
    let server = tokio::spawn(async move {
        let (mut stream, peer) = listener.accept().await.expect("accept");
        println!("accepted from {peer}");
        let mut buf = [0u8; 16];
        let n = stream.read(&mut buf).await.expect("read");
        String::from_utf8_lossy(&buf[..n]).into_owned()
    });
    let mut client = TcpStream::connect(addr).await.expect("connect");
    client.write_all(b"hello").await.expect("write");
    let got = server.await.expect("server task");
    println!("tokio got: {got}");
}
