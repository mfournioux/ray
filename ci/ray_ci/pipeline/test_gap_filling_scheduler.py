import sys
from unittest import mock

import pytest

from ci.ray_ci.pipeline.gap_filling_scheduler import GapFillingScheduler

@mock.patch(
    "ci.ray_ci.pipeline.gap_filling_scheduler.GapFillingScheduler._get_latest_commits"
)
@mock.patch("ci.ray_ci.pipeline.gap_filling_scheduler.GapFillingScheduler._get_builds")
def test_get_latest_commit_for_build_state(mock_get_builds, mock_get_latest_commits):
    mock_get_builds.return_value = [
        {
            "state": "failed",
            "commit": "000",
        },
        {
            "state": "passed",
            "commit": "111",
        },
        {
            "state": "failed",
            "commit": "222",
        },
    ]
    mock_get_latest_commits.return_value = [
        "222",
        "111",
        "000",
    ]
    scheduler = GapFillingScheduler("org", "pipeline", "token")
    assert scheduler._get_latest_revision_for_build_state("failed") == "222"
    assert scheduler._get_latest_revision_for_build_state("passed") == "111"


@mock.patch("ray_ci.pipeline.gap_filling_scheduler.GapFillingScheduler._init_buildkite")
@mock.patch("ray_ci.pipeline.gap_filling_scheduler.GapFillingScheduler._get_builds")
@mock.patch("subprocess.check_output")
def test_get_commits_between_passing_and_failing(mock_init_buildkite, mock_get_builds, mock_check_output):
    def _mock_check_output_side_effect(cmd: str, shell: bool) -> str:
        assert cmd == "git rev-list --reverse ^111 444~"
        return b"222\n333\n"
    mock_get_builds.return_value = [
        {
            "number": 2,
            "state": "passed",
            "commit": "111",
        },
        {
            "number": 3,
            "state": "failed",
            "commit": "444",
        },
    ]
    mock_check_output.side_effect = _mock_check_output_side_effect
    scheduler = GapFillingScheduler("org", "pipeline")
    assert scheduler._get_gap_commits() == ["222", "333"]


@mock.patch("ray_ci.pipeline.gap_filling_scheduler.GapFillingScheduler._init_buildkite")
@mock.patch("ray_ci.pipeline.gap_filling_scheduler.GapFillingScheduler._get_gap_commits")
@mock.patch("ray_ci.pipeline.gap_filling_scheduler.GapFillingScheduler._create_build")
def test_run(mock_init_buildkite, mock_get_gap_commits, mock_create_build):
    mock_get_gap_commits.return_value = ["222", "333"]
    scheduler = GapFillingScheduler("org", "pipeline")
    scheduler.run(dry_run=True)
    mock_create_build.assert_not_called()
    scheduler.run(dry_run=False)
    mock_create_build.assert_has_calls([
        mock.call("222"),
        mock.call("333"),
    ])

if __name__ == "__main__":
    sys.exit(pytest.main(["-v", __file__]))