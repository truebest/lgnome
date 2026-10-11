#!/usr/bin/env python3
"""Release preparation contract in isolated repositories; no network or publication."""
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


def run(*args, cwd, ok=True, env=None):
    result = subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True)
    assert (result.returncode == 0) == ok, result.stdout + result.stderr
    return result.stdout.strip()


def main():
    with tempfile.TemporaryDirectory(prefix="lgnome-release-test-") as directory:
        root = Path(directory)
        repo = root / "repo"
        repo.mkdir()
        run("git", "init", "-b", "main", cwd=repo)
        run("git", "config", "user.email", "test@example.invalid", cwd=repo)
        run("git", "config", "user.name", "Release Test", cwd=repo)
        tools = repo / "tools"
        tools.mkdir()
        shutil.copy2(REPO / "tools/release-github.sh", tools)
        verifier = tools / "build-native-webos.sh"
        verifier.write_text('#!/bin/sh\n[ "$1" = --verify-ipk ] && [ -f "$2" ]\n')
        verifier.chmod(0o755)
        appinfo = repo / "native/deploy/webos/appinfo.json"
        appinfo.parent.mkdir(parents=True)
        appinfo.write_text(json.dumps({"id": APP_ID, "version": "1.2.3"}))
        backend = repo / "third_party/backend_ndl"
        backend.mkdir(parents=True)
        run("git", "init", "-b", "main", cwd=backend)
        run("git", "config", "user.email", "test@example.invalid", cwd=backend)
        run("git", "config", "user.name", "Release Test", cwd=backend)
        (backend / "api.h").write_text("test backend\n")
        run("git", "add", ".", cwd=backend)
        run("git", "commit", "-m", "backend", cwd=backend)
        (repo / ".gitmodules").write_text('[submodule "third_party/backend_ndl"]\n'
                                          '\tpath = third_party/backend_ndl\n'
                                          '\turl = git@bitbucket.org:kodavr/backend_ndl.git\n')
        run("git", "add", ".", cwd=repo)
        run("git", "commit", "-m", "previous release", cwd=repo)
        remote = root / "mirror.git"
        run("git", "clone", "--bare", str(repo), str(remote), cwd=root)
        # Reject every push: preparation must neither push nor create a premature tag.
        hook = remote / "hooks/pre-receive"
        hook.write_text("#!/bin/sh\nexit 1\n")
        hook.chmod(0o755)
        run("git", "remote", "add", "github", str(remote), cwd=repo)
        previous = run("git", "rev-parse", "main", cwd=remote)
        (repo / "new.txt").write_text("cumulative change\n")
        run("git", "add", ".", cwd=repo)
        run("git", "commit", "-m", "development change", cwd=repo)
        env = dict(os.environ, BACKEND_NDL_MIRROR_URL=str(backend))
        payload = root / "data.tar.gz"
        ipk = root / "verified.ipk"

        def package(version):
            metadata = json.dumps({"id": APP_ID, "version": version}).encode()
            with tarfile.open(payload, "w:gz") as archive:
                item = tarfile.TarInfo(f"usr/palm/applications/{APP_ID}/appinfo.json")
                item.size = len(metadata)
                archive.addfile(item, io.BytesIO(metadata))
            ipk.unlink(missing_ok=True)
            run("ar", "qc", str(ipk), str(payload), cwd=root)

        def prepare(version, ok=True):
            return run("bash", str(tools / "release-github.sh"), version, str(ipk),
                       cwd=repo, env=env, ok=ok)

        package("1.2.3")
        prepare("1.2.4", ok=False)
        assert not run("git", "branch", "--list", "release/*", cwd=repo)
        package("9.9.9")
        prepare("1.2.3", ok=False)
        assert not run("git", "branch", "--list", "release/*", cwd=repo)
        package("1.2.3")
        prepare("1.2.3")
        candidate = run("git", "rev-parse", "release/v1.2.3", cwd=repo)
        assert run("git", "rev-parse", f"{candidate}^", cwd=repo) == previous
        assert run("git", "rev-list", "--count", f"{previous}..{candidate}", cwd=repo) == "1"
        assert run("git", "show", f"{candidate}:new.txt", cwd=repo) == "cumulative change"
        assert str(backend) in run("git", "show", f"{candidate}:.gitmodules", cwd=repo)
        assert not run("git", "tag", "--list", cwd=repo)
        assert not run("git", "tag", "--list", cwd=remote)
        assert run("git", "rev-parse", "main", cwd=remote) == previous
        assert not run("git", "branch", "--list", "release/*", cwd=remote)
        prepare("1.2.3", ok=False)
    print("release-github: all checks passed")


if __name__ == "__main__":
    main()
