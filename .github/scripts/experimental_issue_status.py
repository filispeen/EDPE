#!/usr/bin/env python3
"""Discover linked GitHub issues for a branch and maintain a live build-progress comment."""

from __future__ import annotations

import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

DEFAULT_BRANCHES = {"master", "main"}

BRANCH_ISSUE_PATTERNS = (
    re.compile(r"^#?(\d+)(?:[-_/]|$)"),
    re.compile(r"^(?:issue|fix|bug|feat|hotfix)[-_ /]#?(\d+)(?:[-_/]|$)", re.IGNORECASE),
)

GRAPHQL_QUERY_TEMPLATE = """
query($owner: String!, $repo: String!, $qualifiedRef: String!, $states: [IssueState!], $cursor: String) {
  repository(owner: $owner, name: $repo) {
    ref(qualifiedName: $qualifiedRef) {
      associatedPullRequests(first: 10) {
        nodes {
          number
          closingIssuesReferences(first: 10) {
            nodes {
              number
            }
          }
        }
      }
    }
    issues(first: 100, states: $states, orderBy: {field: UPDATED_AT, direction: DESC}, after: $cursor) {
      pageInfo {
        hasNextPage
        endCursor
      }
      nodes {
        number
        linkedBranches(first: 20) {
          nodes {
            ref {
              name
            }
          }
        }
      }
    }
  }
}
"""


def write_github_outputs(values: dict[str, str]) -> None:
    output_path = os.environ.get("GITHUB_OUTPUT", "").strip()
    if not output_path:
        for k, v in values.items():
            print(f"[output] {k}={v}")
        return
    with open(output_path, "a", encoding="utf-8") as f:
        for k, v in values.items():
            f.write(f"{k}={v}\n")


def github_request(
    url: str,
    token: str,
    method: str = "GET",
    payload: dict[str, Any] | None = None,
) -> tuple[int, Any]:
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "EDPE-Experimental-Issue-Reporter/1.0",
    }
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json; charset=utf-8"

    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            return resp.status, json.loads(raw) if raw else None
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"GitHub API {method} {url} failed with HTTP {exc.code}: {body}") from exc


def extract_issue_numbers_from_branch(branch_name: str) -> list[int]:
    if not branch_name or branch_name in DEFAULT_BRANCHES:
        return []
    found: list[int] = []
    for pattern in BRANCH_ISSUE_PATTERNS:
        m = pattern.match(branch_name)
        if m:
            num = int(m.group(1))
            if num > 0 and num not in found:
                found.append(num)
    return found


def resolve_candidate_branches(
    api_url: str,
    repo: str,
    token: str,
    ref_type: str,
    ref_name: str,
    sha: str,
) -> list[str]:
    if ref_type == "branch" and ref_name:
        return [] if ref_name in DEFAULT_BRANCHES else [ref_name]

    if not sha:
        return []

    url = f"{api_url}/repos/{repo}/commits/{sha}/branches-where-head"
    try:
        _, branches = github_request(url, token)
    except Exception as exc:
        print(f"Warning: could not resolve branches for commit {sha}: {exc}")
        return []

    if not isinstance(branches, list):
        return []
    return [
        b["name"]
        for b in branches
        if isinstance(b, dict) and b.get("name") and b["name"] not in DEFAULT_BRANCHES
    ]


def verify_is_issue(api_url: str, repo: str, token: str, number: int) -> bool:
    url = f"{api_url}/repos/{repo}/issues/{number}"
    try:
        _, data = github_request(url, token)
    except Exception as exc:
        print(f"Warning: could not verify issue #{number}: {exc}")
        return False
    return isinstance(data, dict) and "pull_request" not in data


def is_issue_closed(api_url: str, repo: str, token: str, number: int) -> bool:
    url = f"{api_url}/repos/{repo}/issues/{number}"
    try:
        _, data = github_request(url, token)
    except Exception as exc:
        print(f"Warning: could not check state of issue #{number}: {exc}")
        return False
    return isinstance(data, dict) and data.get("state") == "closed"


