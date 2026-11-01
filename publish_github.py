#!/usr/bin/env python3
"""Publie ScanFolder sur GitHub : commit, push et release avec l'exe.

Le token vient de Git Credential Manager (celui qu'utilise deja git push).
Sans option, rien n'est commite ni pousse : on affiche l'audit et les changements.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOSITORY = "ScanFolder"
DESCRIPTION = "Analyse d'espace disque native pour Windows : un seul exe, hors ligne (C++, Direct2D)"
DIST_EXE = HERE / "dist" / "ScanFolder.exe"
SEVEN_ZIP_LICENSE = HERE / "third_party" / "7zip" / "extra" / "License.txt"
VERSION_TAG = re.compile(r"^v\d+\.\d+\.\d+$")
TEXT_SUFFIXES = {
    ".py", ".ps1", ".cmd", ".md", ".txt", ".json", ".cpp", ".hpp", ".rc", ".manifest",
    ".svg", ".gitignore", "",
}


def run(*args: object, capture: bool = False, check: bool = True) -> str:
    result = subprocess.run(
        [str(arg) for arg in args], cwd=HERE, check=check, text=True, capture_output=capture
    )
    return result.stdout.strip() if capture else ""


def github_token() -> str:
    result = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        text=True,
        capture_output=True,
        check=True,
    )
    values = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    token = values.get("password")
    if not token:
        raise RuntimeError("Aucun identifiant GitHub dans Git Credential Manager")
    return token


def github_api(
    method: str,
    url: str,
    token: str,
    payload: dict[str, object] | None = None,
    *,
    data: bytes | None = None,
    content_type: str = "application/json",
    allow_not_found: bool = False,
) -> dict[str, object] | None:
    if not url.startswith("https://"):
        url = f"https://api.github.com{url}"
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "ScanFolder-publisher",
            "Content-Type": content_type,
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=600) as response:
            body = response.read()
    except urllib.error.HTTPError as exc:
        if allow_not_found and exc.code == 404:
            return None
        detail = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"GitHub API {method} {url}: HTTP {exc.code}: {detail}") from exc
    return json.loads(body) if body else None


def machine_markers() -> list[str]:
    """Ce qui trahirait la machine locale : dossier utilisateur, login, nom du PC."""
    markers = {os.environ.get("USERPROFILE", ""), os.environ.get("USERNAME", "")}
    markers.add(os.environ.get("COMPUTERNAME", ""))
    markers.add(str(HERE))
    return sorted(m for m in markers if len(m) >= 3)


def audit() -> None:
    """Refuse de publier un fichier suivi qui contient un chemin ou nom local."""
    files = run("git", "ls-files", "--cached", "--others", "--exclude-standard", capture=True)
    markers = [m.lower() for m in machine_markers()]
    problems = []
    for name in files.splitlines():
        path = HERE / name
        if path.suffix.lower() not in TEXT_SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace").lower()
        problems += [f"{name}: contient '{m}'" for m in markers if m in text]
    if problems:
        raise RuntimeError("Audit echoue :\n  " + "\n  ".join(problems))
    print("Audit OK : aucun chemin ni nom de machine dans les fichiers publies.")


def ensure_repository(token: str, private: bool) -> tuple[str, str]:
    """Cree le depot s'il manque, renvoie (owner, email noreply)."""
    account = github_api("GET", "/user", token) or {}
    owner = str(account["login"])
    if github_api("GET", f"/repos/{owner}/{REPOSITORY}", token, allow_not_found=True) is None:
        github_api(
            "POST",
            "/user/repos",
            token,
            {"name": REPOSITORY, "description": DESCRIPTION, "private": private},
        )
        print(f"Depot cree : {owner}/{REPOSITORY} ({'prive' if private else 'public'})")
    elif private:
        print("--private ignore : le depot existe deja et garde sa visibilite.")
    return owner, f"{account['id']}+{owner}@users.noreply.github.com"


def configure_git(owner: str, email: str) -> None:
    remote = f"https://github.com/{owner}/{REPOSITORY}.git"
    if run("git", "remote", capture=True, check=False):
        run("git", "remote", "set-url", "origin", remote)
    else:
        run("git", "remote", "add", "origin", remote)
    run("git", "config", "user.name", owner)
    run("git", "config", "user.email", email)


