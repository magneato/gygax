// SGP4/SDP4 and two-line element parsing.
//
// The propagator follows Vallado, Crawford, Hujsak and Kelso, "Revisiting Spacetrack Report #3"
// (AIAA 2006-6753), in its "improved" operation mode, routine by routine and with the reference
// variable names, so it can be read side by side with the paper and the published reference code.
// It reproduces the paper's verification output (tcppver.out) for all its satellites; see
// tests/unit/test_satlink.cpp. Credits and licences: THIRD_PARTY_NOTICES.md.

#include <gygax/satlink/satlink.hpp>

#include <gygax/core/charconv.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <numbers>
#include <tuple>

namespace gygax::satlink {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kX2o3 = 2.0 / 3.0;
constexpr double kMinutesPerDay = 1440.0;
constexpr double kSecondsPerDay = 86400.0;
constexpr double kUnixEpochJulianDate = 2440587.5;
constexpr double kSgp4EpochJulianDate = 2433281.5; // 0 January 1950, SGP4's day zero
constexpr double kDeepSpacePeriodMinutes = 225.0;

struct GravityModel {
    double tumin, mu, radiusearthkm, xke, j2, j3, j4, j3oj2;
};

GravityModel gravityModel(Gravity which) {
    GravityModel g{};
    if (which == Gravity::Wgs84) {
        g.mu = 398600.5;
        g.radiusearthkm = 6378.137;
        g.j2 = 0.00108262998905;
        g.j3 = -0.00000253215306;
        g.j4 = -0.00000161098761;
    } else {
        g.mu = 398600.8;
        g.radiusearthkm = 6378.135;
        g.j2 = 0.001082616;
        g.j3 = -0.00000253881;
        g.j4 = -0.00000165597;
    }
    g.xke = 60.0 / std::sqrt(g.radiusearthkm * g.radiusearthkm * g.radiusearthkm / g.mu);
    g.tumin = 1.0 / g.xke;
    g.j3oj2 = g.j3 / g.j2;
    return g;
}

} // namespace

// The initialised element record: Vallado's elsetrec, less what only the TLE reader needs.
struct Sgp4::Elements {
    GravityModel grav{};
    bool deep{false};
    int isimp{0};
    double aycof{}, con41{}, cc1{}, cc4{}, cc5{}, d2{}, d3{}, d4{}, delmo{}, eta{}, argpdot{}, omgcof{}, sinmao{};
    double t2cof{}, t3cof{}, t4cof{}, t5cof{}, x1mth2{}, x7thm1{}, mdot{}, nodedot{}, xlcof{}, xmcof{}, nodecf{};
    // Deep space.
    int irez{0};
    double d2201{}, d2211{}, d3210{}, d3222{}, d4410{}, d4422{}, d5220{}, d5232{}, d5421{}, d5433{};
    double dedt{}, del1{}, del2{}, del3{}, didt{}, dmdt{}, dnodt{}, domdt{};
    double e3{}, ee2{}, peo{}, pgho{}, pho{}, pinco{}, plo{}, se2{}, se3{}, sgh2{}, sgh3{}, sgh4{}, sh2{}, sh3{};
    double si2{}, si3{}, sl2{}, sl3{}, sl4{}, gsto{}, xfact{}, xgh2{}, xgh3{}, xgh4{}, xh2{}, xh3{};
    double xi2{}, xi3{}, xl2{}, xl3{}, xl4{}, xlamo{}, zmol{}, zmos{}, xli{}, xni{};
    // Mean elements at epoch (radians, rad/min).
    double bstar{}, ecco{}, argpo{}, inclo{}, mo{}, no_kozai{}, nodeo{}, no_unkozai{};
};

