"""Check the image without a GPU; board acceleration needs separate validation."""
import json
import subprocess
from pathlib import Path
from diagnostics import VIDEO_RUNTIME


def check(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"Failed {args}:\n{result.stdout}\n{result.stderr}")
    return result.stdout + result.stderr


def main():
    print(check(["/usr/local/bin/embedded-browser", "--version"]).strip())
    adapter = check(["gst-inspect-1.0", "westerossink"])
    for name in ("rectangle", "zorder", "res-usage"):
        assert name in adapter, name
    kms = check(["gst-inspect-1.0", "kmssink"])
    assert "fd " in kms and "skip-vsync" in kms
    library = next(Path("/usr/lib").glob("*/libWPEWebKit-2.0.so.1"))
    data = library.read_bytes()
    assert b"WEBKIT_GST_HOLE_PUNCH_QUIRK" in data and b"westeros" in data
    if not (VIDEO_RUNTIME / "drm.sock").exists():
        result = subprocess.run(["gst-launch-1.0", "-q", "videotestsrc", "num-buffers=1", "!",
            "video/x-raw,format=NV12", "!", "westerossink"], capture_output=True, text=True, timeout=15)
        assert result.returncode != 0, "Adapter must reject a missing DRM broker"
        status = json.loads((VIDEO_RUNTIME / "video-status.json").read_text())
        assert status["hardware_decode"]["verified"] is False
        assert status["holepunch"]["verified"] is False
        assert status["error"]
    print("Image smoke checks passed. GPU, decode and plane blending require the T527 board.")


if __name__ == "__main__":
    main()
