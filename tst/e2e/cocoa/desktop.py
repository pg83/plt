"""Drive a native plt/Metal desktop through its IPC, WindowServer and pasteboard."""
import ctypes as C
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

out = Path(os.environ["PLT_E2E_ARTIFACTS"])
cg = C.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
cf = C.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")

class Point(C.Structure):
    _fields_ = [("x", C.c_double), ("y", C.c_double)]

cg.CGEventCreateKeyboardEvent.argtypes = [C.c_void_p, C.c_uint16, C.c_bool]
cg.CGEventCreateKeyboardEvent.restype = C.c_void_p
cg.CGEventCreateMouseEvent.argtypes = [C.c_void_p, C.c_uint32, Point, C.c_uint32]
cg.CGEventCreateMouseEvent.restype = C.c_void_p
cg.CGEventSetFlags.argtypes = [C.c_void_p, C.c_uint64]
cg.CGEventPostToPid.argtypes = [C.c_int, C.c_void_p]
cg.CGEventPost.argtypes = [C.c_uint32, C.c_void_p]
cf.CFRelease.argtypes = [C.c_void_p]

with tempfile.TemporaryDirectory(prefix="plt-cocoa-") as temp:
    drop_file = Path(temp) / "document.txt"
    drop_file.write_text("document to import")
    fifo = Path(temp) / "commands"
    os.mkfifo(fifo)
    with (out / "client.log").open("w") as log:
        app = subprocess.Popen([os.environ["PLT_E2E_BINARY"]],
            env={**os.environ, "PLT_COMMAND_PIPE": str(fifo), "PLT_DROP_FILE": str(drop_file)}, stdout=log, stderr=subprocess.STDOUT)
    try:
        def wait(predicate, label, timeout=15):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if app.poll() is not None:
                    raise AssertionError(f"client exited {app.returncode}: {label}")
                result = predicate()
                if result:
                    return result
                time.sleep(.05)
            raise AssertionError(f"timed out: {label}\n{(out / 'client.log').read_text()}")

        def text():
            return (out / "client.log").read_text()

        wait(lambda: "FRAME " in text(), "first Metal frame")
        with fifo.open("w", buffering=1) as commands:
            def command(value):
                before = len(text())
                commands.write(value + "\n")
                wait(lambda: f"COMMAND {value} " in text()[before:], value)

            def geometry():
                command("describe")
                return tuple(map(int, re.findall(r"WINDOW (\d+) (-?\d+) (-?\d+) (\d+) (\d+)", text())[-1]))

            def screenshot(name, color):
                window, *_ = geometry()
                png = out / f"{name}.png"
                bmp = Path(temp) / "capture.bmp"
                target = bytes.fromhex(color)
                def capture():
                    subprocess.run(["/usr/sbin/screencapture", "-x", "-o", "-l", str(window), str(png)], check=True, timeout=10)
                    subprocess.run(["sips", "-s", "format", "bmp", str(png), "--out", str(bmp)], check=True, stdout=subprocess.DEVNULL, timeout=10)
                    data = bmp.read_bytes()
                    assert data[:2] == b"BM"
                    offset = struct.unpack_from("<I", data, 10)[0]
                    width, height = struct.unpack_from("<ii", data, 18)
                    bits = struct.unpack_from("<H", data, 28)[0]
                    assert bits in (24, 32), bits
                    stride = (width * bits + 31) // 32 * 4
                    # Interior pixels exclude the window title, rounded corners and cursor.
                    for y in range(abs(height) // 3, abs(height) * 2 // 3, 11):
                        for x in range(width // 3, width * 2 // 3, 11):
                            pos = offset + y * stride + x * (bits // 8)
                            rgb = data[pos:pos + 3][::-1]
                            if any(abs(a - b) > 8 for a, b in zip(rgb, target)):
                                return False
                    return True
                wait(capture, f"screenshot {name}")

            def key(code, flags=0):
                for pressed in (True, False):
                    event = cg.CGEventCreateKeyboardEvent(None, code, pressed)
                    assert event
                    cg.CGEventSetFlags(event, flags)
                    cg.CGEventPostToPid(app.pid, event)
                    cf.CFRelease(event)
                    time.sleep(.02)

            screenshot("initial", "204060")
            command("title")
            command("caret")
            command("compose")
            wait(lambda: "TEXT 127757" in text() and "PREEDIT 0 -1 -1" in text(), "IME commit and preedit clear")
            key(0)  # Physical A, delivered by WindowServer to the application.
            wait(lambda: "TEXT 97" in text(), "keyboard text input")
            screenshot("typed", "c04040")
            if os.environ.get("PLT_RECOVERY_ONLY"):
                fault = os.environ["PLT_CHAOS"].split("@")[0]
                wait(lambda: "CHAOS " + fault in text(), "fault injection")
                command("resize")
                screenshot("recovered", "c04040")
                commands.write("quit\n")
                assert app.wait(timeout=15) == 0
                raise SystemExit(0)
            # Named keys, arrows, keypad and modifiers take native translation paths.
            for code in (36, 48, 51, 53, 115, 119, 116, 121, 123, 124, 125, 126,
                         122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111,
                         82, 83, 84, 85, 86, 87, 88, 89, 91, 92, 65, 67, 69, 75, 76, 78, 81):
                key(code)
            key(0, 1 << 17)  # Shift
            key(11, 1 << 18)  # Control+B
            key(3, 1 << 19)  # Option+F
            window, x, y, width, height = geometry()
            point = Point(x + width / 4, y + height / 2)
            for kind in (5, 1, 6, 2, 3, 7, 4, 25, 27, 26):
                button = 1 if kind in (3, 4, 7) else 2 if kind in (25, 26, 27) else 0
                event = cg.CGEventCreateMouseEvent(None, kind, point, button)
                cg.CGEventPost(0, event)
                cf.CFRelease(event)
                time.sleep(.05)
            wait(lambda: "BUTTON 0 1" in text(), "mouse input")
            screenshot("clicked", "8040a0")
            for code in (56, 60, 59, 62, 58, 61, 55, 54, 57):
                key(code)
            cg.CGEventCreateScrollWheelEvent.restype = C.c_void_p
            cg.CGEventCreateScrollWheelEvent.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.c_int32]
            for unit in (0, 1):
                event = cg.CGEventCreateScrollWheelEvent(None, unit, 1, 3)
                cg.CGEventPost(0, event)
                cf.CFRelease(event)
            wait(lambda: "SCROLL" in text(), "native wheel input")
            command("resize")
            wait(lambda: "FRAME 640 360 " in text(), "resize in pixels")
            command("move")
            screenshot("resized", "8040a0")
            for icon in range(37):
                command(f"cursor {icon}")
            command("copy")
            assert subprocess.check_output(["pbpaste"]).decode() == "plt clipboard: Привет 🌍"
            command("abandon")
            assert subprocess.check_output(["pbpaste"]).decode() == "plt clipboard: Привет 🌍"
            command("primary")
            screenshot("copied", "e0b040")
            subprocess.run(["pbcopy"], input=b"external document", check=True)
            command("paste")
            wait(lambda: "PASTED external document" in text(), "external paste")
            screenshot("pasted", "40a060")
            for kind, expected in (("text", "dropped document"), ("file", drop_file.as_uri())):
                command("drag-" + kind)
                sx, sy = map(int, re.findall(r"SOURCE (\d+) (\d+)", text())[-1])
                _, x, y, width, height = geometry()
                tx, ty = x + width / 2, y + height / 2
                def mouse(kind, x, y):
                    event = cg.CGEventCreateMouseEvent(None, kind, Point(x, y), 0)
                    cg.CGEventPost(0, event)
                    cf.CFRelease(event)
                    time.sleep(.05)
                mouse(5, sx, sy)
                mouse(1, sx, sy)
                for step in range(1, 16):
                    mouse(6, sx + (tx - sx) * step / 15, sy + (ty - sy) * step / 15)
                mouse(2, tx, ty)
                wait(lambda: "DROPPED " + expected in text(), "native drag " + kind)
                command("close-source")
                screenshot("dropped-" + kind, "40a060")
            command("minimize")
            command("restore")
            screenshot("restored", "40a060")
            command("maximize")
            screenshot("maximized", "40a060")
            command("unmaximize")
            command("fullscreen")
            time.sleep(1)
            screenshot("fullscreen", "40a060")
            command("unfullscreen")
            time.sleep(1)
            screenshot("windowed", "40a060")
            commands.write("quit\n")
            assert app.wait(timeout=15) == 0
    finally:
        if app.poll() is None:
            app.terminate()
            try:
                app.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app.kill()
                app.wait()