namespace {

using Elements = Sgp4::Elements;

// Greenwich sidereal time from days since 0 January 1950 plus the 1950 offset (IAU-82).
double gstime(double jdut1) {
    const double tut1 = (jdut1 - 2451545.0) / 36525.0;
    double temp = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 + (876600.0 * 3600 + 8640184.812866) * tut1 + 67310.54841;
    temp = std::fmod(temp * kDegToRad / 240.0, kTwoPi);
    if (temp < 0.0) temp += kTwoPi;
    return temp;
}

// Lunar-solar periodics (dpper). `init` skips the subtraction of the epoch values.
struct Periodics {
    double ep, inclp, nodep, argpp, mp;
};

Periodics dpper(const Elements& s, double t, const Periodics& in) {
    constexpr double zns = 1.19459e-5, zes = 0.01675, znl = 1.5835218e-4, zel = 0.05490;

    double zm = s.zmos + zns * t;
    double zf = zm + 2.0 * zes * std::sin(zm);
    double sinzf = std::sin(zf);
    double f2 = 0.5 * sinzf * sinzf - 0.25;
    double f3 = -0.5 * sinzf * std::cos(zf);
    const double ses = s.se2 * f2 + s.se3 * f3;
    const double sis = s.si2 * f2 + s.si3 * f3;
    const double sls = s.sl2 * f2 + s.sl3 * f3 + s.sl4 * sinzf;
    const double sghs = s.sgh2 * f2 + s.sgh3 * f3 + s.sgh4 * sinzf;
    const double shs = s.sh2 * f2 + s.sh3 * f3;

    zm = s.zmol + znl * t;
    zf = zm + 2.0 * zel * std::sin(zm);
    sinzf = std::sin(zf);
    f2 = 0.5 * sinzf * sinzf - 0.25;
    f3 = -0.5 * sinzf * std::cos(zf);
    const double sel = s.ee2 * f2 + s.e3 * f3;
    const double sil = s.xi2 * f2 + s.xi3 * f3;
    const double sll = s.xl2 * f2 + s.xl3 * f3 + s.xl4 * sinzf;
    const double sghl = s.xgh2 * f2 + s.xgh3 * f3 + s.xgh4 * sinzf;
    const double shll = s.xh2 * f2 + s.xh3 * f3;

    const double pe = ses + sel - s.peo;
    const double pinc = sis + sil - s.pinco;
    const double pl = sls + sll - s.plo;
    double pgh = sghs + sghl - s.pgho;
    double ph = shs + shll - s.pho;

    Periodics out = in;
    out.inclp = in.inclp + pinc;
    out.ep = in.ep + pe;
    const double sinip = std::sin(out.inclp);
    const double cosip = std::cos(out.inclp);

    if (out.inclp >= 0.2) {
        ph /= sinip;
        pgh -= cosip * ph;
        out.argpp += pgh;
        out.nodep += ph;
        out.mp += pl;
    } else {
        // Lyddane modification for low inclinations.
        const double sinop = std::sin(out.nodep);
        const double cosop = std::cos(out.nodep);
        double alfdp = sinip * sinop;
        double betdp = sinip * cosop;
        const double dalf = ph * cosop + pinc * cosip * sinop;
        const double dbet = -ph * sinop + pinc * cosip * cosop;
        alfdp += dalf;
        betdp += dbet;
        out.nodep = std::fmod(out.nodep, kTwoPi);
        const double xls = out.mp + out.argpp + pl + pgh + (cosip - pinc * sinip) * out.nodep;
        const double xnoh = out.nodep;
        out.nodep = std::atan2(alfdp, betdp);
        if (std::fabs(xnoh - out.nodep) > kPi) out.nodep += out.nodep < xnoh ? kTwoPi : -kTwoPi;
        out.mp += pl;
        out.argpp = xls - out.mp - cosip * out.nodep;
    }
    return out;
}

// Deep-space common terms (dscom): what dsinit needs, and the lunar-solar coefficients it keeps.
struct Dscom {
    double snodm, cnodm, sinim, cosim, sinomm, cosomm, day, em, emsq, gam, rtemsq;
    double s1, s2, s3, s4, s5, s6, s7, ss1, ss2, ss3, ss4, ss5, ss6, ss7;
    double sz1, sz2, sz3, sz11, sz12, sz13, sz21, sz22, sz23, sz31, sz32, sz33;
    double nm, z1, z2, z3, z11, z12, z13, z21, z22, z23, z31, z32, z33;
};

Dscom dscom(Elements& s, double epoch, double ep, double argpp, double tc, double inclp, double nodep, double np) {
    constexpr double zes = 0.01675, zel = 0.05490, c1ss = 2.9864797e-6, c1l = 4.7968065e-7;
    constexpr double zsinis = 0.39785416, zcosis = 0.91744867, zcosgs = 0.1945905, zsings = -0.98088458;

    Dscom d{};
    d.nm = np;
    d.em = ep;
    d.snodm = std::sin(nodep);
    d.cnodm = std::cos(nodep);
    d.sinomm = std::sin(argpp);
    d.cosomm = std::cos(argpp);
    d.sinim = std::sin(inclp);
    d.cosim = std::cos(inclp);
    d.emsq = d.em * d.em;
    const double betasq = 1.0 - d.emsq;
    d.rtemsq = std::sqrt(betasq);

    s.peo = s.pinco = s.plo = s.pgho = s.pho = 0.0;
    d.day = epoch + 18261.5 + tc / kMinutesPerDay;
    const double xnodce = std::fmod(4.5236020 - 9.2422029e-4 * d.day, kTwoPi);
    const double stem = std::sin(xnodce);
    const double ctem = std::cos(xnodce);
    const double zcosil = 0.91375164 - 0.03568096 * ctem;
    const double zsinil = std::sqrt(1.0 - zcosil * zcosil);
    const double zsinhl = 0.089683511 * stem / zsinil;
    const double zcoshl = std::sqrt(1.0 - zsinhl * zsinhl);
    d.gam = 5.8351514 + 0.0019443680 * d.day;
    double zx = 0.39785416 * stem / zsinil;
    const double zy = zcoshl * ctem + 0.91744867 * zsinhl * stem;
    zx = std::atan2(zx, zy);
    zx = d.gam + zx - xnodce;
    const double zcosgl = std::cos(zx);
    const double zsingl = std::sin(zx);

    // First pass: the Sun; second pass: the Moon.
    double zcosg = zcosgs, zsing = zsings, zcosi = zcosis, zsini = zsinis;
    double zcosh = d.cnodm, zsinh = d.snodm, cc = c1ss;
    const double xnoi = 1.0 / d.nm;

    for (int lsflg = 1; lsflg <= 2; ++lsflg) {
        const double a1 = zcosg * zcosh + zsing * zcosi * zsinh;
        const double a3 = -zsing * zcosh + zcosg * zcosi * zsinh;
        const double a7 = -zcosg * zsinh + zsing * zcosi * zcosh;
        const double a8 = zsing * zsini;
        const double a9 = zsing * zsinh + zcosg * zcosi * zcosh;
        const double a10 = zcosg * zsini;
        const double a2 = d.cosim * a7 + d.sinim * a8;
        const double a4 = d.cosim * a9 + d.sinim * a10;
        const double a5 = -d.sinim * a7 + d.cosim * a8;
        const double a6 = -d.sinim * a9 + d.cosim * a10;

        const double x1 = a1 * d.cosomm + a2 * d.sinomm;
        const double x2 = a3 * d.cosomm + a4 * d.sinomm;
        const double x3 = -a1 * d.sinomm + a2 * d.cosomm;
        const double x4 = -a3 * d.sinomm + a4 * d.cosomm;
        const double x5 = a5 * d.sinomm;
        const double x6 = a6 * d.sinomm;
        const double x7 = a5 * d.cosomm;
        const double x8 = a6 * d.cosomm;

        d.z31 = 12.0 * x1 * x1 - 3.0 * x3 * x3;
        d.z32 = 24.0 * x1 * x2 - 6.0 * x3 * x4;
        d.z33 = 12.0 * x2 * x2 - 3.0 * x4 * x4;
        d.z1 = 3.0 * (a1 * a1 + a2 * a2) + d.z31 * d.emsq;
        d.z2 = 6.0 * (a1 * a3 + a2 * a4) + d.z32 * d.emsq;
        d.z3 = 3.0 * (a3 * a3 + a4 * a4) + d.z33 * d.emsq;
        d.z11 = -6.0 * a1 * a5 + d.emsq * (-24.0 * x1 * x7 - 6.0 * x3 * x5);
        d.z12 = -6.0 * (a1 * a6 + a3 * a5) + d.emsq * (-24.0 * (x2 * x7 + x1 * x8) - 6.0 * (x3 * x6 + x4 * x5));
        d.z13 = -6.0 * a3 * a6 + d.emsq * (-24.0 * x2 * x8 - 6.0 * x4 * x6);
        d.z21 = 6.0 * a2 * a5 + d.emsq * (24.0 * x1 * x5 - 6.0 * x3 * x7);
        d.z22 = 6.0 * (a4 * a5 + a2 * a6) + d.emsq * (24.0 * (x2 * x5 + x1 * x6) - 6.0 * (x4 * x7 + x3 * x8));
        d.z23 = 6.0 * a4 * a6 + d.emsq * (24.0 * x2 * x6 - 6.0 * x4 * x8);
        d.z1 = d.z1 + d.z1 + betasq * d.z31;
        d.z2 = d.z2 + d.z2 + betasq * d.z32;
        d.z3 = d.z3 + d.z3 + betasq * d.z33;
        d.s3 = cc * xnoi;
        d.s2 = -0.5 * d.s3 / d.rtemsq;
        d.s4 = d.s3 * d.rtemsq;
        d.s1 = -15.0 * d.em * d.s4;
        d.s5 = x1 * x3 + x2 * x4;
        d.s6 = x2 * x3 + x1 * x4;
        d.s7 = x2 * x4 - x1 * x3;

        if (lsflg == 1) {
            d.ss1 = d.s1, d.ss2 = d.s2, d.ss3 = d.s3, d.ss4 = d.s4, d.ss5 = d.s5, d.ss6 = d.s6, d.ss7 = d.s7;
            d.sz1 = d.z1, d.sz2 = d.z2, d.sz3 = d.z3;
            d.sz11 = d.z11, d.sz12 = d.z12, d.sz13 = d.z13;
            d.sz21 = d.z21, d.sz22 = d.z22, d.sz23 = d.z23;
            d.sz31 = d.z31, d.sz32 = d.z32, d.sz33 = d.z33;
            zcosg = zcosgl;
            zsing = zsingl;
            zcosi = zcosil;
            zsini = zsinil;
            zcosh = zcoshl * d.cnodm + zsinhl * d.snodm;
            zsinh = d.snodm * zcoshl - d.cnodm * zsinhl;
            cc = c1l;
        }
    }

    s.zmol = std::fmod(4.7199672 + 0.22997150 * d.day - d.gam, kTwoPi);
    s.zmos = std::fmod(6.2565837 + 0.017201977 * d.day, kTwoPi);

    // Solar terms.
    s.se2 = 2.0 * d.ss1 * d.ss6;
    s.se3 = 2.0 * d.ss1 * d.ss7;
    s.si2 = 2.0 * d.ss2 * d.sz12;
    s.si3 = 2.0 * d.ss2 * (d.sz13 - d.sz11);
    s.sl2 = -2.0 * d.ss3 * d.sz2;
    s.sl3 = -2.0 * d.ss3 * (d.sz3 - d.sz1);
    s.sl4 = -2.0 * d.ss3 * (-21.0 - 9.0 * d.emsq) * zes;
    s.sgh2 = 2.0 * d.ss4 * d.sz32;
    s.sgh3 = 2.0 * d.ss4 * (d.sz33 - d.sz31);
    s.sgh4 = -18.0 * d.ss4 * zes;
    s.sh2 = -2.0 * d.ss2 * d.sz22;
    s.sh3 = -2.0 * d.ss2 * (d.sz23 - d.sz21);

    // Lunar terms.
    s.ee2 = 2.0 * d.s1 * d.s6;
    s.e3 = 2.0 * d.s1 * d.s7;
    s.xi2 = 2.0 * d.s2 * d.z12;
    s.xi3 = 2.0 * d.s2 * (d.z13 - d.z11);
    s.xl2 = -2.0 * d.s3 * d.z2;
    s.xl3 = -2.0 * d.s3 * (d.z3 - d.z1);
    s.xl4 = -2.0 * d.s3 * (-21.0 - 9.0 * d.emsq) * zel;
    s.xgh2 = 2.0 * d.s4 * d.z32;
    s.xgh3 = 2.0 * d.s4 * (d.z33 - d.z31);
    s.xgh4 = -18.0 * d.s4 * zel;
    s.xh2 = -2.0 * d.s2 * d.z22;
    s.xh3 = -2.0 * d.s2 * (d.z23 - d.z21);
    return d;
}

constexpr double kRptim = 4.37526908801129966e-3; // Earth rotation, rad/min

// Deep-space initialisation (dsinit): secular lunar-solar rates and the resonance terms for
// 12-hour (irez 2) and 24-hour (irez 1) orbits.
void dsinit(Elements& s, const Dscom& d, double xpidot, double eccsq, double inclm) {
    constexpr double q22 = 1.7891679e-6, q31 = 2.1460748e-6, q33 = 2.2123015e-7;
    constexpr double root22 = 1.7891679e-6, root44 = 7.3636953e-9, root54 = 2.1765803e-9;
    constexpr double root32 = 3.7393792e-7, root52 = 1.1428639e-7;
    constexpr double znl = 1.5835218e-4, zns = 1.19459e-5;

    const double nm = d.nm;
    double em = d.em;
    double emsq = d.emsq;
    const double cosim = d.cosim, sinim = d.sinim;

    s.irez = 0;
    if (nm > 0.0034906585 && nm < 0.0052359877) s.irez = 1;
    if (nm >= 8.26e-3 && nm <= 9.24e-3 && em >= 0.5) s.irez = 2;

    // Solar terms.
    const double ses = d.ss1 * zns * d.ss5;
    const double sis = d.ss2 * zns * (d.sz11 + d.sz13);
    const double sls = -zns * d.ss3 * (d.sz1 + d.sz3 - 14.0 - 6.0 * emsq);
    const double sghs = d.ss4 * zns * (d.sz31 + d.sz33 - 6.0);
    double shs = -zns * d.ss2 * (d.sz21 + d.sz23);
    if (inclm < 5.2359877e-2 || inclm > kPi - 5.2359877e-2) shs = 0.0;
    if (sinim != 0.0) shs /= sinim;
    const double sgs = sghs - cosim * shs;

    // Lunar terms.
    s.dedt = ses + d.s1 * znl * d.s5;
    s.didt = sis + d.s2 * znl * (d.z11 + d.z13);
    s.dmdt = sls - znl * d.s3 * (d.z1 + d.z3 - 14.0 - 6.0 * emsq);
    const double sghl = d.s4 * znl * (d.z31 + d.z33 - 6.0);
    double shll = -znl * d.s2 * (d.z21 + d.z23);
    if (inclm < 5.2359877e-2 || inclm > kPi - 5.2359877e-2) shll = 0.0;
    s.domdt = sgs + sghl;
    s.dnodt = shs;
    if (sinim != 0.0) {
        s.domdt -= cosim / sinim * shll;
        s.dnodt += shll / sinim;
    }

    const double theta = std::fmod(s.gsto, kTwoPi); // tc = 0 at initialisation
    if (s.irez == 0) return;

    const double aonv = std::pow(nm / s.grav.xke, kX2o3);

    if (s.irez == 2) {
        // Geopotential resonance for 12-hour orbits.
        const double cosisq = cosim * cosim;
        em = s.ecco;
        emsq = eccsq;
        const double eoc = em * emsq;
        const double g201 = -0.306 - (em - 0.64) * 0.440;
        double g211, g310, g322, g410, g422, g520, g521, g532, g533;
        if (em <= 0.65) {
            g211 = 3.616 - 13.2470 * em + 16.2900 * emsq;
            g310 = -19.302 + 117.3900 * em - 228.4190 * emsq + 156.5910 * eoc;
            g322 = -18.9068 + 109.7927 * em - 214.6334 * emsq + 146.5816 * eoc;
            g410 = -41.122 + 242.6940 * em - 471.0940 * emsq + 313.9530 * eoc;
            g422 = -146.407 + 841.8800 * em - 1629.014 * emsq + 1083.4350 * eoc;
            g520 = -532.114 + 3017.977 * em - 5740.032 * emsq + 3708.2760 * eoc;
        } else {
            g211 = -72.099 + 331.819 * em - 508.738 * emsq + 266.724 * eoc;
            g310 = -346.844 + 1582.851 * em - 2415.925 * emsq + 1246.113 * eoc;
            g322 = -342.585 + 1554.908 * em - 2366.899 * emsq + 1215.972 * eoc;
            g410 = -1052.797 + 4758.686 * em - 7193.992 * emsq + 3651.957 * eoc;
            g422 = -3581.690 + 16178.110 * em - 24462.770 * emsq + 12422.520 * eoc;
            g520 = em > 0.715 ? -5149.66 + 29936.92 * em - 54087.36 * emsq + 31324.56 * eoc : 1464.74 - 4664.75 * em + 3763.64 * emsq;
        }
        if (em < 0.7) {
            g533 = -919.22770 + 4988.6100 * em - 9064.7700 * emsq + 5542.21 * eoc;
            g521 = -822.71072 + 4568.6173 * em - 8491.4146 * emsq + 5337.524 * eoc;
            g532 = -853.66600 + 4690.2500 * em - 8624.7700 * emsq + 5341.4 * eoc;
        } else {
            g533 = -37995.780 + 161616.52 * em - 229838.20 * emsq + 109377.94 * eoc;
            g521 = -51752.104 + 218913.95 * em - 309468.16 * emsq + 146349.42 * eoc;
            g532 = -40023.880 + 170470.89 * em - 242699.48 * emsq + 115605.82 * eoc;
        }

        const double sini2 = sinim * sinim;
        const double f220 = 0.75 * (1.0 + 2.0 * cosim + cosisq);
        const double f221 = 1.5 * sini2;
        const double f321 = 1.875 * sinim * (1.0 - 2.0 * cosim - 3.0 * cosisq);
        const double f322 = -1.875 * sinim * (1.0 + 2.0 * cosim - 3.0 * cosisq);
        const double f441 = 35.0 * sini2 * f220;
        const double f442 = 39.3750 * sini2 * sini2;
        const double f522 =
            9.84375 * sinim * (sini2 * (1.0 - 2.0 * cosim - 5.0 * cosisq) + 0.33333333 * (-2.0 + 4.0 * cosim + 6.0 * cosisq));
        const double f523 =
            sinim * (4.92187512 * sini2 * (-2.0 - 4.0 * cosim + 10.0 * cosisq) + 6.56250012 * (1.0 + 2.0 * cosim - 3.0 * cosisq));
        const double f542 = 29.53125 * sinim * (2.0 - 8.0 * cosim + cosisq * (-12.0 + 8.0 * cosim + 10.0 * cosisq));
        const double f543 = 29.53125 * sinim * (-2.0 - 8.0 * cosim + cosisq * (12.0 + 8.0 * cosim - 10.0 * cosisq));

        const double xno2 = nm * nm;
        const double ainv2 = aonv * aonv;
        double temp1 = 3.0 * xno2 * ainv2;
        double temp = temp1 * root22;
        s.d2201 = temp * f220 * g201;
        s.d2211 = temp * f221 * g211;
        temp1 *= aonv;
        temp = temp1 * root32;
        s.d3210 = temp * f321 * g310;
        s.d3222 = temp * f322 * g322;
        temp1 *= aonv;
        temp = 2.0 * temp1 * root44;
        s.d4410 = temp * f441 * g410;
        s.d4422 = temp * f442 * g422;
        temp1 *= aonv;
        temp = temp1 * root52;
        s.d5220 = temp * f522 * g520;
        s.d5232 = temp * f523 * g532;
        temp = 2.0 * temp1 * root54;
        s.d5421 = temp * f542 * g521;
        s.d5433 = temp * f543 * g533;
        s.xlamo = std::fmod(s.mo + s.nodeo + s.nodeo - theta - theta, kTwoPi);
        s.xfact = s.mdot + s.dmdt + 2.0 * (s.nodedot + s.dnodt - kRptim) - s.no_unkozai;
    } else {
        // Synchronous resonance for 24-hour orbits.
        const double g200 = 1.0 + emsq * (-2.5 + 0.8125 * emsq);
        const double g310 = 1.0 + 2.0 * emsq;
        const double g300 = 1.0 + emsq * (-6.0 + 6.60937 * emsq);
        const double f220 = 0.75 * (1.0 + cosim) * (1.0 + cosim);
        const double f311 = 0.9375 * sinim * sinim * (1.0 + 3.0 * cosim) - 0.75 * (1.0 + cosim);
        double f330 = 1.0 + cosim;
        f330 = 1.875 * f330 * f330 * f330;
        s.del1 = 3.0 * nm * nm * aonv * aonv;
        s.del2 = 2.0 * s.del1 * f220 * g200 * q22;
        s.del3 = 3.0 * s.del1 * f330 * g300 * q33 * aonv;
        s.del1 = s.del1 * f311 * g310 * q31 * aonv;
        s.xlamo = std::fmod(s.mo + s.nodeo + s.argpo - theta, kTwoPi);
        s.xfact = s.mdot + xpidot - kRptim + s.dmdt + s.domdt + s.dnodt - s.no_unkozai;
    }
    s.xli = s.xlamo;
    s.xni = s.no_unkozai;
}

// Deep-space secular effects and the resonance integrator (dspace). The integrator restarts
// from epoch on every call, so propagation stays a pure function of time (the reference code
// caches its last step; restarting reaches the same 720-minute grid and the same answer).
struct Secular {
    double em, argpm, inclm, mm, nodem, nm;
};

Secular dspace(const Elements& s, double t, const Secular& in) {
    constexpr double fasx2 = 0.13130908, fasx4 = 2.8843198, fasx6 = 0.37448087;
    constexpr double g22 = 5.7686396, g32 = 0.95240898, g44 = 1.8014998, g52 = 1.0508330, g54 = 4.4108898;
    constexpr double stepp = 720.0, stepn = -720.0, step2 = 259200.0;

    Secular out = in;
    const double theta = std::fmod(s.gsto + t * kRptim, kTwoPi);
    out.em += s.dedt * t;
    out.inclm += s.didt * t;
    out.argpm += s.domdt * t;
    out.nodem += s.dnodt * t;
    out.mm += s.dmdt * t;

    if (s.irez == 0) return out;

    double atime = 0.0;
    double xni = s.no_unkozai;
    double xli = s.xlamo;
    const double delt = t > 0.0 ? stepp : stepn;
    double ft = 0.0, xndt = 0.0, xldot = 0.0, xnddt = 0.0;

    for (;;) {
        if (s.irez != 2) {
            // Near-synchronous resonance.
            xndt = s.del1 * std::sin(xli - fasx2) + s.del2 * std::sin(2.0 * (xli - fasx4)) + s.del3 * std::sin(3.0 * (xli - fasx6));
            xldot = xni + s.xfact;
            xnddt = s.del1 * std::cos(xli - fasx2) + 2.0 * s.del2 * std::cos(2.0 * (xli - fasx4)) +
                    3.0 * s.del3 * std::cos(3.0 * (xli - fasx6));
            xnddt *= xldot;
        } else {
            // Near half-day resonance.
            const double xomi = s.argpo + s.argpdot * atime;
            const double x2omi = xomi + xomi;
            const double x2li = xli + xli;
            xndt = s.d2201 * std::sin(x2omi + xli - g22) + s.d2211 * std::sin(xli - g22) + s.d3210 * std::sin(xomi + xli - g32) +
                   s.d3222 * std::sin(-xomi + xli - g32) + s.d4410 * std::sin(x2omi + x2li - g44) + s.d4422 * std::sin(x2li - g44) +
                   s.d5220 * std::sin(xomi + xli - g52) + s.d5232 * std::sin(-xomi + xli - g52) + s.d5421 * std::sin(xomi + x2li - g54) +
                   s.d5433 * std::sin(-xomi + x2li - g54);
            xldot = xni + s.xfact;
            xnddt = s.d2201 * std::cos(x2omi + xli - g22) + s.d2211 * std::cos(xli - g22) + s.d3210 * std::cos(xomi + xli - g32) +
                    s.d3222 * std::cos(-xomi + xli - g32) + s.d5220 * std::cos(xomi + xli - g52) + s.d5232 * std::cos(-xomi + xli - g52) +
                    2.0 * (s.d4410 * std::cos(x2omi + x2li - g44) + s.d4422 * std::cos(x2li - g44) + s.d5421 * std::cos(xomi + x2li - g54) +
                           s.d5433 * std::cos(-xomi + x2li - g54));
            xnddt *= xldot;
        }
        if (std::fabs(t - atime) < stepp) {
            ft = t - atime;
            break;
        }
        xli += xldot * delt + xndt * step2;
        xni += xndt * delt + xnddt * step2;
        atime += delt;
    }

    const double nm = xni + xndt * ft + xnddt * ft * ft * 0.5;
    const double xl = xli + xldot * ft + xndt * ft * ft * 0.5;
    out.mm = s.irez != 1 ? xl - 2.0 * out.nodem + 2.0 * theta : xl - out.nodem - out.argpm + theta;
    out.nm = s.no_unkozai + (nm - s.no_unkozai);
    return out;
}

// Mean elements and short-period terms at `t` minutes (sgp4).
StateVector propagateAt(const Elements& s, double t, Sgp4Status& status) {
    constexpr double temp4 = 1.5e-12;
    const auto& g = s.grav;
    const double vkmpersec = g.radiusearthkm * g.xke / 60.0;
    status = Sgp4Status::Ok;

    // Secular gravity and atmospheric drag.
    const double xmdf = s.mo + s.mdot * t;
    const double argpdf = s.argpo + s.argpdot * t;
    const double nodedf = s.nodeo + s.nodedot * t;
    double argpm = argpdf;
    double mm = xmdf;
    const double t2 = t * t;
    double nodem = nodedf + s.nodecf * t2;
    double tempa = 1.0 - s.cc1 * t;
    double tempe = s.bstar * s.cc4 * t;
    double templ = s.t2cof * t2;

    if (s.isimp != 1) {
        const double delomg = s.omgcof * t;
        const double delmtemp = 1.0 + s.eta * std::cos(xmdf);
        const double delm = s.xmcof * (delmtemp * delmtemp * delmtemp - s.delmo);
        const double temp = delomg + delm;
        mm = xmdf + temp;
        argpm = argpdf - temp;
        const double t3 = t2 * t;
        const double t4 = t3 * t;
        tempa = tempa - s.d2 * t2 - s.d3 * t3 - s.d4 * t4;
        tempe = tempe + s.bstar * s.cc5 * (std::sin(mm) - s.sinmao);
        templ = templ + s.t3cof * t3 + t4 * (s.t4cof + t * s.t5cof);
    }

    double nm = s.no_unkozai;
    double em = s.ecco;
    double inclm = s.inclo;
    if (s.deep) {
        const Secular sec = dspace(s, t, {em, argpm, inclm, mm, nodem, nm});
        em = sec.em, argpm = sec.argpm, inclm = sec.inclm, mm = sec.mm, nodem = sec.nodem, nm = sec.nm;
    }

    if (nm <= 0.0) {
        status = Sgp4Status::MeanMotion;
        return {};
    }
    const double am = std::pow(g.xke / nm, kX2o3) * tempa * tempa;
    nm = g.xke / std::pow(am, 1.5);
    em -= tempe;
    if (em >= 1.0 || em < -0.001) {
        status = Sgp4Status::MeanEccentricity;
        return {};
    }
    if (em < 1.0e-6) em = 1.0e-6;
    mm += s.no_unkozai * templ;
    double xlm = mm + argpm + nodem;

    nodem = std::fmod(nodem, kTwoPi);
    argpm = std::fmod(argpm, kTwoPi);
    xlm = std::fmod(xlm, kTwoPi);
    mm = std::fmod(xlm - argpm - nodem, kTwoPi);

    const double sinim = std::sin(inclm);
    const double cosim = std::cos(inclm);

    // Lunar-solar periodics.
    Periodics p{em, inclm, nodem, argpm, mm};
    double sinip = sinim, cosip = cosim;
    double aycof = s.aycof, xlcof = s.xlcof, con41 = s.con41, x1mth2 = s.x1mth2, x7thm1 = s.x7thm1;
    if (s.deep) {
        p = dpper(s, t, p);
        if (p.inclp < 0.0) {
            p.inclp = -p.inclp;
            p.nodep += kPi;
            p.argpp -= kPi;
        }
        if (p.ep < 0.0 || p.ep > 1.0) {
            status = Sgp4Status::PerturbedEccentricity;
            return {};
        }
        sinip = std::sin(p.inclp);
        cosip = std::cos(p.inclp);
        aycof = -0.5 * g.j3oj2 * sinip;
        xlcof = -0.25 * g.j3oj2 * sinip * (3.0 + 5.0 * cosip) / (std::fabs(cosip + 1.0) > 1.5e-12 ? 1.0 + cosip : temp4);
    }

    // Long-period periodics.
    const double axnl = p.ep * std::cos(p.argpp);
    double temp = 1.0 / (am * (1.0 - p.ep * p.ep));
    const double aynl = p.ep * std::sin(p.argpp) + temp * aycof;
    const double xl = p.mp + p.argpp + p.nodep + temp * xlcof * axnl;

    // Kepler's equation in the equinoctial form, with steps limited to 0.95 rad.
    const double u = std::fmod(xl - p.nodep, kTwoPi);
    double eo1 = u, tem5 = 9999.9, sineo1 = 0.0, coseo1 = 0.0;
    for (int ktr = 1; std::fabs(tem5) >= 1.0e-12 && ktr <= 10; ++ktr) {
        sineo1 = std::sin(eo1);
        coseo1 = std::cos(eo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        tem5 = std::clamp(tem5, -0.95, 0.95);
        eo1 += tem5;
    }

    // Short-period preliminaries.
    const double ecose = axnl * coseo1 + aynl * sineo1;
    const double esine = axnl * sineo1 - aynl * coseo1;
    const double el2 = axnl * axnl + aynl * aynl;
    const double pl = am * (1.0 - el2);
    if (pl < 0.0) {
        status = Sgp4Status::SemiLatusRectum;
        return {};
    }
    const double rl = am * (1.0 - ecose);
    const double rdotl = std::sqrt(am) * esine / rl;
    const double rvdotl = std::sqrt(pl) / rl;
    const double betal = std::sqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    const double sinu = am / rl * (sineo1 - aynl - axnl * temp);
    const double cosu = am / rl * (coseo1 - axnl + aynl * temp);
    double su = std::atan2(sinu, cosu);
    const double sin2u = (cosu + cosu) * sinu;
    const double cos2u = 1.0 - 2.0 * sinu * sinu;
    temp = 1.0 / pl;
    const double temp1 = 0.5 * g.j2 * temp;
    const double temp2 = temp1 * temp;

    // Short-period periodics.
    if (s.deep) {
        const double cosisq = cosip * cosip;
        con41 = 3.0 * cosisq - 1.0;
        x1mth2 = 1.0 - cosisq;
        x7thm1 = 7.0 * cosisq - 1.0;
    }
    const double mrt = rl * (1.0 - 1.5 * temp2 * betal * con41) + 0.5 * temp1 * x1mth2 * cos2u;
    su -= 0.25 * temp2 * x7thm1 * sin2u;
    const double xnode = p.nodep + 1.5 * temp2 * cosip * sin2u;
    const double xinc = p.inclp + 1.5 * temp2 * cosip * sinip * cos2u;
    const double mvt = rdotl - nm * temp1 * x1mth2 * sin2u / g.xke;
    const double rvdot = rvdotl + nm * temp1 * (x1mth2 * cos2u + 1.5 * con41) / g.xke;

    // Orientation vectors.
    const double sinsu = std::sin(su), cossu = std::cos(su);
    const double snod = std::sin(xnode), cnod = std::cos(xnode);
    const double sini = std::sin(xinc), cosi = std::cos(xinc);
    const double xmx = -snod * cosi;
    const double xmy = cnod * cosi;
    const Vector3 uv{xmx * sinsu + cnod * cossu, xmy * sinsu + snod * cossu, sini * sinsu};
    const Vector3 vv{xmx * cossu - cnod * sinsu, xmy * cossu - snod * sinsu, sini * cossu};

    if (mrt < 1.0) status = Sgp4Status::Decayed;
    return {uv * (mrt * g.radiusearthkm), (uv * mvt + vv * rvdot) * vkmpersec};
}

// Initialisation (initl + sgp4init). `epoch` is days since 0 January 1950.
void initialise(Elements& s, double epoch) {
    constexpr double temp4 = 1.5e-12;
    const auto& g = s.grav;
    const double ss = 78.0 / g.radiusearthkm + 1.0;
    const double qzms2ttemp = (120.0 - 78.0) / g.radiusearthkm;
    const double qzms2t = qzms2ttemp * qzms2ttemp * qzms2ttemp * qzms2ttemp;

    // initl: un-Kozai the mean motion and derive the epoch quantities.
    const double eccsq = s.ecco * s.ecco;
    const double omeosq = 1.0 - eccsq;
    const double rteosq = std::sqrt(omeosq);
    const double cosio = std::cos(s.inclo);
    const double cosio2 = cosio * cosio;
    const double ak = std::pow(g.xke / s.no_kozai, kX2o3);
    const double d1 = 0.75 * g.j2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
    double del = d1 / (ak * ak);
    const double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
    del = d1 / (adel * adel);
    s.no_unkozai = s.no_kozai / (1.0 + del);

    const double ao = std::pow(g.xke / s.no_unkozai, kX2o3);
    const double sinio = std::sin(s.inclo);
    const double po = ao * omeosq;
    const double con42 = 1.0 - 5.0 * cosio2;
    s.con41 = -con42 - cosio2 - cosio2;
    const double posq = po * po;
    const double rp = ao * (1.0 - s.ecco);
    s.gsto = gstime(epoch + kSgp4EpochJulianDate);

    if (omeosq >= 0.0 || s.no_unkozai >= 0.0) {
        s.isimp = rp < (220.0 / g.radiusearthkm + 1.0) ? 1 : 0;
        double sfour = ss;
        double qzms24 = qzms2t;
        const double perige = (rp - 1.0) * g.radiusearthkm;

        // For perigees below 156 km, s and qoms2t are altered.
        if (perige < 156.0) {
            sfour = perige < 98.0 ? 20.0 : perige - 78.0;
            const double qzms24temp = (120.0 - sfour) / g.radiusearthkm;
            qzms24 = qzms24temp * qzms24temp * qzms24temp * qzms24temp;
            sfour = sfour / g.radiusearthkm + 1.0;
        }
        const double pinvsq = 1.0 / posq;

        const double tsi = 1.0 / (ao - sfour);
        s.eta = ao * s.ecco * tsi;
        const double etasq = s.eta * s.eta;
        const double eeta = s.ecco * s.eta;
        const double psisq = std::fabs(1.0 - etasq);
        const double coef = qzms24 * std::pow(tsi, 4.0);
        const double coef1 = coef / std::pow(psisq, 3.5);
        const double cc2 =
            coef1 * s.no_unkozai *
            (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) + 0.375 * g.j2 * tsi / psisq * s.con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
        s.cc1 = s.bstar * cc2;
        const double cc3 = s.ecco > 1.0e-4 ? -2.0 * coef * tsi * g.j3oj2 * s.no_unkozai * sinio / s.ecco : 0.0;
        s.x1mth2 = 1.0 - cosio2;
        s.cc4 = 2.0 * s.no_unkozai * coef1 * ao * omeosq *
                (s.eta * (2.0 + 0.5 * etasq) + s.ecco * (0.5 + 2.0 * etasq) -
                 g.j2 * tsi / (ao * psisq) *
                     (-3.0 * s.con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
                      0.75 * s.x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * std::cos(2.0 * s.argpo)));
        s.cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
        const double cosio4 = cosio2 * cosio2;
        const double temp1 = 1.5 * g.j2 * pinvsq * s.no_unkozai;
        const double temp2 = 0.5 * temp1 * g.j2 * pinvsq;
        const double temp3 = -0.46875 * g.j4 * pinvsq * pinvsq * s.no_unkozai;
        s.mdot = s.no_unkozai + 0.5 * temp1 * rteosq * s.con41 + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
        s.argpdot =
            -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) + temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
        const double xhdot1 = -temp1 * cosio;
        s.nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
        const double xpidot = s.argpdot + s.nodedot;
        s.omgcof = s.bstar * cc3 * std::cos(s.argpo);
        s.xmcof = s.ecco > 1.0e-4 ? -kX2o3 * coef * s.bstar / eeta : 0.0;
        s.nodecf = 3.5 * omeosq * xhdot1 * s.cc1;
        s.t2cof = 1.5 * s.cc1;
        s.xlcof = -0.25 * g.j3oj2 * sinio * (3.0 + 5.0 * cosio) / (std::fabs(cosio + 1.0) > 1.5e-12 ? 1.0 + cosio : temp4);
        s.aycof = -0.5 * g.j3oj2 * sinio;
        const double delmotemp = 1.0 + s.eta * std::cos(s.mo);
        s.delmo = delmotemp * delmotemp * delmotemp;
        s.sinmao = std::sin(s.mo);
        s.x7thm1 = 7.0 * cosio2 - 1.0;

        // Deep space.
        if (kTwoPi / s.no_unkozai >= kDeepSpacePeriodMinutes) {
            s.deep = true;
            s.isimp = 1;
            const Dscom d = dscom(s, epoch, s.ecco, s.argpo, 0.0, s.inclo, s.nodeo, s.no_unkozai);
            dsinit(s, d, xpidot, eccsq, s.inclo);
        }

        // Near-Earth drag terms.
        if (s.isimp != 1) {
            const double cc1sq = s.cc1 * s.cc1;
            s.d2 = 4.0 * ao * tsi * cc1sq;
            const double temp = s.d2 * tsi * s.cc1 / 3.0;
            s.d3 = (17.0 * ao + sfour) * temp;
            s.d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s.cc1;
            s.t3cof = s.d2 + 2.0 * cc1sq;
            s.t4cof = 0.25 * (3.0 * s.d3 + s.cc1 * (12.0 * s.d2 + 10.0 * cc1sq));
            s.t5cof = 0.2 * (3.0 * s.d4 + 12.0 * s.cc1 * s.d3 + 6.0 * s.d2 * s.d2 + 15.0 * cc1sq * (2.0 * s.d2 + cc1sq));
        }
    }
}

// --- TLE fields ---------------------------------------------------------------------------------

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

class FieldReader {
public:
    FieldReader(std::string_view line, int number) : line_(line), number_(number) {}

