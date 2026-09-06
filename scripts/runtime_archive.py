"""Extract only the bounded, regular-file layout produced by the runtime packager."""
from pathlib import PurePosixPath
import tarfile


def extract(archive, destination):
    if destination.exists():
        raise FileExistsError("Archive destination must be new")
    members, names, roots, total = [], set(), set(), 0
    with tarfile.open(archive, "r:gz") as stream:
        for member in stream:
            name = member.name.rstrip("/")
            path = PurePosixPath(name)
            if (not name or path.is_absolute() or ".." in path.parts or str(path) != name or
                    "\\" in name or ":" in name or name in names or member.mode & 0o7000 or member.size < 0 or
                    not (member.isdir() or member.isfile())):
                raise ValueError("Archive contains an unsafe or duplicate entry")
            names.add(name); roots.add(path.parts[0]); total += member.size
            if len(names) > 4096 or len(roots) != 1 or total > 256 * 1024 * 1024:
                raise ValueError("Archive layout or size exceeds the runtime profile")
            members.append(member)
        if not members or not all(next(iter(roots)) + "/bin/" + name in names
                                  for name in ("asngn", "asngn-mcp", "asngn-acp", "astools-jail", "astools-check")):
            raise ValueError("Archive omits required runtime files")
        # No links or special files are admitted, and this directory is newly owned.
        destination.mkdir()
        for member in members:
            path = destination / member.name
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with stream.extractfile(member) as source, path.open("xb") as output:
                    while data := source.read(65536):
                        output.write(data)
                path.chmod(member.mode & 0o777)
    return destination / next(iter(roots))
