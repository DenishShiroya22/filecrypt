#!/usr/bin/env python3
"""Linux release-only large-file integrity/RSS test with a hard address limit."""
import argparse, hashlib, json, os, resource, shutil, subprocess, tempfile
from pathlib import Path
def digest(path):
    value=hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4*1024*1024),b""): value.update(block)
    return value.hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("binary",type=Path)
    p.add_argument("--size-gib",type=int,default=11)
    p.add_argument("--address-limit-mib",type=int,default=256)
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--temp-dir",type=Path)
    args=p.parse_args()
    binary=args.binary.resolve()
    if args.size_gib < 1 or args.address_limit_mib < 64: p.error("Invalid size or memory limit")
    if not os.access(binary,os.X_OK): p.error("Executable is required")
    if args.output.exists(): p.error("Report already exists")
    size=args.size_gib*1024**3+123
    limit=args.address_limit_mib*1024**2
    parent=args.temp_dir or Path(tempfile.gettempdir())
    if shutil.disk_usage(parent).free < 3*size+1024**3:
        p.error("Need space for input, two dense outputs, and 1 GiB reserve")
    total_ram=int(next(x.split()[1] for x in Path("/proc/meminfo").read_text().splitlines()
                       if x.startswith("MemTotal:")))*1024
    def set_limit(): resource.setrlimit(resource.RLIMIT_AS,(limit,limit))
    report={"bytes":size,"address_limit_bytes":limit,"host_ram_bytes":total_ram,
            "input_larger_than_host_ram":size>total_ram,
            "fixture":"zero-filled file with random 64 KiB markers at beginning, midpoint, end; sparse where supported",
            "filesystem":subprocess.check_output(["df","-T",str(parent)],text=True).strip(),
            "engine":"pipeline","workers":2,"chunk_size":4194304,"max_in_flight":4,"runs":[]}
    with tempfile.TemporaryDirectory(prefix="filecrypt-large-",dir=parent) as temp:
        work=Path(temp); source=work/"input"; encrypted=work/"encrypted"; recovered=work/"recovered"
        key=work/"key"; key.write_bytes(os.urandom(257))
        with source.open("wb") as stream:
            stream.truncate(size)
            for offset in (0,size//2,size-65536):
                stream.seek(offset); stream.write(os.urandom(65536))
        report["input_allocated_bytes"]=source.stat().st_blocks*512
        print("Hashing input fixture",flush=True)
        report["input_sha256"]=digest(source)
        for mode,src,dst in (("encrypt",source,encrypted),("decrypt",encrypted,recovered)):
            stats=work/(mode+".time")
            command=["/usr/bin/time","-f","%e,%M,%U,%S","-o",str(stats),str(binary),
                     mode,str(src),str(dst),"--key-file",str(key),"--threads","2",
                     "--chunk-size","4194304","--queue-capacity","1","--max-in-flight","4"]
            print("Starting "+mode,flush=True)
            result=subprocess.run(command,preexec_fn=set_limit,check=True,capture_output=True,
                                  text=True,timeout=1800)
            seconds,rss,user,system=stats.read_text().strip().split(",")
            report["runs"].append({"mode":mode,"seconds":float(seconds),"peak_rss_kib":int(rss),
                                   "user_seconds":float(user),"system_seconds":float(system),
                                   "summary":result.stdout.strip()})
            print(mode,result.stdout.strip(),f"peak RSS={rss} KiB",flush=True)
        print("Hashing recovered file",flush=True)
        report["recovered_sha256"]=digest(recovered)
        if report["recovered_sha256"] != report["input_sha256"]:
            raise RuntimeError("Large-file integrity failure")
        # An exact comparison independently supplements the digest.
        subprocess.run(["cmp","--",str(source),str(recovered)],check=True,timeout=1800)
        report["verified"]=True
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+"\n")
    print("PASS: large-file hash/byte equality; temporary fixtures removed",flush=True)
if __name__=="__main__": main()
