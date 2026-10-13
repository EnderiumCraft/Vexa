#!/usr/bin/env python3
"""Runs Servo's HTTPS load test in the Vexa VM: servo-render loads a page over TLS from this
machine, at 10.0.2.2 (QEMU's user-mode NAT, the host as the guest sees it), by address, and by
the name vane.test, which the VM's /etc/hosts maps to 10.0.2.2 (the sandbox has no DNS; libvexa
resolves with /etc/hosts first, and then with the DNS server DHCP gave).
The server's certificate is made here, by a test CA. The VM first loads the page without that CA,
which must be refused, then with it: SSL_CERT_FILE points at a CA bundle that is the system's
bundle plus the test CA, and rustls reads it through rustls-native-certs.

  run-https-test.py WORKDIR

WORKDIR is the one tools/servo/build-servo-vexa.sh built, with servo-render in it. This makes
WORKDIR/https.img (a FAT disk with servo-render and the test CA bundle), makes the certificates in
WORKDIR/https/, serves tests/servo over TLS on port 8443, boots build/vexa.iso with the disk through
tools/qemu-smoke-test.py, and checks that the refused load prints "servo-render: title Certificate
error" and the trusted load prints "servo-render: title fonts ok". The VM's screen is kept in
WORKDIR/https-vm.png, and the server's log in WORKDIR/https/server.log. Needs openssl, mkfs.vfat
and mtools."""
import atexit
import importlib.util
import os
import subprocess
import sys

repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
work = os.path.abspath(sys.argv[1])
binary = os.path.join(work, "servo-render")
image = os.path.join(work, "https.img")
certs = os.path.join(work, "https")
system_bundle = os.path.join(repo, "tools", "linux-files", "ca-certificates.crt")
port = 8443
os.makedirs(certs, exist_ok=True)
os.chdir(repo)


def openssl(*args):
    subprocess.run(["openssl", *args], cwd=certs, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# A test CA, and a server certificate for the address the VM connects to.
openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem",
        "-days", "30", "-subj", "/CN=Vexa test CA",
        "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign")
openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", "server.key", "-out", "server.csr",
        "-subj", "/CN=10.0.2.2")
with open(os.path.join(certs, "server.ext"), "w") as ext:
    ext.write("subjectAltName=IP:10.0.2.2,DNS:vane.test\nbasicConstraints=CA:FALSE\n"
              "keyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
openssl("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
        "-CAcreateserial", "-out", "server.pem", "-days", "30", "-extfile", "server.ext")

# The CA bundle the VM trusts for this test: the system's roots, and the test CA.
bundle = os.path.join(certs, "ca-test.pem")
with open(bundle, "w") as out:
    for source in (system_bundle, os.path.join(certs, "ca.pem")):
        with open(source) as text:
            out.write(text.read())
            out.write("\n")

with open(os.path.join(certs, "serve.py"), "w") as script:
    script.write('''import functools, http.server, ssl, sys
port, directory, cert, key = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=directory)
server = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(cert, key)
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
''')

subprocess.run(["dd", "if=/dev/zero", "of=" + image, "bs=1M", "count=512"], check=True,
               stderr=subprocess.DEVNULL)
subprocess.run(["mkfs.vfat", "-F", "32", "-n", "HTTPS", image], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["mcopy", "-i", image, binary, "::/servo-render"], check=True)
subprocess.run(["mcopy", "-i", image, bundle, "::/ca-test.pem"], check=True)

server_log = open(os.path.join(certs, "server.log"), "w")
server = subprocess.Popen(
    [sys.executable, os.path.join(certs, "serve.py"), str(port),
     os.path.join(repo, "tests", "servo"), os.path.join(certs, "server.pem"),
     os.path.join(certs, "server.key")],
    stdout=server_log, stderr=subprocess.STDOUT)
atexit.register(server.terminate)

spec = importlib.util.spec_from_file_location(
    "smoke", os.path.join(repo, "tools", "qemu-smoke-test.py"))
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)
qemu_popen = smoke.subprocess.Popen


def popen_with_disk(cmd, *args, **kwargs):
    if isinstance(cmd, list) and cmd and cmd[0].startswith("qemu"):
        cmd = cmd + ["-drive", "file=%s,if=virtio,format=raw" % image]
    return qemu_popen(cmd, *args, **kwargs)


smoke.subprocess.Popen = popen_with_disk
smoke.TYPED_COMMANDS = [
    ("#shell",),
    ("ls -l /mnt/vda", "ca-test.pem", 20),
    # Without the test CA, the server's certificate must be refused.
    ("export RUST_LOG=net=warn", None, 5),
    (f"/mnt/vda/servo-render https://10.0.2.2:{port}/font-test.html /mnt/vda/https-refused.png",
     "servo-render: title Certificate error", 900),
    # With it, the same page loads, and its script runs.
    ("export SSL_CERT_FILE=/mnt/vda/ca-test.pem", None, 5),
    (f"/mnt/vda/servo-render https://10.0.2.2:{port}/font-test.html /mnt/vda/https.png",
     "servo-render: title fonts ok", 900),
    # The title comes before the frame is saved; this waits until the file is there.
    ("ls -l /mnt/vda", "https.png", 900),
    # By name: /etc/hosts maps vane.test to the host, the certificate has it as a name, and the
    # name is checked against it.
    ("echo 10.0.2.2 vane.test > /etc/hosts", None, 5),
    (f"/mnt/vda/servo-render https://vane.test:{port}/font-test.html /mnt/vda/https-name.png",
     "servo-render: title fonts ok", 900),
    ("ls -l /mnt/vda", "https-name.png", 900),
]
sys.argv = ["qemu-smoke-test.py", "--no-linux", "--only", "shell", "--keep-log",
            "--memory", "2G", "--screenshot", os.path.join(work, "https-vm.png")]
sys.exit(smoke.main())
