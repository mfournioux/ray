import os
import subprocess

from ci.ray_ci.bisector import Bisector


class MacOSBisector(Bisector):
    def validate(self, revision: str) -> bool:
        subprocess.check_call(
            [
                "cp",
                "/tmp/macos_ci_test.sh",
                os.environ["RAYCI_CHECKOUT_DIR"],
            ]
        )
        return (
            subprocess.run(
                ["./macos_ci_test.sh", self.test],
                cwd=os.environ["RAYCI_CHECKOUT_DIR"],
            ).returncode
            == 0
        )
