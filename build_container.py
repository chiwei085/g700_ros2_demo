#!/usr/bin/env python3

import argparse
import os
import shlex
import subprocess
import time
from pathlib import Path

from tools.gen_realsense_override import generate_override

DEFAULT_BASE_FILES = ["compose.yml"]
DEFAULT_SERVICE_PREBUILT = "ros-stack"
DEFAULT_SERVICE_SOURCE = "ros-stack-source"
DEFAULT_OVERRIDE = Path("compose.realsense.override.yml")
DEFAULT_PROJECT_SUFFIX = "arm64"


def _compose_file_args(files: list[str]) -> list[str]:
    args: list[str] = []
    for f in files:
        args += ["-f", f]
    return args


def _strip_leading_double_dash(xs: list[str]) -> list[str]:
    # argparse.REMAINDER sometimes keeps a leading "--" separator
    if xs and xs[0] == "--":
        return xs[1:]
    return xs


def _run(cmd: list[str], *, env: dict[str, str] | None = None) -> int:
    print("[cmd]", " ".join(shlex.quote(c) for c in cmd))
    run_env = dict(os.environ)
    if env:
        run_env.update(env)

    # Force BuildKit for cache mounts
    run_env.setdefault("DOCKER_BUILDKIT", "1")
    run_env.setdefault("COMPOSE_DOCKER_CLI_BUILD", "1")

    p = subprocess.run(cmd, env=run_env)
    return p.returncode


def _compute_vendor_git_sha() -> str:
    repo = Path("colcon_ws/src/orbslam3_ros2_vendor")
    if not repo.exists():
        print(f"[WARN] vendor repo not found: {repo}")
        return "unknown"
    try:
        p = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "HEAD"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    except OSError as e:
        print(f"[WARN] failed to run git for vendor sha: {e}")
        return "unknown"
    sha = (p.stdout or "").strip()
    if p.returncode != 0 or not sha:
        print("[WARN] failed to resolve vendor git sha; using unknown")
        if p.stderr:
            print(f"[WARN] git stderr: {p.stderr.strip()}")
        return "unknown"
    return sha


def _resolve_service(vendor_mode: str, explicit_service: str) -> str:
    if explicit_service:
        return explicit_service
    if vendor_mode == "source":
        return DEFAULT_SERVICE_SOURCE
    return DEFAULT_SERVICE_PREBUILT


def _resolve_project_name(explicit_project: str) -> str:
    if explicit_project:
        return explicit_project
    env_project = os.environ.get("COMPOSE_PROJECT_NAME", "").strip()
    if env_project:
        return env_project
    return f"{Path.cwd().name}-{DEFAULT_PROJECT_SUFFIX}"