def commit(message: str) -> None:
    run("git", "add", "-A")
    if run("git", "status", "--porcelain", capture=True):
        run("git", "commit", "-m", message)
    else:
        print("Aucun changement a commiter.")


def build_executable() -> None:
    DIST_EXE.parent.mkdir(exist_ok=True)
    # Chemin absolu : cmd ne cherche pas toujours dans le dossier courant
    # (NoDefaultCurrentDirectoryInExePath).
    subprocess.run(["cmd", "/c", str(HERE / "build.cmd"), str(DIST_EXE)], cwd=HERE, check=True)


def package_release(tag: str) -> Path:
    if not DIST_EXE.is_file():
        raise RuntimeError(f"Exe introuvable : {DIST_EXE}. Lance sans --skip-build.")
    archive = HERE / "dist" / f"ScanFolder-{tag}-win64.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        bundle.write(DIST_EXE, "ScanFolder.exe")
        bundle.write(HERE / "LICENSE", "LICENSE.txt")
        # La licence de 7-Zip doit accompagner tout binaire qui l'embarque.
        bundle.write(SEVEN_ZIP_LICENSE, "7zip-LICENSE.txt")
    print(f"Archive : {archive.name} ({archive.stat().st_size / 1e6:.1f} Mo)")
    return archive


def publish_release(token: str, owner: str, tag: str, archive: Path) -> None:
    if run("git", "tag", "--list", tag, capture=True):
        raise RuntimeError(f"Le tag {tag} existe deja")
    run("git", "tag", "-a", tag, "-m", f"ScanFolder {tag}")
    run("git", "push", "origin", tag)
    release = github_api(
        "POST",
        f"/repos/{owner}/{REPOSITORY}/releases",
        token,
        {
            "tag_name": tag,
            "name": f"ScanFolder {tag}",
            "body": "Windows 10/11 x64 : dezipper, lancer ScanFolder.exe. "
            "Un seul exe, aucune installation ni droit admin.",
        },
    ) or {}
    upload = str(release["upload_url"]).split("{", 1)[0]
    github_api(
        "POST",
        f"{upload}?name={archive.name}",
        token,
        data=archive.read_bytes(),
        content_type="application/zip",
    )
    print(f"Release publiee : {release['html_url']}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Publie ScanFolder sur GitHub",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""exemples :
  Apercu (audit + changements, rien n'est envoye) :
    python publish_github.py

  Pousser les modifications :
    python publish_github.py --push -m "Correction du tri par date"

  Pousser et publier une release avec l'exe (build MSVC inclus) :
    python publish_github.py --push -m "ScanFolder v0.2.0" --release v0.2.0
""",
    )
    parser.add_argument("-m", "--message", help="message de commit")
    parser.add_argument("--push", action="store_true", help="commit et push (cree le depot si besoin)")
    parser.add_argument("--release", metavar="vX.Y.Z", help="tag + release GitHub avec l'exe zippe")
    parser.add_argument("--skip-build", action="store_true", help="reutilise dist/ScanFolder.exe existant")
    parser.add_argument("--private", action="store_true", help="depot prive (a la creation seulement)")
    args = parser.parse_args()

    if args.push and not args.message:
        parser.error('--push demande un message : -m "..."')
    if args.release and not args.push:
        parser.error("--release demande --push")
    if args.release and not VERSION_TAG.fullmatch(args.release):
        parser.error("le tag doit suivre vMAJEUR.MINEUR.CORRECTIF")

    audit()
    if not args.push:
        status = run("git", "status", "--short", capture=True)
        print(status or "Aucun changement local.")
        print('Pour publier : python publish_github.py --push -m "..."')
        return

    token = github_token()
    owner, email = ensure_repository(token, args.private)
    configure_git(owner, email)
    archive = None
    if args.release:
        if not args.skip_build:
            build_executable()
        archive = package_release(args.release)
    commit(args.message)
    run("git", "push", "-u", "origin", "main")
    print(f"Push OK : https://github.com/{owner}/{REPOSITORY}")
    if args.release and archive is not None:
        publish_release(token, owner, args.release, archive)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"Erreur : {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
