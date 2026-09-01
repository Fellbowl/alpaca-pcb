"""
Cliente WebSocket de referencia para el controlador ESP32-S3.

Instalación:
    pip install websockets

Uso:
    python python_client.py --host 192.168.1.50

Este script hace dos cosas concurrentemente:
  1. Escucha telemetría entrante (JSON) y la imprime.
  2. Lee comandos de stdin en formato "pasos,direccion" (igual que la
     consola serial original) y los traduce a JSON antes de enviarlos.

Sirve como punto de partida -- para integrarlo en tu propio software
(por ejemplo un backend de control de foco), reemplaza el bucle de stdin
por tu lógica real y consume `recv_loop` como una tarea de fondo.
"""

import asyncio
import json
import argparse
import sys

import websockets


async def recv_loop(ws: "websockets.WebSocketClientProtocol") -> None:
    """Imprime cada mensaje de telemetría/ack que llega del ESP32."""
    async for raw in ws:
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            print(f"[recv] mensaje no-JSON: {raw!r}")
            continue

        if "error" in data:
            print(f"[ERROR] {data['error']}")
        elif "ack" in data:
            print(f"[ack] steps={data.get('steps')} dir={data.get('dir')}")
        else:
            # Telemetría normal
            print(f"[telemetria] {data}")


async def send_loop(ws: "websockets.WebSocketClientProtocol") -> None:
    """Lee líneas de stdin en formato 'pasos,direccion' y las envía como
    JSON {"cmd": "move", "steps": N, "dir": D}."""
    loop = asyncio.get_event_loop()

    print("Escribe comandos como: pasos,direccion  (ej: 2000,1). Ctrl+C para salir.")

    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            await asyncio.sleep(0.1)
            continue

        line = line.strip()
        if not line:
            continue

        try:
            steps_str, dir_str = line.split(",")
            steps = int(steps_str.strip())
            direction = int(dir_str.strip())
        except ValueError:
            print(f"formato invalido: '{line}'. Usa: pasos,direccion")
            continue

        payload = json.dumps({"cmd": "move", "steps": steps, "dir": direction})
        await ws.send(payload)


async def main(host: str, port: int, path: str) -> None:
    uri = f"ws://{host}:{port}{path}"
    print(f"Conectando a {uri} ...")

    async with websockets.connect(uri) as ws:
        print("Conectado.")
        # Corren concurrentemente: recepción de telemetría y envío de comandos.
        await asyncio.gather(recv_loop(ws), send_loop(ws))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Cliente WS para el controlador ESP32-S3")
    parser.add_argument("--host", required=True, help="IP del ESP32 (ver log de wifi_init)")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--path", default="/ws")
    args = parser.parse_args()

    try:
        asyncio.run(main(args.host, args.port, args.path))
    except KeyboardInterrupt:
        print("\nCerrado por el usuario.")