    [[noreturn]] void fail(std::size_t first, std::size_t last, std::string_view what) const {
        const auto shown = first < line_.size() ? line_.substr(first, last - first + 1) : std::string_view{};
        throw TleError(std::format("TLE line {}, columns {}-{} ({}): cannot read \"{}\"", number_, first + 1, last + 1, what, shown));
    }

    // Columns are 1-based and inclusive, as the format is documented.
    [[nodiscard]] std::string_view text(std::size_t first, std::size_t last) const {
        if (line_.size() < last) fail(first - 1, last - 1, "missing");
        return line_.substr(first - 1, last - first + 1);
    }

    [[nodiscard]] double real(std::size_t first, std::size_t last, std::string_view what) const {
        auto t = trim(text(first, last));
        if (!t.empty() && t.front() == '+') t.remove_prefix(1);
        double v = 0.0;
        const auto [end, ec] = fromChars(t.data(), t.data() + t.size(), v);
        if (t.empty() || ec != std::errc() || end != t.data() + t.size() || !std::isfinite(v)) fail(first - 1, last - 1, what);
        return v;
    }

    [[nodiscard]] long integer(std::size_t first, std::size_t last, std::string_view what) const {
        const auto t = trim(text(first, last));
        long v = 0;
        const auto [end, ec] = fromChars(t.data(), t.data() + t.size(), v);
        if (t.empty() || ec != std::errc() || end != t.data() + t.size()) fail(first - 1, last - 1, what);
        return v;
    }

