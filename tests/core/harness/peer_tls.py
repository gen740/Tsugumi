"""Independent Python/OpenSSL TLS clients for the black-box worker scenarios."""
import base64
import os
import ssl
import subprocess
from pathlib import Path


def identity(directory, name="client"):
    directory = Path(directory)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    key, cert = directory / (name + ".key"), directory / (name + ".crt")
    env = {**os.environ, "OPENSSL_CONF": "/dev/null"}
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ED25519", "-nodes", "-days", "3650",
                    "-subj", "/CN=Tsugumi test peer", "-keyout", str(key), "-out", str(cert)],
                   env=env, check=True, capture_output=True)
    pem = directory / (name + ".pem")
    pem.write_bytes(key.read_bytes() + cert.read_bytes())
    pem.chmod(0o600)
    key.unlink()
    cert.unlink()
    return pem


def public_key(pem):
    der = subprocess.check_output(["openssl", "pkey", "-in", str(pem), "-pubout", "-outform", "DER"],
                                  stderr=subprocess.DEVNULL)
    return "ed25519:" + base64.b64encode(der).decode("ascii")


def client_context(client_pem, worker_pem):
    # Python has no SPKI verification callback. This independent test client
    # trusts only the fixture's exact self-signed certificate, not a CA bundle.
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_REQUIRED
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.load_verify_locations(cafile=str(worker_pem))
    context.load_cert_chain(str(client_pem))
    return context
