#!/usr/bin/env python3
"""
Simple menu tool for file transfer between Win11 local/shared folders and Cloud Server K.

Current feature set:
1) SSH file transfer via pscp (PuTTY)

Notes for VM -> Cloud path:
- VM to VM shared folder and VM shared folder to Win11 shared folder are handled by VM sharing mechanism.
- This script handles the final hop: Win11 shared folder -> Cloud Server K.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass
class SshConfig:
    host: str = "47.98.20.225"
    user: str = "root"
    key_file: str = r"E:\myssh\hangzhou.ppk"
    pscp_path: str = r"C:\Program Files\PuTTY\pscp.exe"


DEFAULT_WIN_SHARED_DIR = r"F:\vbox_share"


def prompt(text: str, default: str | None = None) -> str:
    suffix = f" [{default}]" if default else ""
    value = input(f"{text}{suffix}: ").strip()
    return value if value else (default or "")


def choose(prompt_text: str, options: list[str]) -> int:
    print(prompt_text)
    for idx, opt in enumerate(options, start=1):
        print(f"  {idx}. {opt}")
    while True:
        raw = input("Select: ").strip()
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return int(raw)
        print("Invalid selection, try again.")


def file_or_dir_flag(path_text: str) -> list[str]:
    p = Path(path_text)
    if p.exists() and p.is_dir():
        return ["-r"]
    return []


def run_cmd(cmd: list[str]) -> int:
    print("\nExecuting command:")
    print(" ".join(cmd))
    try:
        result = subprocess.run(cmd, check=False)
        return result.returncode
    except FileNotFoundError as exc:
        print(f"Command not found: {exc}")
        return 127


def ensure_pscp_path(default_path: str) -> str:
    user_value = prompt("pscp.exe path", default_path)
    if Path(user_value).exists():
        return user_value

    found = shutil.which("pscp") or shutil.which("pscp.exe")
    if found:
        print(f"Given path not found, fallback to PATH command: {found}")
        return found

    print("pscp executable is not found. Install PuTTY or provide valid pscp.exe path.")
    return user_value


def build_remote(user: str, host: str, remote_path: str) -> str:
    return f"{user}@{host}:{remote_path}"


def transfer_menu() -> None:
    cfg = SshConfig()

    print("\nSSH Transfer Config")
    cfg.host = prompt("Cloud host", cfg.host)
    cfg.user = prompt("Cloud user", cfg.user)
    cfg.key_file = prompt("Private key (.ppk)", cfg.key_file)
    cfg.pscp_path = ensure_pscp_path(cfg.pscp_path)

    mode = choose(
        "\nTransfer modes:",
        [
            "Win local/shared file(or folder) -> Cloud K",
            "Cloud K file(or folder) -> Win local/shared",
            "VM file(or folder) -> shared folder -> Cloud K (final hop)",
            "Back",
        ],
    )

    if mode == 4:
        return

    if mode == 1:
        src = prompt("Source path on Win11")
        dst = prompt("Destination directory on Cloud", "/root")
        cmd = [
            cfg.pscp_path,
            "-batch",
            "-i",
            cfg.key_file,
            *file_or_dir_flag(src),
            src,
            build_remote(cfg.user, cfg.host, dst),
        ]

    elif mode == 2:
        src_remote = prompt("Source path on Cloud", "/root")
        dst_local = prompt("Destination path on Win11")
        recursive = prompt("Recursive copy? (y/n)", "n").lower().startswith("y")
        rec_flag = ["-r"] if recursive else []
        cmd = [
            cfg.pscp_path,
            "-batch",
            "-i",
            cfg.key_file,
            *rec_flag,
            build_remote(cfg.user, cfg.host, src_remote),
            dst_local,
        ]

    else:
        print("\nVM transfer guidance:")
        print("1) Put VM files under the VM shared folder.")
        print("2) Ensure Win11 sees the same file under its shared folder path.")
        shared_dir = prompt("Win11 shared folder path", DEFAULT_WIN_SHARED_DIR)
        relative = prompt("Relative path in shared folder (e.g. data/a.txt)")
        src = str(Path(shared_dir) / relative)
        dst = prompt("Destination directory on Cloud", "/root")
        cmd = [
            cfg.pscp_path,
            "-batch",
            "-i",
            cfg.key_file,
            *file_or_dir_flag(src),
            src,
            build_remote(cfg.user, cfg.host, dst),
        ]

    code = run_cmd(cmd)
    if code == 0:
        print("\nTransfer succeeded.")
    else:
        print(f"\nTransfer failed, exit code: {code}")


def main() -> None:
    while True:
        choice = choose(
            "\nMenu List:",
            [
                "SSH file transfer",
                "Exit",
            ],
        )

        if choice == 1:
            transfer_menu()
        else:
            print("Bye.")
            break


if __name__ == "__main__":
    main()
