# Creates the GitHub release for a tag as a draft, uploads the files, then publishes it.
# The token comes from Git Credential Manager (the one `git push` uses) and is never printed.
# Usage (repo root): python -I scripts/publish-release.py v0.1.5 "Cam Studio 0.1.5" notes.md build/dist/*
import json, os, subprocess, sys, urllib.request

REPO = "2108win/cam-studio"
tag, title, notes_path, files = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]

out = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                     capture_output=True, text=True, check=True).stdout
token = dict(l.split("=", 1) for l in out.splitlines() if "=" in l).get("password", "")
if not token:
    sys.exit("no GitHub credential")

def call(method, url, body=None, data=None, ctype="application/json"):
    req = urllib.request.Request(url, method=method, data=data if data is not None else
                                 (json.dumps(body).encode() if body is not None else None))
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "cam-studio-release")
    if body is not None or data is not None:
        req.add_header("Content-Type", ctype)
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read() or b"{}")

notes = open(notes_path, encoding="utf-8").read()
rel = call("POST", f"https://api.github.com/repos/{REPO}/releases",
           {"tag_name": tag, "name": title, "body": notes, "draft": True, "prerelease": False})
print("draft", rel["id"])
upload = rel["upload_url"].split("{")[0]
for f in files:
    name = os.path.basename(f)
    a = call("POST", f"{upload}?name={name}", data=open(f, "rb").read(), ctype="application/octet-stream")
    print("uploaded", a["name"], a["size"], a.get("digest"))
rel = call("PATCH", f"https://api.github.com/repos/{REPO}/releases/{rel['id']}", {"draft": False, "make_latest": "true"})
print("published", rel["html_url"])
