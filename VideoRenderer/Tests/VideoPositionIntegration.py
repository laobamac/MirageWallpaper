#
#  Mirage Wallpaper
#
#  Copyright © 2026 王孝慈. All rights reserved.
#

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable, ffmpeg = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="mirage-video-position-") as directory:
        root = Path(directory)
        for name in ["base", "rotated"]:
            (root / name).mkdir()
            (root / name / "project.json").write_text(json.dumps({
                "type": "video", "file": "test.mp4", "title": "Position regression"
            }))
        pattern = (
            "color=c=red:s=192x108:r=30,"
            "drawbox=x=64:y=0:w=64:h=108:color=lime:t=fill,"
            "drawbox=x=128:y=0:w=64:h=108:color=blue:t=fill,"
            "drawbox=x=0:y=0:w=16:h=16:color=white:t=fill,"
            "drawbox=x=176:y=92:w=16:h=16:color=yellow:t=fill"
        )
        subprocess.run([
            ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", pattern,
            "-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(root / "base/test.mp4")
        ], check=True)
        help_text = subprocess.check_output([ffmpeg, "-hide_banner", "-h", "full"], text=True)
        rotation = ["-display_rotation", "90"] if "-display_rotation" in help_text else []
        metadata = [] if rotation else ["-metadata:s:v:0", "rotate=90"]
        subprocess.run([
            ffmpeg, "-hide_banner", "-loglevel", "error", *rotation,
            "-i", str(root / "base/test.mp4"), "-c", "copy", *metadata,
            str(root / "rotated/test.mp4")
        ], check=True)
        subprocess.run([executable, str(root)], check=True, timeout=60)


if __name__ == "__main__":
    main()
