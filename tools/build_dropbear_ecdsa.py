#!/usr/bin/env python3
from pathlib import Path
import subprocess,json,hashlib,shutil
p=Path(__file__).resolve().parents[1];s=p/'work/lan-fix/sdk';o=p/'work/ssh-access';o.mkdir(exist_ok=True)
link=s/'package/feeds/base/dropbear'
if not link.exists():link.symlink_to(s/'base-source/package/network/services/dropbear',target_is_directory=True)
with (o/'dropbear-build.log').open('w') as log:
 subprocess.run(['make','package/dropbear/compile','CONFIG_DROPBEAR_ECC=y','CONFIG_DROPBEAR_ECDSA_SK=y','FORCE=1','V=s','-j4'],cwd=s,stdout=log,stderr=subprocess.STDOUT,check=True)
candidates=list((s/'build_dir/target-aarch64_generic_musl').glob('dropbear-*/.pkgdir/dropbear/usr/sbin/dropbear'));assert len(candidates)==1,candidates
f=o/'dropbear';shutil.copy2(candidates[0],f)
assert all(x in f.read_bytes() for x in [b'ecdsa-sha2-nistp256',b'sk-ecdsa-sha2-nistp256@openssh.com'])
m={'version':'2024.86','features_enabled':['CONFIG_DROPBEAR_ECC','CONFIG_DROPBEAR_ECDSA_SK'],'sha256':hashlib.sha256(f.read_bytes()).hexdigest(),'bytes':f.stat().st_size}
(o/'build-manifest.json').write_text(json.dumps(m,indent=2)+'\n');print(json.dumps(m))
