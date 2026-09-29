"""Null optional IME payloads are harmless and a later commit still renders."""
import os
from session import Session
from wire_proxy import Proxy

os.environ["PLT_IME"] = "1"
with Session("compose-protocol") as s, Proxy(s, [
    {"interface": "zwp_text_input_v3", "event": "preedit_string", "replace": {"text": None}},
    {"interface": "zwp_text_input_v3", "event": "commit_string", "replace": {"text": None}},
    {"interface": "zwp_text_input_v3", "event": "done", "insert": {
        "event": "delete_surrounding_text", "values": {"before_length": 1, "after_length": 1}}},
]) as proxy:
    s.launch(WAYLAND_DISPLAY=proxy.path, PLT_ZERO_CARET="1")
    s.focus()
    s.input("ime-preedit 61")
    s.input("ime-commit 61")
    s.screenshot("ignored", [(.1, .5, .9, .8, "204060")])
    assert "TEXT " not in (s.artifacts / "client.log").read_text()
    s.input("ime-commit 42")
    s.logged("TEXT 1 66")
    s.screenshot("recovered", [(.1, .5, .9, .8, "40a060")])
    s.close()
