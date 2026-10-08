#!/usr/bin/env python3
"""Reproduce the README's image, bytecode and optional release-ZIP checks.

Run from any directory in this clone. Dependencies: c2pa-python==0.38.0,
Pillow==12.3.0, numpy==2.5.3. Downloads pinned public C2PA trust lists.
Reads source at REVIEW_COMMIT; writes only to a temporary directory and stdout.
An optional positional argument supplies the already-downloaded v2.1 ZIP.
"""

import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

import c2pa


ROOT = Path(__file__).resolve().parents[1]
REVIEW_COMMIT = "c8142cef3cf7bc28906c947fab53d58385ef823a"
TRUST_REVISION = "43a0a6f09091a062083a4ba33e3acc19dd721282"
TRUST_BASE = (
    "https://raw.githubusercontent.com/c2pa-org/conformance-public/"
    + TRUST_REVISION + "/trust-list/"
)
TRUST_FILES = (
    ("manifest", "C2PA-TRUST-LIST.pem",
     "75cacc98b79ecac33713c7ecfb58d4a0ef383f3c1f886e7409f9e37e8664aea5"),
    ("tsa", "C2PA-TSA-TRUST-LIST.pem",
     "c688d3555f4a2f1f8d663472bbd37888ff234abdd234c25934c0f9292e4eb5c9"),
)
ZIP_HASH = "c7fe539acb1e865b65767532e2ccf56971cf79c94fbff8b0fa4456a04d8467da"
ZIP_PREFIX = "PicoOS-Pro-v2.1-doom-package/PicoOS-Pro-v2.1/"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def source(path):
    return git("show", REVIEW_COMMIT + ":" + path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", nargs="?", type=Path)
    args = parser.parse_args()
    report = {
        "review_commit": REVIEW_COMMIT,
        "packages": {name: importlib.metadata.version(name) for name in
                     ("c2pa-python", "pillow", "numpy")},
        "c2pa_sdk_version": c2pa.sdk_version(),
        "trust_sources": [],
    }
    anchors = []
    for kind, filename, expected in TRUST_FILES:
        url = TRUST_BASE + filename
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()
        require(sha256(data) == expected, "Trust-list hash mismatch: " + filename)
        anchors.append({"trust_kind": kind, "trust_uri": url,
                        "trust_anchors": data.decode()})
        report["trust_sources"].append({"url": url, "sha256": expected})

    # Explicit C2PA claim-signing, document-signing and email-protection EKUs.
    # Trust roots come solely from the official lists; no certificate allow-list.
    settings = {
        "trust": {"anchors": anchors, "trust_config":
                  "1.3.6.1.4.1.62558.2.1\n1.3.6.1.5.5.7.3.36\n1.3.6.1.5.5.7.3.4"},
        "verify": {"verify_after_reading": True, "verify_trust": True,
                   "verify_timestamp_trust": True, "remote_manifest_fetch": False,
                   "ocsp_fetch": False},
    }
    report["verification_options"] = {
        "trust_config": settings["trust"]["trust_config"],
        "verify": settings["verify"],
    }
    with tempfile.TemporaryDirectory(prefix="slopos-provenance-") as directory:
        temp = Path(directory)
        picture = temp / "wallpaper-source.png"
        picture.write_bytes(source("docs/wallpaper-source.png"))
        with c2pa.Context(c2pa.Settings.from_dict(settings)) as context:
            with c2pa.Reader(str(picture), context=context) as reader:
                result = json.loads(reader.json())
        status = result["validation_results"]["activeManifest"]
        successes = {entry["code"] for entry in status["success"]}
        require(result["validation_state"] == "Trusted" and not status["failure"],
                "Image manifest failed validation")
        require({"claimSignature.validated", "signingCredential.trusted",
                 "assertion.dataHash.match"} <= successes,
                "Missing signature, trust or asset-integrity verification")
        converter = temp / "mkwallpaper.py"
        converter.write_bytes(source("tools/mkwallpaper.py"))
        header = temp / "wallpaper.h"
        subprocess.run([sys.executable, str(converter), str(picture), str(header)],
                       check=True, stdout=sys.stderr)
        require(header.read_bytes() == source("apps/wallpaper.h"),
                "Regenerated wallpaper differs from reviewed header")
        report["wallpaper"] = {
            "source_sha256": sha256(picture.read_bytes()),
            "header_sha256": sha256(header.read_bytes()),
            "header_reproduced_exactly": True,
            "c2pa": result,
        }

    report["bytecode"] = []
    for name in ("lzss", "mkesp"):
        path = "tools/__pycache__/" + name + ".cpython-313.pyc"
        data = source(path)
        paths = sorted({match.decode() for match in
                        re.findall(rb"/(?:mnt/data|home/user)/[A-Za-z0-9_./-]+\.py", data)})
        report["bytecode"].append({"file": path, "sha256": sha256(data),
                                   "embedded_path_strings": paths})

    if args.archive:
        require(sha256(args.archive.read_bytes()) == ZIP_HASH,
                "Archive differs from the reviewed public download")
        paths = git("ls-tree", "-rz", "--name-only", REVIEW_COMMIT).decode().split("\0")[:-1]
        comparison = {"sha256": ZIP_HASH, "tracked_files": len(paths),
                      "matching_files": 0, "different_files": [], "missing_files": []}
        with zipfile.ZipFile(args.archive) as archive:
            for path in paths:
                try:
                    data = archive.read(ZIP_PREFIX + path)
                except KeyError:
                    comparison["missing_files"].append(path)
                    continue
                original = source(path)
                if data == original:
                    comparison["matching_files"] += 1
                else:
                    comparison["different_files"].append({
                        "file": path, "archive_sha256": sha256(data),
                        "git_sha256": sha256(original)})
        require(comparison["matching_files"] == 311 and not comparison["missing_files"]
                and [item["file"] for item in comparison["different_files"]] == ["LICENSE"],
                "Unexpected archive/source comparison")
        report["archive_comparison"] = comparison
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
