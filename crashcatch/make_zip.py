import os
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
version = sys.argv[1] if len(sys.argv) > 1 else "1.0.0"
out_dir = os.path.join(ROOT, "dist")
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, f"crashcatch-{version}.zip")
files = [(os.path.join(ROOT, "HOW TO USE.txt"), "HOW TO USE.txt"), (os.path.join(ROOT, "build", "crashcatch.exe"), "crashcatch.exe"), (os.path.join(ROOT, "diagnose.bat"), "diagnose.bat"),
         (os.path.join(ROOT, "known_crashes.txt"), "known_crashes.txt"),
         (os.path.join(ROOT, "bisect.bat"), "bisect.bat"),
         (os.path.join(ROOT, "deep.bat"), "deep.bat"),
         (os.path.join(ROOT, "README.md"), "README.md")]
stamp = time.localtime()[:6]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for path, name in files:
        info = zipfile.ZipInfo(name, date_time=stamp)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.create_system = 0
        info.external_attr = 0x20
        with open(path, "rb") as f:
            z.writestr(info, f.read())
with zipfile.ZipFile(out) as z:
    bad = z.testzip()
    names = z.namelist()
data = open(out, "rb").read()
user = os.environ.get("USERNAME", "")
hits = [w for w in {user, os.path.basename(os.path.expanduser("~"))} if w and w.lower().encode() in data.lower()]
print(out, len(data), "bytes,", names, "damaged:" if bad else "intact", bad or "")
print("user name in the zip:", hits or "no")
sys.exit(1 if bad or hits else 0)
