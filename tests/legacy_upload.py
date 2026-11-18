"""Run the legacy driver methods on the host with deterministic USB and thread doubles."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "BrcmPatchRAM/BrcmPatchRAM.cpp").read_text()


def method(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


methods = "\n".join(method(sig) for sig in (
    "IOReturn BrcmPatchRAM::onTimerEvent()", "void BrcmPatchRAM::uploadFirmwareThread(",
    "void BrcmPatchRAM::uploadFirmware()"))
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / "test.cpp"
    exe = cpp.with_suffix("")
    doubles, scenarios = Path(__file__).with_suffix(".cpp").read_text().split("// DRIVER METHODS")
    cpp.write_text(doubles + methods + scenarios)
    subprocess.run(["clang++", "-std=c++11", "-Wall", "-Werror", cpp, "-o", exe], check=True)
    subprocess.run([exe], check=True)
