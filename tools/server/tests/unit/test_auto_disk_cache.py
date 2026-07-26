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


def test_auto_disk_cache_requires_matching_model_identity(tmp_path):
    global server
    server.slot_save_path = str(tmp_path)
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


def test_auto_disk_cache_index_does_not_store_plaintext_messages(tmp_path):
    global server
    server.slot_save_path = str(tmp_path)
    server.start()

    user_text = "sensitive cache text 1 2 3 4"
    body = {
        "max_tokens": 8,
        "messages": [
            {"role": "system", "content": "Test"},
            {"role": "user", "content": user_text},
        ],
    }

    res = server.make_request("POST", "/chat/completions", data=body)
    assert res.status_code == 200

    server.stop()

    index_path = tmp_path / "index.json"
    index_text = index_path.read_text(encoding="utf-8")
    assert user_text not in index_text

    server.stop()

    server = ServerPreset.tinyllama2()
    server.slot_save_path = str(tmp_path)
    server.auto_disk_cache = True
    server.server_slots = True
    server.temperature = 0.0
    server.model_alias = "tinyllama-2-renamed"
    server.start()

    res = server.make_request("POST", "/chat/completions", data=body)
    assert res.status_code == 200
    assert res.body["usage"]["prompt_tokens_details"]["cached_tokens"] == 0
