import pytest

from .conftest import find_widget, goto_room

pytestmark = pytest.mark.device


def test_climate_adjust_temperature(at_home, ha, cfg):
    """Tap the climate widget's +0.5 control and verify HA target temp rises."""
    original = ha.get_state(cfg.climate_entity)
    if original["state"] == "off":
        pytest.skip("climate is off; skipping temperature adjustment")
    original_temp = original["attributes"]["temperature"]

    state = goto_room(at_home, cfg)
    widget = find_widget(state, cfg.climate_entity)
    if widget is None:
        pytest.skip(f"{cfg.climate_entity} not on the first controls page")

    rect = widget["rect"]
    # ClimateWidget: temperature +/- on the lower row; + on the right
    plus = (rect["x"] + rect["w"] - rect["w"] // 6, rect["y"] + rect["h"] - rect["h"] // 4)

    try:
        at_home.tap(*plus)

        def temp_rose(_state):
            current = ha.get_state(cfg.climate_entity)["attributes"]["temperature"]
            return current == pytest.approx(original_temp + 0.5)

        at_home.wait_for(temp_rose, timeout=15, poll=1.0)
    finally:
        ha.call_service("climate", "set_temperature", {"entity_id": cfg.climate_entity, "temperature": original_temp})
