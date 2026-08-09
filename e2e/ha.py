import time
from typing import Any

import requests


class HAClient:
    def __init__(self, base_url: str, token: str, timeout: float = 10.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session = requests.Session()
        self.session.headers["Authorization"] = f"Bearer {token}"

    def get_state(self, entity_id: str) -> dict:
        resp = self.session.get(f"{self.base_url}/api/states/{entity_id}", timeout=self.timeout)
        resp.raise_for_status()
        return resp.json()

    def call_service(self, domain: str, service: str, data: dict[str, Any]) -> None:
        resp = self.session.post(f"{self.base_url}/api/services/{domain}/{service}", json=data, timeout=self.timeout)
        resp.raise_for_status()

    def turn_on(self, entity_id: str) -> None:
        self.call_service(entity_id.split(".")[0], "turn_on", {"entity_id": entity_id})

    def turn_off(self, entity_id: str) -> None:
        self.call_service(entity_id.split(".")[0], "turn_off", {"entity_id": entity_id})

    def wait_for_state(self, entity_id: str, expected: str | set[str], timeout: float = 15.0, poll: float = 0.5) -> dict:
        expected_states = {expected} if isinstance(expected, str) else expected
        return self.wait_for(lambda s: s["state"] in expected_states, entity_id, timeout=timeout, poll=poll)

    def wait_for(self, predicate, entity_id: str, timeout: float = 15.0, poll: float = 0.5) -> dict:
        deadline = time.monotonic() + timeout
        state = self.get_state(entity_id)
        while not predicate(state):
            if time.monotonic() > deadline:
                raise TimeoutError(f"{entity_id} condition not met within {timeout}s; state={state['state']}")
            time.sleep(poll)
            state = self.get_state(entity_id)
        return state
