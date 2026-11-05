"""DFC bench client (needs: pip install bleak): connect to "DFC" (or the old dev name "Coffee"), stream the NUS diagnostic line,
decode FTMS Indoor Bike Data + CPS Power Measurement, and send pin commands.

usage: dfc_ble.py scan
       dfc_ble.py watch SECONDS [cmd1 cmd2 ...]   (each cmd sent after 10 s, then wait)
       dfc_ble.py seq  "t0:10,h0:10,p0:10,e0:10"  (cmd:seconds steps)
"""
import asyncio, sys, time, struct
from bleak import BleakScanner, BleakClient

NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # notify (board -> us)
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # write  (us -> board)
FTMS_IBD = "00002ad2-0000-1000-8000-00805f9b34fb"
CPS_MEAS = "00002a63-0000-1000-8000-00805f9b34fb"
CSC_MEAS = "00002a5b-0000-1000-8000-00805f9b34fb"

def ts(): return time.strftime("%H:%M:%S")

async def find():
    devs = await BleakScanner.discover(timeout=15.0, return_adv=True)  # slow advertising (152 ms) needs a longer window
    for d, adv in devs.values():
        if (d.name or adv.local_name or "") in ("DFC", "Coffee"):
            return d, adv
    return None, None

async def scan():
    d, adv = await find()
    if not d:
        print("DFC not found"); return
    print("found", d.address, "rssi", adv.rssi, "uuids", adv.service_uuids, "mfg", adv.manufacturer_data)

def on_nus(_, data: bytearray):
    print(ts(), "NUS", data.decode(errors="replace").strip(), flush=True)

def on_ibd(_, data: bytearray):
    if len(data) >= 10:
        flags, spd, cad, res, pw = struct.unpack_from("<HHHhh", data, 0)
        print(ts(), "FTMS speed=%.2fkm/h cad=%.1frpm res=%d pow=%dW" % (spd/100, cad/2, res, pw), flush=True)

def on_csc(_, data: bytearray):
    if len(data) < 1: return
    flags = data[0]; off = 1; out = "CSC"
    if flags & 0x01 and len(data) >= off + 6:
        wr, wt = struct.unpack_from("<IH", data, off); off += 6; out += " wheel_revs=%u wtime1024=%u" % (wr, wt)
    if flags & 0x02 and len(data) >= off + 4:
        cr, ct = struct.unpack_from("<HH", data, off); off += 4; out += " crank_revs=%u ctime=%u" % (cr, ct)
    print(ts(), out, flush=True)

def on_cps(_, data: bytearray):
    if len(data) < 4: return
    flags, pw = struct.unpack_from("<Hh", data, 0); off = 4; out = "CPS pow=%dW" % pw
    if flags & 0x10 and len(data) >= off + 6:
        wr, wt = struct.unpack_from("<IH", data, off); off += 6; out += " wheel_revs=%u wevt=%u" % (wr, wt)
    if flags & 0x20 and len(data) >= off + 4:
        cr, ct = struct.unpack_from("<HH", data, off); off += 4; out += " crank_revs=%d evt=%u" % (cr, ct)
    print(ts(), out, flush=True)

async def session(steps):
    d, _ = await find()
    if not d:
        print("DFC not found"); return
    async with BleakClient(d, timeout=20.0) as c:
        print(ts(), "connected", d.address, flush=True)
        svcs = sorted(str(sv.uuid)[4:8] for sv in c.services if str(sv.uuid).endswith("-0000-1000-8000-00805f9b34fb"))
        print(ts(), "SERVICES:", " ".join(svcs), flush=True)
        await c.start_notify(NUS_TX, on_nus)
        for uuid, cb in ((FTMS_IBD, on_ibd), (CPS_MEAS, on_cps), (CSC_MEAS, on_csc)):
            if c.services.get_characteristic(uuid) is not None:
                await c.start_notify(uuid, cb)
        await asyncio.sleep(1)
        await c.write_gatt_char(NUS_RX, b"?", response=False)
        for cmd, secs in steps:
            if cmd:
                print(ts(), ">>> send", cmd, flush=True)
                await c.write_gatt_char(NUS_RX, cmd.encode(), response=False)
            await asyncio.sleep(secs)
        print(ts(), "done", flush=True)

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "scan"
    if mode == "scan":
        asyncio.run(scan())
    elif mode == "watch":
        secs = float(sys.argv[2]); cmds = sys.argv[3:]
        steps = [("", secs)] + [(c, 10) for c in cmds]
        asyncio.run(session(steps))
    elif mode == "seq":
        steps = [("", 8)] + [(p.split(":")[0], float(p.split(":")[1])) for p in sys.argv[2].split(",")]
        asyncio.run(session(steps))
