"""A real input method commits candidate text through Sway's text-input bridge."""
import os
from session import Session

os.environ["PLT_IME"] = "1"
with Session("compose-editor") as s:
    s.launch()
    s.focus()
    s.input("ime-preedit " + "にほん".encode().hex())
    s.logged("PREEDIT 9 0 9")
    s.screenshot("composing", [(.1, .5, .9, .8, "e0b040")])
    s.input("ime-commit " + "日本🌍".encode().hex())
    s.logged("TEXT 3 127757")
    s.logged("PREEDIT 0 -1 -1")
    s.screenshot("committed", [(.1, .5, .9, .8, "40a060")])
    # Bad input-method output must not leak invalid scalars into the document.
    for malformed in (b"\xe2", b"\xe2X", b"\xe2XA", b"\xf4\x90\x80\x80", b"\xed\xa0\x80",
                      b"\xc0\xaf", b"\xff", b"\x1f\x7f", b""):
        s.input("ime-commit " + malformed.hex())
    s.input("ime-commit 41")
    # The malformed sequences retain valid X/A bytes; the final A follows them.
    s.logged("TEXT 7 65")
    s.input("ime-preedit 61")
    s.logged("PREEDIT 1 0 1")
    s.ipc("workspace 2")
    s.logged("FOCUS plt-compose-editor 0")
    s.ipc("workspace 1")
    s.focus()
    s.input("ime-preedit ")
    s.wait(lambda: (s.artifacts / "client.log").read_text().count("PREEDIT 0 ") >= 2, "input method cancels composition")
    s.close()
