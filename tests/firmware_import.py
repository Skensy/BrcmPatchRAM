"""Run the macOS firmware updater against a disposable copy of the repository inputs."""
import hashlib
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLISTS = sorted((ROOT / "BrcmPatchRAM").glob("BrcmPatchRAM*-Info.plist"))
KEY = "BCM20702A1_001.002.014.1502.1780_v5876"


def snapshot(repo):
    paths = [repo / "BrcmPatchRAM" / p.name for p in PLISTS]
    paths.extend(p for p in (repo / "firmwares").rglob("*") if p.is_file())
    return {p.relative_to(repo): hashlib.sha256(p.read_bytes()).digest() for p in paths}


with tempfile.TemporaryDirectory() as temp:
    repo = Path(temp) / "source with spaces"
    (repo / "BrcmPatchRAM").mkdir(parents=True)
    (repo / "firmwares").mkdir()
    for name in ("firmware.rb", "firmware_update.tool"):
        shutil.copy2(ROOT / name, repo / name)
    for p in PLISTS:
        shutil.copy2(p, repo / "BrcmPatchRAM")
    source = Path(temp) / "input"
    source.mkdir()
    name = KEY.split("_v")[0] + ".hex"
    (source / name).write_text(":0100000001FE\n:00000001FF\n")
    inf = ["[Broadcom.NTamd64.10.0]", r"%BRCM.DeviceDesc%=RAMUSB6417, USB\VID_0A5C&PID_6417 ; Zebra 4352",
           "[DestinationDirs]", "[RAMUSB6417.CopyList]", name, 'BRCM.DeviceDesc="Example"']
    command = [repo / "firmware_update.tool", source]
    for newline in ("\r\n", "\n"):
        (source / "bcbtums.inf").write_bytes(newline.join(inf).encode())
        subprocess.run(command, check=True)
        assert (repo / "firmwares" / (KEY + ".zhx")).is_file()
        for p in PLISTS:
            parsed = plistlib.loads((repo / "BrcmPatchRAM" / p.name).read_bytes())
            assert parsed["IOKitPersonalities"]["0a5c_6417"]["FirmwareKey"] == KEY

    before = snapshot(repo)
    # Simulate interruption after the firmware directory and one plist were replaced.
    pending = repo / ".firmware-update-pending"
    (pending / "old/BrcmPatchRAM").mkdir(parents=True)
    for target in (Path("firmwares"), Path("BrcmPatchRAM") / PLISTS[0].name):
        (repo / target).rename(pending / "old" / target)
    (repo / "firmwares").mkdir()
    (repo / "BrcmPatchRAM" / PLISTS[0].name).write_bytes(b"incomplete")
    (source / name).unlink()
    assert subprocess.run(command).returncode != 0
    assert snapshot(repo) == before
    (source / "bcbtums.inf").write_text("[DestinationDirs]\n")
    assert subprocess.run(command).returncode != 0
    assert snapshot(repo) == before