def query_linked_issues_graphql(
    graphql_url: str,
    repo: str,
    token: str,
    branch_name: str,
    states: list[str],
    max_pages: int = 2,
) -> tuple[list[int], list[int]]:
    owner, repo_name = repo.split("/", 1)
    linked_branch_issues: list[int] = []
    pr_closing_issues: list[int] = []
    cursor: str | None = None

    for page in range(max_pages):
        variables = {
            "owner": owner,
            "repo": repo_name,
            "qualifiedRef": f"refs/heads/{branch_name}",
            "states": states,
            "cursor": cursor,
        }
        _, resp = github_request(
            graphql_url,
            token,
            method="POST",
            payload={"query": GRAPHQL_QUERY_TEMPLATE, "variables": variables},
        )
        if not isinstance(resp, dict):
            break
        if resp.get("errors"):
            raise RuntimeError(f"GraphQL errors: {resp['errors']}")

        repo_data = (resp.get("data") or {}).get("repository") or {}

        if page == 0:
            ref_data = repo_data.get("ref") or {}
            prs = (ref_data.get("associatedPullRequests") or {}).get("nodes") or []
            for pr in prs:
                if not isinstance(pr, dict):
                    continue
                closing = (pr.get("closingIssuesReferences") or {}).get("nodes") or []
                for item in closing:
                    if isinstance(item, dict) and isinstance(item.get("number"), int):
                        num = item["number"]
                        if num not in pr_closing_issues:
                            pr_closing_issues.append(num)

        issues_conn = repo_data.get("issues") or {}
        for issue in issues_conn.get("nodes") or []:
            if not isinstance(issue, dict):
                continue
            issue_num = issue.get("number")
            if not isinstance(issue_num, int):
                continue
            links = (issue.get("linkedBranches") or {}).get("nodes") or []
            for link in links:
                if not isinstance(link, dict):
                    continue
                ref = link.get("ref") or {}
                if ref.get("name") == branch_name and issue_num not in linked_branch_issues:
                    linked_branch_issues.append(issue_num)

        page_info = issues_conn.get("pageInfo") or {}
        if not page_info.get("hasNextPage") or not page_info.get("endCursor"):
            break
        cursor = page_info["endCursor"]

    return linked_branch_issues, pr_closing_issues


def discover_linked_issues(
    api_url: str,
    graphql_url: str,
    repo: str,
    token: str,
    ref_type: str,
    ref_name: str,
    sha: str,
) -> tuple[str, list[int]]:
    branches = resolve_candidate_branches(api_url, repo, token, ref_type, ref_name, sha)
    if not branches:
        return "", []

    primary_branch = branches[0]
    discovered: list[int] = []

    def add_unique(nums: list[int]) -> None:
        for n in nums:
            if n not in discovered:
                discovered.append(n)

    for branch in branches:
        pr_issues: list[int] = []
        try:
            open_linked, pr_issues = query_linked_issues_graphql(
                graphql_url, repo, token, branch, ["OPEN"], max_pages=2
            )
            add_unique(open_linked)
        except Exception as exc:
            print(f"Warning: GraphQL lookup failed for branch '{branch}': {exc}")

        for num in extract_issue_numbers_from_branch(branch):
            if num not in discovered and verify_is_issue(api_url, repo, token, num):
                discovered.append(num)

        add_unique(pr_issues)

    return primary_branch, discovered


def markdown_safe_url(url: str) -> str:
    return url.replace("(", "%28").replace(")", "%29")


def step_icon(outcome: str) -> str:
    return {
        "success": "✅",
        "failure": "❌",
        "cancelled": "⚪",
        "skipped": "⏭️",
        "running": "🔄",
        "pending": "⏳",
    }.get(outcome, "⏳")


