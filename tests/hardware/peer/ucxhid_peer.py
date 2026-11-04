#!/usr/bin/env python3
"""BlueZ HID test peer, copied to a Linux board (i.MX91) and run there by tests/hardware/linuxpeer.py.

  host <name|addr> <seconds> [keep]   be the HID host of the module's hid.setup: pair, subscribe to every
                                      HID input report and print each one as "REPORT <uuid> <ref> <hex>"
  kbd <name> <seconds> <text>         be a BLE HID keyboard for the module's hid.client: advertise, wait for
                                      the module to subscribe, then type <text>

Talks to bluetoothd over D-Bus at GATT level, so no uhid/HoG kernel support is needed. Every line it prints
starts with a keyword (READY, REPORT, SENT, INFO, ERROR, DONE) that the test parses.
"""

import sys
import time

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

TYPE_DELAY_MS = 3000

BLUEZ = "org.bluez"
OM = "org.freedesktop.DBus.ObjectManager"
PROPS = "org.freedesktop.DBus.Properties"
ADAPTER = "org.bluez.Adapter1"
DEVICE = "org.bluez.Device1"
CHAR = "org.bluez.GattCharacteristic1"
DESC = "org.bluez.GattDescriptor1"
HID_UUID = "00001812-0000-1000-8000-00805f9b34fb"
AGENT_PATH = "/ucx/agent"


def say(*words):
    print(" ".join(str(w) for w in words), flush=True)


def uuid16(uuid):
    return uuid[4:8].lower()


class Agent(dbus.service.Object):
    """NoInputNoOutput agent: accepts every pairing (Just Works)."""

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, device, uuid):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, device, passkey):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, device):
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self):
        pass


class Peer:
    def __init__(self):
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SystemBus()
        self.loop = GLib.MainLoop()
        self.om = dbus.Interface(self.bus.get_object(BLUEZ, "/"), OM)
        self.adapter_path = next(p for p, i in self.objects().items() if ADAPTER in i)
        self.adapter_obj = self.bus.get_object(BLUEZ, self.adapter_path)
        self.adapter = dbus.Interface(self.adapter_obj, ADAPTER)
        self.adapter_props = dbus.Interface(self.adapter_obj, PROPS)
        self.agent = Agent(self.bus, AGENT_PATH)
        manager = dbus.Interface(self.bus.get_object(BLUEZ, "/org/bluez"), "org.bluez.AgentManager1")
        manager.RegisterAgent(AGENT_PATH, "NoInputNoOutput")
        manager.RequestDefaultAgent(AGENT_PATH)
        self.set_adapter("Powered", True)
        self.set_adapter("Pairable", True)
        say("INFO adapter", self.adapter_props.Get(ADAPTER, "Address"))

    def objects(self):
        return self.om.GetManagedObjects()

    def set_adapter(self, name, value):
        self.adapter_props.Set(ADAPTER, name, dbus.Boolean(value))

    def pump(self, seconds):
        """Runs the main loop for a while (or until something quits it) so signals and D-Bus calls are served."""
        fired = []

        def stop():
            fired.append(True)
            self.loop.quit()
            return False

        source = GLib.timeout_add(int(seconds * 1000), stop)
        self.loop.run()
        if not fired:
            GLib.source_remove(source)

    def find_device(self, target):
        for path, ifaces in self.objects().items():
            dev = ifaces.get(DEVICE)
            if dev and dev.get("Adapter") == self.adapter_path and target in (
                    str(dev.get("Alias", "")), str(dev.get("Name", "")), str(dev.get("Address", "")).upper()):
                return path
        return None

    def forget(self, target):
        """Removes a cached or bonded device so every run pairs from scratch."""
        while True:
            path = self.find_device(target)
            if not path:
                return
            self.adapter.RemoveDevice(path)
            self.pump(0.5)

    def scan_for(self, target, seconds):
        self.adapter.SetDiscoveryFilter({"Transport": dbus.String("le"), "DuplicateData": dbus.Boolean(False)})
        self.adapter.StartDiscovery()
        try:
            deadline = time.time() + seconds
            while time.time() < deadline:
                path = self.find_device(target)
                if path:
                    return path
                self.pump(0.5)
        finally:
            try:
                self.adapter.StopDiscovery()
            except dbus.exceptions.DBusException:
                pass
        return None

    def device(self, path, name):
        return dbus.Interface(self.bus.get_object(BLUEZ, path), PROPS).Get(DEVICE, name)


