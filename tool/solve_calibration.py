import numpy as np

A = [
    [37836,  9960, -40395],
    [56256, 14775, -61115],
    [79919, 19538, -77603]
]
b = [200, 300, 400]

x = np.linalg.solve(A, b)
print('x = %.8f' % x[0])
print('y = %.8f' % x[1])
print('z = %.8f' % x[2])

# Also as fractions for exactness
from fractions import Fraction
A_f = [[Fraction(v) for v in row] for row in A]
b_f = [Fraction(v) for v in b]

# Gaussian elimination for exact fraction solution
n = 3
M = [A_f[i] + [b_f[i]] for i in range(n)]

for col in range(n):
    # Pivot
    pivot = M[col][col]
    for j in range(col+1, n):
        factor = M[j][col] / pivot
        for k in range(col, n+1):
            M[j][k] = M[j][k] - factor * M[col][k]

# Back substitution
x_f = [Fraction(0)] * n
for i in range(n-1, -1, -1):
    s = M[i][n]
    for j in range(i+1, n):
        s -= M[i][j] * x_f[j]
    x_f[i] = s / M[i][i]

print()
print('Exact fractions:')
print('x = %s = %.6f' % (str(x_f[0]), float(x_f[0])))
print('y = %s = %.6f' % (str(x_f[1]), float(x_f[1])))
print('z = %s = %.6f' % (str(x_f[2]), float(x_f[2])))

print()
print('Verification:')
for i in range(3):
    s = sum(A[i][j] * float(x_f[j]) for j in range(3))
    print('  Eq%d: %.10f (expected %d)' % (i+1, s, b[i]))