def resolve_release_links(
    server_url: str,
    repo: str,
    tag_name: str,
    archive_name: str,
    release_url: str,
    assets_json: str,
) -> tuple[str, str]:
    encoded_tag = urllib.parse.quote(tag_name, safe="")
    resolved_release_url = release_url.strip() or f"{server_url}/{repo}/releases/tag/{encoded_tag}"
    resolved_asset_url = ""

    if assets_json.strip():
        try:
            assets = json.loads(assets_json)
            if isinstance(assets, list):
                for asset in assets:
                    if isinstance(asset, dict) and asset.get("name") == archive_name:
                        resolved_asset_url = str(asset.get("browser_download_url", "")).strip()
                        break
                if not resolved_asset_url:
                    for asset in assets:
                        if isinstance(asset, dict) and str(asset.get("name", "")).endswith(".zip"):
                            resolved_asset_url = str(asset.get("browser_download_url", "")).strip()
                            break
        except json.JSONDecodeError:
            pass

    if not resolved_asset_url and archive_name:
        encoded_archive = urllib.parse.quote(archive_name, safe="")
        resolved_asset_url = f"{server_url}/{repo}/releases/download/{encoded_tag}/{encoded_archive}"

    return markdown_safe_url(resolved_release_url), markdown_safe_url(resolved_asset_url)


def render_comment_body(phase: str, ctx: dict[str, str]) -> str:
    tag_name = ctx.get("tag_name", "exp-build")
    branch_name = ctx.get("branch_name", "")
    sha = ctx.get("sha", "")
    short_sha = sha[:7] if sha else "unknown"
    server_url = ctx.get("server_url", "https://github.com").rstrip("/")
    repo = ctx.get("repo", "")
    run_id = ctx.get("run_id", "")
    run_number = ctx.get("run_number", "")
    has_artifact = ctx.get("has_artifact", "false").lower() == "true"
    archive_name = ctx.get("archive_name") or f"EDPE({tag_name}).zip"
    sha256 = ctx.get("sha256", "").strip()

    commit_url = f"{server_url}/{repo}/commit/{sha}" if sha else ""
    run_url = f"{server_url}/{repo}/actions/runs/{run_id}" if run_id else ""

    configure_outcome = ctx.get("configure_outcome", "")
    build_outcome = ctx.get("build_outcome", "")
    download_outcome = ctx.get("download_outcome", "")
    package_outcome = ctx.get("package_outcome", "")
    upload_outcome = ctx.get("upload_outcome", "")
    release_outcome = ctx.get("release_outcome", "")

    if phase == "start":
        header_icon, status_text = "🛠️", "In Progress"
        if has_artifact:
            build_line = "- ✅ **Build EDPE (Release x64)**: built by CI"
            pkg_line = f"- 🔄 **Package Archive**: `{archive_name}` (downloading staged binaries...)"
            rel_line = "- ⏳ **Publish GitHub Release**"
        else:
            build_line = "- 🔄 **Build EDPE (Release x64)**: configuring & compiling..."
            pkg_line = f"- ⏳ **Package Archive**: `{archive_name}`"
            rel_line = "- ⏳ **Publish GitHub Release**"
    elif phase == "after_build":
        header_icon, status_text = "🛠️", "Packaging"
        build_line = "- ✅ **Build EDPE (Release x64)**"
        pkg_line = f"- 🔄 **Package Archive**: creating `{archive_name}`..."
        rel_line = "- ⏳ **Publish GitHub Release**"
    elif phase == "after_package":
        header_icon, status_text = "🛠️", "Publishing"
        build_line = (
            "- ✅ **Build EDPE (Release x64)**: built by CI"
            if has_artifact
            else "- ✅ **Build EDPE (Release x64)**"
        )
        pkg_line = f"- ✅ **Package Archive**: `{archive_name}`"
        rel_line = "- 🔄 **Publish GitHub Release**: uploading build & creating release..."
    else:
        outcomes = [
            configure_outcome,
            build_outcome,
            download_outcome,
            package_outcome,
            upload_outcome,
            release_outcome,
        ]
        if release_outcome == "success" and package_outcome == "success" and "failure" not in outcomes and "cancelled" not in outcomes:
            header_icon, status_text = "✅", "Ready"
        elif "cancelled" in outcomes and "failure" not in outcomes:
            header_icon, status_text = "⚪", "Cancelled"
        else:
            header_icon, status_text = "❌", "Failed"

        if has_artifact:
            dl_state = download_outcome or "skipped"
            build_line = "- ✅ **CI Build & Tests**: completed"
            if dl_state != "success":
                pkg_line = f"- {step_icon(dl_state)} **Download Staged Binaries**: {dl_state}"
            else:
                pkg_state = package_outcome or "skipped"
                pkg_line = f"- {step_icon(pkg_state)} **Package Archive**: `{archive_name}` ({pkg_state})"
        else:
            if configure_outcome in ("failure", "cancelled"):
                build_state = configure_outcome
                build_detail = f"configure {configure_outcome}"
            else:
                build_state = build_outcome or "skipped"
                build_detail = build_state
            build_line = f"- {step_icon(build_state)} **Build EDPE (Release x64)**: {build_detail}"
            pkg_state = package_outcome or "skipped"
            pkg_line = f"- {step_icon(pkg_state)} **Package Archive**: `{archive_name}` ({pkg_state})"

        rel_state = release_outcome or "skipped"
        rel_line = f"- {step_icon(rel_state)} **Publish GitHub Release**: {rel_state}"

    lines = [
        f"### {header_icon} Experimental Build `{tag_name}`: {status_text}",
        "",
    ]
    if branch_name:
        lines.append(f"- **Branch:** `{branch_name}`")
    if commit_url:
        lines.append(f"- **Commit:** [`{short_sha}`]({commit_url})")
    if run_url:
        run_label = f"Actions Run #{run_number}" if run_number else "View Workflow Run"
        lines.append(f"- **Workflow Run:** [{run_label}]({run_url})")

    lines.extend(
        [
            "",
            "#### Build Progress",
            build_line,
            pkg_line,
            rel_line,
        ]
    )

    if phase == "finish" and status_text == "Ready":
        release_url, asset_url = resolve_release_links(
            server_url=server_url,
            repo=repo,
            tag_name=tag_name,
            archive_name=archive_name,
            release_url=ctx.get("release_url", ""),
            assets_json=ctx.get("release_assets", ""),
        )
        lines.extend(
            [
                "",
                "#### Build Download",
                f"- **GitHub Release:** [{tag_name}]({release_url})",
                f"- **Direct Archive:** [`{archive_name}`]({asset_url})",
            ]
        )
        if sha256:
            lines.append(f"- **SHA-256:** `{sha256}`")
    elif phase == "finish" and run_url:
        lines.extend(
            [
                "",
                f"> Check the [workflow run logs]({run_url}) for failure details.",
            ]
        )

    return "\n".join(lines)


