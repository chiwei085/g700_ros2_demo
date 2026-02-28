#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str]) -> tuple[int, str, str]:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def path_index(path_list: str, target: str) -> int | None:
    items = [p for p in path_list.split(":") if p]
    for i, value in enumerate(items):
        if value == target:
            return i
    return None


def main() -> int:
    parser = argparse.ArgumentParser(prog="vendor_check.py")
    parser.add_argument("--json", action="store_true", help="Output JSON report")
    parser.add_argument(
        "--assert-prebuilt-ready",
        action="store_true",
        help="Hard-fail if prebuilt vendor library/linking is not ready",
    )
    args = parser.parse_args()

    report: dict[str, object] = {
        "checks": [],
        "warnings": [],
        "errors": [],
        "stamp": {},
        "paths": {
            "CMAKE_PREFIX_PATH": os.environ.get("CMAKE_PREFIX_PATH", ""),
            "AMENT_PREFIX_PATH": os.environ.get("AMENT_PREFIX_PATH", ""),
        },
    }

    vendor_lib = Path("/opt/vendor/lib/libORB_SLAM3.so")
    stamp_path = Path("/opt/vendor/.vendor.stamp")

    def info(msg: str) -> None:
        report["checks"].append(msg)
        if not args.json:
            print(f"[INFO] {msg}")

    def warn(msg: str) -> None:
        warnings = report["warnings"]
        assert isinstance(warnings, list)
        warnings.append(msg)
        if not args.json:
            print(f"[WARN] {msg}")

    def error(msg: str) -> None:
        errors = report["errors"]
        assert isinstance(errors, list)
        errors.append(msg)
        if not args.json:
            print(f"[ERROR] {msg}")

    # A) vendor lib / stamp
    if vendor_lib.exists():
        info(f"vendor library found: {vendor_lib}")
    else:
        error(f"vendor library missing: {vendor_lib}")

    stamp_data: dict[str, str] = {}
    if stamp_path.exists():
        for line in stamp_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                stamp_data[k.strip()] = v.strip()
        report["stamp"] = stamp_data
        info(f"vendor stamp found: {stamp_path}")
    else:
        warn(f"vendor stamp missing: {stamp_path}")

    # C) compare workspace vendor repo HEAD with stamp vendor_rev
    ws_vendor_repo = Path("/ws/src/orbslam3_ros2_vendor")
    ws_git_dir = ws_vendor_repo / ".git"
    stamp_vendor_rev = stamp_data.get("vendor_rev", "")
    if ws_git_dir.exists() and stamp_vendor_rev:
        rc, out, _ = run(["git", "-C", str(ws_vendor_repo), "rev-parse", "HEAD"])
        if rc == 0:
            ws_rev = out.strip()
            if stamp_vendor_rev != "unknown" and ws_rev != stamp_vendor_rev:
                warn(
                    "workspace vendor rev differs from image stamp "
                    f"(ws={ws_rev}, stamp={stamp_vendor_rev})"
                )

    # D) ldconfig librealsense path source check
    rc, out, _ = run(["ldconfig", "-p"])
    librealsense_paths: list[str] = []
    if rc == 0:
        for line in out.splitlines():
            if "librealsense2" not in line or "=>" not in line:
                continue
            path = line.split("=>", 1)[1].strip()
            librealsense_paths.append(path)
        librealsense_paths = sorted(set(librealsense_paths))
        bad = [p for p in librealsense_paths if not p.startswith("/usr/local/lib")]
        if bad:
            warn(
                "librealsense may not be from /usr/local: " + ", ".join(bad)
            )
        elif librealsense_paths:
            info("librealsense paths resolve to /usr/local")
        else:
            warn("no librealsense entries found in ldconfig")
    else:
        warn("ldconfig not available")

    # E) dpkg check for apt librealsense2* packages
    rc, out, _ = run(["bash", "-lc", "dpkg -l | grep librealsense2 || true"])
    if rc == 0 and out.strip():
        warn("apt librealsense2 packages detected")

    # F) ldd on vendor lib for missing linkage
    ldd_not_found: list[str] = []
    if vendor_lib.exists():
        rc, out, err = run(["ldd", str(vendor_lib)])
        ldd_text = out + "\n" + err
        for line in ldd_text.splitlines():
            if "not found" in line:
                ldd_not_found.append(line.strip())
        if ldd_not_found:
            error("vendor library has unresolved dependencies")
            if not args.json:
                for line in ldd_not_found:
                    print(f"[ERROR] {line}")
        else:
            info("vendor library ldd check passed")

    # G) AMENT prefix order check
    ament_prefix = os.environ.get("AMENT_PREFIX_PATH", "")
    vendor_idx = path_index(ament_prefix, "/opt/vendor")
    ws_install_idx = path_index(ament_prefix, "/ws/install")
    if vendor_idx is not None and ws_install_idx is not None and ws_install_idx < vendor_idx:
        warn("/ws/install appears before /opt/vendor in AMENT_PREFIX_PATH")

    # H) prefix order display
    if not args.json:
        print("[INFO] CMAKE_PREFIX_PATH order:")
        for item in [p for p in os.environ.get("CMAKE_PREFIX_PATH", "").split(":") if p]:
            print(item)
        print("[INFO] AMENT_PREFIX_PATH order:")
        for item in [p for p in ament_prefix.split(":") if p]:
            print(item)

    # strict mode exit policy
    if args.assert_prebuilt_ready:
        if not vendor_lib.exists():
            if args.json:
                print(json.dumps(report, indent=2, ensure_ascii=False))
            return 10
        if ldd_not_found:
            if args.json:
                print(json.dumps(report, indent=2, ensure_ascii=False))
            return 11
        errors = report["errors"]
        assert isinstance(errors, list)
        if errors:
            if args.json:
                print(json.dumps(report, indent=2, ensure_ascii=False))
            return 12

    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
