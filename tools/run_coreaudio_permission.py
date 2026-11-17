#!/usr/bin/env python3
"""Create an isolated denial-test app and request normal OS authorization.

Choose Don't Allow if macOS prompts. No privacy databases or system settings
are changed; the host has App Sandbox entitlement without audio-input rights.
Launching through LaunchServices gives this app its own authorization identity,
whereas direct child execution can inherit the terminal's authorized identity.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--test',type=Path,required=True)
p.add_argument('--input',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--timeout',type=int,default=180)
a=p.parse_args()
root=Path(__file__).resolve().parent.parent
base=a.test.resolve().parent/'HailPermissionDenied.app'/'Contents'
(base/'MacOS').mkdir(parents=True,exist_ok=True)
exe=base/'MacOS/hail_coreaudio_permission'
shutil.copy2(a.test,exe)
(base/'Info.plist').write_bytes(plistlib.dumps({
 'CFBundleIdentifier':'io.artalis.hail.permission-denied',
 'CFBundleName':'Hail permission denied test','CFBundleExecutable':exe.name,
 'CFBundlePackageType':'APPL','NSMicrophoneUsageDescription':
 'Verify capture rejection when this test host lacks access. No audio is saved.'}))
ent=base.parent.parent/'deny-input-entitlements.plist'
ent.write_bytes(plistlib.dumps({'com.apple.security.app-sandbox':True}))
subprocess.run(['codesign','--force','--sign','-','--entitlements',str(ent),str(base.parent)],check=True)
out=base.parent.parent/'permission-app-stdout.log'
err=base.parent.parent/'permission-app-stderr.log'
# Truncate previous logs explicitly; retain every invocation via --output.
out.write_text(''); err.write_text('')
argv=['open','-W','-n','-g','--stdout',str(out),'--stderr',str(err),str(base.parent),'--args',a.input]
start=time.monotonic(); timed_out=False
try:
 result=subprocess.run(argv,timeout=a.timeout,check=False)
 exit_code=result.returncode
except subprocess.TimeoutExpired:
 timed_out=True; exit_code=1
 # Stop only this exact owned executable. Process termination is not Hail
 # retirement/free and never counts as a successful qualification.
 rows=subprocess.run(['ps','-axo','pid=,comm='],capture_output=True,text=True,check=True).stdout
 for row in rows.splitlines():
  parts=row.strip().split(None,1)
  if len(parts)==2 and parts[1]==str(exe):
   os.kill(int(parts[0]),signal.SIGTERM)
stdout=out.read_text(); stderr=err.read_text()
passed=not timed_out and exit_code==0 and 'PASS explicit native permission denial' in stdout
report={'command':argv,'elapsed_s':time.monotonic()-start,'launch_exit':exit_code,
 'timed_out':timed_out,'passed':passed,'stdout':stdout,'stderr':stderr,
 'binary_sha256':hashlib.sha256(a.test.read_bytes()).hexdigest(),
 'source_sha256':{str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest()
                  for f in [root/'src/backend_coreaudio.c', root/'src/coreaudio_permission.m',
                            root/'tests/coreaudio_permission.m', root/'tests/coreaudio_authorize.m']},
 'entitlements':plistlib.loads(ent.read_bytes()),'no_privacy_reset':True}
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(report,indent=2)+'\n')
print('PASS native OS denial' if passed else 'UNQUALIFIED: denial not observed; see retained output')
raise SystemExit(0 if passed else 1)
