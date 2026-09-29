"""A modal pointer-leave handler processes entry into another document recursively."""
from session import Session

with Session("pointer-handoff") as s:
    s.launch(PLT_DIRECT_INPUT="1")
    source = s.window("plt-handoff-source")
    target = s.window("plt-handoff-target")
    s.ipc(f'[con_id={source["id"]}] move position 20 50')
    s.ipc(f'[con_id={target["id"]}] move position 500 50')
    s.screenshot("source", [(.1, .5, .9, .8, "e0b040")], "plt-handoff-source")
    s.pointer(100, 100, "plt-handoff-source")
    s.logged("PRESENCE 1 1")
    s.pointer(100, 100, "plt-handoff-target")
    s.logged("HANDOFF COMPLETE")
    s.button()
    s.button(pressed=False)
    s.logged("DESTINATION CLICKED")
    s.screenshot("destination", [(.1, .5, .9, .8, "40a060")], "plt-handoff-target")
    s.focus("plt-handoff-target")
    s.close()
