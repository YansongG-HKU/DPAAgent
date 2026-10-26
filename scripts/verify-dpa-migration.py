"""Verify the exact curated file set and target bytes against the transfer receipt."""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / "docs/migration/source-manifest-20261026.json").read_text())
paths = set()
for entry in manifest["files"]:
    path = entry["target_path"]
    if path in paths or Path(path).is_absolute() or ".." in Path(path).parts:
        raise SystemExit("invalid/duplicate target path")
    paths.add(path)
    target = root / path
    if target.is_symlink() or not target.is_file():
        raise SystemExit("missing or non-regular target: " + path)
    data = target.read_bytes()
    if hashlib.sha256(data).hexdigest() != entry["target_sha256"] or len(data) != entry["target_bytes"]:
        raise SystemExit("target receipt mismatch: " + path)
    unchanged = entry["source_sha256"] == entry["target_sha256"]
    if unchanged != (entry["status"] == "byte-identical"):
        raise SystemExit("incorrect modification status: " + path)
print(f"Verified {len(paths)} curated source receipts; private history not imported.")
