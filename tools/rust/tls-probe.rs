use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;
use std::thread;

use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer, ServerName};
use rustls::{ClientConfig, ClientConnection, RootCertStore, ServerConfig, ServerConnection, StreamOwned};

fn main() {
    // A self-signed certificate for "localhost", made here, so no key is kept in the repo.
    let key_pair = rcgen::KeyPair::generate().expect("key pair");
    let params = rcgen::CertificateParams::new(vec!["localhost".to_string()]).expect("params");
    let cert = params.self_signed(&key_pair).expect("self-signed certificate");
    let cert_der = CertificateDer::from(cert.der().to_vec());
    let key_der = PrivatePkcs8KeyDer::from(key_pair.serialize_der());
    println!("step 1: certificate made");

    // Both sides use ring, the crypto provider that builds for Vexa.
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let server_config = ServerConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()
        .expect("protocol versions")
        .with_no_client_auth()
        .with_single_cert(vec![cert_der.clone()], key_der.into())
        .expect("server certificate");
    let mut roots = RootCertStore::empty();
    roots.add(cert_der).expect("root certificate");
    let client_config = ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .expect("protocol versions")
        .with_root_certificates(roots)
        .with_no_client_auth();

    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("local address");
    println!("step 2: listening on {addr}");

    let server = thread::spawn(move || {
        let (tcp, peer) = listener.accept().expect("accept");
        println!("accepted from {peer}");
        let conn = ServerConnection::new(Arc::new(server_config)).expect("server connection");
        let mut tls = StreamOwned::new(conn, tcp);
        let mut buf = [0u8; 64];
        let n = tls.read(&mut buf).expect("server read");
        tls.write_all(b"pong").expect("server write");
        String::from_utf8_lossy(&buf[..n]).into_owned()
    });

    let tcp = TcpStream::connect(addr).expect("connect");
    let name = ServerName::try_from("localhost").expect("server name");
    let conn = ClientConnection::new(Arc::new(client_config), name).expect("client connection");
    let mut tls = StreamOwned::new(conn, tcp);
    tls.write_all(b"ping").expect("client write");
    let mut reply = [0u8; 64];
    let n = tls.read(&mut reply).expect("client read");
    println!("tls client got: {}", String::from_utf8_lossy(&reply[..n]));
    println!("tls server got: {}", server.join().expect("server thread"));
}
