import pytest

pytestmark = pytest.mark.device


def test_swipe_pagination(at_home):
    """On the room list, swipe left to page forward and right to page back."""
    state = at_home.state()
    floors = state["floors"]
    assert floors, "no floors discovered"

    from . import layout
    from .conftest import goto_room  # noqa: F401 (kept for symmetry)

    x, y = layout.floor_tile_center(0, len(floors), state["floor_list_page"])
    at_home.tap(x, y)
    state = at_home.wait_for_mode("RoomList")

    rooms = state["rooms"]
    if len(rooms) <= layout.ROOM_LIST_ROOMS_PER_PAGE:
        pytest.skip("room list fits on one page; nothing to paginate")

    assert state["room_list_page"] == 0
    at_home.swipe(450, 480, 100, 480)
    at_home.wait_for(lambda s: s["room_list_page"] == 1)

    at_home.swipe(100, 480, 450, 480)
    at_home.wait_for(lambda s: s["room_list_page"] == 0)
