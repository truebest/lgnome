#!/usr/bin/env python3
"""Deploy contract checks; all device commands are replaced with local recorders."""
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

REPO = Path(__file__).resolve().parents[1]
APP_ID = "com.truebest.lgnome.native"

MOCK = """#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
name = pathlib.Path(sys.argv[0]).name
stage = "verify" if name == "build-native-webos.sh" else (
    "install" if name == "ares-install" else
    "running" if "--running" in args else
    "close" if "--close" in args else "launch")
with open(os.environ["MOCK_LOG"], "a") as log:
    log.write(json.dumps([stage, args]) + "\\n")
if os.environ.get("MOCK_FAIL") == stage:
    sys.exit(1)
if stage == "running":
    print(os.environ.get("MOCK_RUNNING", ""))
"""


def main():
    with tempfile.TemporaryDirectory(prefix="lgnome-deploy-test-") as directory:
        root = Path(directory)
        scripts = root / "tools"
        scripts.mkdir()
        deploy = scripts / "deploy-native-webos.sh"
        shutil.copy2(REPO / "tools/deploy-native-webos.sh", deploy)
        for name in ("ares-launch", "ares-install", "build-native-webos.sh"):
            path = scripts / name
            path.write_text(MOCK)
            path.chmod(0o755)
        log = root / "commands.jsonl"
        ipk = root / "explicit package.ipk"
        ipk.touch()
        config = root / "config.json"
        # An implicit config must never be read.
        (root / "native").mkdir()
        (root / "native/config.local.json").write_text("broken")
        env = dict(os.environ, PATH=f"{scripts}:{os.environ['PATH']}",
                   ARES_DEVICE="test-tv", MOCK_LOG=str(log),
                   LGNOME_NATIVE_CONFIG=str(root / "native/config.local.json"),
                   LGNOME_LAUNCH_WITH_DEFAULTS="1", NATIVE_WEBOS_IPK=str(ipk))
        env.pop("MOCK_FAIL", None)
        env.pop("MOCK_RUNNING", None)

        def run(*args, ok=True, **overrides):
            log.write_text("")
            result = subprocess.run(["bash", str(deploy), *map(str, args)],
                                    env=env | overrides, capture_output=True, text=True)
            assert (result.returncode == 0) == ok, result.stderr
            assert "secret" not in result.stdout + result.stderr
            return [json.loads(line) for line in log.read_text().splitlines()]

        normal = run("--ipk", ipk)
        assert normal == [
            ["verify", ["--verify-ipk", str(ipk)]],
            ["install", ["-d", "test-tv", str(ipk)]],
            ["launch", ["-d", "test-tv", APP_ID]],
        ]
        assert [c[0] for c in run("--ipk", ipk, "--no-launch")] == ["verify", "install"]
        assert run("--no-install") == normal[-1:]
        for color in ("red", "green", "yellow", "blue"):
            commands = run("--no-install", "--connect-slot", color,
                           MOCK_RUNNING=f"{APP_ID} - display 0\nother.app")
            assert [c[0] for c in commands] == ["running", "close", "launch"]
            assert commands[1][1] == ["-d", "test-tv", "--close", APP_ID]
            assert json.loads(commands[-1][1][-1]) == {"connectSlot": color}
        commands = run("--no-install", "--connect-slot", "green",
                       MOCK_RUNNING=f"{APP_ID}.other")
        assert [c[0] for c in commands] == ["running", "launch"]
        for option, expected in (
            ("--with-defaults", {"ignoreSavedConfig": True}),
            ("--camera-preview", {"cameraPreview": True, "ignoreSavedConfig": True}),
        ):
            commands = run("--no-install", option)
            assert json.loads(commands[-1][1][-1]) == expected
        document = {"password": "secret $(false)", "name": "Юрий", "sessions": []}
        config.write_text(json.dumps(document))
        commands = run("--no-install", "--config", config)
        params = commands[-1][1][-1]
        assert "'" not in params and json.loads(params) == document
        for apostrophe in ("O'Reilly", "secret 'x'"):
            config.write_text(json.dumps({"username": apostrophe}))
            assert run("--ipk", ipk, "--config", config, ok=False) == []

        config.write_text(r'{"username":"\u0027"}')
        assert run("--ipk", ipk, "--config", config, ok=False) == []

        boundary = json.dumps({"extra": "x" * (122881 - len('{"extra":""}'))}, separators=(",", ":"))
        assert len(boundary.encode()) == 122881
        config.write_text(boundary)
        assert run("--ipk", ipk, "--config", config, ok=False) == []

        config.write_text(json.dumps({"extra": [1e-6] * 19000}, separators=(",", ":")))
        assert run("--ipk", ipk, "--config", config, ok=False) == []
        config.write_text('{"extra":1e-6}')
        assert json.loads(run("--no-install", "--config", config)[-1][1][-1]) == {"extra": 1e-6}

        for arguments in (
            (), ("--no-install", "--no-launch"),
            ("--ipk", ipk, "--no-install"),
            ("--ipk", ipk, "--no-launch", "--with-defaults"),
            ("--no-install", "--with-defaults", "--camera-preview"),
            ("--no-install", "--connect-slot", "orange"),
            ("--no-install", "--config"),
        ):
            assert run(*arguments, ok=False) == []
        for raw in ('[]', '{"password":"secret",}', '{"ignoreSavedConfig":false}',
                    '{"params":{}}', '{"cameraPreview":true}', '{"connectSlot":"red"}',
                    '{"launchParams":"{}"}', '{"fps":NaN}', " " * 131073,
                    '{"sessions":[{"ignoreSavedConfig":true}]}',
                    '{"extra":{"cameraPreview":true}}', '{"password":"' + "é" * 70000 + '"}'):
            config.write_text(raw)
            assert run("--ipk", ipk, "--config", config, ok=False) == []
        for failure, expected in (
            ("verify", ["verify"]), ("install", ["verify", "install"]),
            ("running", ["verify", "install", "running"]),
            ("close", ["verify", "install", "running", "close"]),
            ("launch", ["verify", "install", "running", "close", "launch"]),
        ):
            commands = run("--ipk", ipk, "--connect-slot", "green", ok=False,
                           MOCK_FAIL=failure, MOCK_RUNNING=APP_ID)
            assert [c[0] for c in commands] == expected

        # Exercise the real verifier's rejection paths without a toolchain or device.
        for issue in ("identity", "saved settings", "configuration files", "settings directory",
                      "settings directory must have mode"):
            payload = root / "data.tar.gz"
            package_root = f"usr/palm/applications/{APP_ID}/"
            files = {"appinfo.json": json.dumps({"id": "wrong.id"}).encode(),
                     "bin/lgnome-native": b"", "icon.png": b""}
            for name in ("IBMPlex-OFL-1.1.txt", "JetBrainsMono-OFL-1.1.txt"):
                source = REPO / "third_party" / name.replace("-1.1", "")
                files[f"licenses/{name}"] = source.read_bytes()
            files["licenses/THIRD_PARTY_PROVENANCE.md"] = b"test"
            if issue == "saved settings":
                files["settings/123/settings.json"] = b"{}"
            if issue == "configuration files":
                files["config.local.json"] = b"{}"
            with tarfile.open(payload, "w:gz") as archive:
                if issue == "settings directory must have mode":
                    item = tarfile.TarInfo(package_root + "settings")
                    item.type = tarfile.DIRTYPE
                    item.mode = 0o777
                    archive.addfile(item)
                if issue == "settings directory":
                    item = tarfile.TarInfo(package_root + "settings")
                    item.type = tarfile.SYMTYPE
                    item.linkname = "missing-directory"
                    archive.addfile(item)
                for name, data in files.items():
                    item = tarfile.TarInfo(package_root + name)
                    item.size = len(data)
                    archive.addfile(item, io.BytesIO(data))
            ipk.unlink()
            subprocess.run(["ar", "qc", str(ipk), str(payload)], check=True)
            result = subprocess.run(
                ["bash", str(REPO / "tools/build-native-webos.sh"), "--verify-ipk", ipk],
                capture_output=True, text=True)
            assert result.returncode != 0
            assert (issue if issue != "identity" else "expected 'com.truebest.lgnome.native'") in result.stderr
        subprocess.run(["python3", str(REPO / "tools/fix-ipk-settings-mode.py"), str(ipk)], check=True)
        repaired = root / "repaired"
        repaired.mkdir()
        subprocess.run(["ar", "x", str(ipk)], cwd=repaired, check=True)
        with tarfile.open(repaired / "data.tar.gz") as archive:
            assert archive.getmember(package_root + "settings").mode == 0o1777
            assert archive.extractfile(package_root + "icon.png").read() == b""

        def raw_member(name, data):
            header = (f"{name:<16}{1791734055:<12}{0:<6}{0:<6}"
                      f"{'100644':<8}{len(data):<10}`\n").encode("ascii")
            assert len(header) == 60
            return header, header + data + (b"\n" if len(data) % 2 else b"")

        # ares-package uses exact names; GNU ar's trailing slashes break TV installation.
        _, debian = raw_member("debian-binary", b"2.0\n")
        _, control = raw_member("control.tar.gz", b"control-payload")
        original_payload = payload.read_bytes()
        data_header, data_member = raw_member("data.tar.gz", original_payload)
        _, suffix = raw_member("extra-metadata", b"metadata-payload!")
        prefix = b"!<arch>\n" + debian + control
        ipk.write_bytes(prefix + data_member + suffix)
        subprocess.run(["python3", str(REPO / "tools/fix-ipk-settings-mode.py"), str(ipk)], check=True)
        updated = ipk.read_bytes()
        assert updated.startswith(prefix)
        assert updated.endswith(suffix)
        offset = len(prefix)
        assert updated[offset:offset + 48] == data_header[:48]
        assert updated[offset + 58:offset + 60] == data_header[58:]
        size = int(updated[offset + 48:offset + 58])
        assert len(updated) == len(prefix) + 60 + size + size % 2 + len(suffix)
        with tarfile.open(fileobj=io.BytesIO(updated[offset + 60:offset + 60 + size])) as archive:
            assert archive.getmember(package_root + "settings").mode == 0o1777
            assert archive.extractfile(package_root + "icon.png").read() == b""
    print("deploy-webos: all checks passed")


if __name__ == "__main__":
    main()
