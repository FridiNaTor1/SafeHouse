#!/usr/bin/env python3
"""The launcher's languages (launcher/lang/*.json, launcher/l10n.h).

Checks: every tr() / trf() / N_() / trv() literal in the launcher sources (plus the Sly 2 action names the launcher
shows, platform/sly2_action_bindings.h) is a key of en.json and every en.json key is used; every language file has
exactly en.json's keys, no empty text, the same printf specifiers in the same order and the same leading / trailing
spaces and line breaks; then, with the launcher binary: --l10n-check (every embedded language loads, the fonts have
every character it prints) and the system-language detection (LANG / LC_* / LANGUAGE) in private HOME / XDG folders.

  test-launcher-l10n.py [LAUNCHER]       run the checks (the binary ones only with LAUNCHER)
  test-launcher-l10n.py --write-en       rewrite en.json from the sources (keeps nothing else)
  test-launcher-l10n.py --status         per language: missing and unused keys
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LANG = ROOT / "launcher" / "lang"
SOURCES = [p for d in ("launcher", "launcher/ui", "launcher/collection") for p in sorted((ROOT / d).glob("*.h"))] + [
    ROOT / "launcher" / "main.cpp"]
FORMAT = re.compile(r"%(?:%|[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfFgGaAcspn])")


def unescape(body):
    return bytes(body, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


def literals(text, start):
    """The string-literal sequence at text[start:] (adjacent literals concatenate); returns (value, end)."""
    out, i = [], start
    while True:
        m = re.compile(r'\s*"((?:[^"\\\n]|\\.)*)"').match(text, i)
        if not m:
            return ("".join(out) if out else None), i
        out.append(unescape(m.group(1)))
        i = m.end()


def strip_comments(text):
    # Line comments outside strings and char literals; keeps offsets stable enough for the regex scan.
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def extract():
    keys = {}
    for path in SOURCES:
        text = strip_comments(path.read_text(encoding="utf-8"))
        for m in re.finditer(r"(?<![\w.>])(tr|trf|N_|trv)\(", text):
            if m.group(1) == "trv":
                depth, i = 1, m.end()
                while depth and i < len(text):
                    if text[i] == '"':
                        value, i = literals(text, i)
                        keys.setdefault(value, path.name)
                        continue
                    depth += {"(": 1, ")": -1}.get(text[i], 0)
                    i += 1
                continue
            value, _ = literals(text, m.end())
            if value is None:
                continue
            if "##" in value and not value.startswith("##"):
                value = value[:value.index("##")]
            keys.setdefault(value, path.name)
    actions = (ROOT / "platform" / "sly2_action_bindings.h").read_text(encoding="utf-8")
    for m in re.finditer(r'X\(\w+, "[^"]*", "([^"]*)", "([^"]*)"', actions):
        keys.setdefault(m.group(1), "sly2_action_bindings.h")
        keys.setdefault(m.group(2), "sly2_action_bindings.h")
    brand = (ROOT / "platform" / "brand.h").read_text(encoding="utf-8")
    for m in re.finditer(r'\b(?:credit|discordLabel|patreonLabel|sly3Notice) =\s*((?:"(?:[^"\\]|\\.)*"\s*)+);', brand):
        keys.setdefault(literals(m.group(1), 0)[0], "brand.h")
    keys.pop("", None)
    return keys


def load(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    return data["language"], data["strings"]


def shape(s):
    return FORMAT.findall(s), s[:1].isspace(), s[-1:].isspace(), s.count("\n")


def check_files(keys):
    errors = []
    en_meta, en = load(LANG / "en.json")
    for k in keys:
        if k not in en:
            errors.append(f"en.json: missing key used in {keys[k]}: {k!r}")
    for k in en:
        if k not in keys:
            errors.append(f"en.json: key not used by the sources: {k!r}")
        if en[k] != k:
            errors.append(f"en.json: text differs from its key: {k!r}")
    codes = set()
    for path in sorted(LANG.glob("*.json")):
        meta, strings = load(path)
        code = path.stem
        if meta.get("code") != code:
            errors.append(f"{path.name}: language.code {meta.get('code')!r} is not the file name")
        for field in ("name", "english"):
            if not meta.get(field):
                errors.append(f"{path.name}: language.{field} missing")
        codes.add(code)
        if code == "en":
            continue
        for k in en:
            if k not in strings:
                errors.append(f"{path.name}: missing {k!r}")
            elif not strings[k].strip():
                errors.append(f"{path.name}: empty text for {k!r}")
            elif shape(strings[k])[0] != shape(k)[0]:
                errors.append(f"{path.name}: format specifiers {shape(strings[k])[0]} differ from {shape(k)[0]} in {k!r}")
            elif shape(strings[k])[1:] != shape(k)[1:]:
                errors.append(f"{path.name}: leading / trailing spaces or line breaks differ in {k!r}: {strings[k]!r}")
        for k in strings:
            if k not in en:
                errors.append(f"{path.name}: unknown key {k!r}")
    return errors, codes


def private_env(home, **extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("LC_", "LANG", "SAFEHOUSE_LANGUAGE"))}
    env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / "config"), XDG_DATA_HOME=str(home / "data"),
               XDG_STATE_HOME=str(home / "state"), XDG_CACHE_HOME=str(home / "cache"))
    env.update(extra)
    return env


def check_binary(launcher, codes):
    errors = []
    with tempfile.TemporaryDirectory(prefix="safehouse-l10n-") as tmp:
        home = Path(tmp)
        r = subprocess.run([launcher, "--l10n-check"], env=private_env(home), capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            errors.append(f"--l10n-check failed ({r.returncode}): {r.stdout}{r.stderr}")
        listed = set()
        for line in r.stdout.splitlines():
            if line.startswith("language "):
                listed.add(line.split()[1])
        if listed != codes:
            errors.append(f"--l10n-check lists {sorted(listed)}, the files are {sorted(codes)}")
        cases = [
            ({"LANG": "fr_FR.UTF-8"}, "fr"),
            ({"LANG": "de_AT.UTF-8"}, "de"),
            ({"LANG": "pt_BR.UTF-8"}, "pt"),
            ({"LANG": "nb_NO.UTF-8"}, "nb"),
            ({"LANG": "nn_NO.UTF-8"}, "nb"),
            ({"LANG": "no_NO"}, "nb"),
            ({"LANG": "ja_JP.UTF-8"}, "ja"),
            ({"LANG": "sv_SE.UTF-8", "LC_MESSAGES": "fi_FI.UTF-8"}, "fi"),
            ({"LANG": "sv_SE.UTF-8", "LC_ALL": "da_DK.UTF-8"}, "da"),
            ({"LANG": "en_US.UTF-8", "LANGUAGE": "xx:it:fr"}, "it"),
            ({"LANG": "C", "LANGUAGE": "fr"}, "en"),
            ({"LANG": "tlh_XX.UTF-8"}, "en"),
            ({"LANG": "C"}, "en"),
            ({}, "en"),
            ({"LANG": "fr_FR.UTF-8", "SAFEHOUSE_LANGUAGE": "es"}, "es"),
        ]
        for extra, want in cases:
            if want not in codes:
                continue
            r = subprocess.run([launcher, "--l10n-detect"], env=private_env(home, **extra), capture_output=True, text=True, timeout=30)
            got = r.stdout.strip()
            if got != want:
                errors.append(f"--l10n-detect with {extra}: {got!r}, expected {want!r}")
        # The setting: launcher.json "language" wins over the system language; "system" follows it.
        config = home / "config" / "safehouse"
        r = subprocess.run([launcher, "--l10n-detect", "--setting", "de"], env=private_env(home, LANG="fr_FR.UTF-8"),
                           capture_output=True, text=True, timeout=30)
        if r.stdout.strip() != "de":
            errors.append(f"--l10n-detect --setting de: {r.stdout.strip()!r}")
        r = subprocess.run([launcher, "--l10n-detect", "--setting", "system"], env=private_env(home, LANG="fr_FR.UTF-8"),
                           capture_output=True, text=True, timeout=30)
        if r.stdout.strip() != "fr":
            errors.append(f"--l10n-detect --setting system: {r.stdout.strip()!r}")
        if config.exists() or (home / ".config").exists():
            errors.append("--l10n-detect created profile folders")
    return errors


def main():
    args = sys.argv[1:]
    keys = extract()
    if args[:1] == ["--write-en"]:
        path = LANG / "en.json"
        meta = load(path)[0] if path.exists() else {"code": "en", "name": "English", "english": "English", "script": "latin"}
        path.write_text(json.dumps({"language": meta, "strings": {k: k for k in sorted(keys, key=str.casefold)}},
                                   ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        print(f"en.json: {len(keys)} keys")
        return 0
    if args[:1] == ["--status"]:
        _, en = load(LANG / "en.json")
        for path in sorted(LANG.glob("*.json")):
            _, strings = load(path)
            missing = [k for k in keys if k not in strings]
            unused = [k for k in strings if k not in keys]
            print(f"{path.stem}: {len(strings)} strings, {len(missing)} missing, {len(unused)} unused")
            for k in missing[:int(os.environ.get("SHOW", "0"))]:
                print("   missing", repr(k))
        return 0
    errors, codes = check_files(keys)
    if args:
        errors += check_binary(args[0], codes)
    for e in errors:
        print(e)
    print(f"{len(keys)} keys, {len(codes)} languages: {'FAIL' if errors else 'PASS'} ({len(errors)} problems)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
