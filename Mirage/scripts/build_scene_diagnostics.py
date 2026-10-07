__copyright__ = "Copyright © 2026 王孝慈. All rights reserved."

import sys

sys.dont_write_bytecode = True

from build_moltenvk import ORIGINAL, PATCHED, patch_source, main


if __name__ == "__main__":
    main(default_mode="diagnostics")