def post_comments(
    api_url: str,
    repo: str,
    token: str,
    issue_numbers: list[int],
    body: str,
) -> dict[str, int]:
    comment_ids: dict[str, int] = {}
    for issue_num in issue_numbers:
        if is_issue_closed(api_url, repo, token, issue_num):
            print(f"Skipping closed issue #{issue_num}.")
            continue
        url = f"{api_url}/repos/{repo}/issues/{issue_num}/comments"
        try:
            _, resp = github_request(url, token, method="POST", payload={"body": body})
            if isinstance(resp, dict) and isinstance(resp.get("id"), int):
                comment_ids[str(issue_num)] = resp["id"]
                print(f"Posted build-status comment {resp['id']} on issue #{issue_num}.")
        except Exception as exc:
            print(f"Warning: failed to post comment on issue #{issue_num}: {exc}")
    return comment_ids


def update_comments(
    api_url: str,
    repo: str,
    token: str,
    comment_ids: dict[str, int],
    body: str,
) -> None:
    for issue_str, comment_id in comment_ids.items():
        url = f"{api_url}/repos/{repo}/issues/comments/{comment_id}"
        try:
            github_request(url, token, method="PATCH", payload={"body": body})
            print(f"Updated build-status comment {comment_id} on issue #{issue_str}.")
        except Exception as exc:
            print(f"Warning: failed to update comment {comment_id} on issue #{issue_str}: {exc}")


