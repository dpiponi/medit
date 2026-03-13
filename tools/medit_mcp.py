#!/usr/bin/env python3

import argparse
import json
import socket
import sys
from typing import Any, Dict


TOOLS = [
    {
        "name": "medit_status",
        "description": "Get the current editor status, active buffer/window, and runtime summary.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "medit_list_buffers",
        "description": "List all open buffers.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "medit_list_windows",
        "description": "List all open editor windows and the buffers they show.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "medit_get_buffer",
        "description": "Get a buffer summary and full text. Defaults to the active buffer.",
        "inputSchema": {
            "type": "object",
            "properties": {"buffer_id": {"type": "integer", "minimum": 1}},
            "additionalProperties": False,
        },
    },
    {
        "name": "medit_get_selection",
        "description": "Get the current selection in the active window, including selected text.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "medit_open_file",
        "description": "Open a file in the active window.",
        "inputSchema": {
            "type": "object",
            "properties": {"path": {"type": "string"}},
            "required": ["path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "medit_switch_buffer",
        "description": "Show an existing buffer in the active window.",
        "inputSchema": {
            "type": "object",
            "properties": {"buffer_id": {"type": "integer", "minimum": 1}},
            "required": ["buffer_id"],
            "additionalProperties": False,
        },
    },
    {
        "name": "medit_apply_text_edits",
        "description": "Apply one or more text edits to a buffer. Defaults to the active buffer.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "buffer_id": {"type": "integer", "minimum": 1},
                "edits": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "range": {
                                "type": "object",
                                "properties": {
                                    "start": {
                                        "type": "object",
                                        "properties": {
                                            "row": {"type": "integer", "minimum": 0},
                                            "column": {"type": "integer", "minimum": 0},
                                        },
                                        "required": ["row", "column"],
                                        "additionalProperties": False,
                                    },
                                    "end": {
                                        "type": "object",
                                        "properties": {
                                            "row": {"type": "integer", "minimum": 0},
                                            "column": {"type": "integer", "minimum": 0},
                                        },
                                        "required": ["row", "column"],
                                        "additionalProperties": False,
                                    },
                                },
                                "required": ["start", "end"],
                                "additionalProperties": False,
                            },
                            "text": {"type": "string"},
                        },
                        "required": ["range", "text"],
                        "additionalProperties": False,
                    },
                },
            },
            "required": ["edits"],
            "additionalProperties": False,
        },
    },
    {
        "name": "medit_save_buffer",
        "description": "Save a buffer, optionally to a new path. Defaults to the active buffer.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "buffer_id": {"type": "integer", "minimum": 1},
                "path": {"type": "string"},
            },
            "additionalProperties": False,
        },
    },
]


TOOL_TO_METHOD = {
    "medit_status": "status",
    "medit_list_buffers": "list_buffers",
    "medit_list_windows": "list_windows",
    "medit_get_buffer": "get_buffer",
    "medit_get_selection": "get_selection",
    "medit_open_file": "open_file",
    "medit_switch_buffer": "switch_buffer",
    "medit_apply_text_edits": "apply_text_edits",
    "medit_save_buffer": "save_buffer",
}


def send_editor_request(socket_path: str, method: str, params: Dict[str, Any]) -> Dict[str, Any]:
    request = json.dumps({"method": method, "params": params}) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.connect(socket_path)
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
        return {"ok": False, "error": "empty response from medit"}
    return json.loads(line)


def mcp_result(result: Any) -> Dict[str, Any]:
    return {
        "content": [{"type": "text", "text": json.dumps(result, indent=2, sort_keys=True)}],
        "structuredContent": result,
    }


def mcp_error(message: str) -> Dict[str, Any]:
    return {
        "content": [{"type": "text", "text": message}],
        "isError": True,
    }


def handle_request(socket_path: str, request: Dict[str, Any]) -> Dict[str, Any] | None:
    method = request.get("method")
    request_id = request.get("id")

    if method == "notifications/initialized":
        return None

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "protocolVersion": "2025-03-26",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "medit-mcp", "version": "0.1"},
            },
        }

    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": request_id, "result": {"tools": TOOLS}}

    if method == "tools/call":
        params = request.get("params", {})
        name = params.get("name")
        arguments = params.get("arguments", {})
        bridge_method = TOOL_TO_METHOD.get(name)
        if bridge_method is None:
            return {"jsonrpc": "2.0", "id": request_id, "result": mcp_error(f"unknown tool: {name}")}
        try:
            response = send_editor_request(socket_path, bridge_method, arguments)
        except Exception as error:  # pragma: no cover - integration path
            return {"jsonrpc": "2.0", "id": request_id, "result": mcp_error(str(error))}
        if not response.get("ok"):
            return {"jsonrpc": "2.0", "id": request_id, "result": mcp_error(response.get("error", "unknown error"))}
        return {"jsonrpc": "2.0", "id": request_id, "result": mcp_result(response.get("result"))}

    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "error": {"code": -32601, "message": f"Method not found: {method}"},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="MCP bridge for a running medit instance")
    parser.add_argument("--socket", required=True, help="Path to the medit control socket")
    args = parser.parse_args()

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        request = json.loads(line)
        response = handle_request(args.socket, request)
        if response is None:
            continue
        sys.stdout.write(json.dumps(response) + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
