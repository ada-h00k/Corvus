#!/usr/bin/env python3
"""Apply Corvus's Cargo source fixes to the pinned flatpak-cargo-generator."""

from pathlib import Path
import sys


def replace_once(source: str, old: str, new: str, description: str) -> str:
    count = source.count(old)
    if count != 1:
        raise SystemExit(
            f"Cannot patch flatpak-cargo-generator: expected one {description}, found {count}"
        )
    return source.replace(old, new, 1)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"Usage: {sys.argv[0]} PATH_TO_FLATPAK_CARGO_GENERATOR")

    path = Path(sys.argv[1])
    source = path.read_text(encoding="utf-8")

    old_git_config = '''    cargo_vendored_entry: _VendorEntryType = {
        repo_url: {
            "git": repo_url,
            "replace-with": VENDORED_SOURCES,
        }
    }
    rev = parse_qs(urlparse(source).query).get("rev")
    tag = parse_qs(urlparse(source).query).get("tag")
    branch = parse_qs(urlparse(source).query).get("branch")
    if rev:
        assert len(rev) == 1
        cargo_vendored_entry[repo_url]["rev"] = rev[0]
    elif tag:
        assert len(tag) == 1
        cargo_vendored_entry[repo_url]["tag"] = tag[0]
    elif branch:
        assert len(branch) == 1
        cargo_vendored_entry[repo_url]["branch"] = branch[0]
'''
    new_git_config = '''    query = parse_qs(urlparse(source).query)
    rev = query.get("rev")
    tag = query.get("tag")
    branch = query.get("branch")
    ref_key = None
    ref_value = None
    if rev:
        assert len(rev) == 1
        ref_key, ref_value = "rev", rev[0]
    elif tag:
        assert len(tag) == 1
        ref_key, ref_value = "tag", tag[0]
    elif branch:
        assert len(branch) == 1
        ref_key, ref_value = "branch", branch[0]

    # Cargo identifies Git sources by URL and Git reference. Keep each reference
    # in its own source mapping and vendor directory; a URL-only key causes tags
    # from the same repository to overwrite one another.
    source_identity = "\\0".join((repo_url, ref_key or "", ref_value or ""))
    source_hash = hashlib.sha256(source_identity.encode("utf-8")).hexdigest()[:16]
    git_source_name = f"git-{source_hash}"
    vendor_source_name = f"vendored-git-{source_hash}"
    vendor_dir = f"{CARGO_HOME}/git-vendor-{source_hash}"
    git_source: Dict[str, str] = {
        "git": repo_url,
        "replace-with": vendor_source_name,
    }
    if ref_key is not None and ref_value is not None:
        git_source[ref_key] = ref_value
    cargo_vendored_entry: _VendorEntryType = {
        git_source_name: git_source,
        vendor_source_name: {"directory": vendor_dir},
    }
'''
    source = replace_once(source, old_git_config, new_git_config, "Git source configuration block")

    old_git_sources = '''    git_sources: List[_FlatpakSourceType] = [
        {
            "type": "shell",
            "commands": [
                f'cp -r --reflink=auto "{pkg_repo_dir}" "{CARGO_CRATES}/{name}"'
            ],
        },
        {
            "type": "inline",
            "contents": tomlkit.dumps(git_pkg.normalized),
            "dest": f"{CARGO_CRATES}/{name}",  # -{version}',
            "dest-filename": "Cargo.toml",
        },
        {
            "type": "inline",
            "contents": json.dumps({"package": None, "files": {}}),
            "dest": f"{CARGO_CRATES}/{name}",  # -{version}',
            "dest-filename": ".cargo-checksum.json",
        },
    ]
'''
    new_git_sources = '''    vendor_pkg_dir = os.path.join(vendor_dir, f"{name}-{package['version']}")
    tarball = os.path.join(
        CARGO_HOME, f".git-vendor-{source_hash}-{name}-{package['version']}.tar"
    )
    git_sources: List[_FlatpakSourceType] = [
        {
            "type": "shell",
            "commands": [
                f'mkdir -p "{vendor_pkg_dir}" && '
                f'tar --exclude-vcs -C "{pkg_repo_dir}" -cf "{tarball}" . && '
                f'tar -C "{vendor_pkg_dir}" -xf "{tarball}" && '
                f'rm -f "{tarball}" && '
                f'chmod -R u+rwX "{vendor_pkg_dir}"'
            ],
        },
        {
            "type": "inline",
            "contents": tomlkit.dumps(git_pkg.normalized),
            "dest": vendor_pkg_dir,
            "dest-filename": "Cargo.toml",
        },
        {
            "type": "inline",
            "contents": json.dumps({"package": None, "files": {}}),
            "dest": vendor_pkg_dir,
            "dest-filename": ".cargo-checksum.json",
        },
    ]
'''
    source = replace_once(source, old_git_sources, new_git_sources, "Git package source block")

    path.write_text(source, encoding="utf-8")
    print("Applied Corvus Git-source vendoring fixes to flatpak-cargo-generator.")


if __name__ == "__main__":
    main()
