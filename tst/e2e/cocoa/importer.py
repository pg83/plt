"""A native event loop imports an externally prepared document byte for byte."""
import os
from pathlib import Path
import subprocess
import tempfile

out = Path(os.environ["PLT_E2E_ARTIFACTS"])
data = bytes(range(256)) * 1024
checksum = 0
for value in data:
    checksum = (checksum * 131 + value) & ((1 << 64) - 1)
with tempfile.TemporaryDirectory(prefix="plt-import-") as temp:
    document = Path(temp) / "document.bin"
    document.write_bytes(data)
    result = subprocess.run([os.environ["PLT_E2E_BINARY"]],
                            env={**os.environ, "PLT_IMPORT_FILE": str(document)},
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    (out / "client.log").write_bytes(result.stdout)
    assert result.returncode == 0, result.stdout.decode()
    assert f"IMPORTED {len(data)} {checksum}" in result.stdout.decode()
    assert b"INDEXED" in result.stdout
    assert b"INITIAL TIMEOUT" in result.stdout and b"WORKER DONE" in result.stdout
