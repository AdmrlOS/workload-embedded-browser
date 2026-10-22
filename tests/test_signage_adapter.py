import importlib.util
import unittest
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "examples/signage/entrypoint.py"
spec = importlib.util.spec_from_file_location("signage_adapter", path)
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class SignageAdapterTests(unittest.TestCase):
    def test_player_default_and_backend_command(self):
        env = {}
        adapter.configure(env)
        self.assertEqual(env["BROWSER_URL"], "http://127.0.0.1:8080/player/")
        self.assertEqual(env["BROWSER_APP_READY_URL"], "http://127.0.0.1:8080/healthz")
        self.assertIn("/opt/signage/app/server.py", env["BROWSER_APP_COMMAND"])

    def test_legacy_settings_and_explicit_new_controls(self):
        env = {"SIGNAGE_VIDEO_MODE": "holepunch", "BROWSER_VIDEO_MODE": "composited",
               "SIGNAGE_DATA_DIR": "/legacy-data", "SIGNAGE_PAGE": "menuboard", "SIGNAGE_HTTP_PORT": "9000"}
        adapter.configure(env)
        self.assertEqual(env["BROWSER_VIDEO_MODE"], "composited")
        self.assertEqual(env["SIGNAGE_VIDEO_MODE"], "composited")
        self.assertEqual(env["BROWSER_DATA_DIR"], "/legacy-data")
        self.assertEqual(env["BROWSER_URL"], "http://127.0.0.1:9000/menuboard/")

    def test_url_override_and_unknown_page(self):
        env = {"SIGNAGE_URL": "https://example.com/", "SIGNAGE_PAGE": "unknown"}
        adapter.configure(env)
        self.assertEqual(env["BROWSER_URL"], "https://example.com/")
        with self.assertRaises(ValueError):
            adapter.configure({"SIGNAGE_PAGE": "unknown"})


if __name__ == "__main__":
    unittest.main()
