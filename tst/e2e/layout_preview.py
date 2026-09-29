import re
from session import Session

with Session("layout_preview") as s:
    s.launch(PLT_EXPORT_DIR=str(s.artifacts))
    def verify(name, width, height, left="204060"):
        s.logged(f"EXPORTED {name} {width} {height}")
        ppm = (s.artifacts / f"{name}.ppm").read_bytes()
        match = re.match(rb"P6\n(\d+) (\d+)\n255\n", ppm)
        assert tuple(map(int, match.groups())) == (width, height)
        row = bytes.fromhex(left) * (width // 2) + bytes.fromhex("e0b040") * (width - width // 2)
        assert ppm[match.end():] == row * height
        s.screenshot(name, [(.2, .3, .4, .7, left), (.6, .3, .8, .7, "e0b040")])
    verify("initial", 96, 64)
    for key, name, width, height in [
        ("r", "resized", 256, 192), ("m", "maximized", 640, 480),
        ("n", "restored", 256, 192), ("f", "fullscreen", 640, 480),
        ("w", "windowed", 256, 192), ("x", "nested", 256, 192),
        ("z", "restored-config", 256, 192), ("l", "linked", 256, 192),
    ]:
        s.type(key)
        verify(name, width, height)
    s.type("p")
    verify("retried", 256, 192, "8040a0")
    s.close()
