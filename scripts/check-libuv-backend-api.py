#!/usr/bin/env python3

import re
import sys
from pathlib import Path


FORBIDDEN_LITERAL = (
    "sys/socket.h",
    "sys/epoll.h",
    "sys/event.h",
    "unistd.h",
    "windows.h",
    "__linux__",
    "__APPLE__",
    "_WIN32",
    "WIN32",
    "_DARWIN",
    "uv_fileno",
)
FORBIDDEN_CALL = re.compile(
    r"(?<![A-Za-z0-9_])"
    r"(accept|accept4|bind|connect|kevent|kqueue|socket|close|closesocket|"
    r"shutdown|read|write|"
    r"readv|writev|recv|recvfrom|send|sendto|poll|select|futex|fcntl|"
    r"ioctl|setsockopt|getsockopt|listen|dup|dup2|dup3|"
    r"epoll_[A-Za-z0-9_]*|pthread_[A-Za-z0-9_]*)\s*\("
)
PROJECT_CALL = re.compile(
    r"(?<![A-Za-z0-9_])(Tq[A-Za-z0-9_]*(?:::[A-Za-z0-9_]+)?)\s*\("
)
APPROVED_PROJECT_CALLS = frozenset((
    "TqAllocateRelayBuffer",
    "TqApplyRelayPoolBudget",
    "TqActiveRelayJsonValue",
    "TqAppendJsonString",
    "TqAppendNeutralRelayMetricsJson",
    "TqAppendRelayMetricsJson",
    "TqDumpMemoryAllocatorStatsToLog",
    "TqFormatMemoryAllocatorStatsLine",
    "TqGetActiveRelayCount",
    "TqJsonEscape",
    "TqMemoryAllocatorStatsFromMimallocStats",
    "TqMemoryAllocatorStatsJson",
    "TqNonNegativeInt64",
    "TqNonNegativeSumInt64",
    "TqRelayAccountingDuplicateReleaseCount",
    "TqRelayActiveRelayJson",
    "TqRelayActiveRelaysJson",
    "TqRelayCapabilitiesJsonValue",
    "TqRelayControlGenerationMismatchCount",
    "TqRelayControlStopSignaledCount",
    "TqRelayBackendReleaseReady",
    "TqRelayLinuxFastPathEnabled",
    "TqRelayMetricsFieldsJson",
    "TqRelayRegisterActive",
    "TqRelaySetTraceContext",
    "TqRelayStart",
    "TqRelayStartManaged",
    "TqRelayStartQuicReceiveSink",
    "TqRelayStartQuicReceiveSinkManaged",
    "TqRelayStop",
    "TqRelayUnregisterActive",
    "TqRelayWorkerDetailJson",
    "TqRelayWorkerJsonValue",
    "TqRelayWorkersJson",
    "TqSnapshotActiveRelays",
    "TqSnapshotMemoryAllocatorStats",
    "TqSnapshotRelayAllocStats",
    "TqSnapshotRelayMetrics",
    "TqSnapshotRelayWorkers",
    "TqStreamLifetime::SnapshotTerminalRetentions",
    "TqTerminalMetricsSnapshot",
    "TqTerminalReleaseReady",
    "TqRecordTerminalHandoffCompleted",
    "TqRecordTerminalHandoffFailed",
    "TqRecordTerminalHandoffStarted",
))
SOURCE_SUFFIXES = frozenset((".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"))
BACKEND_MACRO = "TCPQUIC_RELAY_BACKEND_LIBUV"
PLATFORM_MACROS = ("__linux__", "__APPLE__", "_WIN32", "WIN32", "_DARWIN")
PLATFORM_SYMBOL_TABLES = (
    {"__linux__": 1, "__APPLE__": 0, "_WIN32": 0, "WIN32": 0, "_DARWIN": 0},
    {"__linux__": 0, "__APPLE__": 1, "_WIN32": 0, "WIN32": 0, "_DARWIN": 1},
    {"__linux__": 0, "__APPLE__": 0, "_WIN32": 1, "WIN32": 1, "_DARWIN": 0},
)
UNKNOWN = None


