"""Gathers the shared libraries Mach-O binaries depend on into one folder and points the binaries at it."""
import pathlib
import shutil
import subprocess

SYSTEM_PREFIXES = ("/usr/lib/", "/System/")


def run(*args, check=True):
    return subprocess.run([str(a) for a in args], check=check, capture_output=True, text=True)


def dependencies(binary):
    out = run("otool", "-L", binary).stdout.splitlines()[1:]
    return [line.split()[0] for line in out if line.strip()]


def rpaths(binary):
    lines = run("otool", "-l", binary).stdout.splitlines()
    found = []
    for index, line in enumerate(lines):
        if "LC_RPATH" in line and index + 2 < len(lines):
            found.append(lines[index + 2].split()[1])
    return found


class DylibBundler:
    """Copies every non-system dylib reachable from the executables into frameworks_dir and
    makes the executables load them through @executable_path-relative rpaths."""

    def __init__(self, frameworks_dir, executable_rpath, search_dirs=()):
        self.frameworks_dir = pathlib.Path(frameworks_dir)
        self.executable_rpath = executable_rpath
        self.search_dirs = [pathlib.Path(d) for d in search_dirs]
        self.copied = {}

    def resolve(self, reference, referrer):
        if reference.startswith("@rpath/") or reference.startswith("@loader_path/"):
            name = reference.split("/", 1)[1]
            candidates = [referrer.parent / name]
            candidates += [pathlib.Path(r) / name for r in rpaths(referrer) if not r.startswith("@")]
            candidates += [d / name for d in self.search_dirs]
        elif reference.startswith("@executable_path/"):
            return None
        else:
            candidates = [pathlib.Path(reference)]
        for candidate in candidates:
            if candidate.is_file():
                return candidate.resolve()
        raise FileNotFoundError(f"{referrer}: cannot locate {reference}")

    def bundle_executable(self, executable):
        executable = pathlib.Path(executable)
        self._rewrite(executable, is_executable=True)

    def _rewrite(self, binary, is_executable):
        for reference in dependencies(binary):
            if reference.startswith(SYSTEM_PREFIXES):
                continue
            source = self.resolve(reference, binary)
            if source is None:
                continue
            name = source.name
            if name not in self.copied:
                destination = self.frameworks_dir / name
                self.frameworks_dir.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
                destination.chmod(0o755)
                run("install_name_tool", "-id", f"@rpath/{name}", destination)
                self.copied[name] = destination
                self._rewrite(destination, is_executable=False)
            if reference != f"@rpath/{name}":
                run("install_name_tool", "-change", reference, f"@rpath/{name}", binary)
        for rpath in rpaths(binary):
            run("install_name_tool", "-delete_rpath", rpath, binary, check=False)
        if is_executable:
            run("install_name_tool", "-add_rpath", self.executable_rpath, binary)
        else:
            run("install_name_tool", "-add_rpath", "@loader_path", binary)

    def verify(self, binaries):
        problems = []
        for binary in binaries:
            for reference in dependencies(binary):
                if reference.startswith(SYSTEM_PREFIXES):
                    continue
                if reference.startswith("@rpath/") and (self.frameworks_dir / reference[7:]).is_file():
                    continue
                problems.append(f"{binary}: {reference}")
        return problems
