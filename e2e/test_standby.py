import time

import pytest

from .conftest import save_artifact

pytestmark = [pytest.mark.device, pytest.mark.slow]

STANDBY_IDLE_TIMEOUT_S = 120


def test_standby_sleep_inhibited(device):
    """The conftest disables standby sleep for the suite; the device must
    report that instead of deep-sleeping mid-run."""
    health = device.health()
    assert health["standby_sleep"] is False
    assert health["sleep_inhibit"] == "disabled"


def test_standby_and_wake(at_home):
    """After the idle timeout the device enters standby; a tap wakes it into the
    device's room (per Bermuda) or the floor list when the room is unknown."""
    time.sleep(STANDBY_IDLE_TIMEOUT_S)
    state = at_home.wait_for_mode("Standby", timeout=60)
    assert state["mode"] == "Standby"

    image = at_home.screenshot()
    save_artifact(image, "standby")

    # Bermuda may move the device's believed room at any time; sample it as
    # close to the wake tap as possible
    device_room = at_home.state()["device_room"]
    # Tap away from the battery node (which triggers a SoC refresh instead of waking)
    at_home.tap(270, 100)
    if device_room >= 0:
        state = at_home.wait_for_mode("RoomControls", timeout=30)
        assert state["selected_room"] == device_room
    else:
        at_home.wait_for_mode("FloorList", timeout=30)
