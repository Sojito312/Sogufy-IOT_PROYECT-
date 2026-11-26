"""Autoriza Spotify con PKCE y guarda la configuración en el ESP32 por USB."""
import argparse
import base64
from getpass import getpass
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import re
import secrets
from time import monotonic
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import Request, urlopen
import webbrowser

import serial

REDIRECT = "http://127.0.0.1:8888/callback"
TOKEN = "https://accounts.spotify.com/api/token"


def authorize(client_id):
    verifier = secrets.token_urlsafe(64)
    state = secrets.token_urlsafe(32)
    digest = hashlib.sha256(verifier.encode("ascii")).digest()
    challenge = base64.urlsafe_b64encode(digest).decode("ascii").rstrip("=")
    params = {
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT,
        "scope": "user-read-currently-playing",
        "code_challenge_method": "S256",
        "code_challenge": challenge,
        "state": state,
    }
    callback = {}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            result = urlsplit(self.path)
            fields = parse_qs(result.query)
            actual_state = fields.get("state", [""])[0]
            if result.path != "/callback" or not secrets.compare_digest(actual_state, state):
                self.send_error(400, "Respuesta invalida")
                return
            callback.update(fields)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(b"Puedes cerrar esta pestana y volver a PowerShell.")

        def log_message(self, *_args):
            pass

    with HTTPServer(("127.0.0.1", 8888), Handler) as server:
        server.timeout = 1
        url = "https://accounts.spotify.com/authorize?" + urlencode(params)
        if not webbrowser.open(url):
            print("Abre esta URL en tu navegador:", url)
        print("Autoriza el acceso a la canción actual en Spotify.")
        deadline = monotonic() + 180
        while not callback and monotonic() < deadline:
            server.handle_request()

    code = callback.get("code", [""])[0]
    if not code:
        raise ValueError("Spotify no entregó un código. Revisa el Redirect URI y el acceso a la app.")
    form = urlencode({
        "client_id": client_id,
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": REDIRECT,
        "code_verifier": verifier,
    }).encode("ascii")
    request = Request(TOKEN, data=form, headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urlopen(request, timeout=20) as response:
            token = json.load(response).get("refresh_token")
    except HTTPError as error:
        raise ValueError(f"Spotify rechazó el intercambio de código (HTTP {error.code}).") from None
    except URLError:
        raise ValueError("PowerShell no pudo comunicarse con Spotify.") from None
    if not isinstance(token, str) or not 0 < len(token.encode("utf-8")) < 1024:
        raise ValueError("Spotify no entregó un refresh token compatible.")
    return token


def send_config(port, config):
    payload = b"FIAD_CONFIG " + json.dumps(config, separators=(",", ":")).encode("ascii") + b"\n"
    if len(payload) >= 2048:
        raise ValueError("La configuración excede el tamaño aceptado por el ESP32.")

    with serial.Serial(port=None, baudrate=115200, timeout=0.25, write_timeout=4) as link:
        link.port = port
        link.dtr = False
        link.rts = False
        link.open()
        print("Buscando el ESP32 en la pantalla «Configura USB».")
        deadline = monotonic() + 30
        while monotonic() < deadline:
            link.write(b"\nFIAD?\n")
            if link.readline().strip() == b"FIAD_CONFIG_READY":
                break
        else:
            raise ValueError("El ESP32 no está en modo USB. Cierra el monitor serie y revisa COM.")

        link.write(payload)
        deadline = monotonic() + 8
        while monotonic() < deadline:
            answer = link.readline().strip()
            if answer == b"FIAD_CONFIG_OK":
                print("Configuración guardada. Empareja el iPhone con SOGUFY.")
                return
            if answer == b"FIAD_CONFIG_ERROR":
                raise ValueError("El ESP32 rechazó la configuración. Revisa la longitud de los datos.")
        raise ValueError("El ESP32 no confirmó el guardado.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM13")
    args = parser.parse_args()
    try:
        client_id = input("Client ID de Spotify: ").strip()
        if not re.fullmatch("[0-9a-fA-F]{32}", client_id):
            raise ValueError("El Client ID debe tener 32 caracteres hexadecimales.")
        ssid = input("Nombre de la red Wi-Fi de 2.4 GHz: ")
        password = getpass("Contraseña Wi-Fi (oculta): ")
        if not 1 <= len(ssid.encode("utf-8")) <= 32 or len(password.encode("utf-8")) > 64:
            raise ValueError("Revisa que el SSID tenga 1–32 bytes y la contraseña hasta 64.")
        refresh_token = authorize(client_id)
        send_config(args.port, {
            "ssid": ssid,
            "password": password,
            "client_id": client_id,
            "refresh_token": refresh_token,
        })
    except (OSError, ValueError, KeyboardInterrupt) as error:
        parser.exit(1, f"Configuración interrumpida. {error}\n")


if __name__ == "__main__":
    main()
