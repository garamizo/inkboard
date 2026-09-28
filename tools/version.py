# PlatformIO post-script: -DINKBOARD_VERSION from INKBOARD_VERSION (`just flash-dev` sets
# <VERSION>-<git hash>) or the repo's VERSION file. The board sends it as its User-Agent.
# Only src/ gets the define (projenv), so a new git hash doesn't rebuild the framework.
import os

Import("env", "projenv")  # noqa: F821 (provided by PlatformIO)

version = os.environ.get("INKBOARD_VERSION", "").strip()
if not version:
    with open(os.path.join(env["PROJECT_DIR"], "VERSION")) as f:  # noqa: F821
        version = f.read().strip()
projenv.Append(CPPDEFINES=[("INKBOARD_VERSION", projenv.StringifyMacro(version))])  # noqa: F821
