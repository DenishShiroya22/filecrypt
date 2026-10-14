#!/usr/bin/env python3
"""Linux warm-cache benchmark. All verification is outside the timed interval."""
import argparse, csv, hashlib, json, os, platform, random, re, shutil
import statistics, subprocess, tempfile, time
from pathlib import Path

def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", action="append", required=True, metavar="LABEL=PATH")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=512)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    if args.size_mib < 1 or args.repeats < 2:
        parser.error("size must be positive and repeats must be >= 2")
    if not Path("/usr/bin/time").is_file():
        parser.error("Linux /usr/bin/time is required")
    binaries = []
    for item in args.binary:
        label, separator, executable = item.partition("=")
        path = Path(executable).resolve()
        if not separator or not label or not os.access(path, os.X_OK):
            parser.error("Expected LABEL=path/to/executable")
        binaries.append((label, path))
    if len({label for label, _ in binaries}) != len(binaries):
        parser.error("Binary labels must be unique")
    args.output.mkdir(parents=True, exist_ok=False)
    columns = ["variant","engine","workers","chunk_size","max_in_flight","repeat",
               "bytes","wall_seconds","mib_per_second","peak_rss_kib","user_seconds",
               "system_seconds","buffer_allocations","task_push_waits","result_push_waits",
               "ciphertext_sha256"]
    metadata = {"platform": platform.platform(), "cpu_count": os.cpu_count(),
                "size_mib": args.size_mib, "repeats": args.repeats,
                "cache_policy": "warm input; no cache dropping; buffered output without fsync",
                "timing": "process launch through successful publication; hashing excluded",
                "queue_capacity": 4, "max_in_flight": 8,
                "binaries": {label: {"path": str(path), "sha256": sha256(path)} for label,path in binaries}}
    with tempfile.TemporaryDirectory(prefix="filecrypt-bench-", dir=args.temp_dir) as temp:
        work = Path(temp)
        fixture, key, output = work/"input.bin", work/"key.bin", work/"output.bin"
        with fixture.open("wb") as stream:
            for _ in range(args.size_mib):
                stream.write(os.urandom(1024 * 1024))
        key.write_bytes(os.urandom(257))
        metadata["fixture_sha256"] = sha256(fixture)
        metadata["filesystem"] = subprocess.check_output(
            ["df", "-T", str(work)], text=True).strip()
        metadata["memory"] = Path("/proc/meminfo").read_text().splitlines()[:3]
        def command(binary, engine, workers, chunk, destination=output, source=fixture, mode="encrypt"):
            return [str(binary), mode, str(source), str(destination), "--key-file", str(key),
                    "--engine", engine, "--threads", str(max(workers,1)),
                    "--chunk-size", str(chunk), "--queue-capacity", "4", "--max-in-flight", "8"]
        # Independent end-to-end baseline round trip establishes the expected bytes.
        subprocess.run(command(binaries[0][1], "sequential", 0, 1048576),
                       check=True, capture_output=True, timeout=120)
        expected = sha256(output)
        recovered = work/"recovered.bin"
        subprocess.run(command(binaries[0][1], "sequential", 0, 65537, recovered, output, "decrypt"),
                       check=True, capture_output=True, timeout=120)
        if sha256(recovered) != metadata["fixture_sha256"]:
            raise RuntimeError("Sequential reference round trip failed")
        output.unlink(); recovered.unlink()
        configurations = [(engine, workers, chunk) for chunk in (65536,1048576,4194304)
                          for engine,workers in [("sequential",0),("pipeline",1),
                                                ("pipeline",2),("pipeline",4),("pipeline",8)]]
        rows = []
        with (args.output/"raw.csv").open("w", newline="") as csvfile:
            writer = csv.DictWriter(csvfile, fieldnames=columns); writer.writeheader()
            # Every variant/configuration gets an untimed warm-up.
            for _,binary in binaries:
                for engine,workers,chunk in configurations:
                    subprocess.run(command(binary,engine,workers,chunk), check=True,
                                   capture_output=True, timeout=120)
                    if sha256(output) != expected: raise RuntimeError("Warm-up integrity failure")
                    output.unlink()
            schedule = [(label,binary,engine,workers,chunk,repeat)
                        for repeat in range(1,args.repeats+1) for label,binary in binaries
                        for engine,workers,chunk in configurations]
            random.Random(739).shuffle(schedule)
            for index,(label,binary,engine,workers,chunk,repeat) in enumerate(schedule,1):
                statsfile = work/"resource.txt"
                cmd = ["/usr/bin/time","-f","%M,%U,%S","-o",str(statsfile),
                       *command(binary,engine,workers,chunk)]
                started = time.perf_counter()
                result = subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=120)
                elapsed = time.perf_counter() - started
                rss,user,system = statsfile.read_text().strip().split(",")
                counters = dict(re.findall(r"(\w+)=([^;\s]+)", result.stdout))
                actual = sha256(output)
                if actual != expected: raise RuntimeError("Benchmark integrity failure")
                row = dict(variant=label,engine=engine,workers=workers,chunk_size=chunk,
                           max_in_flight=8,repeat=repeat,bytes=fixture.stat().st_size,
                           wall_seconds=f"{elapsed:.9f}",mib_per_second=f"{args.size_mib/elapsed:.6f}",
                           peak_rss_kib=int(rss),user_seconds=user,system_seconds=system,
                           buffer_allocations=int(counters["buffer_allocations"]),
                           task_push_waits=int(counters["task_push_waits"]),
                           result_push_waits=int(counters["result_push_waits"]),
                           ciphertext_sha256=actual)
                rows.append(row); writer.writerow(row); csvfile.flush(); output.unlink()
                if index % 15 == 0 or index == len(schedule):
                    print(f"Verified benchmark {index}/{len(schedule)}", flush=True)
        metadata["ciphertext_sha256"] = expected
        metadata["verified_runs"] = len(rows)
        (args.output/"environment.json").write_text(json.dumps(metadata,indent=2)+"\n")
        groups = {}
        for row in rows:
            groups.setdefault((row["variant"],row["engine"],row["workers"],row["chunk_size"]),[]).append(row)
        summary_columns = ["variant","engine","workers","chunk_size","median_mib_s",
                           "min_mib_s","max_mib_s","max_rss_kib","median_buffer_allocations"]
        with (args.output/"summary.csv").open("w",newline="") as stream:
            writer = csv.DictWriter(stream,fieldnames=summary_columns); writer.writeheader()
            for (label,engine,workers,chunk), samples in sorted(groups.items()):
                speeds = [float(row["mib_per_second"]) for row in samples]
                writer.writerow(dict(variant=label,engine=engine,workers=workers,chunk_size=chunk,
                    median_mib_s=f"{statistics.median(speeds):.3f}",min_mib_s=f"{min(speeds):.3f}",
                    max_mib_s=f"{max(speeds):.3f}",max_rss_kib=max(row["peak_rss_kib"] for row in samples),
                    median_buffer_allocations=statistics.median(row["buffer_allocations"] for row in samples)))
    print("Results:",args.output)

if __name__ == "__main__": main()
