import pytest

pytestmark = pytest.mark.device

KNOWN_MODES = {
    "Blank", "Boot", "GenericError", "WifiDisconnected", "HassDisconnected", "HassInvalidKey",
    "FloorList", "RoomList", "RoomControls", "Standby", "SettingsMenu", "WifiSettings", "WifiPassword",
}


def test_health(device):
    health = device.health()
    assert health["status"] == "ok"
    assert health["heap_free"] > 0
    assert health["psram_free"] > 0
    assert health["wifi"] == "up"


def test_state_parses(device):
    state = device.state()
    assert state["mode"] in KNOWN_MODES
    assert isinstance(state["floors"], list)
    assert isinstance(state["widgets"], list)
