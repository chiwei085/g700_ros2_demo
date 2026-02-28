# tools/gen_realsense_override.py
import glob
import os
from collections.abc import Iterable
from pathlib import Path

from ruamel.yaml import YAML

INTEL_VENDOR = "8086"
DEFAULT_SERVICE_NAME = "ros-stack"
DEFAULT_OUTFILE = Path("compose.realsense.override.yml")

CLASSES = [
    ("/dev/video*", "/sys/class/video4linux"),
    ("/dev/media*", "/sys/class/media"),
    ("/dev/hidraw*", "/sys/class/hidraw"),
]


def _read(p: Path) -> str | None:
    try:
        return p.read_text().strip()
    except Exception:
        return None


def _find_usb_id(dev_syspath: Path) -> tuple[str | None, str | None]:
    cur = dev_syspath
    for _ in range(16):
        vid = _read(cur / "idVendor")
        pid = _read(cur / "idProduct")
        if vid and pid:
            return vid.lower(), pid.lower()
        if cur.parent == cur:
            break
        cur = cur.parent
    return None, None


def find_intel_nodes(
    vendor: str = INTEL_VENDOR,
    classes: Iterable[tuple[str, str]] = CLASSES,
) -> list[str]:
    nodes: list[str] = []
    for dev_glob, sys_root in classes:
        for node in sorted(glob.glob(dev_glob)):
            name = Path(node).name
            sys_dev = Path(sys_root) / name / "device"
            if not sys_dev.exists():
                continue
            vid, _ = _find_usb_id(sys_dev.resolve())
            if vid == vendor:
                nodes.append(node)
    return sorted(set(nodes), key=lambda s: (len(s), s))


def generate_override(
    outfile: Path = DEFAULT_OUTFILE,
    service_name: str = DEFAULT_SERVICE_NAME,
    always_mount_usb_bus: bool = True,
) -> tuple[Path, list[str], bool]:
    nodes = find_intel_nodes()

    devices: list[str] = []
    if always_mount_usb_bus:
        devices.append("/dev/bus/usb:/dev/bus/usb:rwm")
    for n in nodes:
        devices.append(f"{n}:{n}:rwm")

    svc: dict = {"devices": devices}

    # --- X11 scheme A (xhost): no XAUTHORITY mount, only pass DISPLAY ---
    disp = os.environ.get("DISPLAY", ":0")
    svc.setdefault("environment", {})
    svc["environment"]["DISPLAY"] = disp
    svc["environment"]["QT_X11_NO_MITSHM"] = "1"

    # Optional: if you ever need to force X11 socket mount via override
    # (normally you already have it in base compose.yml)
    # svc.setdefault("volumes", [])
    # svc["volumes"].append("/tmp/.X11-unix:/tmp/.X11-unix:rw")

    override = {"services": {service_name: svc}}
    yaml = YAML()
    yaml.indent(mapping=2, sequence=4, offset=2)
    outfile.parent.mkdir(parents=True, exist_ok=True)
    with outfile.open("w", encoding="utf-8") as f:
        yaml.dump(override, f)

    return outfile, devices, len(nodes) > 0


if __name__ == "__main__":
    out, devices, found = generate_override()
    print(f"[OK] Wrote {out}, devices={len(devices)}, intel_found={found}")
