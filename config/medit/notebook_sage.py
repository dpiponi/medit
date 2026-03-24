#!/usr/bin/env python3

import ast
import contextlib
import io
import sys
import traceback

from sage.all_cmdline import *  # noqa: F401,F403
from sage.repl.preparse import preparse


def write_frame(event, cell_id, payload=""):
    if not isinstance(payload, str):
        payload = str(payload)
    data = payload.encode("utf-8")
    header = f"MEDIT {event} {cell_id} {len(data)}\n".encode("utf-8")
    sys.stdout.buffer.write(header)
    if data:
        sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def read_frame():
    header = sys.stdin.buffer.readline()
    if not header:
        return None
    try:
        prefix, event, cell_id_text, length_text = header.decode("utf-8").rstrip("\n").split(" ", 3)
    except ValueError:
        return None
    if prefix != "MEDIT":
        return None
    length = int(length_text)
    payload = sys.stdin.buffer.read(length)
    if payload is None or len(payload) != length:
        return None
    return event, int(cell_id_text), payload.decode("utf-8")


def execute_cell(namespace, code):
    prepared = preparse(code)
    module = ast.parse(prepared, mode="exec")
    result_text = None
    body = module.body
    stdout_capture = io.StringIO()
    stderr_capture = io.StringIO()
    with contextlib.redirect_stdout(stdout_capture), contextlib.redirect_stderr(stderr_capture):
        if body and isinstance(body[-1], ast.Expr):
            prefix = ast.Module(body=body[:-1], type_ignores=[])
            suffix = ast.Expression(body[-1].value)
            if prefix.body:
                exec(compile(prefix, "<medit-sage-notebook>", "exec"), namespace)
            value = eval(compile(suffix, "<medit-sage-notebook>", "eval"), namespace)
            if value is not None:
                result_text = repr(value)
        else:
            exec(compile(module, "<medit-sage-notebook>", "exec"), namespace)
    return stdout_capture.getvalue(), stderr_capture.getvalue(), result_text


def main():
    namespace = {"__name__": "__main__"}
    for name, value in globals().items():
        if not name.startswith("_"):
            namespace[name] = value

    write_frame("ready", 0, "")
    while True:
        frame = read_frame()
        if frame is None:
            break
        event, cell_id, code = frame
        if event != "exec":
            continue

        write_frame("started", cell_id, "")
        try:
            stdout_text, stderr_text, result_text = execute_cell(namespace, code)
            if stdout_text:
                write_frame("stdout", cell_id, stdout_text)
            if stderr_text:
                write_frame("stderr", cell_id, stderr_text)
            if result_text:
                write_frame("result", cell_id, result_text)
            write_frame("done", cell_id, "ok")
        except Exception:
            write_frame("error", cell_id, traceback.format_exc())
            write_frame("done", cell_id, "error")


if __name__ == "__main__":
    main()
