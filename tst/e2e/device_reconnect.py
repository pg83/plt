"""Keep a document open while physical input capabilities disappear and return."""
import json
import os
import subprocess
from session import Session

with Session("device-reconnect") as s:
    s.launch()
    s.focus()
    s.key("a")
    s.logged("TEXT 97")
    s.pointer(100, 100)
    s.button()
    # Removing a pressed device must cancel its grab and keyboard repeat.
    s.devices.terminate()
    assert s.devices.wait(timeout=5) != 0
    for process in s.processes:
        if process.args[0] == "wtype" and process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
    s.wait(lambda: not json.loads(s.command("swaymsg", "-r", "-t", "get_inputs")), "all input devices disconnected")
    s.screenshot("disconnected", [(.1, .5, .9, .8, "40a060")])
    s.devices = s.start([os.environ["PLT_E2E_DEVICES"]], "devices", stdin=subprocess.PIPE)
    s.input_serial = 0
    s.wait(lambda: "READY" in (s.artifacts / "devices.log").read_text(), "replacement pointer")
    s.key("b")
    s.logged("TEXT 98")
    s.pointer(120, 100)
    s.button()
    s.button(pressed=False)
    s.ipc("input * repeat_rate 0")
    s.ipc("input * repeat_delay 0")
    s.key("ISO_Level3_Shift")
    s.key("c")
    s.logged("TEXT 99")
    s.screenshot("reconnected", [(.1, .5, .9, .8, "40a060")])
    s.close()
