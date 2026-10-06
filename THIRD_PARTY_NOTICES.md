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
