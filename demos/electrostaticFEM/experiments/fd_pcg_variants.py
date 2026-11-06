"""Canonical Alpine FD CG/PCG parameter registry used by runners and plans."""

from collections import OrderedDict


def _entry(method, args=(), *, family=None, extra_scalar_fields=0, fem=False, **metadata):
    value = dict(
        method=method,
        args=[str(value) for value in args],
        family=family or method,
        extra_scalar_fields=extra_scalar_fields,
        applications=['alpine', 'fem'] if fem else ['alpine'],
    )
    value.update(metadata)
    return value


VARIANTS = OrderedDict()

# Keep the original short names and order for existing FEM and phase-1 recipes.
VARIANTS['cg'] = _entry('plain', fem=True)
VARIANTS['jacobi'] = _entry('jacobi', ['jacobi'], fem=True)
for inner, outer, name in ((2, 1, 'gs21'), (2, 2, 'gs22')):
    VARIANTS[name] = _entry(
        'gauss-seidel', ['gauss-seidel', inner, outer, 1],
        family='gauss-seidel', extra_scalar_fields=2, fem=True,
        inner=inner, outer=outer, communication=1)
for degree in (2, 4, 8, 16):
    VARIANTS[f'cheb{degree}'] = _entry(
        'chebyshev', ['chebyshev', degree], family='chebyshev',
        extra_scalar_fields=4, fem=True, degree=degree)

DEFAULT_VARIANTS = ['cg', 'jacobi', 'gs21', 'gs22', 'cheb2', 'cheb4', 'cheb8', 'cheb16']

# Alpine-only tuning points.
for level in (1, 2, 3, 4, 5):
    VARIANTS[f'newton{level}'] = _entry(
        'newton', ['newton', level], family='newton',
        extra_scalar_fields=3 * level, level=level)
for degree in (1, 31):
    VARIANTS[f'cheb{degree}'] = _entry(
        'chebyshev', ['chebyshev', degree], family='chebyshev',
        extra_scalar_fields=4, degree=degree)
# With the full periodic Laplacian, an even number of undamped Jacobi-Richardson
# sweeps has a zero response at the Nyquist mode. That makes the preconditioner
# inadmissible for PCG and can produce false recursive-residual convergence.
# Communication-free block sweeps do not contain that global Nyquist eigenmode,
# so retain their historical even settings while using odd counts for every
# full-operator path.
RICHARDSON_LOCAL_ITERATIONS = (1, 2, 4, 8)
RICHARDSON_GLOBAL_ITERATIONS = (1, 3, 5, 7)
RICHARDSON_ALT_ITERATIONS = RICHARDSON_GLOBAL_ITERATIONS

for iterations in RICHARDSON_LOCAL_ITERATIONS:
    VARIANTS[f'rich{iterations}c0'] = _entry(
        'richardson', ['richardson', iterations, 0],
        family='richardson', extra_scalar_fields=1,
        iterations=iterations, communication=0)
for iterations in RICHARDSON_GLOBAL_ITERATIONS:
    VARIANTS[f'rich{iterations}c1'] = _entry(
        'richardson', ['richardson', iterations, 1],
        family='richardson', extra_scalar_fields=1,
        iterations=iterations, communication=1)
for iterations in RICHARDSON_ALT_ITERATIONS:
    VARIANTS[f'richa{iterations}'] = _entry(
        'richardson_alt', ['richardson_alt', iterations],
        family='richardson_alt', extra_scalar_fields=2, iterations=iterations)

GS_PAIRS = ((1, 1), (2, 1), (2, 2), (4, 2), (5, 2))
for inner, outer in GS_PAIRS:
    for communication in (0, 1):
        VARIANTS[f'gs{inner}{outer}c{communication}'] = _entry(
            'gauss-seidel', ['gauss-seidel', inner, outer, communication],
            family='gauss-seidel', extra_scalar_fields=2,
            inner=inner, outer=outer, communication=communication)

