use std::convert::Infallible;
use std::time::Duration;

use bytes::Bytes;
use http_body_util::{BodyExt, Empty, Full};
use hyper::body::Incoming;
use hyper::header::HOST;
use hyper::service::service_fn;
use hyper::{Request, Response};
use hyper_util::rt::TokioIo;
use tokio::net::{TcpListener, TcpStream};

async fn serve(request: Request<Incoming>) -> Result<Response<Full<Bytes>>, Infallible> {
    println!("server got {} {}", request.method(), request.uri().path());
    let body = format!("hello {} {}\n", request.method(), request.uri().path());
    Ok(Response::new(Full::new(Bytes::from(body))))
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    println!("step 1: runtime");
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
    let addr = listener.local_addr().expect("local address");
    println!("step 2: listening on {addr}");

    tokio::spawn(async move {
        loop {
            let (stream, peer) = listener.accept().await.expect("accept");
            println!("server accepted {peer}");
            tokio::spawn(async move {
                let io = TokioIo::new(stream);
                let served = hyper::server::conn::http1::Builder::new()
                    .serve_connection(io, service_fn(serve))
                    .await;
                if let Err(error) = served {
                    eprintln!("server connection: {error}");
                }
            });
        }
    });

    let stream = TcpStream::connect(addr).await.expect("connect");
    let (mut sender, connection) =
        hyper::client::conn::http1::handshake(TokioIo::new(stream)).await.expect("handshake");
    tokio::spawn(async move {
        if let Err(error) = connection.await {
            eprintln!("client connection: {error}");
        }
    });
    println!("step 3: connected");

    let request = Request::builder()
        .uri("/probe")
        .header(HOST, "localhost")
        .body(Empty::<Bytes>::new())
        .expect("request");
    let response = tokio::time::timeout(Duration::from_secs(5), sender.send_request(request))
        .await
        .expect("no response within 5 seconds")
        .expect("send request");
    println!("step 4: status {}", response.status());
    let body = response.into_body().collect().await.expect("read body").to_bytes();
    print!("hyper got: {}", String::from_utf8_lossy(&body));
}
