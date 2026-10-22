"""Integration checks through the real OCI init, launcher and app supervisor."""
import json
import subprocess
import sys
import time
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent / "fixtures/browser.py"


def docker(*args, check=True):
    result = subprocess.run(["docker", *args], capture_output=True, text=True, timeout=60)
    if check and result.returncode:
        raise AssertionError(f"docker {args}:\n{result.stdout}\n{result.stderr}")
    return result


def state(container):
    return json.loads(docker("inspect", container).stdout)[0]["State"]


def start(image, *options, command="run", fixture=False):
    if fixture:
        options = (*options, "--mount", f"type=bind,src={FIXTURE},dst=/usr/local/bin/embedded-browser,readonly")
    return docker("run", "--detach", "--workdir", "/", "--tmpfs", "/run", "--tmpfs", "/tmp",
                  "-e", "BROWSER_INPUT=0", *options, image, command).stdout.strip()


def probe(container, script):
    return docker("exec", container, "/usr/bin/python3", "-c", script, check=False)


def ready(container, script):
    for _ in range(100):
        result = probe(container, script)
        if result.returncode == 0:
            return result.stdout
        if not state(container)["Running"]:
            raise AssertionError(docker("logs", container).stdout + docker("logs", container).stderr)
        time.sleep(0.1)
    raise AssertionError("Readiness probe failed: " + result.stderr)


def stopped(container):
    for _ in range(100):
        current = state(container)
        if not current["Running"]:
            assert current["ExitCode"] not in (0, 137), current
            assert not current["OOMKilled"], current
            return
        time.sleep(0.1)
    raise AssertionError("Supervisor did not stop after required-service failure")


def main(image):
    result = docker("run", "--rm", "--workdir", "/", "--tmpfs", "/run", "--tmpfs", "/tmp",
        image, "/usr/bin/python3", "-c",
        "import os,sys; assert os.getcwd() == '/'; assert sys.argv[1:] == ['argument with spaces']; sys.exit(37)",
        "argument with spaces", check=False)
    assert result.returncode == 37, result

    container = start(image, command="server")
    try:
        ready(container, "from urllib.request import urlopen; "
              "assert urlopen('http://127.0.0.1:8080/healthz', timeout=1).status == 200; "
              "assert b'Admiral embedded browser' in urlopen('http://127.0.0.1:8080/').read()")
        docker("stop", "--time", "8", container)
        assert state(container)["ExitCode"] == 143, state(container)
    finally:
        docker("rm", "--force", container)

    # Remote URL: no static or signage server is required to launch the browser.
    container = start(image, "-e", "BROWSER_URL=https://example.com/dashboard", fixture=True)
    try:
        observed = json.loads(ready(container, "from pathlib import Path; print(Path('/tmp/browser.json').read_text())"))
        assert observed["args"] == ["https://example.com/dashboard"], observed
        assert probe(container, "import socket; socket.create_connection(('127.0.0.1',8080),timeout=.2)").returncode != 0
        docker("stop", "--time", "8", container)
        assert state(container)["ExitCode"] == 0, state(container)
        assert "Fixture browser received SIGTERM" in docker("logs", container).stdout
    finally:
        docker("rm", "--force", container)

    app = ["/usr/bin/python3", "-u", "-c", "import os,time; from pathlib import Path; "
           "Path('/tmp/app.pid').write_text(str(os.getpid())); time.sleep(300)"]
    container = start(image, "-e", "BROWSER_HTTP_ROOT=/opt/embedded-browser/www", "-e",
                      "BROWSER_APP_COMMAND=" + json.dumps(app), fixture=True)
    try:
        observed = json.loads(ready(container, "from pathlib import Path; print(Path('/tmp/browser.json').read_text())"))
        assert observed["args"] == ["http://127.0.0.1:8080/"], observed
        probe(container, "import os,signal; from pathlib import Path; os.kill(int(Path('/tmp/app.pid').read_text()),signal.SIGTERM)")
        stopped(container)
        assert "Fixture browser received SIGTERM" in docker("logs", container).stdout
    finally:
        docker("rm", "--force", container)

    container = start(image, "-e", "BROWSER_START_TIMEOUT=0.5", "-e", "BROWSER_APP_COMMAND=" + json.dumps(app),
                      "-e", "BROWSER_APP_READY_URL=http://127.0.0.1:59999/healthz", fixture=True)
    try:
        stopped(container)
        logs = docker("logs", container)
        assert "Timed out waiting" in logs.stderr, logs
        assert "Embedded browser started" not in logs.stdout, logs
    finally:
        docker("rm", "--force", container)

    # The native executable itself must fail clearly without any display devices.
    result = docker("run", "--rm", "--workdir", "/", "--tmpfs", "/run", "--tmpfs", "/tmp",
                    "-e", "BROWSER_INPUT=0", image, check=False)
    assert result.returncode != 0, result
    assert "No connected atomic DRM display" in result.stderr, result
    print("OCI dispatch, static server, remote URL, app supervision, readiness failure, native failure and SIGTERM checks passed.")


if __name__ == "__main__":
    main(sys.argv[1])
