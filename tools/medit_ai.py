#!/usr/bin/env python3

import argparse
import json
import os
import ssl
import sys
import urllib.error
import urllib.request

try:
    import certifi
except ImportError:
    certifi = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Selection-oriented AI helper for medit.")
    parser.add_argument("--mode", choices=("edit", "ask"), default="edit")
    parser.add_argument("--provider", choices=("openai", "mistral"))
    parser.add_argument("--model", required=True)
    parser.add_argument("--prompt", required=True)
    return parser.parse_args()


def post_json(url: str, api_key: str, payload: dict) -> dict:
    ssl_context = ssl.create_default_context(cafile=certifi.where()) if certifi is not None else None
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, context=ssl_context) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace").strip()
        raise RuntimeError(body or f"HTTP {exc.code}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(str(exc.reason)) from exc


def extract_openai_text(payload: dict) -> str:
    text = payload.get("output_text")
    if isinstance(text, str) and text.strip():
        return text.strip()
    parts: list[str] = []
    for item in payload.get("output", []) or []:
        for content in item.get("content", []) or []:
            chunk = content.get("text")
            if isinstance(chunk, str) and chunk.strip():
                parts.append(chunk.strip())
    return "\n".join(parts).strip()


def extract_mistral_text(payload: dict) -> str:
    choices = payload.get("choices") or []
    if not choices:
        return ""
    message = choices[0].get("message") or {}
    content = message.get("content")
    if isinstance(content, str):
        return content.strip()
    if isinstance(content, list):
        parts: list[str] = []
        for item in content:
            if not isinstance(item, dict):
                continue
            chunk = item.get("text")
            if isinstance(chunk, str) and chunk.strip():
                parts.append(chunk.strip())
        return "\n".join(parts).strip()
    return ""


def openai_request(model: str, prompt: str, source_text: str, mode: str) -> str:
    api_key = os.getenv("OPENAI_API_KEY", "").strip()
    if not api_key:
        raise RuntimeError("Set OPENAI_API_KEY in your environment")
    if mode == "edit":
        instructions = (
            "You are an editing assistant inside a text editor. "
            "Apply the user's instruction to the supplied text. "
            "Return only the revised text with no explanation, no markdown fences, and no preamble."
        )
        user_input = f"Instruction:\n{prompt}\n\nText to revise:\n{source_text}"
    else:
        instructions = (
            "You are an assistant inside a text editor. "
            "Answer the user's request directly and concisely. "
            "If context text is supplied, use it."
        )
        user_input = prompt if not source_text else f"Request:\n{prompt}\n\nContext:\n{source_text}"
    payload = {
        "model": model,
        "instructions": instructions,
        "input": user_input,
    }
    return extract_openai_text(post_json("https://api.openai.com/v1/responses", api_key, payload))


def mistral_request(model: str, prompt: str, source_text: str, mode: str) -> str:
    api_key = os.getenv("MISTRAL_API_KEY", "").strip()
    if not api_key:
        raise RuntimeError("Set MISTRAL_API_KEY in your environment")
    if mode == "edit":
        system_prompt = (
            "You are an editing assistant inside a text editor. "
            "Apply the user's instruction to the supplied text. "
            "Return only the revised text with no explanation, no markdown fences, and no preamble."
        )
        user_prompt = f"Instruction:\n{prompt}\n\nText to revise:\n{source_text}"
    else:
        system_prompt = (
            "You are an assistant inside a text editor. "
            "Answer the user's request directly and concisely. "
            "If context text is supplied, use it."
        )
        user_prompt = prompt if not source_text else f"Request:\n{prompt}\n\nContext:\n{source_text}"
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": 0.2,
    }
    return extract_mistral_text(post_json("https://api.mistral.ai/v1/chat/completions", api_key, payload))


def main() -> int:
    args = parse_args()
    source_text = sys.stdin.read()
    try:
        if args.provider == "openai":
            result = openai_request(args.model, args.prompt, source_text, args.mode)
        else:
            result = mistral_request(args.model, args.prompt, source_text, args.mode)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
