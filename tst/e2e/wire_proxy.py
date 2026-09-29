"""Wayland transport faults between a real compositor and a real desktop client.

The relay preserves object IDs and SCM_RIGHTS descriptors. Rules alter one
protocol event; rendering, input injection and screenshots still use Sway.
"""
import array
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import threading
import xml.etree.ElementTree as ET


def word(data, offset):
    return struct.unpack_from("=I", data, offset)[0]


def padded(size):
    return (size + 3) & ~3


def decode(arguments, data):
    result = {}
    offset = 8
    for argument in arguments:
        kind = argument.attrib["type"]
        name = argument.attrib["name"]
        if kind == "fd":
            result[name] = None
        elif kind == "new_id" and "interface" not in argument.attrib:
            size = word(data, offset)
            interface = data[offset + 4:offset + 3 + size].decode()
            offset += 4 + padded(size)
            result[name] = (interface, word(data, offset), word(data, offset + 4))
            offset += 8
        elif kind in ("string", "array"):
            size = word(data, offset)
            value = data[offset + 4:offset + 4 + size]
            result[name] = None if size == 0 and kind == "string" else value
            offset += 4 + padded(size)
        else:
            result[name] = word(data, offset)
            offset += 4
    assert offset == len(data), (arguments, len(data), offset)
    return result


def encode(object_id, opcode, arguments, values):
    payload = bytearray()
    for argument in arguments:
        kind = argument.attrib["type"]
        value = values[argument.attrib["name"]]
        if kind == "fd":
            continue
        if kind in ("string", "array"):
            value = value or b""
            if isinstance(value, str):
                value = value.encode() + (b"\0" if kind == "string" else b"")
            payload.extend(struct.pack("=I", len(value)))
            payload.extend(value)
            payload.extend(b"\0" * (padded(len(value)) - len(value)))
        else:
            payload.extend(struct.pack("=I", value))
    return struct.pack("=II", object_id, ((len(payload) + 8) << 16) | opcode) + payload


class Proxy:
    def __init__(self, session, rules):
        self.session = session
        self.rules = [dict(rule, fired=False) for rule in rules]
        self.path = str(Path(session.runtime.name) / "fault-wayland")
        self.interfaces = {}
        core = subprocess.check_output(["pkg-config", "--variable=pkgdatadir", "wayland-scanner"], text=True).strip()
        protocols = subprocess.check_output(["pkg-config", "--variable=pkgdatadir", "wayland-protocols"], text=True).strip()
        # Retired xdg-shell v5 reused stable interface names with incompatible
        # opcodes. Prefer the stable definitions used by the actual client.
        files = sorted(Path(protocols).rglob("*.xml"), key=lambda path: ("/stable/" in str(path), str(path)))
        files.append(Path(core) / "wayland.xml")
        for path in files:
            for interface in ET.parse(path).getroot().findall("interface"):
                self.interfaces[interface.attrib["name"]] = {
                    direction: interface.findall(direction) for direction in ("request", "event")
                }
        self.error = None
        self.stopping = threading.Event()
        self.workers = []
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.path)
        self.listener.listen(1)
        self.listener.settimeout(.1)
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def transform(self, frame, direction, objects):
        object_id, header = struct.unpack_from("=II", frame)
        opcode = header & 0xffff
        interface = objects.get(object_id)
        messages = self.interfaces.get(interface, {}).get(direction, [])
        if opcode >= len(messages):
            return frame
        message = messages[opcode]
        arguments = message.findall("arg")
        try:
            values = decode(arguments, frame)
        except (AssertionError, struct.error) as error:
            raise RuntimeError(f"decode {direction} {object_id}:{interface}.{message.attrib['name']} {frame.hex()}") from error
        for argument in arguments:
            if argument.attrib["type"] == "new_id":
                value = values[argument.attrib["name"]]
                if "interface" in argument.attrib:
                    objects[value] = argument.attrib["interface"]
                else:
                    objects[value[2]] = value[0]
        if direction != "event":
            return frame
        for rule in self.rules:
            if (rule["fired"] and not rule.get("repeat")) or (rule["interface"], rule["event"]) != (interface, message.attrib["name"]):
                continue
            if rule.get("skip", 0):
                rule["skip"] -= 1
                continue
            rule["fired"] = True
            self.session.artifacts.joinpath("wire-faults.json").write_text(json.dumps(self.rules))
            if rule.get("drop"):
                assert all(argument.attrib["type"] != "fd" for argument in arguments)
                return b""
            if "insert" in rule:
                inserted = rule["insert"]
                event = next(event for event in messages if event.attrib["name"] == inserted["event"])
                index = messages.index(event)
                return encode(object_id, index, event.findall("arg"), inserted["values"]) + frame
            values.update(rule.get("replace", {}))
            if "append_state" in rule:
                values["states"] += struct.pack("=I", rule["append_state"])
            return encode(object_id, opcode, arguments, values)
        return frame

    def run(self):
        try:
            while not self.stopping.is_set():
                try:
                    client, _ = self.listener.accept()
                except socket.timeout:
                    continue
                # Mesa may establish a separate discovery connection before
                # using the supplied display for WSI. Its traffic stays intact.
                worker = threading.Thread(target=self.relay, args=(client, not self.workers), daemon=True)
                self.workers.append(worker)
                worker.start()
        finally:
            for worker in self.workers:
                worker.join(timeout=2)

    def relay(self, client, primary):
        pending = {}
        connections = [client]
        objects = {1: "wl_display"}
        try:
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connections.append(server)
            server.connect(str(Path(self.session.runtime.name) / self.session.env["WAYLAND_DISPLAY"]))
            buffers = {client: bytearray(), server: bytearray()}
            pending = {client: [], server: []}
            while not self.stopping.is_set():
                readable, _, _ = select.select(connections, [], [], .1)
                for source in readable:
                    data, ancillary, flags, _ = source.recvmsg(65536, socket.CMSG_SPACE(256 * 4))
                    descriptors = pending[source]
                    assert not flags & socket.MSG_CTRUNC, "Wayland descriptor batch truncated"
                    for level, kind, payload in ancillary:
                        assert (level, kind) == (socket.SOL_SOCKET, socket.SCM_RIGHTS)
                        descriptors.extend(array.array("i", payload))
                    if not data:
                        return
                    buffer = buffers[source]
                    buffer.extend(data)
                    outgoing = bytearray()
                    while len(buffer) >= 8:
                        size = word(buffer, 4) >> 16
                        assert size >= 8 and size % 4 == 0
                        if len(buffer) < size:
                            break
                        frame = bytes(buffer[:size])
                        del buffer[:size]
                        outgoing.extend(self.transform(frame, "request" if source is client else "event", objects) if primary else frame)
                    # libwayland queues descriptors independently of bytes.
                    # Send the batch with the complete prefix, retaining any
                    # partial message so the next read starts at its header.
                    destination = server if source is client else client
                    if outgoing:
                        control = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", descriptors))] if descriptors else []
                        sent = destination.sendmsg([outgoing], control)
                        destination.sendall(outgoing[sent:])
                        for descriptor in descriptors:
                            socket.close(descriptor)
                        descriptors.clear()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except BaseException as error:
            self.error = error
        finally:
            for descriptors in pending.values():
                for descriptor in descriptors:
                    socket.close(descriptor)
            for connection in connections:
                connection.close()

    def disconnect(self):
        self.stopping.set()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "Wayland relay did not stop"

    def __exit__(self, kind, value, traceback):
        self.disconnect()
        self.listener.close()
        if self.error:
            raise self.error
        if kind is None:
            assert all(rule["fired"] for rule in self.rules), self.rules
