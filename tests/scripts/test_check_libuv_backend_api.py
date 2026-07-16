import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "scripts" / "check-libuv-backend-api.py"
LANE_FIXTURES = ROOT / "tests" / "fixtures" / "libuv_backend_api"


class CheckLibuvBackendApiTest(unittest.TestCase):
    def run_checker(self, *paths):
        return subprocess.run(
            [sys.executable, str(CHECKER), *(str(path) for path in paths)],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )

    def make_project_overlay(self, root, fragment_text):
        root = Path(root)
        for entry in ROOT.iterdir():
            if entry.name in {".git", "build", "src"}:
                continue
            os.symlink(entry, root / entry.name, target_is_directory=entry.is_dir())

        source_dir = root / "src"
        source_dir.mkdir()
        for entry in (ROOT / "src").iterdir():
            if entry.name in {"CMakeLists.txt", "cmake", "tunnel"}:
                continue
            os.symlink(entry, source_dir / entry.name, target_is_directory=entry.is_dir())
        shutil.copy2(ROOT / "src" / "CMakeLists.txt", source_dir / "CMakeLists.txt")

        tunnel_dir = source_dir / "tunnel"
        tunnel_dir.mkdir()
        for entry in (ROOT / "src" / "tunnel").iterdir():
            os.symlink(entry, tunnel_dir / entry.name, target_is_directory=entry.is_dir())

        cmake_dir = source_dir / "cmake"
        cmake_dir.mkdir()
        (cmake_dir / "libuv_relay_quic_to_tcp.cmake").write_text(
            fragment_text,
            encoding="utf-8",
        )

    def configure_overlay(self, root):
        return subprocess.run(
            [
                "cmake",
                "-S",
                str(root),
                "-B",
                str(Path(root) / "build"),
                "-DTCPQUIC_RELAY_BACKEND=libuv",
                "-DTCPQUIC_ENABLE_CRASHPAD=OFF",
            ],
            text=True,
            capture_output=True,
        )

    def test_rejects_os_headers_and_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "libuv_relay_bad.cpp"
            source.write_text(
                "#include <sys/socket.h>\nint f(){ return close(1); }\n",
                encoding="utf-8",
            )
            result = self.run_checker(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sys/socket.h", result.stdout)
        self.assertIn("close", result.stdout)

    def test_rejects_extended_socket_and_handle_escape_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "libuv_relay_escape.cpp"
            source.write_text(
                "void f(int fd) {\n"
                "  bind(fd, nullptr, 0); listen(fd, 1);\n"
                "  getsockopt(fd, 0, 0, nullptr, nullptr);\n"
                "  dup(fd); dup2(fd, 2); dup3(fd, 3, 0);\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_checker(source)

        self.assertNotEqual(result.returncode, 0)
        for token in ("bind", "listen", "getsockopt", "dup", "dup2", "dup3"):
            self.assertIn(token, result.stdout)

    def test_recursively_rejects_platform_macros(self):
        with tempfile.TemporaryDirectory() as tmp:
            nested = Path(tmp) / "relay"
            nested.mkdir()
            source = nested / "worker.cpp"
            source.write_text("#if defined(__linux__)\n#endif\n", encoding="utf-8")
            result = self.run_checker(tmp)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("worker.cpp:1:__linux__", result.stdout)

    def test_allows_libuv_api_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "libuv_relay_good.cpp"
            source.write_text(
                "void f(uv_handle_t* h, uv_stream_t* s, uv_write_t* w) {\n"
                "  uv_close(h, nullptr);\n"
                "  uv_shutdown(nullptr, s, nullptr);\n"
                "  uv_read_start(s, nullptr, nullptr);\n"
                "  uv_write(w, s, nullptr, 0, nullptr);\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_checker(source)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_native_calls_hidden_behind_project_wrappers(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "libuv_relay_wrapper.cpp"
            source.write_text(
                "void f(int fd) {\n"
                "  TqNativeClose(fd);\n"
                "  TqPlatformClose(fd);\n"
                "  TqCloseHandle(fd);\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_checker(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TqNativeClose", result.stdout)
        self.assertIn("TqPlatformClose", result.stdout)
        self.assertIn("TqCloseHandle", result.stdout)

    def test_allows_audited_project_public_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "libuv_relay_project_api.cpp"
            source.write_text(
                "void f() {\n"
                "  TqRelayRegisterActive();\n"
                "  TqRelayUnregisterActive();\n"
                "  TqUvInstallAllocator();\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_checker(source)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_libuv_branch_mode_scans_only_compiled_backend_branch(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "common_metrics.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV\n"
                "void bad() { TqPlatformClose(1); }\n"
                "#else\n"
                "void native_only() { close(1); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TqPlatformClose", result.stdout)
        self.assertNotIn(":close", result.stdout)

    def test_libuv_branch_mode_rejects_top_level_project_wrapper(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "common_metrics.cpp"
            source.write_text(
                "void common() { TqPlatformClose(1); }\n"
                "#if TCPQUIC_RELAY_BACKEND_LIBUV\n"
                "void backend() { uv_close(nullptr, nullptr); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TqPlatformClose", result.stdout)

    def test_libuv_branch_mode_rejects_platform_directive_in_compiled_view(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "common_metrics.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV\n"
                "#if defined(__linux__)\n"
                "void backend() { uv_close(nullptr, nullptr); }\n"
                "#endif\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("__linux__", result.stdout)

    def test_libuv_branch_mode_rejects_platform_in_backend_selector(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "common_metrics.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV && defined(__linux__)\n"
                "void backend() { uv_close(nullptr, nullptr); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("__linux__", result.stdout)

    def test_libuv_selector_skips_backend_equal_zero_body(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV == 0\n"
                "void native_only() { close(1); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_libuv_selector_checks_true_negated_or_platform_expression(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if !TCPQUIC_RELAY_BACKEND_LIBUV || defined(__linux__)\n"
                "void bad() { TqPlatformClose(1); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("__linux__", result.stdout)
        self.assertIn("TqPlatformClose", result.stdout)

    def test_libuv_selector_skips_backend_and_zero_dead_body(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV && 0\n"
                "void dead() { close(1); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_libuv_selector_supports_parentheses_comparisons_and_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if ((TCPQUIC_RELAY_BACKEND_LIBUV != 0) && !(1 == 0))\n"
                "void selected() { TqPlatformClose(1); }\n"
                "#elif TCPQUIC_RELAY_BACKEND_LIBUV == 1\n"
                "void unreachable() { close(1); }\n"
                "#else\n"
                "void native_only() { close(1); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TqPlatformClose", result.stdout)
        self.assertNotIn(":close", result.stdout)

    def test_libuv_selector_unknown_identifiers_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if defined(TCPQUIC_UNKNOWN_FEATURE)\n"
                "void maybe_enabled() { TqPlatformClose(1); }\n"
                "#else\n"
                "void maybe_disabled() { TqCloseHandle(1); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TqPlatformClose", result.stdout)
        self.assertIn("TqCloseHandle", result.stdout)

    def test_libuv_selector_rejects_windows_branch_from_linux_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV && defined(_WIN32)\n"
                "void windows_only() { uv_close(nullptr, nullptr); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("_WIN32", result.stdout)

    def test_libuv_selector_rejects_apple_branch_from_linux_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV && defined(__APPLE__)\n"
                "void apple_only() { uv_close(nullptr, nullptr); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("__APPLE__", result.stdout)

    def test_libuv_selector_rejects_win32_and_darwin_aliases(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if defined(WIN32) || defined(_DARWIN)\n"
                "void platform_alias() { uv_close(nullptr, nullptr); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("WIN32", result.stdout)
        self.assertIn("_DARWIN", result.stdout)

    def test_libuv_selector_allows_three_platform_constant_expression(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "selector.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV || "
                "((defined(__linux__) || defined(__APPLE__) || "
                "defined(_WIN32) || defined(WIN32) || defined(_DARWIN)) && "
                "!TCPQUIC_RELAY_BACKEND_LIBUV)\n"
                "void common() { uv_close(nullptr, nullptr); }\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_libuv_branch_mode_handles_mixed_backend_condition(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "common_metrics.cpp"
            source.write_text(
                "#if TCPQUIC_RELAY_BACKEND_LIBUV || "
                "(defined(__linux__) && !TCPQUIC_RELAY_BACKEND_LIBUV)\n"
                "#if TCPQUIC_RELAY_BACKEND_LIBUV\n"
                "void bad() { TqPlatformClose(1); }\n"
                "#else\n"
                "void native_only() { close(1); }\n"
                "#endif\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker("--libuv-branches", source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TqPlatformClose", result.stdout)
        self.assertNotIn(":close", result.stdout)

    def test_api_gate_registers_common_libuv_conditional_sources(self):
        cmake = (ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("TCPQUIC_LIBUV_RELAY_CONDITIONAL_GUARD_FILES", cmake)
        self.assertIn("runtime/relay_metrics.cpp", cmake)
        self.assertIn("runtime/memory_stats.cpp", cmake)
        self.assertIn("--libuv-branches", cmake)

    def test_allows_audited_q2t_and_t2q_lane_fixtures(self):
        q2t = LANE_FIXTURES / "libuv_relay_quic_to_tcp.cpp"
        t2q = LANE_FIXTURES / "libuv_relay_tcp_to_quic.cpp"
        result = self.run_checker(q2t, t2q)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_scans_completed_lane_sources_when_worktrees_are_available(self):
        worktrees = ROOT.parent
        q2t = worktrees / "libuv-q2t" / "src/tunnel/libuv_relay_quic_to_tcp.cpp"
        t2q = worktrees / "libuv-t2q" / "src/tunnel/libuv_relay_tcp_to_quic.cpp"
        if not q2t.exists() or not t2q.exists():
            self.skipTest("completed libuv lane worktrees are not available")

        result = self.run_checker(q2t, t2q)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_all_supported_platform_macros(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "libuv_relay_platform.cpp"
            source.write_text(
                "#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)\n"
                "#endif\n",
                encoding="utf-8",
            )
            result = self.run_checker(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("__linux__", result.stdout)
        self.assertIn("__APPLE__", result.stdout)
        self.assertIn("_WIN32", result.stdout)

    def test_rejects_native_call_after_uv_fileno(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "libuv_relay_handle_escape.cpp"
            source.write_text(
                "int f(uv_handle_t* h) {\n"
                "  uv_os_fd_t fd{};\n"
                "  if (uv_fileno(h, &fd) != 0) return -1;\n"
                "  return setsockopt(fd, 0, 0, nullptr, 0);\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_checker(source)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uv_fileno", result.stdout)
        self.assertIn("setsockopt", result.stdout)

    def test_allows_approved_public_dependencies(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "libuv_relay_approved.cpp"
            source.write_text(
                "void f(uv_tcp_t* tcp, uv_stream_t* stream,\n"
                "       uv_shutdown_t* request, uv_os_sock_t socket,\n"
                "       ZSTD_CCtx* zstd, MsQuicStream* quic) {\n"
                "  uv_tcp_open(tcp, socket);\n"
                "  uv_shutdown(request, stream, nullptr);\n"
                "  void* p = mi_malloc(16);\n"
                "  mi_free(p);\n"
                "  ZSTD_compressCCtx(zstd, nullptr, 0, nullptr, 0, 1);\n"
                "  quic->Send(nullptr, 0, QUIC_SEND_FLAG_NONE, nullptr);\n"
                "}\n",
                encoding="utf-8",
            )
            result = self.run_checker(source)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_does_not_scan_src_unittest_harnesses(self):
        with tempfile.TemporaryDirectory() as tmp:
            harness_dir = Path(tmp) / "src" / "unittest"
            harness_dir.mkdir(parents=True)
            (harness_dir / "libuv_harness.cpp").write_text(
                "#include <sys/socket.h>\nint f(){ return close(1); }\n",
                encoding="utf-8",
            )
            result = self.run_checker(tmp)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_invalid_backend_at_configure_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(ROOT),
                    "-B",
                    tmp,
                    "-DTCPQUIC_RELAY_BACKEND=invalid",
                    "-DTCPQUIC_ENABLE_CRASHPAD=OFF",
                ],
                text=True,
                capture_output=True,
            )

        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TCPQUIC_RELAY_BACKEND must be native or libuv", output)

    def test_lane_fragment_appends_to_raypx2_manifest_after_initialization(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_project_overlay(
                root,
                "list(APPEND TCPQUIC_LIBUV_RELAY_PRODUCTION_FILES "
                "tunnel/lane_missing.cpp)\n",
            )
            result = self.configure_overlay(root)

        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lane_missing.cpp", output)

    def test_api_target_scans_sources_appended_by_lane_fragment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_project_overlay(
                root,
                "list(APPEND TCPQUIC_LIBUV_RELAY_PRODUCTION_FILES "
                "tunnel/lane_bad.cpp)\n",
            )
            (root / "src" / "tunnel" / "lane_bad.cpp").write_text(
                "#include <sys/socket.h>\n",
                encoding="utf-8",
            )
            configured = self.configure_overlay(root)
            self.assertEqual(
                configured.returncode,
                0,
                configured.stdout + configured.stderr,
            )
            result = subprocess.run(
                [
                    "cmake",
                    "--build",
                    str(root / "build"),
                    "--target",
                    "tcpquic_libuv_backend_api_check",
                ],
                text=True,
                capture_output=True,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lane_bad.cpp:1:sys/socket.h", result.stdout + result.stderr)

    def test_current_empty_manifest_configures_and_api_target_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            configured = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(ROOT),
                    "-B",
                    str(build),
                    "-DTCPQUIC_RELAY_BACKEND=libuv",
                    "-DTCPQUIC_ENABLE_CRASHPAD=OFF",
                ],
                text=True,
                capture_output=True,
            )
            self.assertEqual(
                configured.returncode,
                0,
                configured.stdout + configured.stderr,
            )
            checked = subprocess.run(
                [
                    "cmake",
                    "--build",
                    str(build),
                    "--target",
                    "tcpquic_libuv_backend_api_check",
                ],
                text=True,
                capture_output=True,
            )

        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_manifest_only_registers_existing_production_files(self):
        cmake = (ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
        manifest = cmake.split(
            "set(TCPQUIC_LIBUV_RELAY_PRODUCTION_FILES)", 1
        )[1].split("set(TCPQUIC_PLATFORM_SOURCES)", 1)[0]
        for relative_path in re.findall(r"tunnel/[A-Za-z0-9_.-]+", manifest):
            self.assertTrue(
                (ROOT / "src" / relative_path).is_file(),
                f"manifest entry does not exist: {relative_path}",
            )
        self.assertIn("${TCPQUIC_LIBUV_RELAY_PRODUCTION_FILES}", cmake)


if __name__ == "__main__":
    unittest.main()
