use std::fs;
use std::io::{ErrorKind, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;
use std::thread;

use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer, ServerName};
use rustls::{ClientConfig, ClientConnection, RootCertStore, ServerConfig, ServerConnection, StreamOwned};

const BUNDLE: &str = "/etc/ssl/certs/ca-certificates.crt";
const SITE: &str = "example.com";

fn client_config(roots: RootCertStore) -> Arc<ClientConfig> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let config = ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .expect("protocol versions")
        .with_root_certificates(roots)
        .with_no_client_auth();
    Arc::new(config)
}

// One TLS exchange on loopback: the server presents `chain` with `key`, and the client
// verifies it against `roots`. Ok: the server's message and the client's reply. Err: the
// client's handshake or I/O error.
fn loopback(
    chain: Vec<CertificateDer<'static>>,
    key: PrivatePkcs8KeyDer<'static>,
    roots: RootCertStore,
) -> Result<String, String> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let server_config = ServerConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .expect("protocol versions")
        .with_no_client_auth()
        .with_single_cert(chain, key.into())
        .expect("server certificate");

    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("local address");
    let server = thread::spawn(move || {
        let (tcp, _) = listener.accept().expect("accept");
        let conn = ServerConnection::new(Arc::new(server_config)).expect("server connection");
        let mut tls = StreamOwned::new(conn, tcp);
        let mut buf = [0u8; 16];
        match tls.read(&mut buf) {
            Ok(n) => {
                let received = String::from_utf8_lossy(&buf[..n]).into_owned();
                let _ = tls.write_all(b"pong");
                received
            }
            Err(_) => String::new(), // the client gave up on the handshake
        }
    });

    let tcp = TcpStream::connect(addr).expect("connect");
    let name = ServerName::try_from("localhost").expect("server name");
    let conn = ClientConnection::new(client_config(roots), name).expect("client connection");
    let mut tls = StreamOwned::new(conn, tcp);
    let exchange = tls.write_all(b"ping").and_then(|()| {
        let mut reply = [0u8; 16];
        let n = tls.read(&mut reply)?;
        Ok(String::from_utf8_lossy(&reply[..n]).into_owned())
    });
    drop(tls);
    let received = server.join().expect("server thread");
    exchange
        .map(|reply| format!("the server got {received:?}, the client got {reply:?}"))
        .map_err(|error| error.to_string())
}

// A CA and a leaf it signs, for the test: the CA's certificate joins the bundle's roots,
// and the leaf must verify through it.
fn test_ca_and_leaf() -> (CertificateDer<'static>, CertificateDer<'static>, PrivatePkcs8KeyDer<'static>) {
    let ca_key = rcgen::KeyPair::generate().expect("CA key");
    let mut ca_params = rcgen::CertificateParams::new(vec!["Vexa test CA".to_string()]).expect("CA params");
    ca_params.is_ca = rcgen::IsCa::Ca(rcgen::BasicConstraints::Unconstrained);
    ca_params.key_usages = vec![rcgen::KeyUsagePurpose::KeyCertSign, rcgen::KeyUsagePurpose::CrlSign];
    let ca_cert = ca_params.self_signed(&ca_key).expect("CA certificate");
    let issuer = rcgen::Issuer::from_params(&ca_params, &ca_key);

    let leaf_key = rcgen::KeyPair::generate().expect("leaf key");
    let leaf_params = rcgen::CertificateParams::new(vec!["localhost".to_string()]).expect("leaf params");
    let leaf_cert = leaf_params.signed_by(&leaf_key, &issuer).expect("leaf certificate");
    (
        CertificateDer::from(ca_cert.der().to_vec()),
        CertificateDer::from(leaf_cert.der().to_vec()),
        PrivatePkcs8KeyDer::from(leaf_key.serialize_der()),
    )
}

// The real site over HTTPS, verified against the bundle. Returns the status line.
fn fetch_site(roots: RootCertStore) -> Result<String, String> {
    let tcp = TcpStream::connect((SITE, 443)).map_err(|e| format!("connect: {e}"))?;
    let name = ServerName::try_from(SITE).map_err(|e| format!("server name: {e}"))?;
    let conn = ClientConnection::new(client_config(roots), name).map_err(|e| e.to_string())?;
    let mut tls = StreamOwned::new(conn, tcp);
    let request = format!("GET / HTTP/1.1\r\nHost: {SITE}\r\nConnection: close\r\n\r\n");
    tls.write_all(request.as_bytes()).map_err(|e| format!("send: {e}"))?;
    let mut response = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        match tls.read(&mut chunk) {
            Ok(0) => break,
            Ok(n) => response.extend_from_slice(&chunk[..n]),
            // A server that closes without a TLS close_notify ends the body this way.
            Err(e) if e.kind() == ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(format!("receive: {e}")),
        }
    }
    let text = String::from_utf8_lossy(&response);
    Ok(text.lines().next().unwrap_or("").to_string())
}

fn main() {
    let pem = fs::read(BUNDLE).expect("read the CA bundle");
    let mut roots = RootCertStore::empty();
    let (added, ignored) = roots.add_parsable_certificates(
        CertificateDer::pem_slice_iter(&pem).map(|cert| cert.expect("a certificate in the bundle")),
    );
    println!("step 1: {added} roots from {BUNDLE} ({ignored} not parsable)");

    // A self-signed server that is not in the bundle must be refused.
    let key_pair = rcgen::KeyPair::generate().expect("key pair");
    let params = rcgen::CertificateParams::new(vec!["localhost".to_string()]).expect("params");
    let cert = params.self_signed(&key_pair).expect("self-signed certificate");
    let untrusted = loopback(
        vec![CertificateDer::from(cert.der().to_vec())],
        PrivatePkcs8KeyDer::from(key_pair.serialize_der()),
        roots.clone(),
    );
    match untrusted {
        Ok(exchange) => panic!("a certificate outside the bundle was accepted: {exchange}"),
        Err(error) => println!("step 2: untrusted server rejected: {error}"),
    }

    // A leaf signed by a CA that is in the store must be accepted, through the store.
    let (ca, leaf, leaf_key) = test_ca_and_leaf();
    roots.add(ca).expect("the test CA");
    match loopback(vec![leaf], leaf_key, roots.clone()) {
        Ok(exchange) => println!("step 3: trusted server accepted: {exchange}"),
        Err(error) => panic!("a leaf signed by a CA in the store was rejected: {error}"),
    }

    match fetch_site(roots) {
        Ok(status) => println!("https {SITE}: {status}"),
        Err(error) => println!("https {SITE}: no response ({error})"),
    }
}
