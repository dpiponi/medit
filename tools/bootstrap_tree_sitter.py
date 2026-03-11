#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd, cwd=None):
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def load_manifest(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def ensure_repo(repo_url: str, ref: str, checkout_dir: Path):
    if not (checkout_dir / ".git").exists():
        run(["git", "clone", repo_url, str(checkout_dir)])
    else:
        run(["git", "fetch", "--tags", "origin"], cwd=checkout_dir)
    run(["git", "checkout", "--force", ref], cwd=checkout_dir)


def shared_library_suffix():
    if sys.platform == "darwin":
        return ".dylib"
    return ".so"


def shared_link_command(compiler: str, objects, output: Path):
    if sys.platform == "darwin":
        return [compiler, "-dynamiclib", *objects, "-o", str(output)]
    return [compiler, "-shared", *objects, "-o", str(output)]


def build_runtime(runtime_repo: Path, output_path: Path, build_root: Path, cc: str):
    output_path.parent.mkdir(parents=True, exist_ok=True)
    obj_path = build_root / "runtime" / "libtree-sitter.o"
    obj_path.parent.mkdir(parents=True, exist_ok=True)
    run([
        cc,
        "-fPIC",
        "-I",
        str(runtime_repo / "lib" / "include"),
        "-I",
        str(runtime_repo / "lib" / "src"),
        "-c",
        str(runtime_repo / "lib" / "src" / "lib.c"),
        "-o",
        str(obj_path),
    ])
    run(shared_link_command(cc, [str(obj_path)], output_path))


def compile_source(compiler: str, source_path: Path, output_path: Path, include_dir: Path):
    output_path.parent.mkdir(parents=True, exist_ok=True)
    run([
        compiler,
        "-fPIC",
        "-I",
        str(include_dir),
        "-c",
        str(source_path),
        "-o",
        str(output_path),
    ])


def build_language(language, repo_dir: Path, config_root: Path, build_root: Path, cc: str, cxx: str):
    source_root = repo_dir / language.get("source_subdir", "")
    parser_path = source_root / "src" / "parser.c"
    if not parser_path.exists():
        raise FileNotFoundError(f"missing parser source: {parser_path}")

    include_dir = source_root / "src"
    grammar_dir = config_root / "grammars"
    query_dir = config_root / "queries" / language["name"]
    grammar_dir.mkdir(parents=True, exist_ok=True)
    query_dir.mkdir(parents=True, exist_ok=True)

    objects = []
    parser_obj = build_root / "objects" / language["name"] / "parser.o"
    compile_source(cc, parser_path, parser_obj, include_dir)
    objects.append(str(parser_obj))

    linker = cc
    scanner_path = language.get("scanner_path")
    if scanner_path is None:
        for candidate in ("src/scanner.c", "src/scanner.cc", "src/scanner.cpp", "src/scanner.cxx"):
            if (source_root / candidate).exists():
                scanner_path = candidate
                break
    if scanner_path:
        scanner_source = source_root / scanner_path
        if not scanner_source.exists():
            raise FileNotFoundError(f"missing scanner source: {scanner_source}")
        scanner_obj = build_root / "objects" / language["name"] / "scanner.o"
        scanner_compiler = cxx if scanner_source.suffix in {".cc", ".cpp", ".cxx"} else cc
        compile_source(scanner_compiler, scanner_source, scanner_obj, include_dir)
        objects.append(str(scanner_obj))
        if scanner_compiler == cxx:
            linker = cxx

    output_name = f"libtree-sitter-{language['name']}{shared_library_suffix()}"
    output_path = grammar_dir / output_name
    run(shared_link_command(linker, objects, output_path))

    query_source = source_root / language["query_path"]
    if not query_source.exists():
        raise FileNotFoundError(f"missing highlights query: {query_source}")
    shutil.copyfile(query_source, query_dir / "highlights.scm")

    return {
        "name": language["name"],
        "extensions": language["extensions"],
        "grammar_path": f"grammars/{output_name}",
        "symbol_name": language["symbol_name"],
        "highlights_path": f"queries/{language['name']}/highlights.scm",
    }


def write_syntax_config(config_root: Path, languages):
    syntax_config = {"languages": languages}
    path = config_root / "syntax.json"
    with path.open("w", encoding="utf-8") as handle:
        json.dump(syntax_config, handle, indent=2)
        handle.write("\n")


def parse_args():
    parser = argparse.ArgumentParser(description="Build local tree-sitter runtime and grammar assets for medit.")
    parser.add_argument(
        "--manifest",
        default="tools/tree_sitter_languages.json",
        help="Path to the tree-sitter language manifest.",
    )
    parser.add_argument(
        "--config-root",
        default=".config/medit",
        help="Directory where medit runtime assets should be written.",
    )
    parser.add_argument(
        "--build-root",
        default="build/tree-sitter",
        help="Directory for temporary clones and object files.",
    )
    parser.add_argument(
        "--languages",
        default="",
        help="Optional comma-separated language names to build.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    manifest_path = Path(args.manifest).resolve()
    config_root = Path(args.config_root).resolve()
    build_root = Path(args.build_root).resolve()
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")

    manifest = load_manifest(manifest_path)
    requested = {name.strip() for name in args.languages.split(",") if name.strip()}

    runtime_dir = build_root / "sources" / "tree-sitter"
    ensure_repo(manifest["runtime"]["repo"], manifest["runtime"]["ref"], runtime_dir)

    runtime_output = config_root / f"libtree-sitter{shared_library_suffix()}"
    build_runtime(runtime_dir, runtime_output, build_root, cc)

    generated_languages = []
    for language in manifest["languages"]:
        if requested and language["name"] not in requested:
            continue
        repo_dir = build_root / "sources" / f"tree-sitter-{language['name']}"
        ensure_repo(language["repo"], language["ref"], repo_dir)
        generated_languages.append(build_language(language, repo_dir, config_root, build_root, cc, cxx))

    if requested:
        selected_names = {language["name"] for language in generated_languages}
        missing = sorted(requested - selected_names)
        if missing:
            raise SystemExit(f"unknown language(s): {', '.join(missing)}")

    write_syntax_config(config_root, generated_languages)
    print(f"Wrote syntax config to {config_root / 'syntax.json'}")


if __name__ == "__main__":
    main()
