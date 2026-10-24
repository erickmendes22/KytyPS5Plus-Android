"""Reuse an audited APK's native core only when all native sources/build commands are unchanged."""
import hashlib,io,json,os,re,subprocess,tempfile,zipfile
from pathlib import Path
BASE='1f5c2de19adea39062700d3484fd9de1ee86684b'
REPO='erickmendes22/KytyPS5Plus-Android'
ARTIFACT=12560921156
DIGEST='594e285c2050280075cdf1e4274dc20ebfd707abde0029f28e1b0a793226f375'
def git(*args):return subprocess.check_output(['git',*args],text=True)
def unchanged():
    paths=git('diff','--name-only',BASE,'HEAD').splitlines()
    if any(not (p.startswith(('android/','docs/','tests/')) or p=='.github/workflows/android.yml') for p in paths):return False
    old=git('show',BASE+':.github/workflows/android.yml')
    new=Path('.github/workflows/android.yml').read_text()
    for step in ['Android NDK','Configure','Build native libraries','Stage native libraries']:
        pattern=r'      - name: '+re.escape(step)+r'\n.*?        run: \|\n(.*?)(?=\n      - |\Z)'
        a=re.search(pattern,old,re.S);b=re.search(pattern,new,re.S)
        if not a or not b or a.group(1)!=b.group(1):return False
    if re.search(r'NDK_VERSION: (\S+)',old).group(1)!=re.search(r'NDK_VERSION: (\S+)',new).group(1):return False
    return True
reused=False
try:
    if unchanged():
        artifact=subprocess.check_output(['gh','api',f'repos/{REPO}/actions/artifacts/{ARTIFACT}/zip'])
        if hashlib.sha256(artifact).hexdigest()!=DIGEST:raise RuntimeError('Audited artifact SHA-256 mismatch')
        with zipfile.ZipFile(io.BytesIO(artifact)) as archive:
            apks=[n for n in archive.namelist() if n.endswith('.apk')]
            if len(apks)!=1:raise RuntimeError('Expected one audited APK')
            apk=archive.read(apks[0])
        stage=Path(os.environ['RUNNER_TEMP'])/'native'
        entries={'libmain.so':'libmain.so','libSDL3.so':'3rdparty/SDL3/libSDL3.so'}
        entries.update({name:'3rdparty/libadrenotools/src/hook/'+name for name in ['libhook_impl.so','libmain_hook.so','libfile_redirect_hook.so','libgsl_alloc_hook.so']})
        hashes={}
        with zipfile.ZipFile(io.BytesIO(apk)) as package:
            for name,target in entries.items():
                data=package.read('lib/arm64-v8a/'+name)
                if not data.startswith(b'\x7fELF'):raise RuntimeError('Invalid native library')
                output=stage/target;output.parent.mkdir(parents=True,exist_ok=True);output.write_bytes(data)
                hashes[name]=hashlib.sha256(data).hexdigest()
        Path('android/app/src/main/assets/native-core.json').write_text(json.dumps({'source_commit':BASE,'artifact_sha256':DIGEST,'libraries':hashes},indent=2)+'\n')
        print('Reused audited native libraries; native source/build inputs are unchanged')
        reused=True
except Exception as error:
    print('Native reuse unavailable; full native build will run:',error)
with open(os.environ['GITHUB_OUTPUT'],'a') as output:output.write('reused='+str(reused).lower()+'\n')
