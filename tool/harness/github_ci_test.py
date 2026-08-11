#!/usr/bin/env python3

from __future__ import annotations

import json
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

import github_ci


REPOSITORY = github_ci.Repository(owner="blackfrog638", name="XnnTransfer")
BRANCH = "ci/XT-068"
SHA = "a" * 40


def run_payload(
    *,
    run_id: int = 123,
    run_attempt: int = 1,
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
                "run_attempt": run_attempt,
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


class ClientTests(unittest.TestCase):
    def client(self) -> github_ci.GitHubClient:
        return github_ci.GitHubClient(REPOSITORY, "user", "secret")

    def response(self, value: object) -> mock.MagicMock:
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.read.return_value = json.dumps(value).encode("utf-8")
        response.headers = {"Content-Length": "2"}
        return response

    def test_fetches_structured_jobs_artifacts_and_archive(self) -> None:
        client = self.client()
        jobs = {"total_count": 1, "jobs": [{"id": 1}]}
        artifacts = {"total_count": 1, "artifacts": [{"id": 2}]}
        archive = b"PK\x03\x04fixture"
        responses = [
            self.response(jobs),
            self.response(artifacts),
            self.response({}),
        ]
        responses[2].read.return_value = archive
        responses[2].headers = {"Content-Length": str(len(archive))}
        with mock.patch(
            "urllib.request.urlopen",
            side_effect=responses,
        ):
            self.assertEqual(client.workflow_jobs(123), jobs)
            self.assertEqual(client.workflow_artifacts(123), artifacts)
            self.assertEqual(client.artifact_archive(2), archive)

    def test_api_failure_does_not_echo_secret_bearing_detail(self) -> None:
        detail = "https://example.test/?token=ghp_super_secret"
        with mock.patch(
            "urllib.request.urlopen",
            side_effect=urllib.error.URLError(detail),
        ):
            with self.assertRaises(github_ci.GitHubCIError) as context:
                self.client().workflow_jobs(123)
        self.assertNotIn("ghp_super_secret", str(context.exception))
        self.assertIn("request failed", str(context.exception))

    def test_rejects_oversized_artifact_before_reading_it(self) -> None:
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.headers = {
            "Content-Length": str(github_ci.MAX_ARTIFACT_BYTES + 1)
        }
        with mock.patch("urllib.request.urlopen", return_value=response):
            with self.assertRaisesRegex(
                github_ci.GitHubCIError,
                "size limit",
            ):
                self.client().artifact_archive(2)
        response.read.assert_not_called()


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
