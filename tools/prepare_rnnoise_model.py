from io import BytesIO
from pathlib import Path
import hashlib
import re
import sys
import tarfile
import urllib.request

MODEL_URL="https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz"
MODEL_ARCHIVE_SHA256="4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43"
MODEL_GIT_BLOB_SHA1="04638db60c30177c5f79c7b32ad429b2d435b691"

def transform(source):
    result,count=re.subn(r"#ifndef DISABLE_DEBUG_FLOAT\n.*?#endif /\*DISABLE_DEBUG_FLOAT\*/","",source,flags=re.S)
    if count!=7:
        raise ValueError(f"Unexpected model layout: {count} optional debug arrays")
    return result

def git_blob_sha(data):
    return hashlib.sha1(f"blob {len(data)}\0".encode()+data).hexdigest()

def fetch_model(output_path):
    output=Path(output_path)
    if output.exists() and git_blob_sha(output.read_bytes())==MODEL_GIT_BLOB_SHA1:
        return
    with urllib.request.urlopen(MODEL_URL,timeout=120) as response:
        archive=response.read()
    if hashlib.sha256(archive).hexdigest()!=MODEL_ARCHIVE_SHA256:
        raise ValueError("RNNoise model archive SHA-256 mismatch")
    with tarfile.open(fileobj=BytesIO(archive),mode="r:gz") as package:
        members=[m for m in package.getmembers() if m.isfile() and (m.name=="src/rnnoise_data.c" or m.name.endswith("/src/rnnoise_data.c"))]
        if len(members)!=1:
            raise ValueError(f"Unexpected RNNoise model archive layout: {len(members)} rnnoise_data.c files")
        stream=package.extractfile(members[0])
        if stream is None:
            raise ValueError("Unable to read RNNoise model source")
        source=stream.read().decode("utf-8")
    data=transform(source).encode("utf-8")
    if git_blob_sha(data)!=MODEL_GIT_BLOB_SHA1:
        raise ValueError("Generated RNNoise model does not match the pinned source blob")
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_bytes(data)

def main():
    if len(sys.argv)==3 and sys.argv[1]=="--fetch":
        fetch_model(sys.argv[2])
        return
    if len(sys.argv)==3:
        source=Path(sys.argv[1]).read_text()
        Path(sys.argv[2]).write_text(transform(source))
        return
    raise SystemExit("usage: prepare_rnnoise_model.py --fetch <output> | <input> <output>")

if __name__=="__main__":
    main()
