#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path
from unittest import mock

import github_ci


REPOSITORY = github_ci.Repository(owner="blackfrog638", name="XnnTransfer")
BRANCH = "ci/XT-068"
SHA = "a" * 40


def run_payload(
    *,
    run_id: int = 123,
    status: str = "completed",
    conclusion: str | None = "success",
    branch: str = BRANCH,
    sha: str = SHA,
    event: str = "push",
    url: str = (
        "https://github.com/blackfrog638/XnnTransfer/actions/runs/123"
    ),
) -> dict[str, object]:
    return {
        "workflow_runs": [
            {
                "id": run_id,
                "status": status,
                "conclusion": conclusion,
                "head_branch": branch,
                "head_sha": sha,
                "event": event,
                "html_url": url,
            }
        ]
    }


class RemoteParsingTests(unittest.TestCase):
    def test_accepts_https_and_ssh_github_remotes(self) -> None:
        expected = github_ci.Repository("blackfrog638", "XnnTransfer")
        self.assertEqual(
            github_ci.parse_remote_url(
                "https://github.com/blackfrog638/XnnTransfer.git"
            ),
            expected,
        )
        self.assertEqual(
            github_ci.parse_remote_url(
                "git@github.com:blackfrog638/XnnTransfer.git"
            ),
            expected,
        )

    def test_rejects_non_github_remote(self) -> None:
        with self.assertRaisesRegex(github_ci.GitHubCIError, "github.com"):
            github_ci.parse_remote_url(
                "https://example.com/blackfrog638/XnnTransfer.git"
            )


class CredentialTests(unittest.TestCase):
    def test_loads_git_credential_helper_result(self) -> None:
        with mock.patch.object(
            github_ci,
            "git_text",
            return_value="protocol=https\nusername=user\npassword=secret",
        ):
            self.assertEqual(
                github_ci.load_credentials(Path("/repository")),
                ("user", "secret"),
            )

    def test_rejects_missing_git_credentials(self) -> None:
        with mock.patch.object(
            github_ci,
            "git_text",
            return_value="protocol=https\nhost=github.com",
        ):
            with self.assertRaisesRegex(
                github_ci.GitHubCIError, "returned no github.com credentials"
            ):
                github_ci.load_credentials(Path("/repository"))


class RunSelectionTests(unittest.TestCase):
    def test_selects_latest_exact_run(self) -> None:
        payload = run_payload(
            run_id=122,
            url=(
                "https://github.com/blackfrog638/XnnTransfer/"
                "actions/runs/122"
            ),
        )
        payload["workflow_runs"].append(
            run_payload(
                run_id=124,
                status="in_progress",
                conclusion=None,
                url=(
                    "https://github.com/blackfrog638/XnnTransfer/"
                    "actions/runs/124"
                ),
            )["workflow_runs"][0]
        )
        selected = github_ci.select_run(payload, REPOSITORY, BRANCH, SHA)
        self.assertIsNotNone(selected)
        assert selected is not None
        self.assertEqual(selected.run_id, 124)

    def test_ignores_wrong_branch_sha_and_event(self) -> None:
        payload = {
            "workflow_runs": [
                run_payload(branch="harness")["workflow_runs"][0],
                run_payload(sha="b" * 40)["workflow_runs"][0],
                run_payload(event="pull_request")["workflow_runs"][0],
            ]
        }
        self.assertIsNone(
            github_ci.select_run(payload, REPOSITORY, BRANCH, SHA)
        )

    def test_rejects_mismatched_run_url(self) -> None:
        payload = run_payload(
            url="https://github.com/other/repository/actions/runs/123"
        )
        with self.assertRaisesRegex(github_ci.GitHubCIError, "does not match"):
            github_ci.select_run(payload, REPOSITORY, BRANCH, SHA)


class WorkflowWaitTests(unittest.TestCase):
    def test_waits_for_discovery_and_success(self) -> None:
        responses = iter(
            [
                {"workflow_runs": []},
                run_payload(status="queued", conclusion=None),
                run_payload(),
            ]
        )
        clock = iter([0.0, 0.0, 1.0, 2.0])
        sleeps: list[float] = []
        url = github_ci.wait_for_workflow(
            lambda: next(responses),
            REPOSITORY,
            BRANCH,
            SHA,
            10,
            1,
            monotonic=lambda: next(clock),
            sleep=sleeps.append,
        )
        self.assertEqual(
            url,
            "https://github.com/blackfrog638/XnnTransfer/actions/runs/123",
        )
        self.assertEqual(sleeps, [1, 1])

    def test_rejects_terminal_failure(self) -> None:
        with self.assertRaisesRegex(
            github_ci.GitHubCIError, "completed with failure"
        ):
            github_ci.wait_for_workflow(
                lambda: run_payload(conclusion="failure"),
                REPOSITORY,
                BRANCH,
                SHA,
                10,
                1,
            )

    def test_rejects_unknown_status(self) -> None:
        with self.assertRaisesRegex(github_ci.GitHubCIError, "unknown status"):
            github_ci.wait_for_workflow(
                lambda: run_payload(status="mystery", conclusion=None),
                REPOSITORY,
                BRANCH,
                SHA,
                10,
                1,
            )

    def test_times_out_when_run_is_missing(self) -> None:
        clock = iter([0.0, 10.0])
        with self.assertRaisesRegex(github_ci.GitHubCIError, "Timed out"):
            github_ci.wait_for_workflow(
                lambda: {"workflow_runs": []},
                REPOSITORY,
                BRANCH,
                SHA,
                10,
                1,
                monotonic=lambda: next(clock),
                sleep=lambda _: None,
            )

    def test_rejects_malformed_api_payload(self) -> None:
        with self.assertRaisesRegex(github_ci.GitHubCIError, "workflow_runs"):
            github_ci.wait_for_workflow(
                lambda: {"workflow_runs": "invalid"},
                REPOSITORY,
                BRANCH,
                SHA,
                10,
                1,
            )


if __name__ == "__main__":
    unittest.main()
