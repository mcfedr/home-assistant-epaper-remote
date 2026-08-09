import pytest

from .conftest import find_widget, goto_room

pytestmark = pytest.mark.device


def test_climate_adjust_temperature(at_home, ha, cfg):
    """Set a known target via HA, wait for the device to converge, tap +0.5, verify HA."""
    original = ha.get_state(cfg.climate_entity)
    if original["state"] in ("off", "unavailable"):
        pytest.skip(f"climate is {original['state']}; skipping temperature adjustment")
    original_temp = original["attributes"]["temperature"]

    state = goto_room(at_home, cfg)
    widget = find_widget(state, cfg.climate_entity)
    if widget is None:
        pytest.skip(f"{cfg.climate_entity} not on the first controls page")

    base_temp = 21.0

    try:
        # The AC integration polls (~30s) and drops out periodically; use generous timeouts
        ha.call_service("climate", "set_temperature", {"entity_id": cfg.climate_entity, "temperature": base_temp})
        ha.wait_for(lambda s: s["attributes"]["temperature"] == base_temp, cfg.climate_entity, timeout=40)

        # Wait for the device widget to pick up the pushed state; remember its packed value.
        # One temperature step (0.5C) is one increment of the packed value.
        baseline = at_home.wait_for(
            lambda s: (find_widget(s, cfg.climate_entity) or {}).get("value") is not None, timeout=5
        )
        v0 = find_widget(baseline, cfg.climate_entity)["value"]
        import time

        time.sleep(2)  # let any in-flight updates settle
        v0 = find_widget(at_home.state(), cfg.climate_entity)["value"]

        rect = widget["rect"]
        plus = (rect["x"] + rect["w"] - rect["w"] // 6, rect["y"] + rect["h"] - rect["h"] // 4)
        at_home.tap(*plus)

        at_home.wait_for(lambda s: (find_widget(s, cfg.climate_entity) or {}).get("value") == v0 + 1, timeout=10)
        ha.wait_for(lambda s: s["attributes"]["temperature"] == base_temp + 0.5, cfg.climate_entity, timeout=40)
    finally:
        ha.call_service("climate", "set_temperature", {"entity_id": cfg.climate_entity, "temperature": original_temp})
