#!/usr/bin/env python3
"""Makes the standard root certificates (tools/linux-files/ca-certificates.crt)
from Mozilla's list, certdata.txt, as Debian's ca-certificates does: every
certificate Mozilla trusts to identify websites, in PEM.

Usage: tools/make-ca-bundle.py [certdata.txt | URL of a ca-certificates source
tarball] [output]

The default source is Ubuntu's ca-certificates package, which carries
Mozilla's certdata.txt; run this again (with a newer tarball) to update.
"""
import base64
import io
import sys
import tarfile
import urllib.request

DEFAULT_SOURCE = ("https://archive.ubuntu.com/ubuntu/pool/main/c/ca-certificates/"
                  "ca-certificates_20260601~24.04.1.tar.xz")
DEFAULT_OUTPUT = "tools/linux-files/ca-certificates.crt"


def read_certdata(source):
    if not source.startswith("https://"):
        with open(source, encoding="utf-8") as f:
            return f.read()
    with urllib.request.urlopen(source) as response:
        data = response.read()
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for member in tar.getmembers():
            if member.name.endswith("mozilla/certdata.txt"):
                return tar.extractfile(member).read().decode("utf-8")
    sys.exit("make-ca-bundle: no mozilla/certdata.txt in " + source)


def parse(text):
    """certdata.txt's objects, each a dict of attribute -> value."""
    objects, current = [], None
    lines = iter(text.splitlines())
    for line in lines:
        if line.startswith("#") or not line.strip():
            continue
        if line.startswith("CKA_CLASS"):
            current = {}
            objects.append(current)
        if current is None:
            continue
        words = line.split(None, 2)
        if len(words) < 2:
            continue
        name, kind = words[0], words[1]
        if kind == "MULTILINE_OCTAL":
            octets = bytearray()
            for line in lines:
                if line.strip() == "END":
                    break
                for number in line.split("\\")[1:]:
                    octets.append(int(number, 8))
            current[name] = bytes(octets)
        elif kind == "UTF8":
            current[name] = words[2].strip('"') if len(words) > 2 else ""
        else:
            current[name] = words[2] if len(words) > 2 else kind
    return objects


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SOURCE
    output = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUTPUT
    objects = parse(read_certdata(source))
    trusted = set()
    for o in objects:
        if (o.get("CKA_CLASS") == "CKO_NSS_TRUST" and
                o.get("CKA_TRUST_SERVER_AUTH") == "CKT_NSS_TRUSTED_DELEGATOR"):
            trusted.add((o.get("CKA_ISSUER"), o.get("CKA_SERIAL_NUMBER")))
    pems = []
    for o in objects:
        if o.get("CKA_CLASS") != "CKO_CERTIFICATE":
            continue
        if (o.get("CKA_ISSUER"), o.get("CKA_SERIAL_NUMBER")) not in trusted:
            continue
        body = base64.b64encode(o["CKA_VALUE"]).decode("ascii")
        lines = [body[i:i + 64] for i in range(0, len(body), 64)]
        pems.append("# %s\n-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n"
                    % (o.get("CKA_LABEL", "?"), "\n".join(lines)))
    if len(pems) < 100:
        sys.exit("make-ca-bundle: only %d certificates; is that certdata.txt?" % len(pems))
    with open(output, "w", encoding="utf-8") as f:
        f.write("# The root certificates Mozilla trusts to identify websites\n"
                "# (from certdata.txt; made by tools/make-ca-bundle.py).\n\n")
        f.write("\n".join(pems))
    print("make-ca-bundle: %d certificates in %s" % (len(pems), output))


if __name__ == "__main__":
    main()
