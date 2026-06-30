#!/usr/bin/env python3
"""统计 client.log 中不同日志类型的输出数量。"""

from __future__ import annotations

import argparse
import collections
import re
from pathlib import Path
from typing import Iterable


DEFAULT_LOG = Path("build/bin/Release/log/client.log")
LOG_PREFIX_RE = re.compile(
    r"^\[[0-9-]+ [0-9:.]+\]\s+\[(?P<level>[A-Za-z]+)\]\s*(?P<message>.*)$"
)
EVENT_RE = re.compile(r"(?:^|\s)event=(?P<event>[A-Za-z0-9_:-]+)(?:\s|$)")
KEY_VALUE_PREFIX_RE = re.compile(r"^(?P<key>[A-Za-z_][A-Za-z0-9_]*)=")


def classify_line(line: str) -> str:
    match = LOG_PREFIX_RE.match(line.rstrip("\n"))
    if not match:
        return "unparsed"

    message = match.group("message").strip()
    event_match = EVENT_RE.search(message)
    if event_match:
        return f"event={event_match.group('event')}"

    if not message:
        return f"level={match.group('level').lower()}"

    if ":" in message:
        prefix = message.split(":", 1)[0].strip()
        if prefix:
            return prefix

    key_value_match = KEY_VALUE_PREFIX_RE.match(message)
    if key_value_match:
        return key_value_match.group("key")

    return message


def rank_log_types(lines: Iterable[str]) -> list[tuple[str, int]]:
    counts: collections.Counter[str] = collections.Counter()
    first_seen: dict[str, int] = {}

    for index, line in enumerate(lines):
        log_type = classify_line(line)
        counts[log_type] += 1
        first_seen.setdefault(log_type, index)

    return sorted(counts.items(), key=lambda item: (-item[1], first_seen[item[0]]))


def iter_log_lines(path: Path) -> Iterable[str]:
    with path.open("r", encoding="utf-8", errors="replace") as log_file:
        yield from log_file


def print_ranking(ranking: list[tuple[str, int]], *, top: int | None) -> None:
    selected = ranking[:top] if top is not None else ranking
    total = sum(count for _, count in ranking)
    width = max((len(str(count)) for _, count in selected), default=5)

    print(f"{'rank':>4}  {'count':>{width}}  percent  type")
    for index, (log_type, count) in enumerate(selected, start=1):
        percent = (count / total * 100) if total else 0.0
        print(f"{index:>4}  {count:>{width}}  {percent:6.2f}%  {log_type}")
    print(f"\ntotal_lines={total} unique_types={len(ranking)}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="按日志类型统计 client.log 输出数量，并按数量倒序排名。"
    )
    parser.add_argument(
        "log_path",
        nargs="?",
        type=Path,
        default=DEFAULT_LOG,
        help=f"日志文件路径，默认 {DEFAULT_LOG}",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=None,
        help="只显示前 N 个类型，默认显示全部。",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.top is not None and args.top < 1:
        raise SystemExit("--top 必须大于 0")
    if not args.log_path.is_file():
        raise SystemExit(f"日志文件不存在: {args.log_path}")

    ranking = rank_log_types(iter_log_lines(args.log_path))
    print_ranking(ranking, top=args.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
