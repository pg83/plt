"""Import real files from an external URI-list clipboard, rejecting invalid entries."""
from pathlib import Path
import subprocess
from session import Session

with Session("uri-import") as s:
    work = Path(s.runtime.name)
    first = work / "café notes.txt"
    second = work / "café lower.txt"
    first.write_bytes("first document: Привет\n".encode())
    second.write_bytes(b"second document\x00binary\xff\n")
    invalid = ["https://example.invalid/file", "file://", "file://localhost",
               "file://remote.invalid/etc/passwd", "file:///bad%", "file:///bad%2",
               "file:///bad%G0", "file:///bad%0Z"]
    lower = second.as_uri().replace("%C3%A9", "%c3%a9")
    local = first.as_uri().replace("file:///", "file://localhost/", 1)
    manifest = work / "manifest.txt"
    manifest.write_bytes(("# copied file references\r\n\n" + first.as_uri() + "\r\n" +
                          "\n".join(invalid) + "\n# another group\n" + local + "\n" + lower).encode())
    output = work / "imported.bin"
    s.launch(PLT_IMPORT_OUTPUT=str(output))
    with manifest.open("rb") as data:
        copy = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"], stdin=data, env=s.env)
    s.processes.append(copy)
    def offered():
        result = subprocess.run(["wl-paste", "--no-newline"], env=s.env, capture_output=True, timeout=5)
        return result.returncode == 0 and result.stdout == manifest.read_bytes()
    s.wait(offered, "URI list on clipboard")
    s.key("v")
    s.logged(f"IMPORTED 3 REJECTED {len(invalid)}")
    assert output.read_bytes() == first.read_bytes() * 2 + second.read_bytes()
    s.screenshot("imported", [(.1, .5, .9, .8, "40a060")])
    s.close()