    // An assumed-decimal-point mantissa with a signed exponent, e.g. " 12345-4" = 0.12345e-4.
    [[nodiscard]] double exponential(std::size_t first, std::size_t last, std::string_view what) const {
        const auto t = text(first, last);
        const char sign = t[0];
        const auto mantissa = t.substr(1, t.size() - 3);
        const auto exponent = t.substr(t.size() - 2);
        if ((sign != ' ' && sign != '+' && sign != '-') || (exponent[0] != '+' && exponent[0] != '-' && exponent[0] != ' ') ||
            exponent[1] < '0' || exponent[1] > '9')
            fail(first - 1, last - 1, what);
        // The decimal point sits before the first mantissa column, so a blank column is a zero digit.
        double digits = 0.0;
        for (const char c : mantissa) {
            if (c != ' ' && (c < '0' || c > '9')) fail(first - 1, last - 1, what);
            digits = digits * 10.0 + (c == ' ' ? 0 : c - '0');
        }
        const int power = (exponent[1] - '0') * (exponent[0] == '-' ? -1 : 1);
        const double value = digits * std::pow(10.0, power - static_cast<int>(mantissa.size()));
        return sign == '-' ? -value : value;
    }

    void expect(std::size_t column, char c, std::string_view what) const {
        if (line_.size() < column || line_[column - 1] != c) fail(column - 1, column - 1, what);
    }

private:
    std::string_view line_;
    int number_;
};

void verifyChecksum(std::string_view line, int number) {
    if (line.size() < 69 || line[68] < '0' || line[68] > '9') return;
    const int expected = line[68] - '0';
    const int actual = tleChecksum(line);
    if (expected != actual)
        throw TleError(
            std::format("TLE line {}: checksum is {} but the line adds up to {} (corrupted or hand-edited?)", number, expected, actual));
}

} // namespace

