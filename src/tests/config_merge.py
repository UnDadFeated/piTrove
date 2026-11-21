"""Verify that installer updates preserve private and unrecognized config entries."""
from pathlib import Path
import stat
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="pitrove-config-merge-") as temporary:
    source = Path(temporary) / "config.toml"
    source.write_text(
        '[slideshow]\nbrightness_auto = 1\nbrightness_auto_min = 65\n'
        '[remote]\napi_key = "fixture-key"\n'
        '[mqtt]\npass = "fixture-secret # still inside the string"\n'
        '[future_section]\nfuture_setting = ["first", "second"]\n',
        encoding="utf-8",
    )
    source.chmod(0o644)
    command = [sys.executable, "scripts/merge_config.py", "src/config.toml", str(source)]
    subprocess.run(command, check=True)
    assert stat.S_IMODE(source.stat().st_mode) == 0o600
    result = source.read_text(encoding="utf-8")
    assert 'api_key = "fixture-key"' in result
    assert 'pass = "fixture-secret # still inside the string"' in result
    assert '[future_section]\nfuture_setting = ["first", "second"]' in result
    assert '[brightness]\nauto = 1\nauto_min = 65' in result
    assert '[infopanels]' in result
    subprocess.run(command, check=True)
    assert source.read_text(encoding="utf-8") == result
    assert list(Path(temporary).iterdir()) == [source]
print("config merge preserved private settings and unknown sections")
