"""Read-only, bounded device probes. Detection is deliberately distinct from proof."""
import glob
import json
import os
import platform
import subprocess
import time
from pathlib import Path

APP_ROOT = Path(os.environ.get("BROWSER_APP_ROOT", "/opt/embedded-browser"))
RUNTIME = Path(os.environ.get("XDG_RUNTIME_DIR", "/run/embedded-browser"))
# Fixed path shared with the sandboxed web process (BROWSER_VIDEO_RUNTIME_DIR in native/drm-broker.h).
VIDEO_RUNTIME = Path("/run/embedded-browser/video")


def command(args, timeout=8):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return {"command": args, "returncode": result.returncode,
                "output": (result.stdout + result.stderr)[-24000:]}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"command": args, "returncode": None, "output": str(exc)}


def read_json(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return {}


def device_nodes(patterns):
    nodes = []
    for pattern in patterns:
        for name in sorted(glob.glob(pattern)):
            path = Path(name)
            driver = Path("/sys/class") / ("drm" if "/dri/" in name else "video4linux") / path.name / "device/driver"
            nodes.append({"path": name, "driver": driver.resolve().name if driver.exists() else None,
                          "readable": os.access(name, os.R_OK), "writable": os.access(name, os.W_OK)})
    return nodes


def snapshot(probe=False):
    native = read_json(VIDEO_RUNTIME / "video-status.json")
    broker = read_json(RUNTIME / "drm-status.json")
    browser = read_json(RUNTIME / "browser-status.json")
    if browser.get("holepunch_error"):
        broker["error"] = browser["holepunch_error"]
    # Adapter reports evidence; presence of device nodes or decoder factories is never proof.
    hardware = native.get("hardware_decode", {"verified": False, "evidence": "No decoded frame evidence yet"})
    holepunch = native.get("holepunch", {"verified": False, "evidence": "Visual plane blending needs board validation"})
    if not isinstance(hardware, dict):
        hardware = {"verified": False, "evidence": hardware}
    if not isinstance(holepunch, dict):
        holepunch = {"verified": False, "evidence": holepunch}
    if native:
        alive = True
        try:
            pid = int(native.get("pid", 0))
            if pid <= 0:
                raise ValueError("Missing native process")
            os.kill(pid, 0)
        except (OSError, TypeError, ValueError):
            alive = False
        fresh = time.time() * 1000 - native.get("updated_at_unix_ms", 0) < 5000
        if not alive or not fresh:
            hardware = {"verified": False, "evidence": "Native playback evidence is stale or its process exited"}
            holepunch = {**holepunch, "verified": False}
    result = {
        "kernel": platform.release(), "architecture": platform.machine(),
        "gpu": {"nodes": device_nodes(["/dev/dri/card*", "/dev/dri/renderD*"]),
                "note": "Browser WebGL renderer is the live GPU check; device presence alone is not acceleration."},
        "video": {"nodes": device_nodes(["/dev/video*", "/dev/media*"]), "native": native,
                  "hardware_decode": hardware},
        "holepunch": holepunch, "drm": broker, "browser": browser,
        "input": read_json(RUNTIME / "input-status.json"),
        "runtime": {name: os.environ.get(name) for name in (
            "BROWSER_VIDEO_MODE", "BROWSER_ALLOW_SOFTWARE", "BROWSER_DECODER",
            "BROWSER_VIDEO_PLANE_ID", "BROWSER_VIDEO_SINK", "BROWSER_FRAME_CALLBACK", "BROWSER_DRM_DEVICE", "BROWSER_RENDER_DEVICE", "BROWSER_DRM_MODE", "BROWSER_UI_SIZE", "PAN_MESA_DEBUG", "XDG_CACHE_HOME", "GST_REGISTRY",
            "WEBKIT_GST_HOLE_PUNCH_QUIRK",
            "WEBKIT_GST_QUIRKS", "GST_PLUGIN_FEATURE_RANK", "GST_DEBUG", "GST_DEBUG_DUMP_DOT_DIR")},
        "versions": command(["dpkg-query", "-W", "-f=${Package} ${Version}\n", "libwpewebkit-2.0-1",
                             "libwpe-1.0-1", "libwpebackend-fdo-1.0-1", "libgl1-mesa-dri",
                             "gstreamer1.0-plugins-bad"]),
    }
    if probe:
        result["probes"] = {
            "egl": command(["eglinfo", "-B", "-p", "surfaceless"]),
            "v4l2": command(["v4l2-ctl", "--list-devices"]),
            "stateless_h264": command(["gst-inspect-1.0", "v4l2slh264dec"]),
            "stateless_h265": command(["gst-inspect-1.0", "v4l2slh265dec"]),
            "adapter": command(["gst-inspect-1.0", "westerossink"]),
            "kms": command(["drm_info"]),
        }
    return result


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quick", action="store_true", help="Skip device probes")
    args = parser.parse_args()
    print(json.dumps(snapshot(probe=not args.quick), indent=2))