// --- Public API ---------------------------------------------------------------------------------

int tleChecksum(std::string_view line) noexcept {
    int sum = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>(line.size(), 68); ++i) {
        if (line[i] >= '0' && line[i] <= '9') sum += line[i] - '0';
        if (line[i] == '-') sum += 1;
    }
    return sum % 10;
}

Tle Tle::parse(std::string_view line1, std::string_view line2, std::string_view name) {
    line1 = trim(line1);
    line2 = trim(line2);
    // Some sources prefix the title with "0 " (three-line format).
    name = trim(name);
    if (name.starts_with("0 ")) name.remove_prefix(2);

    const FieldReader a(line1, 1);
    const FieldReader b(line2, 2);
    a.expect(1, '1', "line number");
    b.expect(1, '2', "line number");
    for (const auto& [line, number, needed] : {std::tuple{line1, 1, 64}, std::tuple{line2, 2, 63}}) {
        if (line.size() < static_cast<std::size_t>(needed))
            throw TleError(std::format("TLE line {} has {} characters; it needs at least {}", number, line.size(), needed));
    }
    verifyChecksum(line1, 1);
    verifyChecksum(line2, 2);

    Tle t;
    t.name = std::string(name);
    t.catalog = std::string(trim(a.text(3, 7)));
    if (trim(b.text(3, 7)) != t.catalog)
        throw TleError(std::format("TLE lines disagree on the catalog number: \"{}\" and \"{}\"", t.catalog, trim(b.text(3, 7))));
    if (t.catalog.empty()) a.fail(2, 6, "catalog number");
    t.classification = a.text(8, 8)[0] == ' ' ? 'U' : a.text(8, 8)[0];
    t.international_designator = std::string(trim(a.text(10, 17)));
    const long yy = a.integer(19, 20, "epoch year");
    t.epoch_year = static_cast<int>(yy < 57 ? 2000 + yy : 1900 + yy);
    t.epoch_day = a.real(21, 32, "epoch day");
    if (t.epoch_day < 1.0 || t.epoch_day >= 367.0) a.fail(20, 31, "epoch day");
    t.mean_motion_dot = a.real(34, 43, "mean motion derivative");
    t.mean_motion_ddot = a.exponential(45, 52, "mean motion second derivative");
    t.bstar = a.exponential(54, 61, "B* drag term");
    t.element_set = line1.size() >= 68 && !trim(a.text(65, 68)).empty() ? static_cast<int>(a.integer(65, 68, "element set")) : 0;

    t.inclination_deg = b.real(9, 16, "inclination");
    t.raan_deg = b.real(18, 25, "right ascension of the ascending node");
    {
        std::string ecc = "0.";
        for (const char c : b.text(27, 33)) ecc.push_back(c == ' ' ? '0' : c);
        double e = 0.0;
        const auto [end, ec] = fromChars(ecc.data(), ecc.data() + ecc.size(), e);
        if (ec != std::errc() || end != ecc.data() + ecc.size()) b.fail(26, 32, "eccentricity");
        t.eccentricity = e;
    }
    t.arg_perigee_deg = b.real(35, 42, "argument of perigee");
    t.mean_anomaly_deg = b.real(44, 51, "mean anomaly");
    t.mean_motion_rev_day = b.real(53, 63, "mean motion");
    if (line2.size() >= 68 && !trim(b.text(64, 68)).empty()) t.revolution = b.integer(64, 68, "revolution number");

    if (t.inclination_deg < 0.0 || t.inclination_deg > 180.0) b.fail(8, 15, "inclination");
    if (t.mean_motion_rev_day <= 0.0) b.fail(52, 62, "mean motion");
    return t;
}

