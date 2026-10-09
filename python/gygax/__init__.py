import re as _re

from . import neuro, satlink
from .client import Client, Logistics, ServiceError
from .extension import Extension, ExtensionError
from .local import LocalService

__all__ = ["Client", "Logistics", "ServiceError", "Extension", "ExtensionError", "LocalService", "neuro", "satlink",
           "version_pep440", "__version__"]

# The release version in PEP 440 form. The native library reports the SemVer-style form CMake
# builds it with; version_pep440() maps one to the other (0.3.0 -> 0.3.0, 0.3.1-alpha.1 -> 0.3.1a1).
__version__ = "0.3.0"

_PRE = {"alpha": "a", "beta": "b", "rc": "rc"}


def version_pep440(version: str) -> str:
    """PEP 440 form of a Gygax version string: ``1.2.3[-alpha|beta|rc.N]`` -> ``1.2.3[a|b|rcN]``."""
    m = _re.fullmatch(r"(\d+\.\d+\.\d+)(?:-(alpha|beta|rc)\.(\d+))?", version)
    if not m:
        raise ValueError("not a Gygax version: %r" % version)
    release, label, number = m.groups()
    return release + (_PRE[label] + number if label else "")
