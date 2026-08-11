import os
from dataclasses import dataclass

from dotenv import load_dotenv

load_dotenv()


@dataclass
class TestConfig:
    device_url: str
    ha_url: str
    ha_token: str
    floor_name: str
    room_name: str
    light_entity: str
    cover_entity: str
    climate_entity: str
    outlet_entity: str
    outlet_room: str

    @classmethod
    def from_env(cls) -> "TestConfig":
        return cls(
            device_url=os.environ.get("EPAPER_DEVICE_URL", ""),
            ha_url=os.environ.get("HA_URL", ""),
            ha_token=os.environ.get("HA_TOKEN", ""),
            floor_name=os.environ.get("E2E_FLOOR_NAME", "Ground"),
            room_name=os.environ.get("E2E_ROOM_NAME", "Office"),
            light_entity=os.environ.get("E2E_LIGHT_ENTITY", "light.shellydimmer2_485519f94299"),
            cover_entity=os.environ.get("E2E_COVER_ENTITY", "cover.office_blind_left"),
            climate_entity=os.environ.get("E2E_CLIMATE_ENTITY", "climate.office_ac2"),
            outlet_entity=os.environ.get("E2E_OUTLET_ENTITY", "switch.shellyplus1pm_cc7b5c823d78_switch_0"),
            outlet_room=os.environ.get("E2E_OUTLET_ROOM", "Bathroom"),
        )
