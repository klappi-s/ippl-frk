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

## Acknowledgements

The relativistic bunch initialization and the resonance-power diagnostic are directly ported from [MITHRA](https://github.com/aryafallahi/mithra), a full-wave free-electron-laser solver by A. Fallahi (GPL-licensed). It was also used as reference for much of the other parts. 

## Checkpoint and restart

Build the same executable with `-DIPPL_ENABLE_OPENPMD=ON` and add
`"checkpoint": {"interval": 100}` to the job JSON. The interval counts
completed timesteps. A positive value saves every interval and at the final
step. Omitting the option or using zero disables checkpoints.

Each completed checkpoint is a directory under `output.path/checkpoints`.
For example, resume after timestep 100 with two MPI ranks. The job that wrote
the checkpoint may have used a different number of ranks:

```sh
mpiexec -n 2 ./FreeElectronLaser --restart ./results/checkpoints/step_100
```

Use the FEL option `--info 1` or higher to emit CSV diagnostics.
The checkpoint includes the job configuration. To extend the end time, change
the checkpoint interval, or write a separate continuation, supply a job first:

```sh
mpiexec -n 2 ./FreeElectronLaser longer.json --restart ./results/checkpoints/step_100
```

Restart restores the simulation time, particle attributes, FDTD potential
history and the radiation sample window. Each new rank reads its part of the
saved mesh, including boundary and ghost planes. When the decomposition changes,
IPPL redistributes particles by position. The rank that owns the exit plane
reads the radiation history. This also works with existing version-one checkpoints.

Keep the mesh and FEL physics fixed. Any MPI rank count supported by the FEL
z decomposition may resume the run. Particle order and floating-point results
can vary across decompositions. Diagnostic CSV filenames include the rank count;
changing it starts a new segment and requires fresh or empty CSVs for that count. Relative paths in the job remain
relative to the launch directory. CSV output can append where it ended at the
checkpoint, or start with new headers in an empty output directory. If a run
continued past the checkpoint, choose a new `output.path` to replay it.

The directory is published after all ranks close the openPMD output. Existing
checkpoints are protected. An interrupted write may leave a `.partial`
directory. A retry uses a fresh temporary directory. Retain the
whole checkpoint directory, including the particles subdirectory. The default
backend is parallel HDF5; a BP5-only build uses BP5.
