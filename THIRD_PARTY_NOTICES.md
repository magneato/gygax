# Third-party notices

Gygax depends on the C++ standard library, the platform C library, and two system libraries that are always hidden behind Gygax interfaces (`gygax/math/linalg.hpp`, `gygax/math/bigint.hpp`) so a different backend can replace them without touching callers:

| Component | License | Use |
| --- | --- | --- |
| Eigen 3.3+ | MPL-2.0 | Header-only dense linear algebra behind `math::LinearBackend` (inverse kinematics) |
| GMP (with gmpxx) | LGPL-3.0-or-later / GPL-2.0-or-later | Arbitrary-precision integers behind `math::IntegerBackend` |

Build and test time only:

| Component | Version | License | Use |
| --- | --- | --- | --- |
| GoogleTest | 1.14.0 | BSD-3-Clause | Unit tests, fetched by CMake `FetchContent` when `GYGAX_BUILD_TESTS=ON` |

## SatLink SGP4/SDP4

`src/gygax/satlink/sgp4.cpp` is a C++ adaptation of the SGP4/SDP4 reference implementation published with
D. A. Vallado, P. Crawford, R. Hujsak and T. S. Kelso, "Revisiting Spacetrack Report #3", AIAA 2006-6753, and
distributed freely by CelesTrak (https://celestrak.org/publications/AIAA/2006-6753/). It was ported with Brandon
Rhodes's `python-sgp4`, a faithful port of the same code, open alongside; that project carries the notice below.
The test fixtures `tests/fixtures/sgp4/SGP4-VER.TLE` and `tests/fixtures/sgp4/tcppver.out` are the verification
element sets and reference output from the same distribution, unmodified.

```
The MIT License (MIT)
Copyright © 2012–2016 Brandon Rhodes

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
