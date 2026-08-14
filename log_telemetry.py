"""Log device telemetry from the harness /health endpoint to CSV.

Usage: uv run log_telemetry.py [--url http://10.2.0.221:8080] [--interval 60] [--out telemetry.csv]
"""

import argparse
import csv
import datetime
import os
import time

import requests

FIELDS = [
    "timestamp", "uptime_ms", "battery_pct", "battery_mv", "battery_ma",
    "heap_free", "internal_free", "psram_free", "rssi", "wifi", "home_assistant", "beacon", "reset_reason",
]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default=os.environ.get("EPAPER_DEVICE_URL", "http://10.2.0.221:8080"))
    parser.add_argument("--interval", type=float, default=60.0, help="seconds between samples")
    parser.add_argument("--out", default="telemetry.csv")
    args = parser.parse_args()

    new_file = not os.path.exists(args.out)
    with open(args.out, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
        if new_file:
            writer.writeheader()

        while True:
            row = {"timestamp": datetime.datetime.now().isoformat(timespec="seconds")}
            try:
                health = requests.get(f"{args.url}/health", timeout=5).json()
                row.update(health)
                print(f"{row['timestamp']}  battery={health.get('battery_pct', '--')}% "
                      f"{health.get('battery_mv', '--')}mV {health.get('battery_ma', '--')}mA  "
                      f"rssi={health.get('rssi')}  internal={health.get('internal_free')}")
            except Exception as exc:
                row["wifi"] = f"unreachable: {type(exc).__name__}"
                print(f"{row['timestamp']}  unreachable ({type(exc).__name__})")
            writer.writerow(row)
            f.flush()
            time.sleep(args.interval)


if __name__ == "__main__":
    main()
