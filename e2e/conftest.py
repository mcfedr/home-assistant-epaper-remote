import pathlib
import time

import pytest

from .config import TestConfig
from .device import DeviceClient
from .ha import HAClient
from . import layout

ARTIFACTS_DIR = pathlib.Path(__file__).parent / "artifacts"


@pytest.fixture(scope="session")
def cfg() -> TestConfig:
    config = TestConfig.from_env()
    if not config.device_url:
        pytest.skip("EPAPER_DEVICE_URL not set")
    return config


@pytest.fixture(scope="session")
def device(cfg: TestConfig) -> DeviceClient:
    client = DeviceClient(cfg.device_url)
    deadline = time.monotonic() + 30
    while True:
        try:
            client.health()
            break
        except Exception:
            if time.monotonic() > deadline:
                pytest.skip(f"device not reachable at {cfg.device_url}")
            time.sleep(1)

    # A deep-sleeping device is only wakeable physically — synthetic taps
    # can't fire the hardware interrupt. Keep it awake for the whole run.
    client.set_power(standby_sleep=False)
    yield client
    try:
        client.set_power(standby_sleep=True)
    except Exception:
        pass  # device may be rebooting at teardown; the default re-arms on next boot


@pytest.fixture(scope="session")
def ha(cfg: TestConfig) -> HAClient:
    if not cfg.ha_url or not cfg.ha_token:
        pytest.skip("HA_URL / HA_TOKEN not set")
    return HAClient(cfg.ha_url, cfg.ha_token)


@pytest.fixture
def at_home(device: DeviceClient):
    """Start the test from the floor list."""
    device.home()
    device.wait_for_mode("FloorList")
    yield device


def save_artifact(image, name: str) -> pathlib.Path:
    ARTIFACTS_DIR.mkdir(exist_ok=True)
    path = ARTIFACTS_DIR / f"{name}.png"
    image.save(path)
    return path


def goto_room(device: DeviceClient, cfg: TestConfig) -> dict:
    """Navigate FloorList -> RoomList -> RoomControls for the configured room.
    Returns the RoomControls state."""
    state = device.wait_for_mode("FloorList")
    floors = [f["name"] for f in state["floors"]]
    assert cfg.floor_name in floors, f"floor {cfg.floor_name!r} not in {floors}"
    floor_idx = floors.index(cfg.floor_name)
    x, y = layout.floor_tile_center(floor_idx, len(floors), state["floor_list_page"])
    device.tap(x, y)
    state = device.wait_for_mode("RoomList")

    rooms = [r["name"] for r in state["rooms"]]
    assert cfg.room_name in rooms, f"room {cfg.room_name!r} not in {rooms}"
    room_idx = rooms.index(cfg.room_name)

    # Swipe to the page holding the room if needed
    target_page = room_idx // layout.ROOM_LIST_ROOMS_PER_PAGE
    while state["room_list_page"] < target_page:
        prev_page = state["room_list_page"]
        device.swipe(450, 480, 100, 480)
        state = device.wait_for(lambda s: s["room_list_page"] != prev_page or s["mode"] != "RoomList")
        assert state["mode"] == "RoomList"

    x, y = layout.room_tile_center(room_idx, len(rooms), state["room_list_page"])
    device.tap(x, y)
    return device.wait_for_mode("RoomControls")


def find_widget(state: dict, entity_id: str) -> dict | None:
    for widget in state["widgets"]:
        if widget["entity_id"] == entity_id:
            return widget
    return None
