import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app"))
import entrypoint


class ConfigurationTests(unittest.TestCase):
    def test_hardware_decoders_and_software_opt_in(self):
        decoder, ranks = entrypoint.decoder_config({})
        self.assertEqual(decoder, "v4l2slh264dec,v4l2slh265dec,v4l2slvp9dec")
        self.assertIn("v4l2slh265dec:512", ranks)
        self.assertIn("avdec_h264:0", ranks)
        with self.assertRaises(ValueError):
            entrypoint.decoder_config({"BROWSER_DECODER": "avdec_h264"})
        self.assertEqual(entrypoint.decoder_config({"BROWSER_ALLOW_SOFTWARE": "1"})[0], "avdec_h264")
        for value in ("nonsense", "v4l2slh264dec,"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                entrypoint.decoder_config({"BROWSER_DECODER": value})

    def test_explicit_decoder_disables_others_and_normalizes(self):
        decoder, ranks = entrypoint.decoder_config({"BROWSER_DECODER": " v4l2slh265dec "})
        self.assertEqual(decoder, "v4l2slh265dec")
        self.assertIn("v4l2slh264dec:0", ranks)

    def test_url_remote_local_and_packaged_defaults(self):
        self.assertEqual(entrypoint.start_url({}), "file:///opt/embedded-browser/www/index.html")
        self.assertEqual(entrypoint.start_url({"BROWSER_HTTP_ROOT": "/app", "BROWSER_HTTP_PORT": "9000"}),
                         "http://127.0.0.1:9000/")
        self.assertEqual(entrypoint.start_url({"BROWSER_HTTP_ROOT": "/app", "BROWSER_URL": "https://example.com/"}),
                         "https://example.com/")

    def test_app_command_is_an_argument_array(self):
        command = ["/usr/bin/python3", "argument with spaces", "$(no shell)"]
        self.assertEqual(entrypoint.app_command({"BROWSER_APP_COMMAND": json.dumps(command)}), command)
        self.assertIsNone(entrypoint.app_command({}))
        for value in ('"/app/server"', "[]", '["relative"]', '["/app", 1]', '["/app\\u0000"]', '{}'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                entrypoint.app_command({"BROWSER_APP_COMMAND": value})
        with self.assertRaises(ValueError):
            entrypoint.app_command({"BROWSER_APP_READY_URL": "http://localhost/"})

    def test_configure_selects_quirks_and_prepares_fresh_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime, video, data = root / "run", root / "run/video", root / "data"
            runtime.mkdir()
            (runtime / "browser-status.json").write_text('stale')
            with patch.object(entrypoint, "RUNTIME", runtime), patch.object(entrypoint, "VIDEO_RUNTIME", video), \
                    patch.dict(os.environ, {"BROWSER_DATA_DIR": str(data)}, clear=True):
                entrypoint.configure()
                self.assertFalse((runtime / "browser-status.json").exists())
                self.assertEqual(os.environ["WEBKIT_GST_HOLE_PUNCH_QUIRK"], "westeros")
                self.assertTrue(video.is_dir())
                self.assertTrue(Path(os.environ["GST_REGISTRY"]).parent.is_dir())
                self.assertEqual(os.environ["XDG_DATA_HOME"], str(data / "browser-data"))
                self.assertTrue((data / "browser-data").is_dir())
                os.environ["BROWSER_VIDEO_MODE"] = "composited"
                entrypoint.configure()
                self.assertNotIn("WEBKIT_GST_HOLE_PUNCH_QUIRK", os.environ)
                self.assertNotIn("WEBKIT_GST_QUIRKS", os.environ)

    def test_cache_invalidation_preserves_only_current_device_registry(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, {}, clear=True):
            data = Path(directory)
            registry_dir = data / "cache/gstreamer-1.0"
            registry_dir.mkdir(parents=True)
            stale = registry_dir / "registry-old.bin"
            stale.write_text("stale")
            entrypoint.configure_caches(data)
            self.assertFalse(stale.exists())
            self.assertTrue(os.environ["GST_REGISTRY"].startswith(str(registry_dir)))


if __name__ == "__main__":
    unittest.main()
