# FEL — Free Electron Laser mini-app

An electromagnetic PIC simulation of a free-electron laser: a relativistic
electron bunch is tracked through an undulator in a co-moving Lorentz frame,
with the self-consistent field advanced by an FDTD Maxwell solver. The radiated
power and FEL diagnostics are written to CSV files.

## Build

```sh
cmake -S . -B build -DIPPL_ENABLE_FEL=ON -DCMAKE_CXX_STANDARD=20
cmake --build build --target FreeElectronLaser
```

The FEL configuration is parsed with the Conduit API shipped by Catalyst. CMake
therefore finds or fetches Catalyst when the FEL demo is enabled, even when
`IPPL_ENABLE_CATALYST` itself is off.

The executable is built at
`build/demos/fel/FreeElectronLaser`. The example configuration is staged beside
it as `build/demos/fel/config.json` whenever the target is built.

## Run

```sh
cd build
./demos/fel/FreeElectronLaser --info 5
```

An optional first argument can select another MITHRA-style JSON job file. By
default, the executable uses the staged `build/demos/fel/config.json`; see
[config.json](config.json) for the available keys. Run on multiple ranks with
`mpirun -np <N> ...`.

Output is written to the directory given by `output.path` in the config:
`radiation_<nranks>.csv` holds the radiated power. The directory is created
automatically; relative paths are resolved from the process working directory.

## Checkpoint and restart

Enable both `-DIPPL_ENABLE_FEL=ON` and `-DIPPL_ENABLE_OPENPMD=ON` when configuring the build. Checkpoints use parallel HDF5 by default, or BP5 in an ADIOS2-only build.

**Save.** From the repository root, save the [complete example below](#example-job-file) as `job.json`. To enable the same checkpoints in an existing job, set these two top-level entries.

```json
{
  "output": {"path": "results"},
  "checkpoint": {"interval": 18}
}
```

```sh
mpiexec -n 2 ./build/demos/fel/FreeElectronLaser job.json --info 1
```

This writes a checkpoint every 18 completed timesteps and at the final step. An interval of zero or an omitted `checkpoint` entry disables checkpointing.

**Resume.** Pass the entire completed `step_<N>` directory to the same executable. If the first job stopped at step 18, this continues on one MPI rank using the saved configuration.

```sh
mpiexec -n 1 ./build/demos/fel/FreeElectronLaser --restart results/checkpoints/step_18 --info 1
```

The checkpoint restores the particles, fields and radiation history. Keep the entire directory when moving it. A directory ending in `.partial` is incomplete. You can change the MPI rank count within the limits of the FEL mesh decomposition.

**Extend or replay.** Copy `results/checkpoints/step_18/config.json` to `continue.json`. To extend the run, increase `mesh.total-time`. To replay an older checkpoint after the original run advanced further, set `output.path` to a fresh directory so later CSV rows and checkpoints are preserved. Keep the mesh and physics settings fixed and pass the edited job file before `--restart`.

```sh
mpiexec -n 2 ./build/demos/fel/FreeElectronLaser continue.json --restart results/checkpoints/step_18 --info 1
```

## Acknowledgements

The relativistic bunch initialization and the resonance-power diagnostic are directly ported from [MITHRA](https://github.com/aryafallahi/mithra), a full-wave free-electron-laser solver by A. Fallahi (GPL-licensed). It was also used as reference for much of the other parts.

## Example job file

Save this as `job.json` for a 30-step run. It writes `results/checkpoints/step_18` and `results/checkpoints/step_30` relative to the directory where you launch the executable.

```json
{
  "timestep-ratio": 1.0,
  "mesh": {
    "length-scale": "micrometers",
    "time-scale": "picoseconds",
    "extents": [3200.0, 3200.0, 280.0],
    "resolution": [10, 10, 300],
    "total-time": 30000.0,
    "space-charge": false
  },
  "bunch": {
    "charge": 1.846e8,
    "mass": 1.846e8,
    "number-of-particles": 131072,
    "gamma": 100.41,
    "position": [0.0, 0.0, 0.0],
    "sigma-position": [260.0, 260.0, 50.25],
    "sigma-momentum": [1.0e-8, 1.0e-8, 100.41e-4],
    "distribution-truncations": [1040.0, 1040.0, 90.0]
  },
  "undulator": {
    "static-undulator": {
      "undulator-parameter": 1.417,
      "period": 30000.0,
      "length": 9e6
    }
  },  
  "output": {
    "path": "results"
  },
  "checkpoint": {
    "interval": 30
  }
}```
