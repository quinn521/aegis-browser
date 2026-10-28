"""Copy an AAB without its old JAR signature, without extracting its entries."""
from pathlib import Path
import re
import sys
import zipfile


def copy_unsigned(source, target):
    with zipfile.ZipFile(source) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or "BundleConfig.pb" not in names:
            raise ValueError("Not an unambiguous Android App Bundle")
        if any(n.startswith("/") or ".." in n.split("/") or "\\" in n for n in names):
            raise ValueError("Unsafe bundle entry")
        with Path(target).open("xb") as output, zipfile.ZipFile(output, "w") as copied:
            for info in archive.infolist():
                if re.fullmatch(r"META-INF/(?:[^/]+\.(?:SF|RSA|DSA|EC)|MANIFEST\.MF)", info.filename, re.I):
                    continue
                # Bounded memory even for large Chromium native libraries.
                with archive.open(info) as reader, copied.open(info, "w", force_zip64=True) as writer:
                    while chunk := reader.read(1024 * 1024):
                        writer.write(chunk)


if __name__ == "__main__":
    copy_unsigned(*sys.argv[1:])
