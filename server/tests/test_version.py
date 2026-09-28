from inkboard_server.version import firmware_version, server_version, version_label


def test_firmware_version_from_user_agent():
    assert firmware_version("inkboard/1.0.0") == "1.0.0"
    assert firmware_version("inkboard/1.0.0-c9d8ef2") == "1.0.0-c9d8ef2"
    assert firmware_version("inkboard/1.0") == "1.0"  # boards flashed before VERSION existed


def test_firmware_version_ignores_other_clients():
    assert firmware_version(None) is None
    assert firmware_version("Mozilla/5.0 (X11; Linux x86_64)") is None
    assert firmware_version("inkboard/") is None
    assert firmware_version("inkboard/<b>1</b>") is None
    assert firmware_version("inkboard/" + "1" * 40) is None


def test_version_label():
    assert version_label("1.0.0", "1.0.0") == "fw 1.0.0 · srv 1.0.0"
    assert version_label(None, "1.0.0-c9d8ef2") == "srv 1.0.0-c9d8ef2"
    assert version_label(None, None) == ""


def test_server_version_prefers_env(tmp_path):
    (tmp_path / "VERSION").write_text("1.0.0\n")
    assert server_version({"INKBOARD_VERSION": "1.0.0-c9d8ef2"}, tmp_path) == "1.0.0-c9d8ef2"
    assert server_version({}, tmp_path) == "1.0.0"
    assert server_version({"INKBOARD_VERSION": ""}, tmp_path) == "1.0.0"
    assert server_version({}, tmp_path / "missing") == "dev"
