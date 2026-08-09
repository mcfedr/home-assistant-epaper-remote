import pytest

from .conftest import goto_room, save_artifact

pytestmark = pytest.mark.device


def test_boot_floor_list(at_home, cfg):
    state = at_home.state()
    assert state["mode"] == "FloorList"
    floors = [f["name"] for f in state["floors"]]
    assert cfg.floor_name in floors

    image = at_home.screenshot()
    save_artifact(image, "floor_list")
    assert image.size == (540, 960)
    extrema = image.getextrema()
    assert extrema[0] != extrema[1], "screenshot is a solid color"


def test_navigate_to_room(at_home, cfg):
    state = goto_room(at_home, cfg)
    assert state["selected_room"] >= 0
    entity_ids = [w["entity_id"] for w in state["widgets"]]
    assert cfg.light_entity in entity_ids, f"light not on first controls page: {entity_ids}"

    image = at_home.screenshot()
    save_artifact(image, "room_controls")
    assert image.size == (540, 960)


def test_home_button(at_home, cfg):
    goto_room(at_home, cfg)
    at_home.home()
    at_home.wait_for_mode("FloorList")


def test_back_button(at_home, cfg):
    from . import layout

    goto_room(at_home, cfg)
    x, y, w, h = layout.ROOM_CONTROLS_BACK
    at_home.tap(x + w // 2, y + h // 2)
    at_home.wait_for_mode("RoomList")
