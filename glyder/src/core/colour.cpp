#include <glyder/colour.hpp>

namespace glyder::colour {

std::string_view name(Vision v) {
    switch (v) {
    case Vision::Typical: return "TYPICAL";
    case Vision::Protan: return "NO RED (PROTAN)";
    case Vision::Deutan: return "NO GREEN (DEUTAN)";
    case Vision::Tritan: return "NO BLUE (TRITAN)";
    }
    return "TYPICAL";
}

std::string_view token(Vision v) {
    switch (v) {
    case Vision::Typical: return "typical";
    case Vision::Protan: return "protan";
    case Vision::Deutan: return "deutan";
    case Vision::Tritan: return "tritan";
    }
    return "typical";
}

Vision fromToken(std::string_view t, Vision fallback) {
    for (Vision v : {Vision::Typical, Vision::Protan, Vision::Deutan, Vision::Tritan})
        if (token(v) == t) return v;
    return fallback;
}

Vision next(Vision v) {
    switch (v) {
    case Vision::Typical: return Vision::Tritan;
    case Vision::Tritan: return Vision::Protan;
    case Vision::Protan: return Vision::Deutan;
    case Vision::Deutan: return Vision::Typical;
    }
    return Vision::Typical;
}

Mat3 operator*(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) {
            float s = 0;
            for (std::size_t k = 0; k < 3; ++k) s += a.m[i * 3 + k] * b.m[k * 3 + j];
            r.m[i * 3 + j] = s;
        }
    return r;
}

Mat3 simulate(Vision v) {
    // Machado et al. 2009, severity 1.0.
    switch (v) {
    case Vision::Typical: return {};
    case Vision::Protan: return {{0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f}};
    case Vision::Deutan: return {{0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f}};
    case Vision::Tritan: return {{1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f}};
    }
    return {};
}

Mat3 correct(Vision v) {
    if (v == Vision::Typical) return {};
    // Daltonization: D = I + E (I - S). E carries what this viewer loses into what they keep: for a
    // red-green deficiency the lost red/green difference goes into lightness and blue; for tritan the
    // lost blue/yellow difference goes into red and green.
    const Mat3 e = v == Vision::Tritan ? Mat3{{1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0}} : Mat3{{0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1}};
    const Mat3 s = simulate(v);
    Mat3 lost;
    for (std::size_t i = 0; i < 9; ++i) lost.m[i] = (i % 4 == 0 ? 1.0f : 0.0f) - s.m[i];
    Mat3 d = e * lost;
    for (std::size_t i = 0; i < 9; i += 4) d.m[i] += 1.0f;
    return d;
}

Mat3 display(Vision v, bool corrected, bool preview) {
    const Mat3 c = corrected ? correct(v) : Mat3{};
    return preview ? simulate(v) * c : c;
}

} // namespace glyder::colour
