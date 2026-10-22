#!/usr/bin/python3
"""A display-free stand-in for exercising the real image supervisor in CI."""
import json
import os
import signal
import sys
import time
from pathlib import Path


def stop(signum, frame):
    print("Fixture browser received SIGTERM", flush=True)
    sys.exit(0)


signal.signal(signal.SIGTERM, stop)
Path("/tmp/browser.json").write_text(json.dumps({"pid": os.getpid(), "args": sys.argv[1:]}))
while True:
    time.sleep(0.1)
