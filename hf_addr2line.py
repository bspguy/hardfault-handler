#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <firmware.elf> <log-file>", file=sys.stderr)
        return 1

    elf_path = Path(sys.argv[1])
    log_path = Path(sys.argv[2])

    if not elf_path.is_file():
        print(f"ELF not found: {elf_path}", file=sys.stderr)
        return 1
    if not log_path.is_file():
        print(f"Log not found: {log_path}", file=sys.stderr)
        return 1

    log_text = log_path.read_text(errors='ignore')

    # Find lines like: HF_ADDR PC=0x08001234 LR=0x08000F00
    pattern = re.compile(r'HF_ADDR\s+PC=0x([0-9a-fA-F]{8})\s+LR=0x([0-9a-fA-F]{8})')

    addrs = []
    for m in pattern.finditer(log_text):
        pc_hex, lr_hex = m.groups()
        addrs.append(int(pc_hex, 16))
        addrs.append(int(lr_hex, 16))

    # De-duplicate while preserving order
    seen = set()
    unique_addrs = []
    for a in addrs:
        if a not in seen:
            seen.add(a)
            unique_addrs.append(a)

    if not unique_addrs:
        print("No HF_ADDR lines found in log.", file=sys.stderr)
        return 0

    print(f"Found {len(unique_addrs)} unique addresses. Resolving with addr2line...\n")

    def addr2line(addr: int) -> str:
        """Call arm-none-eabi-addr2line -f -C -e ELF 0xADDR and capture output."""
        cmd = [
            'arm-none-eabi-addr2line',
            '-f',      # show function name
            '-C',      # demangle
            '-e', str(elf_path),
            f'0x{addr:08X}',
        ]
        try:
            out = subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as e:
            return f'<addr2line error: {e.output.strip()}>'
        return out.strip()

    for addr in unique_addrs:
        print(f'0x{addr:08X}:')
        print(addr2line(addr))
        print()

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
