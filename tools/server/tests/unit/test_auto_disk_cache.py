import pytest
from utils import *

server = ServerPreset.tinyllama2()


@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.tinyllama2()
    server.slot_save_path = "./tmp"
    server.auto_disk_cache = True
    server.server_slots = True
    server.temperature = 0.0


def test_auto_disk_cache_restore_after_slot_erase():
    global server
    server.start()

    body = {
        "max_tokens": 8,
        "messages": [
            {"role": "system", "content": "Test"},
            {"role": "user", "content": "1 2 3 4 5 6"},
        ],
    }

    res = server.make_request("POST", "/chat/completions", data=body)
    assert res.status_code == 200
    assert res.body["usage"]["prompt_tokens_details"]["cached_tokens"] == 0

    res = server.make_request("POST", "/slots/0?action=erase")
    assert res.status_code == 200

    res = server.make_request("POST", "/chat/completions", data=body)
    assert res.status_code == 200
    assert res.body["usage"]["prompt_tokens"] == 77
    assert res.body["usage"]["prompt_tokens_details"]["cached_tokens"] > 0