def run_host(target, seconds, keep):
    """HID host role: pair with the module, subscribe to its input reports and print them."""
    peer = Peer()
    target = target.upper() if target.count(":") == 5 else target
    if not keep:
        peer.forget(target)
    path = peer.find_device(target) if keep else None
    if not path:
        path = peer.scan_for(target, 20)
    if not path:
        say("ERROR not found", target)
        return 2
    dev = dbus.Interface(peer.bus.get_object(BLUEZ, path), DEVICE)
    say("INFO device", peer.device(path, "Address"))
    if not peer.device(path, "Paired"):
        try:
            dev.Pair(timeout=40)
        except dbus.exceptions.DBusException as e:
            say("ERROR pair", e.get_dbus_name(), e.get_dbus_message())
            return 3
    elif peer.device(path, "Connected"):
        # A link left from before a module reboot is dead until its supervision timeout; start a fresh one
        # once BlueZ has really torn it down.
        try:
            dev.Disconnect()
        except dbus.exceptions.DBusException:
            pass
        for _ in range(60):
            if not peer.device(path, "Connected"):
                break
            peer.pump(0.5)
        say("INFO stale link closed")
    if not peer.device(path, "Connected"):
        try:
            dev.Connect(timeout=30)
        except dbus.exceptions.DBusException as e:
            # A bonded device is on BlueZ's accept list: its background auto-connect can win the race, and the
            # explicit Connect then aborts with "ACL Connection Already Exists". Use that link.
            for _ in range(20):
                if peer.device(path, "Connected"):
                    say("INFO connected by auto-connect")
                    break
                peer.pump(0.5)
            else:
                say("ERROR connect", e.get_dbus_name(), e.get_dbus_message())
                return 8
    for _ in range(60):
        if peer.device(path, "ServicesResolved"):
            break
        peer.pump(0.5)
    else:
        say("ERROR services not resolved")
        return 4
    say("INFO paired", bool(peer.device(path, "Paired")), "bonded", bool(peer.device(path, "Bonded")))

    objects = peer.objects()
    hid_services = [p for p, i in objects.items()
                    if p.startswith(path + "/") and i.get("org.bluez.GattService1", {}).get("UUID") == HID_UUID]
    if not hid_services:
        say("ERROR no HID service exported (bluetoothd input plugin may have claimed it)")
        return 5
    inputs = []
    for cpath, ifaces in sorted(objects.items()):
        ch = ifaces.get(CHAR)
        if not ch or not any(cpath.startswith(s + "/") for s in hid_services) or "notify" not in ch["Flags"]:
            continue
        ref = "-"
        for dpath, difaces in objects.items():
            d = difaces.get(DESC)
            if d and dpath.startswith(cpath + "/") and uuid16(d["UUID"]) == "2908":
                try:
                    ref = bytes(dbus.Interface(peer.bus.get_object(BLUEZ, dpath), DESC).ReadValue({})).hex()
                except dbus.exceptions.DBusException as e:
                    say("ERROR read report reference", uuid16(ch["UUID"]), e.get_dbus_message())
                    return 6
        inputs.append((cpath, uuid16(ch["UUID"]), ref))

    def on_change(iface, changed, invalidated, path=None):
        if iface == CHAR and "Value" in changed:
            for cpath, uuid, ref in inputs:
                if cpath == path:
                    say("REPORT", uuid, ref, bytes(changed["Value"]).hex())
        elif iface == DEVICE and "Connected" in changed and not changed["Connected"]:
            say("INFO disconnected")
            peer.loop.quit()

    peer.bus.add_signal_receiver(on_change, dbus_interface=PROPS, signal_name="PropertiesChanged",
                                 path_keyword="path")
    for cpath, uuid, ref in inputs:
        try:
            dbus.Interface(peer.bus.get_object(BLUEZ, cpath), CHAR).StartNotify()
        except dbus.exceptions.DBusException as e:
            say("ERROR subscribe", uuid, ref, e.get_dbus_message())
            return 7
        say("INFO subscribed", uuid, ref)
    say("READY", peer.device(path, "Address"), len(inputs))
    peer.pump(seconds)
    if not keep:
        try:
            dev.Disconnect()
        except dbus.exceptions.DBusException:
            pass
    say("DONE")
    return 0


