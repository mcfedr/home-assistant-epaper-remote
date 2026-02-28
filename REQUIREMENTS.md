# Home Assistant ePaper Remote Requirements

## 1. Purpose

This project provides a family-friendly touchscreen ePaper remote for Home Assistant.
It should be fast to navigate, reliable for daily use, and simple to understand.

## 2. Product Goals

- Provide room-based control of house devices without opening a phone app.
- Keep the interface readable and touch-friendly on a 540x960 ePaper display.
- Reflect Home Assistant state changes in near real-time.
- Minimize configuration burden by discovering floors, rooms, and entities from Home Assistant.

## 3. Supported Hardware and Platform

- Lilygo T5 E-Paper S3 Pro
- M5Paper S3
- Firmware built and flashed via PlatformIO (`lilygo-t5-s3` environment)

## 4. Functional Requirements

### 4.1 Home Assistant Integration

- The device must connect to Home Assistant using the WebSocket API.
- No Home Assistant custom integration/plugin is required.
- The device must authenticate using a long-lived access token.
- The device must discover:
  - Floors
  - Areas/rooms
  - Entities in each room
- Rooms without a floor must be grouped under an "Other Areas" bucket.

### 4.2 UI Modes and Navigation

- The UI must support these states:
  - Boot
  - Wi-Fi disconnected
  - Home Assistant disconnected
  - Invalid Home Assistant token
  - Floor list
  - Room list
  - Room controls
- Navigation flow:
  - Floor list -> Room list -> Room controls
  - Back button from room controls returns to room list
  - Back button from room list returns to floor list

### 4.3 Floor and Room List Screens

- Floor list and room list must render as a grid of tile cards.
- Grid supports paging by horizontal swipe:
  - Swipe left = next page
  - Swipe right = previous page
- Each tile must show:
  - Name (auto-fit/truncated as needed)
  - Icon (mapped from Home Assistant icon name with fallback)
- Screen must show current list page indicator when page count > 1.

### 4.4 Room Controls Screen

- Room controls must support paging by horizontal swipe:
  - Swipe left = next controls page
  - Swipe right = previous controls page
- The header must show:
  - Back icon button
  - Current room name
  - Page indicator (for multi-page rooms)
- Entity ordering on each room page:
  - Climate widgets first
  - Light widgets after climate widgets

### 4.5 Climate Widget Requirements

- Climate widget must be full-width and larger than light widgets.
- Climate widgets must only be shown for cooling-capable climate devices (AC units).
- Thermostat-only climate devices must not be shown.
- AC filtering must be based on Home Assistant `hvac_modes` capabilities:
  - Show only when `hvac_modes` includes `cool` (or equivalent cooling support).
  - Climate entities should remain hidden until `hvac_modes` has been received.
- Climate widget must support:
  - Mode selection with separate buttons
  - Modes: `off`, `heat`, `cool` (only show modes supported by the entity)
  - Target temperature adjustment with `+` / `-` controls
  - Temperature step size: 0.5 degrees
- Active mode must be visually distinct.

### 4.6 Light Widget Requirements

- Light widget must be half-width to allow two per row.
- Light widget must provide on/off control.
- Light widget icon must stay centered with label below.
- Light labels must fit within widget bounds:
  - Font scaling and truncation are required
  - No label text may overlap card borders or adjacent widgets
- Room prefix should be trimmed from light names when duplicated (for readability).

### 4.7 Dynamic Layout and Fitting

- Climate widgets remain fixed at their designed size.
- Light widgets may dynamically shrink to fit page density.
- A minimum light widget height must be enforced to preserve readability and tapability.
- Layout calculations must honor a bottom safe padding so widgets do not overflow the display.
- If a room cannot fit all controls on one page, controls must be split across pages.

### 4.8 Touch Interaction

- Touch targets must include a margin around controls to improve usability.
- Taps activate controls.
- Swipes must not accidentally trigger control toggles.
- Swipe threshold must be high enough to distinguish intentional page navigation from taps.

### 4.9 State Synchronization

- Device must subscribe to Home Assistant updates and refresh widget states.
- Commands sent from touch interactions must update Home Assistant entities.
- UI should redraw efficiently:
  - Full refresh on screen transitions
  - Partial refresh where possible for value-only updates

## 5. Non-Functional Requirements

### 5.1 Reliability

- Device should recover from Wi-Fi and Home Assistant disconnections.
- Discovery and rendering must be robust against large registries and missing icons.
- Memory use must remain stable when opening rooms with many controls.

### 5.2 Performance

- Navigation transitions should feel responsive on ePaper hardware.
- Touch interactions should not block network handling.

### 5.3 Readability and Usability

- Typography must remain legible at all supported widget sizes.
- UI should prioritize simple, household-friendly language and visual hierarchy.
- Buttons and controls must be easy to hit for non-technical users.

## 6. Constraints and Assumptions

- Device remains Wi-Fi connected to receive state updates; battery life is limited.
- Display and touch coordinate system is portrait (`540x960` effective UI canvas).
- Maximum sizes in firmware data model currently include:
  - Floors: 16
  - Rooms: 32
  - Entities: 128

## 7. Acceptance Criteria

- User can boot device, connect to Wi-Fi, and authenticate to Home Assistant.
- User can browse floors and rooms via paged tile grids.
- User can open any room without crash or memory panic.
- Dense rooms with many controls are navigable using room control pages.
- Climate controls support mode switching and +/-0.5 degree temperature changes.
- Light controls remain fully visible (no overflow), tappable, and responsive.
- Back navigation works consistently between all navigation levels.
- UI reflects external Home Assistant state changes after initial load.
