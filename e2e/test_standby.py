import time

import pytest

from .conftest import save_artifact

pytestmark = [pytest.mark.device, pytest.mark.slow]

STANDBY_IDLE_TIMEOUT_S = 120


def test_standby_and_wake(at_home):
    """After the idle timeout the device enters standby; a tap wakes it to the floor list."""
    time.sleep(STANDBY_IDLE_TIMEOUT_S)
    state = at_home.wait_for_mode("Standby", timeout=60)
    assert state["mode"] == "Standby"

    image = at_home.screenshot()
    save_artifact(image, "standby")

    # Tap away from the battery node (which triggers a SoC refresh instead of waking)
    at_home.tap(270, 100)
    at_home.wait_for_mode("FloorList", timeout=30)