def main() -> int:
    phase = os.environ.get("PHASE", "").strip()
    token = os.environ.get("GITHUB_TOKEN", "").strip()
    repo = os.environ.get("GITHUB_REPOSITORY", "").strip()
    api_url = os.environ.get("GITHUB_API_URL", "https://api.github.com").rstrip("/")
    graphql_url = os.environ.get("GITHUB_GRAPHQL_URL", f"{api_url}/graphql").rstrip("/")

    if not token or not repo:
        print("GITHUB_TOKEN or GITHUB_REPOSITORY is not set; skipping issue status update.")
        if phase == "discover":
            write_github_outputs({"has_issue": "false", "issue_numbers": "", "branch_name": ""})
        return 0

    if phase == "discover":
        branch_name, issue_numbers = discover_linked_issues(
            api_url=api_url,
            graphql_url=graphql_url,
            repo=repo,
            token=token,
            ref_type=os.environ.get("GITHUB_REF_TYPE", "").strip(),
            ref_name=os.environ.get("GITHUB_REF_NAME", "").strip(),
            sha=os.environ.get("GITHUB_SHA", "").strip(),
        )
        print(f"Resolved branch: '{branch_name}', linked issues: {issue_numbers}")
        write_github_outputs(
            {
                "has_issue": "true" if issue_numbers else "false",
                "issue_numbers": ",".join(str(n) for n in issue_numbers),
                "branch_name": branch_name,
            }
        )
        return 0

    ctx = {
        "tag_name": os.environ.get("TAG_NAME", "").strip(),
        "branch_name": os.environ.get("BRANCH_NAME", "").strip(),
        "sha": os.environ.get("GITHUB_SHA", "").strip(),
        "server_url": os.environ.get("GITHUB_SERVER_URL", "https://github.com").strip(),
        "repo": repo,
        "run_id": os.environ.get("GITHUB_RUN_ID", "").strip(),
        "run_number": os.environ.get("GITHUB_RUN_NUMBER", "").strip(),
        "has_artifact": os.environ.get("HAS_ARTIFACT", "false").strip(),
        "archive_name": os.environ.get("ARCHIVE_NAME", "").strip(),
        "sha256": os.environ.get("ARCHIVE_SHA256", "").strip(),
        "release_url": os.environ.get("RELEASE_URL", "").strip(),
        "release_assets": os.environ.get("RELEASE_ASSETS", "").strip(),
        "configure_outcome": os.environ.get("CONFIGURE_OUTCOME", "").strip(),
        "build_outcome": os.environ.get("BUILD_OUTCOME", "").strip(),
        "download_outcome": os.environ.get("DOWNLOAD_OUTCOME", "").strip(),
        "package_outcome": os.environ.get("PACKAGE_OUTCOME", "").strip(),
        "upload_outcome": os.environ.get("UPLOAD_OUTCOME", "").strip(),
        "release_outcome": os.environ.get("RELEASE_OUTCOME", "").strip(),
    }

    body = render_comment_body(phase, ctx)

    if phase == "start":
        raw_issues = os.environ.get("ISSUE_NUMBERS", "").strip()
        issue_numbers = [int(x) for x in raw_issues.split(",") if x.strip().isdigit()]
        if not issue_numbers:
            write_github_outputs({"comment_ids": ""})
            return 0
        comment_ids = post_comments(api_url, repo, token, issue_numbers, body)
        write_github_outputs({"comment_ids": json.dumps(comment_ids, separators=(",", ":")) if comment_ids else ""})
        return 0

    if phase in ("after_build", "after_package", "finish"):
        raw_ids = os.environ.get("COMMENT_IDS", "").strip()
        if not raw_ids:
            return 0
        try:
            comment_ids = json.loads(raw_ids)
        except json.JSONDecodeError as exc:
            print(f"Warning: invalid COMMENT_IDS JSON: {exc}")
            return 0
        if isinstance(comment_ids, dict) and comment_ids:
            update_comments(api_url, repo, token, comment_ids, body)
        return 0

    print(f"Unknown PHASE '{phase}'; nothing to do.")
    return 0


if __name__ == "__main__":
    sys.exit(main())