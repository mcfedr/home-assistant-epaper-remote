import pytest

from . import layout
from .conftest import find_widget, goto_room, save_artifact
from .device import image_diff_ratio

pytestmark = pytest.mark.device


@pytest.fixture
def light_restored(ha, cfg):
    original = ha.get_state(cfg.light_entity)["state"]
    yield
    if original == "on":
        ha.turn_on(cfg.light_entity)
    else:
        ha.turn_off(cfg.light_entity)


def test_tap_light_toggles_ha(at_home, ha, cfg, light_restored):
    ha.turn_off(cfg.light_entity)
    ha.wait_for_state(cfg.light_entity, "off")

    state = goto_room(at_home, cfg)
    widget = find_widget(state, cfg.light_entity)
    assert widget is not None, f"{cfg.light_entity} not found in widgets"

    before = at_home.screenshot()

    x, y = layout.rect_center(widget["rect"])
    at_home.tap(x, y)

    ha.wait_for_state(cfg.light_entity, "on")

    # Device redraws the widget as a partial update; give it a moment
    at_home.wait_for(lambda s: (find_widget(s, cfg.light_entity) or {}).get("value", 0) > 0)
    after = at_home.screenshot()
    save_artifact(before, "light_before")
    save_artifact(after, "light_after")
    rect = widget["rect"]
    assert image_diff_ratio(before, after, (rect["x"], rect["y"], rect["w"], rect["h"])) > 0.01

    # And back off from the device as well
    at_home.tap(x, y)
    ha.wait_for_state(cfg.light_entity, "off")


def test_ha_change_reflected_on_device(at_home, ha, cfg, light_restored):
    ha.turn_off(cfg.light_entity)
    ha.wait_for_state(cfg.light_entity, "off")

    state = goto_room(at_home, cfg)
    widget = find_widget(state, cfg.light_entity)
    assert widget is not None

    ha.turn_on(cfg.light_entity)
    # HASS_IGNORE_UPDATE_DELAY_MS plus websocket latency
    at_home.wait_for(lambda s: (find_widget(s, cfg.light_entity) or {}).get("value", 0) > 0, timeout=15)
