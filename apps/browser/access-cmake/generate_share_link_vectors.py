"""Build desktop parser and QR test inputs from the iOS share-link fixtures."""

import argparse
import json
from pathlib import Path


def cpp_string(value):
    encoded = "".join(
        chr(byte) if 32 <= byte < 127 and byte not in (34, 92) else f"\\{byte:03o}"
        for byte in value.encode("utf-8")
    )
    return f'"{encoded}"'


def write_vectors(fixtures, output):
    accepted = []
    rejected = []
    for path in sorted(fixtures.glob("*.json")):
        fixture = json.loads(path.read_text(encoding="utf-8"))
        share = fixture["share"]
        profile = fixture["profileKey"]
        accepted.append((profile, share))
        share_without_fragment = share.split("#", 1)[0]
        rejected.extend(
            (profile, share_without_fragment + suffix)
            for suffix in fixture["rejectedQuerySuffixes"]
        )
    if not accepted or not rejected:
        raise ValueError("Expected both accepted and rejected share-link vectors")

    lines = [
        "// Generated from fixtures/access/share-links. Do not edit.",
        "namespace aegis_access::test {",
        "struct SharedShareLink { std::string_view profile; std::string_view share; };",
    ]
    for name, vectors in (
        ("kSharedShareLinks", accepted),
        ("kRejectedSharedShareLinks", rejected),
    ):
        lines.append(f"inline constexpr SharedShareLink {name}[] = {{")
        lines.extend(
            f"    {{{cpp_string(profile)}, {cpp_string(share)}}},"
            for profile, share in vectors
        )
        lines.append("};")
    lines.extend(["}  // namespace aegis_access::test", ""])
    output.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixtures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    write_vectors(args.fixtures, args.output)
