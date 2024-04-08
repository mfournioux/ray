import subprocess
from datetime import datetime, timedelta
from typing import List, Dict, Optional, Any

from pybuildkite.buildkite import Buildkite


BRANCH = "master"


class GapFillingScheduler:
    """
    This buildkite pipeline scheduler is responsible for scheduling gap filling builds
    when the latest build is failing.
    """

    def __init__(
        self,
        buildkite_organization: str,
        buildkite_pipeline: str,
        buildkite_access_token: str,
        days_ago: int = 1,
    ):
        self.buildkite_organization = buildkite_organization
        self.buildkite_pipeline = buildkite_pipeline
        self.buildkite = Buildkite()
        self.buildkite.set_access_token(buildkite_access_token)
        self.days_ago = days_ago

    def _get_latest_commit_for_build_state(self, build_state: str) -> Optional[str]:
        latest_commits = self._get_latest_commits()
        builds = sorted(
            [
                build
                for build in self._get_builds()
                if build["state"] == build_state and build["commit"] in latest_commits
            ],
            key=lambda build: latest_commits.index(build["commit"]),

    def _get_latest_revision_for_build_state(self, build_state: str) -> Optional[str]:
        builds = sorted(
            [build for build in self._get_builds() if build["state"] == build_state],
            key=lambda build: build["number"],
            reverse=True,
        )
        if not builds:
            return None

        return builds[0]["commit"]

    def _get_latest_commits(self) -> List[str]:
        return (
            subprocess.check_output(
                [
                    "git",
                    "log",
                    "--pretty=tformat:'%H'",
                    f"--since={self.days_ago}.days",
                ]
            )
            .decode("utf-8")
            .strip()
            .split("\n")
        )

    def _get_builds(self, days_ago: int = 1) -> List[Dict[str, Any]]:
        return self.buildkite.builds().list_all_for_pipeline(
            self.buildkite_organization,
            self.buildkite_pipeline,
            created_from=datetime.now() - timedelta(days=self.days_ago),
            branch=BRANCH,
        )
