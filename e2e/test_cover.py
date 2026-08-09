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
    # CoverWidget: Up control on the left half, Down control on the right half
    left = (rect["x"] + rect["w"] // 4, rect["y"] + rect["h"] // 2)
    right = (rect["x"] + 3 * rect["w"] // 4, rect["y"] + rect["h"] // 2)
    down, up = (right, left) if original == "open" else (left, right)

    moving_or_moved = {"closing", "opening", "closed" if original == "open" else "open"}
    try:
        at_home.tap(*down)
        ha.wait_for_state(cfg.cover_entity, moving_or_moved, timeout=15)
    finally:
        # Reverse to the original position
        at_home.tap(*up)
        ha.call_service("cover", "open_cover" if original == "open" else "close_cover", {"entity_id": cfg.cover_entity})
        ha.wait_for_state(cfg.cover_entity, {original}, timeout=60)