class PreprocessorExpression:
    TOKEN = re.compile(
        r"\s*(defined\b|&&|\|\||==|!=|!|\(|\)|[A-Za-z_][A-Za-z0-9_]*|[0-9]+)"
    )

    def __init__(self, expression, symbols):
        expression = re.sub(r"/\*.*?\*/|//.*", "", expression)
        self.tokens = []
        position = 0
        while position < len(expression):
            match = self.TOKEN.match(expression, position)
            if not match:
                if expression[position:].strip():
                    raise ValueError("unsupported preprocessor expression")
                break
            self.tokens.append(match.group(1))
            position = match.end()
        self.position = 0
        self.symbols = symbols

    def parse(self):
        value = self.parse_or()
        if self.position != len(self.tokens):
            raise ValueError("trailing preprocessor tokens")
        return value

    def peek(self, token=None):
        if self.position >= len(self.tokens):
            return False if token is not None else None
        current = self.tokens[self.position]
        return current == token if token is not None else current

    def take(self, token=None):
        current = self.peek()
        if current is None or (token is not None and current != token):
            raise ValueError("unexpected preprocessor token")
        self.position += 1
        return current

    @staticmethod
    def logical_not(value):
        return UNKNOWN if value is UNKNOWN else int(value == 0)

    @staticmethod
    def logical_and(left, right):
        if left == 0 or right == 0:
            return 0
        if left is UNKNOWN or right is UNKNOWN:
            return UNKNOWN
        return 1

    @staticmethod
    def logical_or(left, right):
        if (left is not UNKNOWN and left != 0) or (right is not UNKNOWN and right != 0):
            return 1
        if left is UNKNOWN or right is UNKNOWN:
            return UNKNOWN
        return 0

    def parse_or(self):
        value = self.parse_and()
        while self.peek("||"):
            self.take("||")
            value = self.logical_or(value, self.parse_and())
        return value

    def parse_and(self):
        value = self.parse_equality()
        while self.peek("&&"):
            self.take("&&")
            value = self.logical_and(value, self.parse_equality())
        return value

    def parse_equality(self):
        value = self.parse_unary()
        while self.peek() in {"==", "!="}:
            operator = self.take()
            right = self.parse_unary()
            if value is UNKNOWN or right is UNKNOWN:
                value = UNKNOWN
            else:
                value = int((value == right) == (operator == "=="))
        return value

    def parse_unary(self):
        if self.peek("!"):
            self.take("!")
            return self.logical_not(self.parse_unary())
        if self.peek("defined"):
            self.take("defined")
            parenthesized = self.peek("(")
            if parenthesized:
                self.take("(")
            name = self.take()
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                raise ValueError("defined requires an identifier")
            if parenthesized:
                self.take(")")
            return self.symbols.get(name, UNKNOWN)
        if self.peek("("):
            self.take("(")
            value = self.parse_or()
            self.take(")")
            return value
        token = self.take()
        if token.isdigit():
            return int(token)
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", token):
            return self.symbols.get(token, UNKNOWN)
        raise ValueError("unsupported preprocessor primary")


def preprocessor_symbols(platform_override=None):
    symbols = {BACKEND_MACRO: 1, **PLATFORM_SYMBOL_TABLES[0]}
    if platform_override is not None:
        symbols.update(platform_override)
    return symbols


def evaluate_condition(kind, expression, symbols=None):
    symbols = preprocessor_symbols() if symbols is None else symbols
    try:
        if kind == "ifdef":
            return symbols.get(expression.strip(), UNKNOWN)
        if kind == "ifndef":
            value = symbols.get(expression.strip(), UNKNOWN)
            return UNKNOWN if value is UNKNOWN else int(value == 0)
        return PreprocessorExpression(expression, symbols).parse()
    except ValueError:
        return UNKNOWN


