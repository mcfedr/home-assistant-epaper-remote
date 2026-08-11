import dataclasses

import pytest

from . import layout
from .conftest import find_widget, goto_room

pytestmark = pytest.mark.device


def test_tap_outlet_toggles_ha(at_home, ha, cfg):
    original = ha.get_state(cfg.outlet_entity)["state"]
    if original == "unavailable":
        pytest.skip("outlet unavailable")

    try:
        ha.turn_off(cfg.outlet_entity)
        ha.wait_for_state(cfg.outlet_entity, "off")

        room_cfg = dataclasses.replace(cfg, room_name=cfg.outlet_room)
        state = goto_room(at_home, room_cfg)
        widget = find_widget(state, cfg.outlet_entity)
        assert widget is not None, f"{cfg.outlet_entity} not in {[w['entity_id'] for w in state['widgets']]}"
        assert widget["type"] == "switch"

        x, y = layout.rect_center(widget["rect"])
        at_home.tap(x, y)
        ha.wait_for_state(cfg.outlet_entity, "on")

        at_home.tap(x, y)
        ha.wait_for_state(cfg.outlet_entity, "off")
    finally:
        if original == "on":
            ha.turn_on(cfg.outlet_entity)
        else:
            ha.turn_off(cfg.outlet_entity)
