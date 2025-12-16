

# In-Situ Visualization and Steering Framework for IPPL (WIP)

This guide explains how to use the **in-situ visualization and steering framework** integrated into IPPL via ParaView Catalyst.

This framework enables real-time visualization and parameter steering for IPPL simulations with minimal code changes.



## Table of Contents

- [In-Situ Visualization and Steering Framework for IPPL (WIP)](#in-situ-visualization-and-steering-framework-for-ippl-wip)
  - [Table of Contents](#table-of-contents)
  - [Overview](#overview)
  - [Supported Data Types](#supported-data-types)
    - [Visualization Types](#visualization-types)
    - [Steering Types](#steering-types)
      - [Primitive Types](#primitive-types)
      - [Vector Types](#vector-types)
      - [Compound Types](#compound-types)
  - [Creating Registries](#creating-registries)
    - [1. Visualization Registry](#1-visualization-registry)
    - [2. Steering Registry](#2-steering-registry)
    - [3. Registering Enums](#3-registering-enums)
    - [4. Registering Custom Structs](#4-registering-custom-structs)
    - [5. Arrays of Steerable Types](#5-arrays-of-steerable-types)
  - [Using the CatalystAdaptor](#using-the-catalystadaptor)
    - [Key Methods](#key-methods)
    - [Initialization](#initialization)
    - [During Time-Stepping](#during-time-stepping)
    - [Finalization](#finalization)
  - [Building and Compiling](#building-and-compiling)
    - [Prerequisites](#prerequisites)
    - [libcatalyst \& ParaView installation](#libcatalyst--paraview-installation)
    - [CMake Configuration](#cmake-configuration)
  - [Environment Variables for execution](#environment-variables-for-execution)
    - [Core Catalyst Configuration (Definitions Mandatory)](#core-catalyst-configuration-definitions-mandatory)
    - [Main Switches](#main-switches)
    - [Advanced IPPL Catalyst Configuration Options](#advanced-ippl-catalyst-configuration-options)
    - [Steering Interface Manipulation](#steering-interface-manipulation)
  - [Possible Visualization Methods](#possible-visualization-methods)
    - [Method 1: Live Connection with ParaView Client](#method-1-live-connection-with-paraview-client)
    - [Method 2: Live Connection with Trame Application](#method-2-live-connection-with-trame-application)
    - [Method 3: In Situ Visualization with PNG Extraction](#method-3-in-situ-visualization-with-png-extraction)
    - [Method 4: VTK Extraction and Post-hoc Visualization](#method-4-vtk-extraction-and-post-hoc-visualization)
  - [Trame Web UI](#trame-web-ui)
    - [Preparing Trame](#preparing-trame)
    - [Starting the Trame Server](#starting-the-trame-server)
    - [Debug Levels](#debug-levels)
    - [Using Web UI](#using-web-ui)
  - [Remote Visualisation](#remote-visualisation)
    - [Remote with ParaView](#remote-with-paraview)
    - [Remote with trame](#remote-with-trame)
  - [Profiling](#profiling)
  - [Example](#example)
    - [Running AlpineSight](#running-alpinesight)
  - [Troubleshooting](#troubleshooting)
    - [Common Issues](#common-issues)
    - [Debug Tips](#debug-tips)
    - [Known Bugs](#known-bugs)
- [TODOs:](#todos)

---

## Overview

IPPL's in-situ visualization framework allows you to:
- **Connect** a running Simulation to ParaView GUI or our custom Trame web application
- **Visualize** particle systems and field data in real-time during simulations
- **Steer** simulation parameters dynamically without restarting
- **Extract** visualization data (PNG images, VTK files)

The framework is based on ParaView Catalyst 2.0 and uses **Conduit** for data exchange.

All code regarding this implementation can be found inside `ippl/src/Stream`.



## Supported Data Types

### Visualization Types

The framework supports three main categories for **visualization**:

1. **Particles** (`ParticleContainer<T, Dim>`)
   - Any class deriving from `ParticleBaseBase`
   - All particle attributes are automatically discovered and visualized

2. **Scalar Fields** (`Field<T, Dim, ...>`)
   - Single-valued fields (e.g., density, potential)

3. **Vector Fields** (`Field<Vector<T, Dim_v>, Dim, ...>`)
   - Multi-component fields (e.g., electric field, magnetic field)

### Steering Types

For **steering** (interactive parameter control), the following types are supported:

#### Primitive Types
- **Arithmetic types**: `int`, `double`, `float`, `long`, etc.
- **Boolean**: `bool`
- **Button**: `ippl::Button` (momentary action trigger)
- **Enums**: Any `enum` or `enum class` (requires registration)

#### Vector Types
- **IPPL Vectors**: `ippl::Vector<T, Dim>` where `T` is arithmetic and `Dim` is 1, 2, or 3.

#### Compound Types
- **User-defined structs** composed of the above types (requires registration via `RegisterStructMembers`)
- **Arrays**: `std::vector<T>` of any steerable type (including vectors, structs, enums)

**Note**: Nested structs and Structures-of-Arrays (SoA) are currently not supported.



## Creating Registries

Registries are used to manage which data is available for visualization and steering.

### 1. Visualization Registry

Create a runtime registry for visualization objects:

```cpp
#include "Stream/Registry/VisRegistryRuntime.h"

// Create visualization registry with zero, one or multiple initial entries
std::shared_ptr<ippl::VisRegistryRuntime> vis_registry = 
    ippl::MakeVisRegistryRuntimePtr(
        "density",  this->fcontainer_m->getRho(),  // Field
        "ions",     this->pcontainer_m
    );

// Add more entries
vis_registry->add("potential_", this->fcontainer_m->getPhi());    // Scalar field
vis_registry->add("electric",   this->fcontainer_m->getE());      // Vector field

// Possibility to add same field twice with different registration name for e.g insitu FFT solvers
vis_registry->add("potential",  this->fcontainer_m->getRho());    // Scalar field
```

### 2. Steering Registry

Create a steering registry for parameters:

```cpp
// Simple scalars
double electric_scale = 30.0;
int timesteps = 1000;
bool enable_feature = false;
ippl::Button reset_btn(false);

std::shared_ptr<ippl::VisRegistryRuntime> steer_registry = 
    ippl::MakeVisRegistryRuntimePtr(
        "electric_scale", electric_scale,
        "timesteps",      timesteps,
        "enable_feature", enable_feature
    );

// Late addition of entries
steer_registry->add("reset_button",   reset_btn);
```


### 3. Registering Enums

Enums require explicit choice registration for the UI:

```cpp
enum class ExperimentType {
    PenningTrap,
    LandauDamping,
    UniformPlasma
};

// Register enum choices (type-based, reusable across labels)
cat_vis.RegisterEnumChoicesTyped<ExperimentType>({
    {"Penning Trap",    ExperimentType::PenningTrap},
    {"Landau Damping",  ExperimentType::LandauDamping},
    {"Uniform Plasma",  ExperimentType::UniformPlasma}
});

// Now you can add enum variables to steering registry
ExperimentType current_exp = ExperimentType::PenningTrap;
steer_registry->add("experiment", current_exp);

```

### 4. Registering Custom Structs

For user-defined structs, register their members **before** adding them to the registry:

```cpp
struct SimParams {
    int steps;
    double temperature;
    bool switch_val;
    ippl::Button reset_btn;
    ippl::Vector<double, 3> vec;
    ExperimentType exp_type;
};

// Register the struct layout (do this once, e.g during simulation initialisation)
ippl::CatalystAdaptor::RegisterStructMembers<SimParams>(
    "steps",       &SimParams::steps,
    "temperature", &SimParams::temperature,
    "switch_val",  &SimParams::switch_val,
    "reset_btn",   &SimParams::reset_btn,
    "vec",         &SimParams::vec,
    "exp_type",    &SimParams::exp_type
);

// Now you can add instances to the steering registry
SimParams my_params{100, 2.5, false, ippl::Button(false), {1,2,3}, MyEnum::Option1};
steer_registry->add("sim_params", my_params);
```

### 5. Arrays of Steerable Types

You can register arrays (vectors) of any steerable type. This will allow you to dynamically change the length of this vector inside the User Interface in the ParaView client or the Trame application.

```cpp
std::vector<double> coefficients = {1.0, 2.0, 3.0};
std::vector<ippl::Vector<double, 3>> positions = {{0,0,0}, {1,1,1}};
std::vector<SimParams> param_sets = { /* ... */ };

steer_registry->add("coefficients", coefficients);
steer_registry->add("positions",    positions);
steer_registry->add("param_sets",   param_sets);
```



## Using the CatalystAdaptor

The `CatalystAdaptor` class is the main interface to Catalyst.

### Key Methods

- **`InitializeRuntime(vis_reg, steer_reg)`**: Initialize Catalyst with registries.
- **`Remember_now(label)`**: Mark a visualization channel for "early visualization".
- **`ExecuteRuntime(iteration, time)`**: Execute the Catalyst pipeline (send data, update steering).
- **`Finalize()`**: Clean up Catalyst resources.


### Initialization

```cpp
#ifdef IPPL_ENABLE_CATALYST
#include "Stream/InSitu/CatalystAdaptor.h"

class MyManager {
public:
    ippl::CatalystAdaptor cat_vis;
    
    void pre_run() {
        // 1. Register any custom types FIRST
        cat_vis.RegisterEnumChoicesTyped<MyEnum>(/* ... */);
        ippl::CatalystAdaptor::RegisterStructMembers<SimParams>(/* ... */);
        
        // 2. Create registries
        auto vis_registry = ippl::MakeVisRegistryRuntimePtr(/* ... */);
        auto steer_registry = ippl::MakeVisRegistryRuntimePtr(/* ... */);
        
        // 3. Initialize Catalyst with both registries
        cat_vis.InitializeRuntime(vis_registry, steer_registry);
    }
};
#endif
```

### During Time-Stepping

```cpp
void advance() {
    // ... simulation logic ...
    #ifdef IPPL_ENABLE_CATALYST
    
    // Mark which channels should be visualized with data from an earlier point (in case the data gets overwritten or deleted before the call to ExecuteRuntime). Be aware this creates a full copy of the data!

    cat_vis.Remember_now("density");
    // You can remember multiple channels
    // cat_vis.Remember_now("electric");
    
    // Execute: Point of visualization. Halts simulation, sends current state of data to Catalyst, and fetches steering updates. The **original** object of the steerable parameters are then overwritten via their references in the steering registries. For visualization entries that have been called with Remember_now, not their current data but their previously made copy is visualized.
    
    cat_vis.ExecuteRuntime(it, time);
    
    #endif
    // ... continue simulation ...
}
```

### Finalization

```cpp
#ifdef IPPL_ENABLE_CATALYST
cat_vis.Finalize();
#endif
```



## Building and Compiling

### Prerequisites

- ParaView Binary 5.12.0+ (https://www.paraview.org/download/)
- libcatalyst (https://catalyst-in-situ.readthedocs.io/en/latest/build_and_install.html)
- MPICH 4.0.0+ or compatible MPI implementation (depends on ParaView binary)
- CMake 3.18+

https://docs.paraview.org/en/latest/Catalyst/getting_started.html

### libcatalyst & ParaView installation

Paraview Binaries can be directly be downloaded, no "installation" needed.

For the additional Catalyst 2.0 install (link also provided above) follow these steps:

```bash 
git clone https://gitlab.kitware.com/paraview/catalyst.git
mkdir catalyst-build
cd catalyst-build
ccmake ../catalyst
# here you open intial cmake configuration window, press c [cofigure] once, press e[exit] once after initial configuration has been done, now change the settings:
# 1. CATALYST_USE_MPI turn ON
# 2. CMAKE_INSTALL_PREFIX, sets a path to install folder   /usr/local  
#                          you might have to/can overwrite these: /.../catalyst-install
# then press 1. c [configure] 2. e[exit] 3. g[generate], and then you should have exited the ccmake menu back to console

cmake --build .
cmake --install .
# now the catalyst install can be found in the previously specified folder ...
```

### CMake Configuration

A `cmake.sh` script should include:

```bash
#!/bin/bash
CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_STANDARD=20 
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
  -DIPPL_ENABLE_FFT=ON 
  -DIPPL_ENABLE_SOLVERS=ON 
  -DIPPL_ENABLE_ALPINE=ON 
  -DIPPL_ENABLE_TESTS=OFF 

  # Enable Catalyst
  -DIPPL_ENABLE_CATALYST=ON
  # If cmake can't find your libcatalyst installation:
  -DCATALYST_HINT_PATH="/path/to/catalyst-install/lib/cmake/catalyst-2.0"
)

rm -rf build
mkdir build
cd build
cmake "${CMAKE_ARGS[@]}" /path/to/ippl
```

**Important**: Ensure all components (IPPL, Catalyst, ParaView) use the **same MPI implementation** to avoid runtime conflicts.




## Environment Variables for execution

Control framework behavior via environment variables in your run script:


### Core Catalyst Configuration (Definitions Mandatory)

| Variable | Description |
|----------|-------------|
| `CATALYST_IMPLEMENTATION_PATHS` | Path to Catalyst implementation inside the ParaView binary (e.g., `$path/to/ParaView-5.12.0/lib/catalyst`) |
| `CATALYST_IMPLEMENTATION_NAME` | Implementation name (currently always: `"paraview"`) |


### Main Switches

| Variable | Values | Default | Description |
|----------|--------|---------|-------------|
| `IPPL_CATALYST_VIS`         | `ON`/`OFF` | `ON` | Enable/disable visualization |
| `IPPL_CATALYST_STEER`       | `ON`/`OFF` | `OFF`| Enable/disable steering |
| `IPPL_CATALYST_PNG`         | `ON`/`OFF` | `OFF`| Enable PNG image extraction |
| `IPPL_CATALYST_VTK`         | `ON`/`OFF` | `OFF`| Enable VTK file extraction |
| `IPPL_CATALYST_GHOST_MASKS` | `ON`/`OFF` | `OFF`| Specifies how to avoid Ghost Cell visualization <sup>1</sup> |



<sup>1</sup> : *`ON` creates a field mask marking all Ghost Cells which enables ParaView to actively ignore the Ghost data, but allows the user with filters and extraction to visualize the Ghost data in the Trame app or the ParaView client. Uses caching logic to only pass reference to one mask if multiple fields rely on the same Field Layout. `OFF` the Ghost cells are cut out before the field data is sent from the simulation to Catalyst, so less data has to be sent.*



### Advanced IPPL Catalyst Configuration Options

For most use cases there will be no need to change these. The user should only use these if they have a good understanding of ParaView-Catalyst.

| Variable | Values | Default | Description |
|----------|--------|---------|-------------|
| `IPPL_CATALYST_PROXY_OPTION` | `ON`/`OFF`/ `PRODUCE_ONLY` | `ON` | Configures Proxy Writer Class <sup>1</sup> |
| `IPPL_PROXY_CONFIG_YAML`           | `<path>` | <sup>2</sup> | Path to custom Poxy config YAML file   |
| `CATALYST_PROXYS_PATH`             | `<path>` | <sup>2</sup> | Proxy XML for magnetic field steering  |
| `CATALYST_PIPELINE_PATH`           | `<path>` | <sup>2</sup> | Path to custom Catalyst Python script  |
| `CATALYST_EXTRACTOR_SCRIPT_<label>`| `<path>` | <sup>2</sup> | Path to custom PNG extractor script for specific registry entry |






<sup>1</sup> : *`ON`/`OFF` runs simulation normally and provides a switch if you write the `catalyst_proxy.xml` file (at path `src/Stream/InSitu/catalyst_scripts/`). `PRODUCE_ONLY` writes the `catalyst_proxy.xml` file and aborts simulation run.*

<sup>2</sup>: *The default scripts that are used and would be overwritten with these variables can be found in the folder:* `${IPPL_DIR}/src/Stream/InSitu/catalyst_scripts/...` 



### Steering Interface Manipulation

With the file: `${IPPL_DIR}/src/Stream/InSitu/catalyst_scripts/proxy_default_config.yaml` you can configure the use of a slider widget for arithmetic steering properties and set its range. This is possible for both steering and integer steering properties.
For IPPL steering vectors of size 2 or 3, specifying a range will not trigger the slider widget in the ParaView client (currently not supported from ParaView side) but only in the Trame app.

```yaml
ranges:
  my_label:
    min: 1.0
    max: 99.0
  another_label:
    min: -1.0
    max: 1.0
```

**Bug**: The YAML parser currently only supports double space as indenting format (not quarter space!).

(TODO: switch to JSON?)




## Possible Visualization Methods
The underlying methods can also be "combined". But certain combinations might lead to some bugs.

### Method 1: Live Connection with ParaView Client

<!-- (CMAKE/Compilation needs to have Catalyst live enabled) -->
1. **Start your simulation** 
2. **Open ParaView** (use the same version as specified in `CATALYST_IMPLEMENTATION_PATH`)

3. **Connect to Catalyst**:
   - `Catalyst` → `Connect...`
   - Host: `localhost`, Port: `22222` (default)
   - Click `OK`

4. **Extract Live Sources**:
   - In the Pipeline Browser, you'll see Catalyst sources.
   - Click on the Catalyst extract symbol to enable visualisation in the GUI's view.

5. **Visualize**: Full versatility of the ParaView client.
   - Apply ParaView filters.
   - Color by different attributes.
   - The view updates as the simulation progresses.
   - Play/Pause via `Catalyst` → `Pause Simulation`
  
### Method 2: Live Connection with Trame Application
See chapter [Trame Web UI](#trame-web-ui) for how to install and use.

1. **Start your simulation** 

2. **Launch Trame Application** 
    - The Trame server will print a URL (typically `http://localhost:8080`).
    - Open this URL in your web browser.

3. **Connect to Catalyst**:
   - In the browser UI specify: Host: `localhost`, Port: `22222` (default).
   - Click `CONNECT`.

4. **Extract Live Sources**:
   - In the Pipeline Browser, search for sources and click `Init`.
   - Default visualization (similar to PNG extraction) is automatically used.
   - Activate `Live` switch to enable live updates.

5. **Visualize**: Click on a source's edit visualization to see available options:
   - Some basic Filters and more.
   - Play/Pause Button
   - Filters (Slice, Extract Component, Extract GhostCells).
   - Color by different attributes.
   - Edit opacity map and choose from standard color mappings.

### Method 3: In Situ Visualization with PNG Extraction

1. Set `IPPL_CATALYST_PNG=ON` in runscript.
2. Start your simulation. 
3. Simulation generates `.png` files in a `data_xxx/` directory.

If you want to avoid PNG extraction for certain elements without having to recompile your IPPL simulation, just set the corresponding variable: `CATALYST_EXTRACTOR_SCRIPT_<label>` to an empty python file and the PNG extraction for this specific entry will be disabled.


### Method 4: VTK Extraction and Post-hoc Visualization

1. Set `IPPL_CATALYST_VTK=ON` in run script.
2. Start your simulation. 
3. Simulation generates `.vtm` or `.vtp` files in the `data_xxx/` directory.
4. Open ParaView.
5. `File` → `Open` → Select the generated files.
6. Apply desired filters and visualizations.


## Trame Web UI

We provide a basic Trame based (https://kitware.github.io/trame/) **web-based interactive UI**. Its current implementation should provide the most important features to imitate the ParaView client, needed for live visualization. We mainly provide this, since the official ParaView binaries for macOS based systems don't provide Catalyst live support. Meaning even for remote run simulation, one can't live connect to this simulation with the local ParaView client.

### Preparing Trame
To ensure compatibility one can use the `pvpython` executable to run the Trame script.
The problem is Trame modules are not preinstalled in the `pvpython` executable. You can check with
```bash 
./pvpython -c "import trame"
```
So instead go on and download the python version (3.9, 3.10,...) as is used for the pvpython executable. If you don't know what version is used you can check with:
```bash 
./pvpython -c "import sys; print(sys.version)"
-> 3.10.13 (main, Dec 30 2024, 00:03:26) [GCC 10.2.1 20210130 (Red Hat 10.2.1-11)]
```
meaning download python 3.10. With this python version create and setup a virtual environment with venv, as described on (https://kitware.github.io/trame/guide/tutorial/setup.html).

```bash

cd /path/to/ParaView-5.XX.X-MPI-<your-os>-Python3.XX-<your-architecture>

python3.10 -m venv trame_env
source ./trame_env/bin/activate
python -m pip install --upgrade pip
pip install trame
pip install trame-vuetify trame-vtk 
pip install vtk                     
```

### Starting the Trame Server
From then on you can run the Trame application directly with:
```bash

export PV_PREFIX=/path/to/ParaView-5.XX.X-MPI-<your_os>-Python3.XX-<your_architecture>
# EG: ParaView-5.13.3-MPI-Linux-Python3.10-x86_64"
export PVPYTHON=${PV_PREFIX}/bin/pvpython
export CATALYST_FOLDER=/path/to/ippl/src/Stream/InSitu/catalyst_scripts/

$PVPYTHON --venv ${PV_PREFIX}/trame_env ${CATALYST_FOLDER}/trame_vis_app.py --server --debug 1
```

### Debug Levels

- `--debug -1`: Silent mode (errors only)
- `--debug 0`: Default (UI interactions only)
- `--debug 1`: Moderate (UI + operation summaries)
- `--debug 2`: Detailed (everything)

### Using Web UI
The interface is designed to be self-explanatory and intuitive.

<!-- 
### Trame vs ParaView GUI

| Feature | ParaView GUI | Trame App |
|---------|--------------|-----------|
| **Access** | Desktop application | Web browser |
| **Steering UI** | Manual proxy loading | Automatic UI generation |
| **Ease of Use** | Requires PV knowledge | User-friendly web interface |
| **Customization** | Limited | Highly customizable (Python) |
| **Versatility** | Access to all filters and PV settings | Only Basic |
| **Remote Access** | VNC/X11 forwarding | Direct HTTP access | -->

## Remote Visualisation
A main interest is running simulation on a cluster, rendering the visualisation on cluster, but acceessing a the visualisation and steering inrerface locall. We shortly described here how you cann live visualize remote run Simulation locally with the paraview client and the trame app.

### Remote with ParaView
TODO

### Remote with trame

TODO


## Profiling

We use certain internal timer to time different Catalyst subroutines. These will be listed in the timing.dat file with all the other timings.

- catalyst_execute
- execVizVisitor
- execSteerVisitor
- fetchSteerParameters
(TODO? add timer for intialisation subroutines ...initViz, initSteer, produceProxy)



## Example

A complete working example can be found with the `alpine/AlpineSight` example.

<!-- to compile and run do the following: -->


### Running AlpineSight

Build script example:
```bash
# when multiple mpi version installed guarantee correct used version like:
export MPICC=/path/to/mpich/bin/mpicc
export MPICXX=/path/to/mpich/bin/mpicxx
export MPIEXEC=/path/to/ParaView-5.XX.X-MPI-<your_os>-Python3.10-<your_architecture>/lib/mpiexec

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_CXX_STANDARD=20 
  -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON
  -DMPI_C_COMPILER="${MPICC}"
  -DMPI_CXX_COMPILER="${MPICXX}"
  -DMPIEXEC_EXECUTABLE="${MPIEXEC}"
)
CMAKE_ARGS+=(
  -DIPPL_ENABLE_FFT=ON 
  -DIPPL_ENABLE_SOLVERS=ON 
  -DIPPL_ENABLE_ALPINE=ON 
  -DIPPL_ENABLE_TESTS=OFF 
)
# if CMake doesn't find catalyst pass hint path like
CATALYST_CMAKE_PATH="/path/to/catalyst-install/lib/cmake/catalyst-2.0"
echo " Passing CMake Catalyst Hint $CATALYST_CMAKE_PATH"
CMAKE_ARGS+=(
    -DIPPL_ENABLE_CATALYST=ON
    -DCATALYST_HINT_PATH=${CATALYST_CMAKE_PATH}
)
# Fresh build
rm -rf build
mkdir build
cd build
cmake "${CMAKE_ARGS[@]}" ../ippl
# cmake --build . -j
cd ..

```
Compiling script example:

```bash
cd build
make AlpineSight  -j 8

# Report linkage
echo "Linked libs (MPI/Catalyst/Python/Ascent):"
ldd alpine/AlpineSight | grep -E "libmpi|libmpicxx|libcatalyst|libpython|libascent" || true

# check for multiple MPI versions
mpi_libs=$(ldd alpine/PenningTrap | grep -o 'libmpi\.[^ ]*' | sort -u)
mpi_lib_count=$(echo "${mpi_libs}" | wc -l)

if [ "${mpi_lib_count}" -gt 1 ]; then
    echo "------------------------------------------------------------------" >&2
    echo "WARNING: Multiple MPI implementations linked into PenningTrap!" >&2
    echo "Found the following conflicting libraries:" >&2
    echo "${mpi_libs}" | sed 's/^/  - /' >&2
    echo "This is caused by linking components (like Catalyst and IPPL)" >&2
    echo "that were built against different MPI versions." >&2
    echo "Ensure all components are built with the same MPI (e.g., MPICH)." >&2
    echo "This will very LIKELY CAUSE ERRORS down the line" >&2
    echo "------------------------------------------------------------------" >&2
else
    if [ "${mpi_lib_count}" -eq 1 ]; then
        echo "------------------------------------------------------------------"
        echo "SUCCESS: Single MPI implementation linked into PenningTrap"
        echo "Using: ${mpi_libs}"
        echo "------------------------------------------------------------------"
    else
        echo "No MPI libraries detected in the binary."
    fi
fi
cd ..
```
Run script example:

```bash

export IPPL_DIR=/path/to/ippl
export PENNINGTRAP_BINDIR=${IPPL_DIR}/alpine
export PV_PREFIX="/path/to/ParaView-5.XX.X-MPI-<your_os>-Python3.10-<your_architecture>"
export MPIEXEC=${PV_PREFIX}/lib/mpiexec

# #####################################################################
#  (Important) CONFIGURE PARAVIEW CATALYST VERSION
# #####################################################################

# These two !!needed!! environment Variables will be automatically set when loading modules on 
# julich system. When running elsewhere they need to be set manually most of the time.


export CATALYST_IMPLEMENTATION_PATHS="${PV_PREFIX}/lib/catalyst"
export CATALYST_IMPLEMENTATION_NAME="paraview"

# on jureca eg. after loading modules succesfully should result to something like:: 
# echo $CATALYST_IMPLEMENTATION_PATHS 
# /p/software/jurecadc/stages/2024/software/ParaView/5.12.0-RC2-gpsmpi-2023a/lib64/catalyst

# #####################################################################
#  (Optional) CONFIGURE CATALYST OPTIONS
# #####################################################################
# any invalid input will switch to default case, check output during initialisation for parsed settings

# does nothing atm
# "ON":
#  any/"OFF": (default)
export IPPL_CATALYST_VIS=ON

# "ON":
#  any/"OFF": (default)
export IPPL_CATALYST_STEER=ON

# "ON":
#  any/"OFF": (default)
export IPPL_CATALYST_PNG=OFF

# "ON":
#  any/"OFF": (default)
export IPPL_CATALYST_VTK=OFF

# any/"ON":(default)    writes catalyst_scripts/catalyst_proxy.xml and continues simulation
# "PRODUCE_ONLY":       writes and throws exception
# "OFF":                doesn't write but (still tries to access old catalyst_scripts/catalyst_proxy.xml 
#                       if CATALYST_PROXY_PATH is not set) and runs simulation
export IPPL_CATALYST_PROXY_OPTION=ON

# "ON":                 <-> masking GHOST_MASKS <-> not finally tested 
#  any/"OFF": (default) <-> cutting GHOST_MASKS <->  works
export IPPL_CATALYST_GHOST_MASKS=OFF


# #####################################################################
# Catalyst Adaptor will try to fetch paths via these environment variables
#  else switch to # harcoded preconfigured defaults inside IPPL src directory
#  (helped via cmake environment options)
# #####################################################################

# overwrites catalyst main script/pipeline (for Live vis and vtk file extractor):
# export CATALYST_PIPELINE_PATH=${IPPL_DIR}/src/Stream/InSitu/catalyst_scripts/pipeline_default.py

# overwrites catalyst script (png extraction) for arbitrary vis channels
# export CATALYST_EXTRACTOR_SCRIPT_<registry_label>

# overwrite steering proxies completeley by referencing different file:
# export CATALYST_PROXYS_PATH = 

# change default ranges for stering channels:
# export IPPL_PROXY_CONFIG_YAML=


cd ${PENNINGTRAP_BINDIR}

# Create output directory
rm -rf data
mkdir data


# possible MPI executable
$MPIEXEC -np 2 ./AlpineSight 8 8 8 4096 22222 FFT 0.05 LeapFrog \
    --overallocate 1.0 --info 4

```


## Troubleshooting

### Common Issues

1. **Multiple MPI libraries detected**
   - Ensure ParaView, Catalyst, and IPPL all use the same MPI
   - Check with `ldd` after compilation (see compile script example above)


2. **ParaView version mismatch:**
The following error might appear in the console running the ParaView client.
It means `CATALYST_IMPLEMENTATION_PATHS` likely does not point to the same ParaView Binary as the client currently running.

```
(   9.514s) [paraview        ]vtkTCPNetworkAccessMana:532    ERR| vtkTCPNetworkAccessManager (0x35359300): Server connect id did not match regular expression on server.  This shouldn't happen.
(   9.523s) [paraview        ]vtkTCPNetworkAccessMana:346    ERR| vtkTCPNetworkAccessManager (0x35359300): 
************************************************************************
Connection failed during handshake.  Unknown error parsing the handshake string
************************************************************************
```

  

3. **Steering not working**
   - Check that struct and enum types are registered before `InitializeRuntime`.
   - Verify proxy XML file is generated.
  
3.1 **"No array named ... present" error/warnings in the simulation log (Catalyst error):**
```
(   9.435s) [pvbatch.0       ]vtkInSituInitialization:692    ERR| No array named 'steerable_field_f_array:some_label' present. Skipping.
```
If this array is currently a steering array with no elements, this warning can be ignored/is expected and won't cause any actual errors. Else it means the UI fails to provide data to Catalyst to create the backward data channel to the UI (should not happen, please create a git ISSUE).


3.2 **Execute()::FetchSteerableChannel(label) | no backward result present; skipping (IPPL warning).**

Messages like these mean that in the Simulation the Catalyst Adaptor expects data, but Catalyst has not provided any. This is an expected log when you haven't opened the ParaView or Trame UI. If an instance of a Steering Interface is active, this should also not happen (please create a GitHub Issue).




---
### Debug Tips

- Set `--info 5` flag to see detailed IPPL output (detailed visualization outputs require minimal output level 4).
- Use `--debug 2` with Trame for verbose logging.
- Review ParaView's Output Messages panel for Catalyst logs.

---

### Known Bugs

- The steering sliders and text fields for the Opacity function don't "appear" to change/react to interactions, but as soon as you click apply, the changes will take effect!
- With PNG extraction enabled, if the ParaView client is opened and connected after the Catalyst script has created the PNG extractor, the PNG extraction will "break" after a couple of timesteps. This is a "ParaView problem" and should be amended in future ParaView versions.
- Currently when closing the Trame app and opening again, the Trame steering parameters will be reset to initial values and not the current values, which might lead to some bugs.
---
---

# TODOs:


when apply is pressed... unpush buttons for trame

particle visibility toggle doesn't work ...


add methods to avoid data of certain data attributes ..


<!-- 

## Additional Resources

- **Source Directory**: `ippl-frk/src/Stream/InSitu/`
- **Python scripts Scripts**: `ippl-frk/src/Stream/InSitu/catalyst_scripts/`
- **Trame App**: `ippl-frk/src/Stream/InSitu/catalyst_scripts/trame_vis_app.py`
- **Example Manager**: `ippl-frk/alpine/AlpineSightManager.h`

For advanced customization, refer to:
- `CatalystAdaptor.h` and `CatalystAdaptor.hpp`
- `CatalystAdaptorSteering.hpp`
- `VisRegistryRuntime.h`
- `ProxyWriter.h` (for custom proxy XML generation)

-->




<!-- 
## Ascent leftovers
```bash
# ==========================================================
# opneMP
# MPICC=mpicc \
# MPICXX=mpicxx \
# -DCATALYST_HINT_PATH="/home/klappi/AddApp/catalyst/install/lib/cmake/catalyst-2.0"
# now paraview binary with openMP only MPI -> problematic later on connecting catalyst and paraview ...
# #   example for ascent on jülich, maybe want to use newer /maybe no hint needed
# #   /p/software/default/stages/2024/software/Ascent/20240122-gpsmpi-2023a/lib64/cmake/ascent
# #####################################################################
# ASCENT
# Ascent Adaptor will try fetch from environment, else swap to default
# #####################################################################
# export ASCENT_ACTIONS_PATH=${IPPL_DIR}/src/Stream/InSitu/ascent_scripts/ascent_actions_default.yaml
```

 -->
