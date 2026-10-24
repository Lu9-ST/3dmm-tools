// Small affine-transform helpers shared by the exporter and the stock-actor retargeting.
#ifndef DMM_GEOM_H
#define DMM_GEOM_H

#include "actor.h"

#include <cmath>

namespace dmm
{

// Affine map, column convention: out = r * p + t
struct Aff
{
    double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double t[3] = {0, 0, 0};
};

inline Aff Mul(const Aff &a, const Aff &b) // a after b
{
    Aff o;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
            o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
        o.t[i] = a.r[i][0] * b.t[0] + a.r[i][1] * b.t[1] + a.r[i][2] * b.t[2] + a.t[i];
    }
    return o;
}

inline Aff Inv(const Aff &a)
{
    const double(*m)[3] = a.r;
    double c[3][3];
    c[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    c[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    c[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    c[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    c[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    c[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    c[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    c[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    c[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    double det = m[0][0] * c[0][0] + m[0][1] * c[1][0] + m[0][2] * c[2][0];
    Aff o;
    if (std::fabs(det) < 1e-12)
        return o;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            o.r[i][j] = c[i][j] / det;
    for (int i = 0; i < 3; i++)
        o.t[i] = -(o.r[i][0] * a.t[0] + o.r[i][1] * a.t[1] + o.r[i][2] * a.t[2]);
    return o;
}

template <class F> inline Aff AffOf(F f) // from a point map (applied to the origin and the three unit vectors)
{
    Aff o;
    double p[3] = {0, 0, 0}, q[3];
    f(p, q);
    for (int i = 0; i < 3; i++)
        o.t[i] = q[i];
    for (int k = 0; k < 3; k++)
    {
        double e[3] = {0, 0, 0};
        e[k] = 1;
        f(e, q);
        for (int i = 0; i < 3; i++)
            o.r[i][k] = q[i] - o.t[i];
    }
    return o;
}

inline Mat34 ToMat34(const Aff &a)
{
    Mat34 m;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
            m.r[i][j] = (float)a.r[j][i]; // row-vector convention: p' = p * R + t
        m.t[i] = (float)a.t[i];
    }
    return m;
}


// Mat34 (row-vector convention, p' = p * R + t) -> Aff (column convention)
inline Aff FromMat34(const Mat34 &m)
{
    Aff a;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
            a.r[j][i] = m.r[i][j];
        a.t[i] = m.t[i];
    }
    return a;
}

inline Aff Translation(double x, double y, double z)
{
    Aff a;
    a.t[0] = x;
    a.t[1] = y;
    a.t[2] = z;
    return a;
}

} // namespace dmm

#endif
