from session import Session

with Session("window_state") as s:
    s.launch()
    s.focus()
    s.type("t")
    s.wait(lambda: s.window()["name"] == "Updated document", "application title")
    s.type("r")
    s.wait(lambda: s.window()["rect"]["width"] == 640, "application resize")
    s.screenshot("resized", [(.1, .5, .9, .8, "204060")])
    s.type("f")
    s.wait(lambda: s.window()["fullscreen_mode"] != 0, "fullscreen")
    s.screenshot("fullscreen", [(.1, .5, .9, .8, "40a060")])
    assert s.window()["rect"]["width"] == 1024
    s.type("f")
    s.wait(lambda: s.window()["fullscreen_mode"] == 0, "leave fullscreen")
    s.screenshot("restored", [(.1, .5, .9, .8, "204060")])
    s.key("g")
    s.logged("GRID READY")
    s.ipc(f'[con_id={s.window()["id"]}] resize set 100 100')
    s.wait(lambda: s.window()["rect"]["width"] == 313, "resize below grid base")
    s.screenshot("grid-base", [(.1, .5, .9, .8, "204060")])
    s.ipc("output HEADLESS-1 scale 1.5")
    s.ipc(f'[con_id={s.window()["id"]}] resize set 437 321')
    s.screenshot("fractional-grid", [(.1, .5, .9, .8, "204060")])
    s.key("u")
    s.logged("FREE RESIZE READY")
    s.ipc("output HEADLESS-1 scale 1")
    s.ipc(f'[con_id={s.window()["id"]}] floating disable')
    s.screenshot("tiled", [(.1, .5, .9, .8, "204060")])
    # Closing through the window manager exercises xdg_toplevel.close.
    s.ipc(f'[con_id={s.window()["id"]}] kill')
    assert s.client.wait(timeout=10) == 0
    s.client = None
