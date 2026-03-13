#!/usr/bin/env python3

import argparse
import json
import socket
import sys
from typing import Any


def send_request(socket_path: str, method: str, params: dict[str, Any]) -> dict[str, Any]:
    request = json.dumps({"method": method, "params": params}) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.connect(socket_path)
        client.sendall(request.encode("utf-8"))
        response = bytearray()
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            response.extend(chunk)
            if response.endswith(b"\n"):
                break
    text = response.decode("utf-8").strip()
    if not text:
        raise RuntimeError("empty response from medit")
    return json.loads(text)


def add_socket_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--socket", required=True, help="Path to the medit control socket")


def main() -> int:
    parser = argparse.ArgumentParser(description="CLI control client for a running medit instance")
    subparsers = parser.add_subparsers(dest="command", required=True)

    status_parser = subparsers.add_parser("status", help="Show editor status")
    add_socket_argument(status_parser)

    list_buffers_parser = subparsers.add_parser("list-buffers", help="List open buffers")
    add_socket_argument(list_buffers_parser)

    list_windows_parser = subparsers.add_parser("list-windows", help="List open windows")
    add_socket_argument(list_windows_parser)

    get_buffer_parser = subparsers.add_parser("get-buffer", help="Read a buffer")
    add_socket_argument(get_buffer_parser)
    get_buffer_parser.add_argument("--buffer-id", type=int)

    get_selection_parser = subparsers.add_parser("get-selection", help="Read current selection")
    add_socket_argument(get_selection_parser)

    open_file_parser = subparsers.add_parser("open-file", help="Open a file")
    add_socket_argument(open_file_parser)
    open_file_parser.add_argument("path")

    switch_buffer_parser = subparsers.add_parser("switch-buffer", help="Switch active window to a buffer")
    add_socket_argument(switch_buffer_parser)
    switch_buffer_parser.add_argument("buffer_id", type=int)

    apply_edits_parser = subparsers.add_parser("apply-edits", help="Apply text edits from a JSON file or stdin")
    add_socket_argument(apply_edits_parser)
    apply_edits_parser.add_argument("--buffer-id", type=int)
    apply_edits_parser.add_argument("edits", nargs="?", help="Path to JSON edits file, or omit to read stdin")

    save_buffer_parser = subparsers.add_parser("save-buffer", help="Save a buffer")
    add_socket_argument(save_buffer_parser)
    save_buffer_parser.add_argument("--buffer-id", type=int)
    save_buffer_parser.add_argument("--path")

    raw_parser = subparsers.add_parser("raw", help="Send an arbitrary control request")
    add_socket_argument(raw_parser)
    raw_parser.add_argument("method")
    raw_parser.add_argument("params", nargs="?", default="{}", help="JSON object for params")

    args = parser.parse_args()

    method = ""
    params: dict[str, Any] = {}

    if args.command == "status":
        method = "status"
    elif args.command == "list-buffers":
        method = "list_buffers"
    elif args.command == "list-windows":
        method = "list_windows"
    elif args.command == "get-buffer":
        method = "get_buffer"
        if args.buffer_id is not None:
            params["buffer_id"] = args.buffer_id
    elif args.command == "get-selection":
        method = "get_selection"
    elif args.command == "open-file":
        method = "open_file"
        params["path"] = args.path
    elif args.command == "switch-buffer":
        method = "switch_buffer"
        params["buffer_id"] = args.buffer_id
    elif args.command == "apply-edits":
        method = "apply_text_edits"
        if args.buffer_id is not None:
            params["buffer_id"] = args.buffer_id
        source = sys.stdin.read() if args.edits is None else open(args.edits, "r", encoding="utf-8").read()
        params["edits"] = json.loads(source)
    elif args.command == "save-buffer":
        method = "save_buffer"
        if args.buffer_id is not None:
            params["buffer_id"] = args.buffer_id
        if args.path:
            params["path"] = args.path
    elif args.command == "raw":
        method = args.method
        parsed = json.loads(args.params)
        if not isinstance(parsed, dict):
            raise SystemExit("raw params must be a JSON object")
        params = parsed

    response = send_request(args.socket, method, params)
    json.dump(response, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
