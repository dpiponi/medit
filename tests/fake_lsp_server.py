#!/usr/bin/env python3

import json
import sys


def read_message():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        if line == b"\r\n":
            break
        key, value = line.decode("utf-8").split(":", 1)
        headers[key.strip().lower()] = value.strip()
    length = int(headers.get("content-length", "0"))
    if length <= 0:
        return None
    payload = sys.stdin.buffer.read(length)
    if not payload:
        return None
    return json.loads(payload.decode("utf-8"))


def send_message(message):
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("utf-8"))
    sys.stdout.buffer.write(payload)
    sys.stdout.buffer.flush()


def publish(uri, message):
    send_message(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {
                "uri": uri,
                "diagnostics": [
                    {
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 0, "character": 1},
                        },
                        "severity": 1,
                        "source": "fake-lsp",
                        "message": message,
                    }
                ],
            },
        }
    )


while True:
    message = read_message()
    if message is None:
        break

    method = message.get("method")
    if method == "initialize":
        send_message({"jsonrpc": "2.0", "id": message["id"], "result": {"capabilities": {}}})
    elif method == "textDocument/didOpen":
        publish(message["params"]["textDocument"]["uri"], "open diagnostic")
    elif method == "textDocument/didChange":
        publish(message["params"]["textDocument"]["uri"], "changed diagnostic")
    elif method == "textDocument/definition":
        send_message(
            {
                "jsonrpc": "2.0",
                "id": message["id"],
                "result": [
                    {
                        "uri": message["params"]["textDocument"]["uri"],
                        "range": {
                            "start": {"line": 0, "character": 1},
                            "end": {"line": 0, "character": 2},
                        },
                    }
                ],
            }
        )
    elif method == "shutdown":
        send_message({"jsonrpc": "2.0", "id": message["id"], "result": None})
    elif method == "exit":
        break
