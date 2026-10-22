"""Launch an embedded browser and optional app services under Admiral's init."""
import hashlib
import json
import os
import platform
import signal
import subprocess
import sys
import time
from pathlib import Path
from urllib.request import urlopen

from diagnostics import APP_ROOT, RUNTIME, VIDEO_RUNTIME

HARDWARE_DECODERS = ("v4l2slh264dec", "v4l2slh265dec", "v4l2slvp9dec")
SOFTWARE_DECODER = "avdec_h264"
DISABLED_DECODERS = ("openh264dec", "v4l2h264dec")


def decoder_config(environ):
    software = environ.get("BROWSER_ALLOW_SOFTWARE", "0") == "1"
    default = SOFTWARE_DECODER if software else ",".join(HARDWARE_DECODERS)
    listed = [name.strip() for name in (environ.get("BROWSER_DECODER") or default).split(",")]
    unknown = [name for name in listed if name not in HARDWARE_DECODERS + (SOFTWARE_DECODER,)]
    if unknown:
        raise ValueError(f"Unsupported BROWSER_DECODER entries: {', '.join(unknown)}")
    if SOFTWARE_DECODER in listed and not software:
        raise ValueError("Software decoder requires BROWSER_ALLOW_SOFTWARE=1")
    ranks = dict.fromkeys(listed, 512)
    for name in HARDWARE_DECODERS:
        ranks.setdefault(name, 0)
    ranks.setdefault(SOFTWARE_DECODER, 256 if software else 0)
    for name in DISABLED_DECODERS:
        ranks.setdefault(name, 0)
    return ",".join(listed), ",".join(f"{name}:{rank}" for name, rank in ranks.items())


def start_url(environ):
    if environ.get("BROWSER_URL"):
        return environ["BROWSER_URL"]
    if environ.get("BROWSER_HTTP_ROOT"):
        return f"http://127.0.0.1:{environ.get('BROWSER_HTTP_PORT', '8080')}/"
    return (APP_ROOT / "www/index.html").as_uri()


def app_command(environ):
    raw = environ.get("BROWSER_APP_COMMAND", "")
    if not raw:
        if environ.get("BROWSER_APP_READY_URL"):
            raise ValueError("BROWSER_APP_READY_URL requires BROWSER_APP_COMMAND")
        return None
    command = json.loads(raw)
    if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
        raise ValueError("BROWSER_APP_COMMAND must be a nonempty JSON array of strings")
    if not command[0].startswith("/") or any("\0" in arg for arg in command):
        raise ValueError("BROWSER_APP_COMMAND requires an absolute executable and no NUL bytes")
    return command


def writable_dir(*candidates):
    for candidate in candidates:
        try:
            candidate.mkdir(parents=True, exist_ok=True)
            probe = candidate / f".write-test-{os.getpid()}"
            probe.touch()
            probe.unlink()
            return candidate
        except OSError:
            continue
    raise OSError("No writable cache directory")


def configure_caches(data):
    cache = writable_dir(data / "cache", Path("/tmp/embedded-browser-cache"))
    os.environ.setdefault("XDG_CACHE_HOME", str(cache))
    if "GST_REGISTRY" not in os.environ:
        # Decoder factories depend on both the kernel and the device enumeration.
        nodes = sorted(str(path) for pattern in ("video*", "media*") for path in Path("/dev").glob(pattern))
        key = hashlib.sha1("\n".join([platform.release(), *nodes]).encode()).hexdigest()[:12]
        registry = cache / "gstreamer-1.0" / f"registry-{key}.bin"
        registry.parent.mkdir(parents=True, exist_ok=True)
        for stale in registry.parent.glob("registry-*.bin*"):
            if not stale.name.startswith(registry.name):
                stale.unlink(missing_ok=True)
        os.environ["GST_REGISTRY"] = str(registry)
    return cache


def configure():
    RUNTIME.mkdir(parents=True, exist_ok=True, mode=0o700)
    RUNTIME.chmod(0o700)
    VIDEO_RUNTIME.mkdir(parents=True, exist_ok=True, mode=0o700)
    VIDEO_RUNTIME.chmod(0o700)
    data = Path(os.environ.get("BROWSER_DATA_DIR", "/data"))
    (data / "logs").mkdir(parents=True, exist_ok=True)
    # WebKit's default data manager follows XDG paths. Retain browser state on
    # the durable volume instead of losing it when Admiral replaces the rootfs.
    for variable, name in (("XDG_DATA_HOME", "browser-data"), ("XDG_CONFIG_HOME", "browser-config")):
        os.environ.setdefault(variable, str(data / name))
        Path(os.environ[variable]).mkdir(parents=True, exist_ok=True, mode=0o700)
    os.environ.setdefault("GST_DEBUG_DUMP_DOT_DIR", str(data / "logs"))
    configure_caches(data)
    for name in ("drm-status.json", "browser-status.json", "input-status.json"):
        (RUNTIME / name).unlink(missing_ok=True)
    (VIDEO_RUNTIME / "video-status.json").unlink(missing_ok=True)
    mode = os.environ.get("BROWSER_VIDEO_MODE", "holepunch")
    if mode not in ("holepunch", "composited"):
        raise ValueError("BROWSER_VIDEO_MODE must be holepunch or composited")
    if mode == "holepunch":
        os.environ.setdefault("WEBKIT_GST_HOLE_PUNCH_QUIRK", "westeros")
        os.environ.setdefault("WEBKIT_GST_QUIRKS", "westeros")
        os.environ.setdefault("WEBKIT_GST_USE_PLAYBIN3", "1")
    else:
        os.environ.pop("WEBKIT_GST_HOLE_PUNCH_QUIRK", None)
        os.environ.pop("WEBKIT_GST_QUIRKS", None)
    decoder, ranks = decoder_config(os.environ)
    os.environ["BROWSER_DECODER"] = decoder
    os.environ.setdefault("GST_PLUGIN_FEATURE_RANK", ranks)
    os.environ["BROWSER_URL"] = start_url(os.environ)
    root = os.environ.get("BROWSER_HTTP_ROOT")
    if root and (not Path(root).is_absolute() or not Path(root).is_dir()):
        raise ValueError("BROWSER_HTTP_ROOT must be an existing absolute directory")
    port = int(os.environ.get("BROWSER_HTTP_PORT", "8080"))
    if not 1 <= port <= 65535:
        raise ValueError("BROWSER_HTTP_PORT must be between 1 and 65535")
    timeout = float(os.environ.get("BROWSER_START_TIMEOUT", "30"))
    if not 0 < timeout <= 3600:
        raise ValueError("BROWSER_START_TIMEOUT must be > 0 and <= 3600 seconds")
    app_command(os.environ)