def _maybe_add_realsense_override(
    base_files: list[str],
    mode: str,
    service: str,
    override_path: Path,
) -> tuple[list[str], bool, bool]:
    """
    Returns:
      files: compose files in merge order
      used_override: whether override file is appended
      found_device: whether Intel-backed nodes were detected
    """
    files = list(base_files)
    used_override = False

    if mode == "off":
        return files, False, False

    out, devices, found = generate_override(
        outfile=override_path,
        service_name=service,
        always_mount_usb_bus=True,
    )

    if mode == "on":
        files.append(str(out))
        used_override = True
        return files, used_override, found

    # auto
    if found:
        files.append(str(out))
        used_override = True
    return files, used_override, found


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="build_container.py")
    p.add_argument(
        "--compose-file",
        action="append",
        default=[],
        help="Base compose file(s). Can be specified multiple times. Default: compose.yml",
    )
    p.add_argument(
        "--service",
        default="",
        help=(
            "Service name to operate on. "
            "Default depends on --vendor-mode (prebuilt=ros-stack, source=ros-stack-source)."
        ),
    )
    p.add_argument(
        "--vendor-mode",
        choices=["prebuilt", "source"],
        default="prebuilt",
        help="Select default service mode: prebuilt=ros-stack, source=ros-stack-source.",
    )
    p.add_argument(
        "--realsense",
        choices=["auto", "on", "off"],
        default="auto",
        help="Generate/use RealSense override compose file. auto=only if detected.",
    )
    p.add_argument(
        "--override-file",
        default=str(DEFAULT_OVERRIDE),
        help=f"Override compose filename to generate. Default: {DEFAULT_OVERRIDE}",
    )
    p.add_argument(
        "--project-name",
        default="",
        help=(
            "Compose project name (-p). Default: "
            "COMPOSE_PROJECT_NAME or '<cwd>-arm64'."
        ),
    )
    p.add_argument(
        "--print-files",
        action="store_true",
        help="Print resolved compose file list and override status.",
    )

    sub = p.add_subparsers(dest="cmd", required=True)

    # up
    sp = sub.add_parser("up", help="docker compose up")
    sp.add_argument(
        "-d", "--detach", action="store_true", help="Run in background"
    )
    sp.add_argument("--build", action="store_true", help="Build images")
    sp.add_argument(
        "--force-recreate",
        action="store_true",
        help="Recreate containers even if config hasn't changed",
    )
    sp.add_argument(
        "--no-deps",
        action="store_true",
        help="Don't start linked services",
    )
    sp.add_argument(
        "extra",
        nargs=argparse.REMAINDER,
        help="Pass-through args to compose up (use -- to separate)",
    )

    # down
    sp = sub.add_parser("down", help="docker compose down")
    sp.add_argument("extra", nargs=argparse.REMAINDER)

    # exec
    sp = sub.add_parser("exec", help="docker compose exec")
    sp.add_argument(
        "exec_cmd",
        nargs=argparse.REMAINDER,
        help="Command to run, default: bash",
    )

    # run (one-off)
    sp = sub.add_parser("run", help="docker compose run --rm <service> ...")
    sp.add_argument(
        "--rm",
        action="store_true",
        default=True,
        help="Remove container after run (default: true)",
    )
    sp.add_argument(
        "run_cmd",
        nargs=argparse.REMAINDER,
        help="Command to run, default: bash",
    )

    # logs
    sp = sub.add_parser("logs", help="docker compose logs")
    sp.add_argument("-f", "--follow", action="store_true")
    sp.add_argument("extra", nargs=argparse.REMAINDER)

    # ps
    sp = sub.add_parser("ps", help="docker compose ps")
    sp.add_argument("extra", nargs=argparse.REMAINDER)

    # build
    sp = sub.add_parser("build", help="docker compose build")
    sp.add_argument("extra", nargs=argparse.REMAINDER)

    # build-vendor
    sp = sub.add_parser(
        "build-vendor",
        help="Rebuild vendor-related image layers with cache busting",
    )
    sp.add_argument("extra", nargs=argparse.REMAINDER)

    # check-vendor
    sp = sub.add_parser("check-vendor", help="Run vendor check tool in service")
    sp.add_argument(
        "extra",
        nargs=argparse.REMAINDER,
        help="Pass-through args to vendor_check.py (use -- to separate)",
    )

    # config (debug merge result)
    sp = sub.add_parser("config", help="docker compose config (merged view)")
    sp.add_argument("extra", nargs=argparse.REMAINDER)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    base_files = args.compose_file or DEFAULT_BASE_FILES
    service: str = _resolve_service(args.vendor_mode, args.service)
    override_path = Path(args.override_file)

    files, used_override, found_device = _maybe_add_realsense_override(
        base_files=base_files,
        mode=args.realsense,
        service=service,
        override_path=override_path,
    )

    project_name = _resolve_project_name(args.project_name)
    compose = ["docker", "compose"] + _compose_file_args(files) + ["-p", project_name]
    if args.vendor_mode == "source":
        compose += ["--profile", "source"]

    if args.print_files:
        print("[info] compose files:", ", ".join(files))
        print("[info] selected service:", service)
        print("[info] project name:", project_name)
        print("[info] vendor mode:", args.vendor_mode)
        print("[info] realsense override:", "ON" if used_override else "OFF")
        if args.realsense != "off":
            print("[info] realsense detected:", "YES" if found_device else "NO")

    vendor_sha = _compute_vendor_git_sha()
    build_env = {"VENDOR_GIT_SHA": vendor_sha}

    if args.cmd == "up":
        cmd = compose + ["up"]
        if args.detach:
            cmd.append("-d")
        if args.build:
            cmd.append("--build")
        if args.force_recreate:
            cmd.append("--force-recreate")
        if args.no_deps:
            cmd.append("--no-deps")
        cmd.append(service)
        cmd += _strip_leading_double_dash(args.extra)
        if args.build:
            return _run(cmd, env=build_env)
        return _run(cmd)

    if args.cmd == "down":
        return _run(compose + ["down"] + _strip_leading_double_dash(args.extra))

    if args.cmd == "exec":
        exec_cmd = _strip_leading_double_dash(args.exec_cmd)
        if not exec_cmd:
            exec_cmd = ["bash"]
        return _run(compose + ["exec", service] + exec_cmd)

    if args.cmd == "run":
        run_cmd = _strip_leading_double_dash(args.run_cmd)
        if not run_cmd:
            run_cmd = ["bash"]
        cmd = compose + ["run"]
        if args.rm:
            cmd.append("--rm")
        cmd += [service] + run_cmd
        return _run(cmd)

    if args.cmd == "logs":
        cmd = compose + ["logs"]
        if args.follow:
            cmd.append("-f")
        cmd += [service]
        cmd += _strip_leading_double_dash(args.extra)
        return _run(cmd)

    if args.cmd == "ps":
        return _run(compose + ["ps"] + _strip_leading_double_dash(args.extra))

    if args.cmd == "build":
        cmd = compose + ["build", service]
        cmd += _strip_leading_double_dash(args.extra)
        return _run(cmd, env=build_env)

    if args.cmd == "build-vendor":
        cache_bust = str(int(time.time()))
        cmd = compose + [
            "build",
            "--build-arg",
            f"VENDOR_CACHE_BUST={cache_bust}",
            "--build-arg",
            f"VENDOR_GIT_SHA={vendor_sha}",
            DEFAULT_SERVICE_PREBUILT,
        ]
        cmd += _strip_leading_double_dash(args.extra)
        return _run(cmd)

    if args.cmd == "check-vendor":
        check_service = args.service or DEFAULT_SERVICE_PREBUILT
        extra_args = _strip_leading_double_dash(args.extra)
        check_cmd = (
            'if ! command -v uv >/dev/null 2>&1; then '
            'echo "[ERROR] uv not found in container PATH" >&2; '
            "exit 127; "
            "fi; "
            'uv run /usr/local/bin/vendor_check.py "$@"'
        )
        cmd = (
            ["docker", "compose"]
            + _compose_file_args(files)
            + [
                "-p",
                project_name,
                "exec",
                check_service,
                "bash",
                "-lc",
                check_cmd,
                "vendor_check.py",
            ]
        )
        cmd += extra_args
        return _run(cmd)

    if args.cmd == "config":
        return _run(
            compose + ["config"] + _strip_leading_double_dash(args.extra)
        )

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