double Tle::epochUnixSeconds() const noexcept {
    using namespace std::chrono;
    const auto jan1 = sys_days{year{epoch_year} / January / 1};
    return static_cast<double>(jan1.time_since_epoch().count()) * kSecondsPerDay + (epoch_day - 1.0) * kSecondsPerDay;
}

double Tle::epochJulianDate() const noexcept {
    // Vallado's jday() for 0 January of the epoch year, plus the day of year.
    const double y = epoch_year;
    return 367.0 * y - std::floor(7.0 * y * 0.25) + 30.0 + 1721013.5 + epoch_day;
}

double Tle::periodMinutes() const noexcept {
    return kMinutesPerDay / mean_motion_rev_day;
}

std::string_view describe(Sgp4Status status) noexcept {
    switch (status) {
    case Sgp4Status::Ok: return "ok";
    case Sgp4Status::MeanEccentricity: return "mean eccentricity is outside [0, 1)";
    case Sgp4Status::MeanMotion: return "mean motion is not positive";
    case Sgp4Status::PerturbedEccentricity: return "perturbed eccentricity is outside [0, 1]";
    case Sgp4Status::SemiLatusRectum: return "semi-latus rectum is negative";
    case Sgp4Status::Decayed: return "the orbit has decayed below the Earth's surface";
    }
    return "unknown SGP4 error";
}

