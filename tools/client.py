#!/usr/bin/env python3
"""Cliente de teste do PokeSigner via BLE (PC).

Requisitos:  pip install bleak
Pareamento (Linux): pareie antes com `bluetoothctl` -> scan on -> pair <MAC>
                   e digite o codigo de 6 digitos mostrado no monitor serial.

Uso:
  python3 client.py ping
  python3 client.py info [--testnet] [--account N]
  python3 client.py sign arquivo.psbt [-o assinado.psbt]   (binario ou base64)
"""
import argparse, asyncio, base64, struct, sys
from bleak import BleakClient, BleakScanner

SVC = "50530001-b5a3-f393-e0a9-50ca5e1d0001"
RX = "50530002-b5a3-f393-e0a9-50ca5e1d0001"   # escrita (celular -> signer)
TX = "50530003-b5a3-f393-e0a9-50ca5e1d0001"   # notify (signer -> celular)

CMD_GET_INFO, CMD_SIGN_PSBT, CMD_PING = 0x01, 0x02, 0x03
CMD_CONFIRM, CMD_CANCEL, CMD_NEXT, CMD_PREV = 0x04, 0x05, 0x06, 0x07
RSP_INFO, RSP_SIGNED, RSP_PONG, RSP_STATUS, RSP_ERROR = 0x81, 0x82, 0x83, 0x90, 0xFF


class Framer:
    def __init__(self):
        self.buf = bytearray()
        self.q = asyncio.Queue()

    def feed(self, _, data: bytearray):
        self.buf += data
        while len(self.buf) >= 5:
            cmd, ln = self.buf[0], struct.unpack_from("<I", self.buf, 1)[0]
            if len(self.buf) < 5 + ln:
                break
            self.q.put_nowait((cmd, bytes(self.buf[5:5 + ln])))
            del self.buf[:5 + ln]


async def send(client, cmd, payload=b""):
    frame = bytes([cmd]) + struct.pack("<I", len(payload)) + payload
    chunk = max(20, client.mtu_size - 3)
    for i in range(0, len(frame), chunk):
        await client.write_gatt_char(RX, frame[i:i + chunk], response=True)


async def remote_control(client):
    """Durante a revisao: ENTER = proxima pagina, 'v' = voltar, 'x' = cancelar, 6 digitos = aprovar."""
    loop = asyncio.get_running_loop()
    print("Controle pelo PC: ENTER=proxima  v=voltar  x=cancelar  <codigo da tela>=aprovar")
    while True:
        line = (await loop.run_in_executor(None, sys.stdin.readline)).strip().lower()
        if line == "x":
            await send(client, CMD_CANCEL)
        elif line == "v":
            await send(client, CMD_PREV)
        elif line.isdigit() and len(line) == 6:
            await send(client, CMD_CONFIRM, line.encode())
        else:
            await send(client, CMD_NEXT)


async def request(client, framer, cmd, payload=b"", timeout=180):
    await send(client, cmd, payload)
    while True:
        rsp, data = await asyncio.wait_for(framer.q.get(), timeout)
        if rsp == RSP_STATUS:
            print("[signer]", data.decode())
            continue
        if rsp == RSP_ERROR:
            raise SystemExit("ERRO do signer: " + data.decode())
        return rsp, data


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["ping", "info", "sign"])
    ap.add_argument("file", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--testnet", action="store_true")
    ap.add_argument("--account", type=int, default=0)
    a = ap.parse_args()

    dev = await BleakScanner.find_device_by_filter(
        lambda d, adv: SVC in [u.lower() for u in adv.service_uuids], timeout=10)
    if not dev:
        sys.exit("PokeSigner nao encontrado")
    print("Conectando em", dev.name, dev.address)
    async with BleakClient(dev) as client:
        try:
            await client.pair()
        except Exception:
            pass
        framer = Framer()
        await client.start_notify(TX, framer.feed)
        if a.cmd == "ping":
            print((await request(client, framer, CMD_PING))[1].decode())
        elif a.cmd == "info":
            p = bytes([1 if a.testnet else 0]) + struct.pack("<I", a.account)
            print((await request(client, framer, CMD_GET_INFO, p))[1].decode())
        else:
            raw = open(a.file, "rb").read()
            if not raw.startswith(b"psbt\xff"):
                raw = base64.b64decode(raw.strip())
            ctl = asyncio.create_task(remote_control(client))
            try:
                _, signed = await request(client, framer, CMD_SIGN_PSBT, raw)
            finally:
                ctl.cancel()
            if a.out:
                open(a.out, "wb").write(signed)
                print("Salvo em", a.out)
            print(base64.b64encode(signed).decode())


if __name__ == "__main__":
    asyncio.run(main())