# Keyboard (report ID 1) with LED output, the usual boot-compatible layout.
REPORT_MAP = bytes([
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
])


def key_for(c):
    if "a" <= c <= "z":
        return 0, 0x04 + ord(c) - ord("a")
    if "A" <= c <= "Z":
        return 0x02, 0x04 + ord(c) - ord("A")
    if "1" <= c <= "9":
        return 0, 0x1E + ord(c) - ord("1")
    return {"0": (0, 0x27), " ": (0, 0x2C), ".": (0, 0x37), "-": (0, 0x2D)}[c]


class Characteristic(dbus.service.Object):
    def __init__(self, bus, path, service, uuid, flags, value):
        super().__init__(bus, path)
        self.path, self.service, self.uuid, self.flags, self.value = path, service, uuid, flags, value
        self.on_subscribe = None

    def props(self):
        return {CHAR: {"Service": dbus.ObjectPath(self.service), "UUID": self.uuid,
                       "Flags": dbus.Array(self.flags, signature="s")}}

    @dbus.service.method(CHAR, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return dbus.Array(self.value, signature="y")

    @dbus.service.method(CHAR, in_signature="aya{sv}", out_signature="")
    def WriteValue(self, value, options):
        self.value = bytes(value)

    @dbus.service.method(CHAR, in_signature="", out_signature="")
    def StartNotify(self):
        if self.on_subscribe:
            self.on_subscribe()

    @dbus.service.method(CHAR, in_signature="", out_signature="")
    def StopNotify(self):
        pass

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def notify(self, value):
        self.value = bytes(value)
        self.PropertiesChanged(CHAR, {"Value": dbus.Array(self.value, signature="y")}, [])
        return False


class Descriptor(dbus.service.Object):
    def __init__(self, bus, path, char, uuid, value):
        super().__init__(bus, path)
        self.path, self.char, self.uuid, self.value = path, char, uuid, value

    def props(self):
        return {DESC: {"Characteristic": dbus.ObjectPath(self.char), "UUID": self.uuid,
                       "Flags": dbus.Array(["read"], signature="s")}}

    @dbus.service.method(DESC, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return dbus.Array(self.value, signature="y")


class Service(dbus.service.Object):
    def __init__(self, bus, path, uuid):
        super().__init__(bus, path)
        self.path, self.uuid = path, uuid

    def props(self):
        return {"org.bluez.GattService1": {"UUID": self.uuid, "Primary": dbus.Boolean(True)}}


class Application(dbus.service.Object):
    def __init__(self, bus, path, objects):
        super().__init__(bus, path)
        self.objects = objects

    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return {dbus.ObjectPath(o.path): o.props() for o in self.objects}


class Advertisement(dbus.service.Object):
    def __init__(self, bus, path, name):
        super().__init__(bus, path)
        self.name = name

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return {"Type": "peripheral", "LocalName": self.name, "Appearance": dbus.UInt16(0x03C1),
                "ServiceUUIDs": dbus.Array(["1812"], signature="s"), "Discoverable": dbus.Boolean(True)}

    @dbus.service.method("org.bluez.LEAdvertisement1", in_signature="", out_signature="")
    def Release(self):
        pass


def run_kbd(name, seconds, text):
    """HID device role: a BLE keyboard that types text once the module has subscribed to its input report."""
    peer = Peer()
    alias = peer.adapter_props.Get(ADAPTER, "Alias")
    peer.adapter_props.Set(ADAPTER, "Alias", dbus.String(name))
    bus, base = peer.bus, "/ucx/hid"
    svc = Service(bus, base, HID_UUID)
    info = Characteristic(bus, base + "/c0", base, "2a4a", ["encrypt-read"], bytes([0x11, 0x01, 0x00, 0x02]))
    rmap = Characteristic(bus, base + "/c1", base, "2a4b", ["encrypt-read"], REPORT_MAP)
    proto = Characteristic(bus, base + "/c2", base, "2a4e", ["encrypt-read", "write-without-response"], b"\x01")
    ctrl = Characteristic(bus, base + "/c3", base, "2a4c", ["write-without-response"], b"\x00")
    kin = Characteristic(bus, base + "/c4", base, "2a4d", ["encrypt-read", "notify"], bytes(8))
    kin_ref = Descriptor(bus, base + "/c4/d0", kin.path, "2908", b"\x01\x01")
    kout = Characteristic(bus, base + "/c5", base, "2a4d",
                          ["encrypt-read", "encrypt-write", "write-without-response"], b"\x00")
    kout_ref = Descriptor(bus, base + "/c5/d0", kout.path, "2908", b"\x01\x02")
    boot_in = Characteristic(bus, base + "/c6", base, "2a22", ["encrypt-read", "notify"], bytes(8))
    boot_out = Characteristic(bus, base + "/c7", base, "2a32",
                              ["encrypt-read", "encrypt-write", "write-without-response"], b"\x00")
    Application(bus, "/ucx", [svc, info, rmap, proto, ctrl, kin, kin_ref, kout, kout_ref, boot_in, boot_out])
    Advertisement(bus, "/ucx/adv0", name)
    errors = []
    gatt = dbus.Interface(peer.adapter_obj, "org.bluez.GattManager1")
    advm = dbus.Interface(peer.adapter_obj, "org.bluez.LEAdvertisingManager1")
    gatt.RegisterApplication("/ucx", {}, reply_handler=lambda: say("INFO gatt registered"),
                             error_handler=lambda e: errors.append(f"gatt {e}"))
    advm.RegisterAdvertisement("/ucx/adv0", {}, reply_handler=lambda: say("INFO advertising", name),
                               error_handler=lambda e: errors.append(f"adv {e}"))
    peer.pump(2)
    if errors:
        say("ERROR", "; ".join(errors))
        return 2
    say("READY", peer.adapter_props.Get(ADAPTER, "Address"))

    def type_text(char, kind):
        # Boot or report protocol: the module subscribes to one of the two, so type on the one it picked.
        protocol = "boot" if proto.value == b"\x00" else "report"
        say("INFO subscribed", kind, "protocol", protocol)
        # The CCCD write can come before bonding has finished; notifications sent before that are lost.
        polls = [0]

        def when_paired():
            polls[0] += 1
            if any(i.get(DEVICE, {}).get("Connected") and i.get(DEVICE, {}).get("Paired")
                   for i in peer.objects().values()):
                start_typing(char, kind)
                return False
            if polls[0] >= 40:
                say("INFO link not paired, typing anyway")
                start_typing(char, kind)
                return False
            return True

        if when_paired():
            GLib.timeout_add(250, when_paired)

    # the host subscribes before its HID command returns; give it time to start listening
    def start_typing(char, kind):
        for i, c in enumerate(text):
            mod, code = key_for(c)
            GLib.timeout_add(TYPE_DELAY_MS + i * 80, char.notify, bytes([mod, 0, code, 0, 0, 0, 0, 0]))
            GLib.timeout_add(TYPE_DELAY_MS + 40 + i * 80, char.notify, bytes(8))
        done = TYPE_DELAY_MS + 100 + len(text) * 80
        GLib.timeout_add(done, lambda: say("SENT", kind, text) or False)
        GLib.timeout_add(done + 3000, lambda: peer.loop.quit() or False)

    kin.on_subscribe = lambda: type_text(kin, "report")
    boot_in.on_subscribe = lambda: type_text(boot_in, "boot")
    peer.pump(seconds)
    for call in (lambda: advm.UnregisterAdvertisement("/ucx/adv0"), lambda: gatt.UnregisterApplication("/ucx"),
                 lambda: peer.adapter_props.Set(ADAPTER, "Alias", alias)):
        try:
            call()
        except dbus.exceptions.DBusException:
            pass
    say("DONE")
    return 0


def main(argv):
    try:
        if len(argv) >= 3 and argv[0] == "host":
            return run_host(argv[1], float(argv[2]), len(argv) > 3 and argv[3] == "keep")
        if len(argv) >= 4 and argv[0] == "kbd":
            return run_kbd(argv[1], float(argv[2]), " ".join(argv[3:]))
    except dbus.exceptions.DBusException as e:
        say("ERROR", e.get_dbus_name(), e.get_dbus_message())
        return 1
    say("ERROR usage: host <name|addr> <seconds> [keep] | kbd <name> <seconds> <text>")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
