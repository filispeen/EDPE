#!/usr/bin/env python3
"""Unit tests for experimental_issue_status.py."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest
from unittest import mock

SCRIPT_PATH = pathlib.Path(__file__).with_name("experimental_issue_status.py")
SPEC = importlib.util.spec_from_file_location("experimental_issue_status", SCRIPT_PATH)
assert SPEC and SPEC.loader
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)


class ExperimentalIssueStatusTests(unittest.TestCase):
    def test_extract_issue_numbers_from_branch(self) -> None:
        self.assertEqual(MOD.extract_issue_numbers_from_branch("11-bug-on-foot-tools"), [11])
        self.assertEqual(MOD.extract_issue_numbers_from_branch("5-dlaa-stuck"), [5])
        self.assertEqual(MOD.extract_issue_numbers_from_branch("issue-42-fix"), [42])
        self.assertEqual(MOD.extract_issue_numbers_from_branch("issue/26-2"), [26])
        self.assertEqual(MOD.extract_issue_numbers_from_branch("fix/19-crash"), [19])
        self.assertEqual(MOD.extract_issue_numbers_from_branch("master"), [])
        self.assertEqual(MOD.extract_issue_numbers_from_branch("main"), [])
        self.assertEqual(MOD.extract_issue_numbers_from_branch("overlay-refactor"), [])

    def test_discover_linked_issues_merges_graphql_branch_and_pr(self) -> None:
        graphql_response = {
            "data": {
                "repository": {
                    "ref": {
                        "associatedPullRequests": {
                            "nodes": [
                                {
                                    "number": 20,
                                    "closingIssuesReferences": {"nodes": [{"number": 15}]},
                                }
                            ]
                        }
                    },
                    "issues": {
                        "pageInfo": {"hasNextPage": False, "endCursor": None},
                        "nodes": [
                            {
                                "number": 11,
                                "linkedBranches": {
                                    "nodes": [
                                        {"ref": None},
                                        {"ref": {"name": "11-bug-on-foot"}},
                                    ]
                                },
                            },
                            {
                                "number": 12,
                                "linkedBranches": {"nodes": [{"ref": {"name": "other-branch"}}]},
                            },
                        ],
                    },
                }
            }
        }

        def fake_github_request(url: str, token: str, method: str = "GET", payload=None):
            if url.endswith("/graphql"):
                return 200, graphql_response
            if url.endswith("/issues/11"):
                return 200, {"number": 11, "title": "On foot flickering"}
            raise AssertionError(f"Unexpected URL: {url}")

        with mock.patch.object(MOD, "github_request", side_effect=fake_github_request):
            branch, issues = MOD.discover_linked_issues(
                api_url="https://api.github.com",
                graphql_url="https://api.github.com/graphql",
                repo="filispeen/EDPE",
                token="tok",
                ref_type="branch",
                ref_name="11-bug-on-foot",
                sha="deadbeef",
            )

        self.assertEqual(branch, "11-bug-on-foot")
        self.assertEqual(issues, [11, 15])

    def test_discover_linked_issues_supports_explicit_override(self) -> None:
        def fake_github_request(url: str, token: str, method: str = "GET", payload=None):
            if url.endswith("/issues/26"):
                return 200, {"number": 26, "title": "DLSS FG on-foot aiming"}
            if url.endswith("/graphql"):
                raise RuntimeError("GraphQL unavailable")
            raise AssertionError(f"Unexpected URL: {url}")

        with mock.patch.object(MOD, "github_request", side_effect=fake_github_request):
            branch, issues = MOD.discover_linked_issues(
                api_url="https://api.github.com",
                graphql_url="https://api.github.com/graphql",
                repo="filispeen/EDPE",
                token="tok",
                ref_type="branch",
                ref_name="custom-experiment",
                sha="deadbeef",
                issue_override="#26",
            )

        self.assertEqual(branch, "custom-experiment")
        self.assertEqual(issues, [26])

    def test_discover_linked_issues_rejects_pull_request_number_from_branch_regex(self) -> None:
        def fake_github_request(url: str, token: str, method: str = "GET", payload=None):
            if url.endswith("/graphql"):
                raise RuntimeError("GraphQL unavailable")
            if url.endswith("/issues/77"):
                return 200, {"number": 77, "pull_request": {"url": "..."}}
            raise AssertionError(f"Unexpected URL: {url}")

        with mock.patch.object(MOD, "github_request", side_effect=fake_github_request):
            branch, issues = MOD.discover_linked_issues(
                api_url="https://api.github.com",
                graphql_url="https://api.github.com/graphql",
                repo="filispeen/EDPE",
                token="tok",
                ref_type="branch",
                ref_name="77-pr-branch",
                sha="deadbeef",
            )

        self.assertEqual(branch, "77-pr-branch")
        self.assertEqual(issues, [])

    def test_render_comment_lifecycle_and_isolation_notice(self) -> None:
        ctx = {
            "tag_name": "test-builds",
            "edpe_version": "v0.8.0-rc2+issue26.1234567",
            "base_tag": "v0.8.0-rc2",
            "branch_name": "issue/26-2",
            "commits_summary": "1234567 fix DLSS FG on-foot aiming",
            "sha": "1234567890abcdef",
            "server_url": "https://github.com",
            "repo": "filispeen/EDPE",
            "run_id": "999",
            "run_number": "18",
            "has_artifact": "false",
            "archive_name": "EDPE-issue26-1234567.zip",
            "sha256": "ABCD1234",
            "release_url": "https://github.com/filispeen/EDPE/releases/tag/test-builds",
            "asset_url": "https://github.com/filispeen/EDPE/releases/download/test-builds/EDPE-issue26-1234567.zip",
            "configure_outcome": "success",
            "build_outcome": "success",
            "download_outcome": "skipped",
            "package_outcome": "success",
            "upload_outcome": "success",
            "release_outcome": "success",
        }

        start_body = MOD.render_comment_body("start", ctx)
        self.assertIn("Test Build `v0.8.0-rc2+issue26.1234567`: In Progress", start_body)
        self.assertIn("Base Release:** `v0.8.0-rc2`", start_body)
        self.assertIn("Does **not** include unmerged fixes from other open issues.", start_body)
        self.assertIn("configuring & compiling", start_body)
        self.assertIn("https://github.com/filispeen/EDPE/actions/runs/999", start_body)

        after_build_body = MOD.render_comment_body("after_build", ctx)
        self.assertIn("Packaging", after_build_body)

        after_pkg_body = MOD.render_comment_body("after_package", ctx)
        self.assertIn("Publishing", after_pkg_body)

        finish_body = MOD.render_comment_body("finish", ctx)
        self.assertIn("Ready", finish_body)
        self.assertIn("https://github.com/filispeen/EDPE/releases/tag/test-builds", finish_body)
        self.assertIn("https://github.com/filispeen/EDPE/releases/download/test-builds/EDPE-issue26-1234567.zip", finish_body)
        self.assertIn("ABCD1234", finish_body)
        self.assertIn("Commits since v0.8.0-rc2", finish_body)
        self.assertIn("1234567 fix DLSS FG on-foot aiming", finish_body)

    def test_render_comment_failure(self) -> None:
        ctx = {
            "tag_name": "test-builds",
            "edpe_version": "v0.8.0-rc2+issue26.1234567",
            "base_tag": "v0.8.0-rc2",
            "branch_name": "issue/26-2",
            "sha": "1234567890abcdef",
            "server_url": "https://github.com",
            "repo": "filispeen/EDPE",
            "run_id": "1000",
            "run_number": "19",
            "has_artifact": "false",
            "configure_outcome": "success",
            "build_outcome": "failure",
            "download_outcome": "skipped",
            "package_outcome": "skipped",
            "upload_outcome": "skipped",
            "release_outcome": "skipped",
        }
        body = MOD.render_comment_body("finish", ctx)
        self.assertIn("❌ Test Build `v0.8.0-rc2+issue26.1234567`: Failed", body)
        self.assertIn("❌ **Build EDPE (Release x64)**: failure", body)
        self.assertIn("workflow run logs", body)


if __name__ == "__main__":
    unittest.main()