SSOR_PAIRS = ((1, 1), (2, 1), (2, 2), (4, 2))
SSOR_OMEGAS = (
    ('100', '1.0'),
    ('125', '1.25'),
    ('157', '1.57079632679'),
    ('175', '1.75'),
)
for inner, outer in SSOR_PAIRS:
    for tag, omega in SSOR_OMEGAS:
        VARIANTS[f'ssor{inner}{outer}w{tag}'] = _entry(
            'ssor', ['ssor', inner, outer, omega], family='ssor',
            extra_scalar_fields=2, inner=inner, outer=outer, omega=float(omega))

MG_SMOOTHING = ((1, 1), (1, 2), (2, 1), (2, 2), (3, 3), (4, 4))
MG_OMEGAS = (
    ('060', '0.6'),
    ('067', '0.6666666666666666'),
    ('080', '0.8'),
    ('100', '1.0'),
)
for pre, post in MG_SMOOTHING:
    for omega_tag, omega in MG_OMEGAS:
        for minimum in (2, 4, 8, 16):
            for communication in (0, 1):
                name = f'mg{pre}{post}w{omega_tag}m{minimum}c{communication}'
                VARIANTS[name] = _entry(
                    'multigrid',
                    ['multigrid', pre, post, omega, minimum, communication],
                    family='multigrid', extra_scalar_fields=5,
                    pre=pre, post=post, omega=float(omega), min_cells=minimum,
                    communication=communication)


def _with_control(names):
    return ['cg'] + list(names)


TUNING_BUNDLES = OrderedDict([
    ('controls', ['cg', 'jacobi']),
    ('newton', _with_control(f'newton{level}' for level in (1, 2, 3, 4, 5))),
    ('chebyshev', _with_control(f'cheb{degree}' for degree in (1, 2, 4, 8, 16, 31))),
    ('richardson', _with_control(
        [f'rich{iterations}c0' for iterations in RICHARDSON_LOCAL_ITERATIONS]
        + [f'rich{iterations}c1' for iterations in RICHARDSON_GLOBAL_ITERATIONS])),
    ('richardson-alt', _with_control(
        f'richa{iterations}' for iterations in RICHARDSON_ALT_ITERATIONS)),
    ('gauss-seidel', _with_control(
        f'gs{inner}{outer}c{communication}'
        for inner, outer in GS_PAIRS for communication in (0, 1))),
    ('ssor-loops', _with_control(
        f'ssor{inner}{outer}w157' for inner, outer in SSOR_PAIRS)),
    # Omega refinement can select any loop pair without changing source.
    ('ssor-all', _with_control(
        f'ssor{inner}{outer}w{tag}'
        for inner, outer in SSOR_PAIRS for tag, _ in SSOR_OMEGAS)),
    ('multigrid-depth', _with_control(
        f'mg22w080m{minimum}c0' for minimum in (2, 4, 8, 16))),
    # The full MG catalog supports sequential refinement after the best depth
    # is known. These broad bundles are available for users who prefer to spend
    # allocation time instead of pausing between coordinate scans.
    ('multigrid-omega', _with_control(
        f'mg22w{tag}m{minimum}c0'
        for minimum in (2, 4, 8, 16) for tag, _ in MG_OMEGAS)),
    ('multigrid-smoothing', _with_control(
        f'mg{pre}{post}w080m{minimum}c0'
        for minimum in (2, 4, 8, 16) for pre, post in MG_SMOOTHING)),
    ('multigrid-communication', _with_control(
        f'mg22w080m{minimum}c{communication}'
        for minimum in (2, 4, 8, 16) for communication in (0, 1))),
])

SMOKE_VARIANTS = [
    'cg', 'jacobi', 'newton5', 'cheb31', 'rich4c0', 'richa5',
    'gs22c0', 'ssor22w157', 'mg22w080m4c0',
]

LARGE_COUNT_PREFLIGHT_VARIANTS = [
    'cg', 'newton5', 'ssor22w157', 'mg22w080m4c0',
]


def selected_definitions(names):
    return {name: VARIANTS[name] for name in names}