def platform_sensitive(kind, expression):
    mentioned = [name for name in PLATFORM_MACROS if name in expression]
    if not mentioned:
        return False
    results = []
    for platform_symbols in PLATFORM_SYMBOL_TABLES:
        results.append(evaluate_condition(
            kind, expression, preprocessor_symbols(platform_symbols)))
    if any(result is UNKNOWN for result in results):
        return True
    return len(set(results)) != 1


def contains_forbidden_literal(line, token):
    if token in PLATFORM_MACROS:
        return re.search(
            rf"(?<![A-Za-z0-9_]){re.escape(token)}(?![A-Za-z0-9_])",
            line,
        ) is not None
    return token in line


def is_test_harness(path):
    parts = path.resolve().parts
    return any(parts[index:index + 2] == ("src", "unittest") for index in range(len(parts) - 1))


def iter_sources(arguments):
    for argument in arguments:
        path = Path(argument)
        if path.is_dir():
            for child in sorted(path.rglob("*")):
                if child.is_file() and child.suffix.lower() in SOURCE_SUFFIXES and not is_test_harness(child):
                    yield child
        elif path.is_file() and not is_test_harness(path):
            yield path
        elif not path.exists():
            raise FileNotFoundError(path)


def libuv_branch_lines(lines):
    active = True
    stack = []
    index = 0
    while index < len(lines):
        line_number = index + 1
        line = lines[index]
        directive_text = line
        index += 1
        if line.lstrip().startswith("#"):
            while directive_text.rstrip().endswith("\\") and index < len(lines):
                directive_text = directive_text.rstrip()[:-1] + " " + lines[index].lstrip()
                index += 1
        stripped = directive_text.lstrip()
        directive = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", stripped)
        if directive:
            kind, expression = directive.group(1), directive.group(2)
            if kind in {"if", "ifdef", "ifndef"}:
                parent = active
                value = evaluate_condition(kind, expression)
                if parent and platform_sensitive(kind, expression):
                    yield line_number, directive_text, True
                taking = parent and value != 0
                remaining = parent and value != 1
                stack.append([parent, remaining])
                active = taking
            elif kind == "elif" and stack:
                parent, remaining = stack[-1]
                value = evaluate_condition(kind, expression)
                if remaining and platform_sensitive(kind, expression):
                    yield line_number, directive_text, True
                active = remaining and value != 0
                stack[-1][1] = remaining and value != 1
            elif kind == "else" and stack:
                _, remaining = stack[-1]
                active = remaining
                stack[-1][1] = False
            elif kind == "endif" and stack:
                parent, _ = stack.pop()
                active = parent
            continue
        if active:
            yield line_number, line, True


def scan(path, libuv_branches=False):
    violations = []
    lines = path.read_text(encoding="utf-8").splitlines()
    selected = (
        libuv_branch_lines(lines)
        if libuv_branches
        else ((line_number, line, True) for line_number, line in enumerate(lines, 1))
    )
    for line_number, line, enforce_backend_boundary in selected:
        if enforce_backend_boundary:
            for token in FORBIDDEN_LITERAL:
                if contains_forbidden_literal(line, token):
                    violations.append((line_number, token))
        violations.extend((line_number, match.group(1)) for match in FORBIDDEN_CALL.finditer(line))
        if enforce_backend_boundary:
            for match in PROJECT_CALL.finditer(line):
                name = match.group(1)
                if not name.startswith("TqUv") and name not in APPROVED_PROJECT_CALLS:
                    violations.append((line_number, name))
    return violations


def main(arguments):
    if not arguments:
        print("usage: check-libuv-backend-api.py SOURCE...", file=sys.stderr)
        return 2

    libuv_branches = arguments[0] == "--libuv-branches"
    if libuv_branches:
        arguments = arguments[1:]
        if not arguments:
            print("--libuv-branches requires SOURCE...", file=sys.stderr)
            return 2
    failed = False
    try:
        sources = iter_sources(arguments)
        for source in sources:
            for line_number, token in scan(source, libuv_branches):
                print(f"{source}:{line_number}:{token}")
                failed = True
    except (OSError, UnicodeError) as error:
        print(error, file=sys.stderr)
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
