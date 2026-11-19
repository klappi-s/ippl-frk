"""Exercise real FEL checkpoint writing and continuation in separate MPI launches."""

import argparse
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def run(command, cwd, expected_error=None):
    result = subprocess.run([*command, "--info", "1"], cwd=cwd, capture_output=True, text=True,
                            env={**os.environ, "IPPL_CATALYST_VIS": "OFF"}, timeout=120)
    log = result.stdout + result.stderr
    if expected_error is None:
        if result.returncode:
            raise AssertionError(f"{command} failed\n{log}")
    elif result.returncode == 0 or expected_error not in log:
        raise AssertionError(f"Expected '{expected_error}'\n{log}")
    return log


def particle_rows(dump, checkpoint, step, scratch):
    """Return complete binary particle records, independent of MPI row order."""
    file = checkpoint / "particles" / f"data_{step}.h5"
    listing = subprocess.check_output([dump, "-n", file], text=True)
    datasets = sorted(line.split()[1] for line in listing.splitlines()
                      if len(line.split()) == 2 and line.split()[0] == "dataset"
                      and line.split()[1].startswith(f"/data/{step}/particles/electrons/"))
    assert datasets, file
    columns = []
    for dataset in datasets:
        subprocess.run([dump, "-d", dataset, "-b", "LE", "-o", scratch, file],
                       stdout=subprocess.DEVNULL, check=True)
        # Every stored FEL particle component is a double. Keep its exact bits.
        columns.append([value for value, in struct.iter_unpack("8s", scratch.read_bytes())])
    assert len({len(column) for column in columns}) == 1 and columns[0]
    return datasets, sorted(zip(*columns))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exact-hdf5", metavar="H5DIFF")
    parser.add_argument("--copy", type=Path, help="Check redistribution without advancing time")
    parser.add_argument("executable", type=Path)
    parser.add_argument("example", type=Path)
    parser.add_argument("ranks", type=int)
    parser.add_argument("launcher", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    def launcher(ranks, executable=None):
        substitutions = {"@FEL@": str(executable or args.executable), "@RANKS@": str(ranks)}
        return [substitutions.get(word, word) for word in args.launcher]

    launch = launcher(args.ranks)
    config = json.loads(args.example.read_text())
    config["mesh"].update({"length-scale": "natural", "time-scale": "natural",
                           "extents": [128, 128, 32], "resolution": [8, 8, 32],
                           "space-charge": True})
    config["bunch"].update({"gamma": 2., "charge": 1., "mass": 1., "number-of-particles": 256,
                            "sigma-position": [2.5, 2., 1.6], "sigma-momentum": [.02, .01, .01],
                            "distribution-truncations": [8., 8., 5.]})
    config["undulator"]["static-undulator"].update({
        "undulator-parameter": 1., "period": 8., "length": 1000.})

    with tempfile.TemporaryDirectory(prefix="fel-restart-", dir=Path.cwd()) as tmp:
        root = Path(tmp)

        def job(output, steps, interval, name):
            config["output"]["path"] = str(root / output)
            config["mesh"]["total-time"] = (steps - .25) * 4 / 1.5
            config["checkpoint"] = {"interval": interval}
            path = root / (name + ".json")
            path.write_text(json.dumps(config))
            return str(path)

        def compare(output, start, reference_dir="reference", ranks=args.ranks):
            for prefix in ("feldiag", "radiation", "radiation_band"):
                name = f"{prefix}_{ranks}.csv"
                ref = (root / reference_dir / name).read_text().splitlines()
                rows = (root / output / name).read_text().splitlines()
                assert rows[0] == ref[0]
                assert len(ref) == (28 if reference_dir == "reference" else 9)
                expected = ref[start + 1:]
                assert len(expected) == len(rows) - 1, (name, len(expected), len(rows))
                for row, baseline in zip(rows[1:], expected):
                    values = list(map(float, row.split()))
                    reference = list(map(float, baseline.split()))
                    assert len(values) == len(reference)
                    assert all(math.isclose(x, y, rel_tol=1e-8, abs_tol=1e-30)
                               for x, y in zip(values, reference)), (name, row, baseline)
                assert any(abs(float(row.split()[1])) > 0 for row in expected), name

        if args.copy:
            # Nonaligned z cuts and a different diagnostic-plane owner. Move the same
            # step through 1 -> 3 -> 2 -> 1 without any FDTD update in between.
            run(launch + [job("reference", 26, 18, "one")], root)
            step = Path("checkpoints/step_18")
            original = root / "reference" / step
            source = original
            for ranks, name in [(3, "three"), (2, "two"), (1, "back")]:
                out = root / name
                run(launcher(ranks, args.copy) + [str(source), str(out)], root)
                source = out / step
            if args.exact_hdf5:
                group = "/data/18/meshes"
                subprocess.run([args.exact_hdf5, str(original / "fields.h5"),
                                str(source / "fields.h5"), group, group], check=True)

                dump = shutil.which("h5dump", path=str(Path(args.exact_hdf5).parent) +
                                    os.pathsep + os.environ.get("PATH", ""))
                if dump:
                    scratch = root / "component.bin"
                    expected = particle_rows(dump, original, 18, scratch)
                    for copy in ("three", "two", "back"):
                        assert particle_rows(dump, root / copy / step, 18, scratch) == expected, copy

            # Resume 2 -> 1 and compare every subsequent diagnostic row with the
            # uninterrupted one-rank donor. This includes the nonzero radiation window.
            saved_two = str(root / "two" / step)
            run(launch + [job("fewer", 26, 0, "fewer"), "--restart", saved_two], root)
            compare("fewer", 19)
            # Resume 2 -> 3. Use the same physical state and three-rank solver as
            # the reference to isolate the read from solver decomposition effects.
            for name, checkpoint in [("three_ref", root / "three" / step), ("more", saved_two)]:
                run(launcher(3) + [job(name, 26, 0, name), "--restart", str(checkpoint)], root)
            compare("more", 0, "three_ref", 3)
            return

        # A checkpoint-disabled run supplies an independent reference for all three diagnostics.
        run(launch + [job("reference", 26, 0, "reference")], root)
        # Save at step 18, extend the end time and append exactly the remaining eight rows.
        # A retry must preserve leftovers from an interrupted earlier attempt.
        partial = root / "split/checkpoints/step_18.0.partial"
        partial.mkdir(parents=True)
        (partial / "sentinel").touch()
        run(launch + [job("split", 18, 18, "first")], root)
        assert (partial / "sentinel").is_file()
        run(launch + ["--restart", str(partial)], root, "Incomplete FEL checkpoint")
        checkpoint = root / "split/checkpoints/step_18"
        assert (checkpoint / "config.json").is_file()
        # Without an override the saved job is sufficient. Its end step is already complete.
        run(launch + ["--restart", str(checkpoint)], root)
        run(launch + [job("split", 26, 18, "resume"), "--restart", str(checkpoint)], root)
        compare("split", 0)
        # A checkpoint from before the end of the CSVs must not append overlapping rows.
        run(launch + [str(root / "resume.json"), "--restart", str(checkpoint)], root,
            "CSV files do not end at the checkpoint")
        # Branching into an empty directory starts a new diagnostic segment with headers.
        run(launch + [job("segment", 26, 0, "branch"), "--restart", str(checkpoint)], root)
        compare("segment", 19)
        # Restart just from a published mid-run directory. Its saved relative output path
        # resolves in a new working directory and the executable runs eight more steps.
        full = root / "full"
        replay = root / "replay"
        full.mkdir()
        replay.mkdir()
        original = Path(job("unused", 26, 18, "portable"))
        config["output"]["path"] = "data"
        original.write_text(json.dumps(config))
        run(launch + [str(original)], full)
        midway = full / "data/checkpoints/step_18"
        run(launch + ["--restart", str(midway)], replay)
        compare("replay/data", 19)
        # When HDF5 tools are available on Serial, independently check every numeric
        # dataset, including all ghost cells, particle attributes and the diagnostic ring.
        if args.exact_hdf5 and (full / "data/checkpoints/step_26/fields.h5").exists():
            for filename, group in (("fields.h5", "meshes"), ("particles/data_26.h5", "particles")):
                dataset = f"/data/26/{group}"
                paths = [str(d / "data/checkpoints/step_26" / filename) for d in (full, replay)]
                subprocess.run([args.exact_hdf5, *paths, dataset, dataset], check=True, timeout=60)
        config["bunch"]["gamma"] = 2.01
        run(launch + [job("bad", 30, 0, "bad"), "--restart", str(checkpoint)], root,
            "incompatible mesh or FEL physics")


if __name__ == "__main__":
    main()
