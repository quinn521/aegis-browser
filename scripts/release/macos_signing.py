"""Read the native source's signing policy and verify the resulting signature."""
from pathlib import Path
import re
import subprocess


def command(*args):
    return subprocess.check_output(args, stderr=subprocess.PIPE, timeout=120)


def load_policy(browser=None, mode="native"):
    if mode == "temporary-ad-hoc":
        return {"signing": "ad-hoc-not-notarized", "temporary": True}
    if mode != "native":
        raise ValueError("Unknown macOS signing mode")
    browser = Path(browser) if browser else Path(__file__).resolve().parents[2] / "apps/browser"
    text = (browser / "scripts/sign-chromium-app.sh").read_text()
    text = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    identity = re.search(r'^SIGNING_IDENTITY="([A-Fa-f0-9]{40})"$', text, re.M)
    if "SIGNING_IDENTITY=" in text:
        team = re.search(r'certificate leaf\[subject\.OU\] = \\"([A-Z0-9]{10})\\"', text)
        if not identity or not team or "anchor apple generic" not in text:
            raise ValueError("Native certificate signing requires its exact SHA-1, Apple anchor and team")
        certificate, identifier = identity[1].upper(), team[1]
        return {"signing": "apple-certificate-not-notarized", "certificateSha1": certificate,
                "teamIdentifier": identifier,
                "requirement": '=anchor apple generic and certificate leaf = H"' + certificate +
                               '" and certificate leaf[subject.OU] = "' + identifier + '"'}
    if re.search(r"--sign[ \t]+-(?:\s|$)", text):
        return {"signing": "ad-hoc-not-notarized"}
    raise ValueError("Unsupported native signing policy; refusing to guess an identity")


def verify_signature(app, policy, execute=None):
    execute = execute or command
    execute("/usr/bin/codesign", "--verify", "--deep", "--strict", str(app))
    if policy["signing"] == "apple-certificate-not-notarized":
        execute("/usr/bin/codesign", "--verify", "--deep", "--strict", "--test-requirement",
                policy["requirement"], str(app))
    signature = subprocess.run(["/usr/bin/codesign", "-d", "--verbose=4", str(app)],
                               capture_output=True, check=True, timeout=120).stderr.decode().splitlines()
    adhoc = "Signature=adhoc" in signature
    if policy["signing"] == "ad-hoc-not-notarized":
        if not adhoc:
            raise ValueError("Native source requires an ad-hoc signature")
    elif adhoc or "TeamIdentifier=" + policy["teamIdentifier"] not in signature:
        raise ValueError("Application signature differs from the native certificate/team policy")
    return {key: value for key, value in policy.items() if key != "requirement"}