Sgp4Error::Sgp4Error(Sgp4Status status, double minutes)
    : std::runtime_error(std::format("SGP4 at {:+.3f} min from epoch: {} (error {})", minutes, describe(status), static_cast<int>(status))),
      status_(status) {}

Sgp4::Sgp4(const Tle& tle, Gravity gravity) : e_(std::make_unique<Elements>()) {
    constexpr double xpdotp = kMinutesPerDay / kTwoPi; // rev/day -> rad/min
    auto& s = *e_;
    s.grav = gravityModel(gravity);
    s.bstar = tle.bstar;
    s.ecco = tle.eccentricity;
    s.argpo = tle.arg_perigee_deg * kDegToRad;
    s.inclo = tle.inclination_deg * kDegToRad;
    s.mo = tle.mean_anomaly_deg * kDegToRad;
    s.no_kozai = tle.mean_motion_rev_day / xpdotp;
    s.nodeo = tle.raan_deg * kDegToRad;
    if (s.ecco >= 1.0 || s.no_kozai <= 0.0) throw TleError("TLE describes no closed orbit (eccentricity >= 1 or mean motion <= 0)");
    initialise(s, tle.epochJulianDate() - kSgp4EpochJulianDate);
}

Sgp4::~Sgp4() = default;
Sgp4::Sgp4(const Sgp4& other) : e_(std::make_unique<Elements>(*other.e_)) {}
Sgp4& Sgp4::operator=(const Sgp4& other) {
    if (this != &other) e_ = std::make_unique<Elements>(*other.e_);
    return *this;
}
Sgp4::Sgp4(Sgp4&&) noexcept = default;
Sgp4& Sgp4::operator=(Sgp4&&) noexcept = default;

