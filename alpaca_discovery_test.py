#!/usr/bin/env python3
"""Prueba del discovery UDP ASCOM Alpaca."""

import argparse
import socket
import sys


DISCOVERY_PORT = 32227
DISCOVERY_MESSAGE = b"alpacadiscovery1"
DEFAULT_TIMEOUT = 3.0


def send_probe(
    destination: str, payload: bytes, timeout: float, broadcast: bool = False
) -> bytes | None:
    """Envía un datagrama y devuelve la respuesta, o None si expira."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if destination == "<broadcast>" or broadcast:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

        sock.settimeout(timeout)
        sock.sendto(payload, (destination, DISCOVERY_PORT))
        try:
            response, source = sock.recvfrom(1024)
        except socket.timeout:
            return None

    print(f"Respuesta desde {source[0]}:{source[1]}: {response!r}")
    return response


def test_valid(
    destination: str, timeout: float, expected_port: int, broadcast: bool = False
) -> bool:
    """Comprueba que un request válido recibe el JSON esperado."""
    print(f"[VALIDO] Enviando {DISCOVERY_MESSAGE!r} a {destination}:{DISCOVERY_PORT}")
    response = send_probe(destination, DISCOVERY_MESSAGE, timeout, broadcast)
    if response is None:
        print("FALLO: no se recibió respuesta.")
        return False

    expected = f'{{"AlpacaPort":{expected_port}}}'.encode("ascii")
    if response != expected:
        print(f"FALLO: se esperaba {expected!r}, se recibió {response!r}")
        return False

    print("OK: respuesta de discovery correcta.")
    return True


def test_invalid(destination: str, timeout: float, broadcast: bool = False) -> bool:
    """Comprueba que un request inválido no recibe respuesta."""
    payload = b"basura"
    print(f"[INVALIDO] Enviando {payload!r} a {destination}:{DISCOVERY_PORT}")
    response = send_probe(destination, payload, timeout, broadcast)
    if response is not None:
        print("FALLO: se recibió respuesta para un payload inválido.")
        return False

    print("OK: no hubo respuesta para el payload inválido.")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Prueba unicast/broadcast del discovery UDP Alpaca"
    )
    parser.add_argument(
        "--host",
        required=True,
        help="IP del ESP32 o '<broadcast>' para broadcast IPv4",
    )
    parser.add_argument(
        "--mode",
        choices=("unicast", "broadcast", "invalid", "all"),
        default="unicast",
        help="Prueba que se ejecuta (por defecto: unicast)",
    )
    parser.add_argument(
        "--alpaca-port",
        type=int,
        default=11111,
        help="Puerto HTTP esperado en la respuesta (por defecto: 11111)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help=f"Tiempo de espera en segundos (por defecto: {DEFAULT_TIMEOUT})",
    )
    args = parser.parse_args()

    destination = args.host
    broadcast = args.mode == "broadcast" or destination == "<broadcast>"

    if args.mode == "invalid":
        success = test_invalid(destination, args.timeout, broadcast)
    elif args.mode == "all":
        success = test_valid(destination, args.timeout, args.alpaca_port, broadcast)
        success = test_invalid(destination, args.timeout, broadcast) and success
    else:
        success = test_valid(destination, args.timeout, args.alpaca_port, broadcast)

    return 0 if success else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrumpido por el usuario.")
        sys.exit(130)
    except OSError as error:
        print(f"ERROR de red: {error}", file=sys.stderr)
        sys.exit(1)
