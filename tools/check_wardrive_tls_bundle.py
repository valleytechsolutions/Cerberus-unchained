#!/usr/bin/env python3
"""Check the real SDK certificate verifier with synthetic cross-signed chains.

Requires the pinned ESP-IDF package, Zig, CMake, Ninja and Python cryptography.
This integration check uses no device, network credentials or upload service.
It builds the SDK's software cryptography on the host; hardware TLS is a
separate acceptance check.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
STUBS = ROOT / "tools/host_checks/tls_stubs"


def fixtures(output: Path) -> None:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    def name(common: str):
        return x509.Name([
            x509.NameAttribute(NameOID.COUNTRY_NAME, "ZZ"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "Cerberus test CA"),
            x509.NameAttribute(NameOID.COMMON_NAME, common),
        ])

    keys = [ec.derive_private_key(i + 1, ec.SECP384R1() if i == 1 else ec.SECP256R1())
            for i in range(4)]
    names = [name(label) for label in ("Old root", "Current root", "Intermediate", "wardrive.test")]

    def certificate(subject, issuer, *, first=2020, last=2045):
        builder = (x509.CertificateBuilder()
            .subject_name(names[subject]).issuer_name(names[issuer])
            .public_key(keys[subject].public_key())
            .serial_number(100 + subject * 10 + issuer)
            .not_valid_before(datetime(first, 1, 1, tzinfo=timezone.utc))
            .not_valid_after(datetime(last, 1, 1, tzinfo=timezone.utc))
            .add_extension(x509.BasicConstraints(ca=subject != 3, path_length=None), critical=True))
        if subject == 3:
            builder = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName("wardrive.test")]), critical=False)
        return builder.sign(keys[issuer], hashes.SHA384() if issuer == 1 else hashes.SHA256())

    root = certificate(1, 1)
    cross = certificate(1, 0)
    intermediate = certificate(2, 1)
    leaf = certificate(3, 2)
    chains = {
        "valid.pem": [leaf, intermediate, cross],
        "direct.pem": [leaf, intermediate],
        "expired.pem": [certificate(3, 2, last=2021), intermediate, cross],
        "future.pem": [certificate(3, 2, first=2040), intermediate, cross],
        "expired-intermediate.pem": [leaf, certificate(2, 1, last=2021), cross],
        "future-intermediate.pem": [leaf, certificate(2, 1, first=2040), cross],
        "untrusted.pem": [certificate(3, 0), certificate(0, 0)],
    }
    damaged = bytearray(leaf.public_bytes(serialization.Encoding.DER))
    damaged[-1] ^= 1
    chains["tampered.pem"] = [x509.load_der_x509_certificate(bytes(damaged)), intermediate, cross]
    for filename, chain in chains.items():
        (output / filename).write_bytes(b"".join(cert.public_bytes(serialization.Encoding.PEM) for cert in chain))
    subject = root.subject.public_bytes()
    key = root.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    # The bundle deliberately omits the old root that signed the final peer certificate.
    (output / "bundle.bin").write_bytes(struct.pack("<IHH", 4, len(subject), len(key)) + subject + key)


def run(command: list[str], log: Path, *, cwd: Path | None = None) -> None:
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    parser.add_argument("--zig", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--ninja", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/wardrive-tls-bundle-checks")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / "result.json"
    report.write_text('{"status": "running"}\n', encoding="utf-8")
    fixtures(output)
    sdk = args.idf_path.resolve() / "components/mbedtls"
    library = sdk / "mbedtls"
    build = output / "libraries"

    if os.name == "nt":
        # CMake needs an archive executable, while Zig exposes it as a subcommand.
        archive, ranlib = output / "zig-ar.cmd", output / "zig-ranlib.cmd"
        archive.write_text(f'@"{args.zig.resolve()}" ar %*\n', encoding="utf-8")
        ranlib.write_text(f'@"{args.zig.resolve()}" ar s %*\n', encoding="utf-8")
    else:
        archive, ranlib = output / "zig-ar", output / "zig-ranlib"
        import shlex
        for path, operation in ((archive, "ar"), (ranlib, "ar s")):
            path.write_text(f'#!/bin/sh\nexec {shlex.quote(str(args.zig.resolve()))} {operation} "$@"\n', encoding="utf-8")
            path.chmod(0o755)

    configure = [str(args.cmake), "-G", "Ninja", "-S", str(library), "-B", str(build),
        f"-DCMAKE_C_COMPILER={args.zig.resolve().as_posix()}", "-DCMAKE_C_COMPILER_ARG1=cc",
        f"-DCMAKE_AR={archive.as_posix()}", f"-DCMAKE_RANLIB={ranlib.as_posix()}",
        f"-DCMAKE_MAKE_PROGRAM={args.ninja.resolve().as_posix()}",
        f'-DCMAKE_C_FLAGS=-I"{STUBS.as_posix()}"', "-DCMAKE_BUILD_TYPE=Release",
        "-DENABLE_PROGRAMS=OFF", "-DENABLE_TESTING=OFF", "-DGEN_FILES=OFF",
        "-DDISABLE_PACKAGE_CONFIG_AND_INSTALL=ON", "-DMBEDTLS_FATAL_WARNINGS=OFF",
        "-DTF_PSA_CRYPTO_FATAL_WARNINGS=OFF",
        f"-DMBEDTLS_USER_CONFIG_FILE={(STUBS / 'mbedtls_user_config.h').as_posix()}",
        f"-DTF_PSA_CRYPTO_USER_CONFIG_FILE={(STUBS / 'crypto_user_config.h').as_posix()}"]
    run(configure, output / "configure.log")
    run([str(args.cmake), "--build", str(build), "--parallel", "4"], output / "build.log")

    includes = [STUBS, sdk / "esp_crt_bundle/include", library / "include",
                library / "tf-psa-crypto/include", library / "tf-psa-crypto/drivers/builtin/include"]
    common = [str(args.zig), "cc", "-std=gnu11", "-O1",
              "-DMBEDTLS_DECLARE_PRIVATE_IDENTIFIERS", "-DMBEDTLS_ALLOW_PRIVATE_ACCESS"]
    for path in includes:
        common += ["-I", str(path)]
    common += [f'-DMBEDTLS_USER_CONFIG_FILE="{(STUBS / "mbedtls_user_config.h").as_posix()}"',
               f'-DTF_PSA_CRYPTO_USER_CONFIG_FILE="{(STUBS / "crypto_user_config.h").as_posix()}"']
    library_paths = [build / path for path in (
        "library/libmbedtls.a", "library/libmbedx509.a",
        "tf-psa-crypto/core/libtfpsacrypto.a",
        "tf-psa-crypto/drivers/builtin/libbuiltin.a",
        "tf-psa-crypto/drivers/everest/libeverest.a",
        "tf-psa-crypto/drivers/p256-m/libp256m.a")]
    assert all(path.is_file() for path in library_paths), "Missing SDK library"
    libraries = [str(path) for path in library_paths]
    sources = {"legacy": sdk / "esp_crt_bundle/esp_crt_bundle.c",
               "corrected": ROOT / "third_party/esp_crt_bundle/esp_crt_bundle.c"}
    results = {}
    for variant, source in sources.items():
        flags = ["-DCHECK_LEGACY"] if variant == "legacy" else []
        obj = output / f"{variant}.o"
        run(common + flags + ["-Dcalloc=bundle_calloc", "-Dfree=bundle_free", "-c", str(source), "-o", str(obj)],
            output / f"{variant}-compile.log")
        executable = output / (variant + (".exe" if os.name == "nt" else ""))
        link = common + flags + [str(ROOT / "tools/host_checks/wardrive_tls_bundle_check.c"), str(obj),
            "-Wl,--start-group", *libraries, "-Wl,--end-group", "-o", str(executable)]
        if os.name == "nt":
            link += ["-lbcrypt", "-lws2_32"]
        run(link, output / f"{variant}-link.log")
        run([str(executable), "bundle.bin"], output / f"{variant}-run.log", cwd=output)
        results[variant] = {"source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                            "result": (output / f"{variant}-run.log").read_text(encoding="utf-8").strip()}
    results = {"status": "passed", "hardware_validated": False, "variants": results}
    report.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
