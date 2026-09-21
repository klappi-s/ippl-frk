#!/usr/bin/env python3
"""Explain Jacobi scaling using independently integrated Basix P3 GLL diagonals."""
import argparse
from fractions import Fraction
import itertools
import json
from pathlib import Path
import basix
import numpy as np

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
order = 3
element = basix.create_element(basix.ElementFamily.P, basix.CellType.interval, order,
                              basix.LagrangeVariant.gll_warped)
points, weights = basix.make_quadrature(basix.CellType.interval, 2*order)
table = element.tabulate(1, points)
permutation = np.argsort(element.points[:, 0])
values = table[0, :, :, 0][:, permutation]
gradients = table[1, :, :, 0][:, permutation]
mass = np.einsum('q,qi,qi->i', weights, values, values)
stiffness = np.einsum('q,qi,qi->i', weights, gradients, gradients)
records = []
for dimension in [1, 2, 3]:
    diagonal = []
    for indices in itertools.product(range(order+1), repeat=dimension):
        incidence = 2**sum(i in (0, order) for i in indices)
        local = sum(stiffness[indices[d]] * np.prod([mass[indices[e]] for e in range(dimension) if e != d])
                    for d in range(dimension))
        diagonal.append(float(incidence*local))
    ratio = max(diagonal)/min(diagonal)
    records.append(dict(dimension=dimension, minimum=min(diagonal), maximum=max(diagonal),
                        ratio=ratio, simple_fraction=str(Fraction(ratio).limit_denominator(10000))))
result = dict(source='Basix P3 GLL basis on [0,1], GL4 exact contractions; periodic incidence weights',
              basix=basix.__version__, numpy=np.__version__, quadrature_points=len(weights),
              nodes=element.points[permutation, 0].tolist(), mass_diagonal=mass.tolist(),
              stiffness_diagonal=stiffness.tolist(), dimensions=records,
              scope='Uniform isotropic cells, fully periodic ownership. Common h^(dimension-2) scaling cancels in the ratio. This is diagonal spread, not condition number.')
a.output.write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(records, indent=2))
