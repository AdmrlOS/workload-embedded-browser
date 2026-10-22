"""Keep legacy signage app settings at the downstream boundary."""
import json
import os
import sys
from pathlib import Path

SHARED_SETTINGS = (
    "DATA_DIR", "VIDEO_MODE", "ALLOW_SOFTWARE", "DECODER", "DRM_DEVICE",
    "RENDER_DEVICE", "DRM_MODE", "UI_SIZE", "DRM_CONNECTOR", "DRM_PRIMARY_PLANE",
    "VIDEO_PLANE_ID", "VIDEO_SINK", "FRAME_CALLBACK", "INPUT", "INPUT_SEAT",
    "KEYBOARD_LAYOUT", "BWRAP_NO_V4L2",
)


def configure(environ):
    # Explicit BROWSER_* settings take precedence over old SIGNAGE_* controls.
    for suffix in SHARED_SETTINGS:
        legacy = environ.get("SIGNAGE_" + suffix)
        if legacy is not None:
            environ.setdefault("BROWSER_" + suffix, legacy)
    if environ.get("SIGNAGE_BROWSER_STATUS"):
        environ.setdefault("BROWSER_STATUS", environ["SIGNAGE_BROWSER_STATUS"])
    # Both the app diagnostics and native adapter must describe the same runtime.
    for suffix in SHARED_SETTINGS:
        if "BROWSER_" + suffix in environ:
            environ["SIGNAGE_" + suffix] = environ["BROWSER_" + suffix]
    pages = {"": "", "displaylab": "", "menuboard": "menuboard/", "player": "player/", "studio": "studio/"}
    page = environ.get("SIGNAGE_PAGE", "player").strip().lower()
    if not environ.get("BROWSER_URL") and not environ.get("SIGNAGE_URL") and page not in pages:
        raise ValueError("Unknown SIGNAGE_PAGE")
    environ["BROWSER_URL"] = (environ.get("BROWSER_URL") or environ.get("SIGNAGE_URL") or
        f"http://127.0.0.1:{environ.get('SIGNAGE_HTTP_PORT', '8080')}/{pages.get(page, '')}")
    environ["SIGNAGE_URL"] = environ["BROWSER_URL"]
    environ["BROWSER_APP_COMMAND"] = json.dumps(["/usr/bin/python3", "/opt/signage/app/server.py"])
    environ["BROWSER_APP_READY_URL"] = f"http://127.0.0.1:{environ.get('SIGNAGE_HTTP_PORT', '8080')}/healthz"
    environ.setdefault("BROWSER_START_TIMEOUT", "90")
    environ.pop("BROWSER_HTTP_ROOT", None)


if __name__ == "__main__":
    configure(os.environ)
    Path("/run/embedded-browser/video").mkdir(parents=True, exist_ok=True, mode=0o700)
    alias = Path("/run/admiral-signage")
    if not alias.exists() and not alias.is_symlink():
        alias.symlink_to("/run/embedded-browser", target_is_directory=True)
    args = sys.argv[1:] or ["run"]
    if args[0] == "server":
        os.execv("/usr/bin/python3", ["/usr/bin/python3", "/opt/signage/app/server.py", *args[1:]])
    os.execv("/usr/local/bin/embedded-browser-entrypoint", ["/usr/local/bin/embedded-browser-entrypoint", *args])
