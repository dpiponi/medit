#!/usr/bin/env python3

from __future__ import annotations

import argparse
import socket
from typing import Any

from mcp.server.fastmcp import FastMCP


mcp = FastMCP("medit-mcp", json_response=True)
SOCKET_PATH: str | None = None


class BridgeError(RuntimeError):
    pass


def send_editor_request(method: str, params: dict[str, Any]) -> Any:
    if SOCKET_PATH is None:
        raise BridgeError("medit socket path is not configured")
    request = __import__("json").dumps({"method": method, "params": params}) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.connect(SOCKET_PATH)
        client.sendall(request.encode("utf-8"))
        response = bytearray()
        while True:
            chunk = client.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
            if b"\n" in chunk:
                break
    line = response.decode("utf-8").strip()
    if not line:
        raise BridgeError("empty response from medit")
    payload = __import__("json").loads(line)
    if not payload.get("ok"):
        raise BridgeError(payload.get("error", "unknown error"))
    return payload.get("result")


@mcp.tool(name="medit_status")
def medit_status() -> dict[str, Any]:
    return send_editor_request("status", {})


@mcp.tool(name="medit_list_buffers")
def medit_list_buffers() -> dict[str, Any]:
    return send_editor_request("list_buffers", {})


@mcp.tool(name="medit_list_windows")
def medit_list_windows() -> dict[str, Any]:
    return send_editor_request("list_windows", {})


@mcp.tool(name="medit_get_buffer")
def medit_get_buffer(buffer_id: int | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {}
    if buffer_id is not None:
        params["buffer_id"] = buffer_id
    return send_editor_request("get_buffer", params)


@mcp.tool(name="medit_get_cursor")
def medit_get_cursor() -> dict[str, Any]:
    return send_editor_request("get_cursor", {})


@mcp.tool(name="medit_set_cursor")
def medit_set_cursor(position: dict[str, int], buffer_id: int | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {"position": position}
    if buffer_id is not None:
        params["buffer_id"] = buffer_id
    return send_editor_request("set_cursor", params)


@mcp.tool(name="medit_set_selection")
def medit_set_selection(range: dict[str, Any], mode: str | None = None, buffer_id: int | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {"range": range}
    if mode is not None:
        params["mode"] = mode
    if buffer_id is not None:
        params["buffer_id"] = buffer_id
    return send_editor_request("set_selection", params)


@mcp.tool(name="medit_clear_selection")
def medit_clear_selection(buffer_id: int | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {}
    if buffer_id is not None:
        params["buffer_id"] = buffer_id
    return send_editor_request("clear_selection", params)


@mcp.tool(name="medit_get_selection")
def medit_get_selection() -> dict[str, Any]:
    return send_editor_request("get_selection", {})


@mcp.tool(name="medit_close_buffer")
def medit_close_buffer(buffer_id: int | None = None, force: bool | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {}
    if buffer_id is not None:
        params["buffer_id"] = buffer_id
    if force is not None:
        params["force"] = force
    return send_editor_request("close_buffer", params)


@mcp.tool(name="medit_open_file")
def medit_open_file(path: str) -> dict[str, Any]:
    return send_editor_request("open_file", {"path": path})


@mcp.tool(name="medit_focus_window")
def medit_focus_window(window_id: int) -> dict[str, Any]:
    return send_editor_request("focus_window", {"window_id": window_id})


@mcp.tool(name="medit_split_window")
def medit_split_window(direction: str) -> dict[str, Any]:
    return send_editor_request("split_window", {"direction": direction})


@mcp.tool(name="medit_close_window")
def medit_close_window(window_id: int | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {}
    if window_id is not None:
        params["window_id"] = window_id
    return send_editor_request("close_window", params)


@mcp.tool(name="medit_close_other_windows")
def medit_close_other_windows() -> dict[str, Any]:
    return send_editor_request("close_other_windows", {})


@mcp.tool(name="medit_switch_buffer")
def medit_switch_buffer(buffer_id: int) -> dict[str, Any]:
    return send_editor_request("switch_buffer", {"buffer_id": buffer_id})


@mcp.tool(name="medit_apply_text_edits")
def medit_apply_text_edits(edits: list[dict[str, Any]], buffer_id: int | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {"edits": edits}
    if buffer_id is not None:
        params["buffer_id"] = buffer_id
    return send_editor_request("apply_text_edits", params)


@mcp.tool(name="medit_open_line")
def medit_open_line(direction: str, buffer_id: int | None = None, autoindent: bool | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {"direction": direction}
    if buffer_id is not None:
        params["buffer_id"] = buffer_id
    if autoindent is not None:
        params["autoindent"] = autoindent
    return send_editor_request("open_line", params)


@mcp.tool(name="medit_save_buffer")
def medit_save_buffer(buffer_id: int | None = None, path: str | None = None) -> dict[str, Any]:
    params: dict[str, Any] = {}
    if buffer_id is not None:
        params["buffer_id"] = buffer_id
    if path is not None:
        params["path"] = path
    return send_editor_request("save_buffer", params)


def main() -> int:
    global SOCKET_PATH
    parser = argparse.ArgumentParser(description="FastMCP bridge for a running medit instance")
    parser.add_argument("--socket", required=True, help="Path to the medit control socket")
    args = parser.parse_args()
    SOCKET_PATH = args.socket
    mcp.run(transport="stdio")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
