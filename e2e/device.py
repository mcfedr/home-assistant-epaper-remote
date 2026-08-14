import time
from typing import Any, Callable

import requests
from PIL import Image


class DeviceClient:
    def __init__(self, base_url: str, timeout: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()

    def health(self) -> dict:
        resp = self.session.get(f"{self.base_url}/health", timeout=self.timeout)
        resp.raise_for_status()
        return resp.json()

    def state(self) -> dict:
        resp = self.session.get(f"{self.base_url}/state", timeout=self.timeout)
        resp.raise_for_status()
        return resp.json()

    def tap(self, x: int, y: int, hold_ms: int = 150) -> None:
        # 60ms holds are consumed but occasionally race the touch task's idle
        # poll and mode transitions; 150ms lands reliably
        resp = self.session.post(f"{self.base_url}/tap", json={"x": x, "y": y, "hold_ms": hold_ms}, timeout=self.timeout)
        resp.raise_for_status()

    def swipe(self, x1: int, y1: int, x2: int, y2: int, duration_ms: int | None = None) -> None:
        body: dict[str, Any] = {"x1": x1, "y1": y1, "x2": x2, "y2": y2}
        if duration_ms is not None:
            body["duration_ms"] = duration_ms
        resp = self.session.post(f"{self.base_url}/swipe", json=body, timeout=self.timeout)
        resp.raise_for_status()

    def home(self) -> None:
        resp = self.session.post(f"{self.base_url}/home", timeout=self.timeout)
        resp.raise_for_status()

    def set_power(self, **kwargs: Any) -> dict:
        """POST /power options: modem_sleep, cpu_mhz, standby_sleep."""
        resp = self.session.post(f"{self.base_url}/power", json=kwargs, timeout=self.timeout)
        resp.raise_for_status()
        return resp.json()

    def screenshot(self) -> Image.Image:
        resp = self.session.get(f"{self.base_url}/screenshot", timeout=30.0)
        resp.raise_for_status()
        mode = resp.headers["X-EPD-Mode"]
        native_w = int(resp.headers["X-EPD-Native-Width"])
        native_h = int(resp.headers["X-EPD-Native-Height"])
        return decode_framebuffer(resp.content, mode, native_w, native_h)

    def wait_for(self, predicate: Callable[[dict], bool], timeout: float = 10.0, poll: float = 0.2) -> dict:
        """Poll /state until predicate(state) is true; returns the state. Raises on timeout."""
        deadline = time.monotonic() + timeout
        state = self.state()
        while not predicate(state):
            if time.monotonic() > deadline:
                raise TimeoutError(f"device state condition not met within {timeout}s; last mode={state.get('mode')}")
            time.sleep(poll)
            state = self.state()
        return state

    def wait_for_mode(self, mode: str, timeout: float = 10.0) -> dict:
        return self.wait_for(lambda s: s["mode"] == mode, timeout=timeout)


def decode_framebuffer(raw: bytes, mode: str, native_w: int, native_h: int) -> Image.Image:
    """Decode the native landscape framebuffer into a logical portrait image.

    The panel buffer is native landscape (960x540); the firmware draws rotated 90deg.
    Logical (x, y) lives at native (col=y, row=native_h-1-x), so rotating the native
    image 270deg CW yields the portrait screen as seen by the user."""
    if mode == "1bpp":
        # MSB-first, bit set = white
        img = Image.frombytes("1", (native_w, native_h), raw).convert("L")
    elif mode == "4bpp":
        # Two pixels per byte, high nibble first, 0xF = white
        expanded = bytearray(native_w * native_h)
        idx = 0
        for byte in raw:
            expanded[idx] = (byte >> 4) * 17
            expanded[idx + 1] = (byte & 0x0F) * 17
            idx += 2
        img = Image.frombytes("L", (native_w, native_h), bytes(expanded))
    else:
        raise ValueError(f"unknown framebuffer mode {mode}")

    return img.transpose(Image.Transpose.ROTATE_270)


def image_diff_ratio(a: Image.Image, b: Image.Image, region: tuple[int, int, int, int] | None = None) -> float:
    """Fraction of pixels differing by more than a small threshold. region = (x, y, w, h)."""
    if region is not None:
        x, y, w, h = region
        a = a.crop((x, y, x + w, y + h))
        b = b.crop((x, y, x + w, y + h))
    if a.size != b.size:
        raise ValueError("image sizes differ")
    pa, pb = a.tobytes(), b.tobytes()
    different = sum(1 for va, vb in zip(pa, pb) if abs(va - vb) > 32)
    return different / len(pa)
