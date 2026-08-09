"""Port of the floor/room grid layout math from src/managers/touch.cpp
(list_grid_layout / list_index_from_touch) and src/constants.h.
Keep in sync with the firmware - tests tap tile centers computed here."""

DISPLAY_WIDTH = 540
DISPLAY_HEIGHT = 960

ROOM_LIST_COLUMNS = 2
ROOM_LIST_ROWS = 4
ROOM_LIST_ROOMS_PER_PAGE = ROOM_LIST_COLUMNS * ROOM_LIST_ROWS
FLOOR_LIST_GRID_START_Y = 120
ROOM_LIST_GRID_START_Y = 100 + 12
ROOM_LIST_GRID_BOTTOM_Y = 860
ROOM_LIST_GRID_MARGIN_X = 20
ROOM_LIST_GRID_GAP_X = 16
ROOM_LIST_GRID_GAP_Y = 16

ROOM_CONTROLS_BACK = (20, 25, 120, 60)
HOME_SETTINGS_BUTTON = (DISPLAY_WIDTH - 92, 31, 64, 64)


def _grid_layout(item_count: int, page_count: int, expand_single_page_layout: bool) -> tuple[int, int, int]:
    columns, rows = ROOM_LIST_COLUMNS, ROOM_LIST_ROWS
    if expand_single_page_layout and page_count == 1 and 0 < item_count <= ROOM_LIST_ROOMS_PER_PAGE:
        if item_count <= 3:
            columns, rows = 1, item_count
        else:
            columns, rows = 2, (item_count + 1) // 2
    return columns, rows, columns * rows


def tile_center(item_idx: int, item_count: int, page: int, grid_start_y: int, expand_single_page_layout: bool) -> tuple[int, int]:
    """Center of the given item's tile, in logical 540x960 coordinates.
    Raises if the item is not on the given page."""
    page_count = max(1, (item_count + ROOM_LIST_ROOMS_PER_PAGE - 1) // ROOM_LIST_ROOMS_PER_PAGE)
    columns, rows, items_per_page = _grid_layout(item_count, page_count, expand_single_page_layout)

    slot = item_idx - page * items_per_page
    if slot < 0 or slot >= items_per_page:
        raise ValueError(f"item {item_idx} is not on page {page}")
    row, col = divmod(slot, columns)

    grid_w = DISPLAY_WIDTH - 2 * ROOM_LIST_GRID_MARGIN_X
    grid_h = ROOM_LIST_GRID_BOTTOM_Y - grid_start_y
    tile_w = (grid_w - (columns - 1) * ROOM_LIST_GRID_GAP_X) // columns
    tile_h = (grid_h - (rows - 1) * ROOM_LIST_GRID_GAP_Y) // rows

    x = ROOM_LIST_GRID_MARGIN_X + col * (tile_w + ROOM_LIST_GRID_GAP_X) + tile_w // 2
    y = grid_start_y + row * (tile_h + ROOM_LIST_GRID_GAP_Y) + tile_h // 2
    return x, y


def floor_tile_center(floor_idx: int, floor_count: int, page: int) -> tuple[int, int]:
    return tile_center(floor_idx, floor_count, page, FLOOR_LIST_GRID_START_Y, True)


def room_tile_center(room_idx: int, room_count: int, page: int) -> tuple[int, int]:
    return tile_center(room_idx, room_count, page, ROOM_LIST_GRID_START_Y, False)


def rect_center(rect: dict) -> tuple[int, int]:
    return rect["x"] + rect["w"] // 2, rect["y"] + rect["h"] // 2
