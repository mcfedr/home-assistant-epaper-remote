import pytest

from . import layout
from .conftest import find_widget, goto_room

pytestmark = pytest.mark.device


def test_cover_from_device(at_home, ha, cfg):
    """Tap the cover widget's Down then Up controls and verify HA sees movement.
    The blind physically moves; we stop it quickly and restore by reopening."""
    original = ha.get_state(cfg.cover_entity)["state"]
    if original not in ("open", "closed"):
        pytest.skip(f"cover in transient state {original!r}")

    state = goto_room(at_home, cfg)
    widget = find_widget(state, cfg.cover_entity)
    if widget is None:
        pytest.skip(f"{cfg.cover_entity} not on the first controls page")

    rect = widget["rect"]
    # CoverWidget: Up | Stop | Down thirds
    left = (rect["x"] + rect["w"] // 6, rect["y"] + rect["h"] // 2)
    middle = (rect["x"] + rect["w"] // 2, rect["y"] + rect["h"] // 2)
    right = (rect["x"] + 5 * rect["w"] // 6, rect["y"] + rect["h"] // 2)
    down = right if original == "open" else left

    moving_or_moved = {"closing", "opening", "closed" if original == "open" else "open"}
    try:
        at_home.tap(*down)
        # The blind group reports lazily, notably slower under suite load
        ha.wait_for_state(cfg.cover_entity, moving_or_moved, timeout=40)

        # Stop mid-travel and verify the cover settles without completing
        import time

        time.sleep(2)
        at_home.tap(*middle)

        # The blinds report position lazily; wait for a mid-travel position to
        # show up rather than sampling the (stale) state right after the tap.
        def stopped_mid_travel(s):
            position = s["attributes"].get("current_position")
            return s["state"] not in ("closing", "opening") and position is not None and 0 < position < 100

        ha.wait_for(stopped_mid_travel, cfg.cover_entity, timeout=30)
    finally:
        ha.call_service("cover", "open_cover" if original == "open" else "close_cover", {"entity_id": cfg.cover_entity})
        ha.wait_for_state(cfg.cover_entity, {original}, timeout=90)
