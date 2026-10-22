"""Classify initial input devices into Admiral's private udev database.

libinput's udev backend skips uninitialized event devices. Admiral provides the
real /dev and sysfs, but a private /run. Use only systemd's read-only input_id
builtin; never run udev rules, trigger kernel events, or alter device nodes.
"""
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path


def _log(message):
    print(f"Input metadata: {message}", flush=True)


def _mounted_database(root, mountinfo):
    # A host bind mount must never receive our generated records, even if it is
    # writable. Admiral's private /run tmpfs is outside this subtree.
    for line in mountinfo.read_text().splitlines():
        fields = line.split()
        if len(fields) < 5:
            continue
        mountpoint = Path(re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), fields[4]))
        if mountpoint == root or root in mountpoint.parents:
            return True
    return False


def _classify(udevadm, syspath, timeout):
    result = subprocess.run([str(udevadm), "test-builtin", "input_id", str(syspath)],
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        detail = result.stderr.strip()[-2000:]
        raise RuntimeError(detail or f"classifier exited {result.returncode}")
    # Only input_id's numeric properties are needed. Do not import diagnostics,
    # rule actions, unrelated environment variables or arbitrary database text.
    properties = {}
    for line in result.stdout.splitlines():
        match = re.fullmatch(r"(ID_INPUT(?:_[A-Z0-9_]+)?)=([0-9]+)", line)
        if match:
            properties[match[1]] = match[2]
    return properties


def _write_record(destination, properties, seat):
    records = [f"I:{max(1, time.monotonic_ns() // 1000)}", f"E:ID_SEAT={seat}"]
    records.extend(f"E:{key}={value}" for key, value in sorted(properties.items()))
    records.extend(["G:seat", "Q:seat", "V:1", ""])
    # Publish a complete record without overwriting anything already present.
    with tempfile.NamedTemporaryFile(mode="w", dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write("\n".join(records))
            stream.flush()
            os.fchmod(stream.fileno(), 0o644)
            os.link(temporary, destination)
        finally:
            temporary.unlink(missing_ok=True)


def prepare(*, sys_input=Path("/sys/class/input"), udev_root=Path("/run/udev"),
            udevadm=Path("/usr/bin/udevadm"), mountinfo=Path("/proc/self/mountinfo")):
    """Prepare initial devices once; preserve any existing or mounted database.

    Paths are keyword arguments for isolated integration fixtures, not runtime
    environment overrides. Returns a compact report as well as logging errors.
    """
    report = {"created": [], "errors": [], "skipped": None}
    if os.environ.get("BROWSER_INPUT", "1") == "0":
        report["skipped"] = "native input disabled"
        return report
    udev_root, sys_input, mountinfo = Path(udev_root), Path(sys_input), Path(mountinfo)
    try:
        if udev_root.exists() or udev_root.is_symlink() or _mounted_database(udev_root, mountinfo):
            report["skipped"] = "preserving preexisting or mounted udev database"
            _log(report["skipped"])
            return report
        events = sorted(path for path in sys_input.glob("event*") if re.fullmatch(r"event[0-9]+", path.name))
        if not events:
            report["skipped"] = "no initial event devices"
            _log(report["skipped"])
            return report
        seat = os.environ.get("BROWSER_INPUT_SEAT", "seat0") or "seat0"
        if len(seat) > 256 or not re.fullmatch(r"[A-Za-z0-9_.-]+", seat):
            raise ValueError("invalid BROWSER_INPUT_SEAT")
        udev_root.mkdir(mode=0o755)
        database = udev_root / "data"
        database.mkdir(mode=0o755)
        deadline = time.monotonic() + 10
        for event in events:
            try:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("initial enumeration exceeded 10 seconds")
                devnum = (event / "dev").read_text().strip()
                if not re.fullmatch(r"[0-9]+:[0-9]+", devnum):
                    raise ValueError("invalid sysfs device number")
                properties = _classify(udevadm, event.resolve(strict=True), min(2, remaining))
                if properties.get("ID_INPUT") != "1":
                    continue
                name = f"c{devnum}"
                _write_record(database / name, properties, seat)
                report["created"].append(name)
            except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
                message = f"{event.name}: {exc}"
                report["errors"].append(message)
                _log(message)
        _log(f"prepared {len(report['created'])} initial devices; restart to discover newly attached devices")
    except (OSError, ValueError) as exc:
        report["errors"].append(str(exc))
        _log(f"initial discovery unavailable: {exc}")
    return report


if __name__ == "__main__":
    prepare()