bool Sgp4::deepSpace() const noexcept {
    return e_->deep;
}

StateVector Sgp4::propagate(double minutes) const {
    Sgp4Status status = Sgp4Status::Ok;
    const StateVector state = propagateAt(*e_, minutes, status);
    if (status != Sgp4Status::Ok) throw Sgp4Error(status, minutes);
    return state;
}

double unixToJulianDate(double unix_seconds) noexcept {
    return kUnixEpochJulianDate + unix_seconds / kSecondsPerDay;
}

double julianDateToUnix(double julian_date) noexcept {
    return (julian_date - kUnixEpochJulianDate) * kSecondsPerDay;
}

double greenwichSiderealRad(double unix_seconds) noexcept {
    // gstime() from Unix seconds without forming a Julian date: near 2.46e6 a double resolves only
    // about 40 microseconds, which would jitter the Earth-fixed frame by a couple of centimetres.
    // Its (876600 h + 8640184.812866 s) * T term is 86400 s per day plus the precession term, and
    // whole days are whole turns, so only the day's fraction is kept.
    constexpr double kJ2000Unix = 946728000.0; // 2000-01-01 12:00:00 UTC
    const double days = (unix_seconds - kJ2000Unix) / kSecondsPerDay;
    const double tut1 = days / 36525.0;
    const double seconds = 67310.54841 + kSecondsPerDay * (days - std::floor(days)) + 8640184.812866 * tut1 + 0.093104 * tut1 * tut1 -
                           6.2e-6 * tut1 * tut1 * tut1;
    double theta = std::fmod(seconds * kDegToRad / 240.0, kTwoPi);
    if (theta < 0.0) theta += kTwoPi;
    return theta;
}

} // namespace gygax::satlink