def supervise(browser_args):
    """A required service exiting stops the workload so Admiral can restart it."""
    from input_metadata import prepare
    (RUNTIME / "input-status.json").write_text(json.dumps(prepare(), indent=2))
    processes = []
    stopping = False

    def shutdown(signum=None, frame=None):
        nonlocal stopping
        stopping = True
        for process in processes:
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass

    def spawn(command):
        process = subprocess.Popen(command, start_new_session=True)
        processes.append(process)
        # A signal can arrive between Popen and registering this process.
        if stopping:
            shutdown()
        return process

    def wait_ready(url):
        deadline = time.monotonic() + float(os.environ.get("BROWSER_START_TIMEOUT", "30"))
        while time.monotonic() < deadline:
            if stopping:
                return False
            if any(process.poll() is not None for process in processes):
                raise RuntimeError("Required service exited before readiness")
            try:
                with urlopen(url, timeout=min(1, max(0.01, deadline - time.monotonic()))) as response:
                    if 200 <= response.status < 300:
                        return True
            except OSError:
                pass
            time.sleep(0.1)
        raise RuntimeError("Timed out waiting for required app service")

    old_handlers = {sig: signal.signal(sig, shutdown) for sig in (signal.SIGTERM, signal.SIGINT)}
    code = 0
    try:
        command = app_command(os.environ)
        if command and not stopping:
            spawn(command)
            ready = os.environ.get("BROWSER_APP_READY_URL")
            if ready:
                wait_ready(ready)
        if os.environ.get("BROWSER_HTTP_ROOT") and not stopping:
            spawn([sys.executable, str(APP_ROOT / "app/server.py")])
            wait_ready(f"http://127.0.0.1:{os.environ.get('BROWSER_HTTP_PORT', '8080')}/healthz")
        if not stopping and not any(process.poll() is not None for process in processes):
            spawn(["/usr/bin/dbus-run-session", "--", "/usr/local/bin/embedded-browser",
                   os.environ["BROWSER_URL"], *browser_args])
        print("Embedded browser started; native status is in /run/embedded-browser.", flush=True)
        while not stopping:
            for process in processes:
                if process.poll() is not None:
                    code = process.returncode or 1
                    print(f"Required service exited ({process.returncode}); stopping workload.", flush=True)
                    shutdown()
                    break
            time.sleep(0.1)
    except (OSError, RuntimeError) as exc:
        print(f"Embedded browser startup failed: {exc}", file=sys.stderr, flush=True)
        code = 1
    finally:
        shutdown()
        deadline = time.monotonic() + 5
        for process in processes:
            try:
                process.wait(timeout=max(0.01, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
            # Also stop grandchildren if the service leader exited first.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
    return code if code >= 0 else 128 - code


def main():
    args = sys.argv[1:] or ["run"]
    operation = args.pop(0)
    if operation == "shell":
        os.execv("/bin/bash", ["/bin/bash", *args])
    if operation not in ("run", "doctor", "smoke", "server"):
        os.execvp(operation, [operation, *args])
    # Inspection must preserve current evidence and work alongside a running browser.
    if operation == "doctor":
        os.execv(sys.executable, [sys.executable, str(APP_ROOT / "app/diagnostics.py"), *args])
    if operation == "run" and args:
        if len(args) != 1 or args[0].startswith("-"):
            raise ValueError("Usage: run [URL]")
        os.environ["BROWSER_URL"] = args.pop()
    configure()
    if operation in ("smoke", "server"):
        if operation == "server" and not os.environ.get("BROWSER_HTTP_ROOT"):
            os.environ["BROWSER_HTTP_ROOT"] = str(APP_ROOT / "www")
        module = "smoke.py" if operation == "smoke" else "server.py"
        os.execv(sys.executable, [sys.executable, str(APP_ROOT / "app" / module), *args])
    return supervise(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print(f"Embedded browser configuration error: {exc}", file=sys.stderr)
        sys.exit(2)
