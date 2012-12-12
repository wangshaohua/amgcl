#ifndef AMGCL_LEVEL_CPU_HPP
#define AMGCL_LEVEL_CPU_HPP

/*
The MIT License

Copyright (c) 2012 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   level_cpu.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  Level of an AMG hierarchy for use with arrays located in main (CPU) memory.
 */

#include <vector>
#include <array>

#include <amgcl/level_params.hpp>
#include <amgcl/spmat.hpp>

namespace amgcl {

/// Storage schemes for levels of AMG hierarchy.
namespace level {

/** \defgroup levels Level storage backends */

/// CPU-based AMG hierarchy.
/**
 * Level of an AMG hierarchy for use with arrays located in main (CPU) memory.
 * \ingroup levels
 */
struct cpu {

/// Parameters for CPU-based level storage scheme.
struct params
    : public amgcl::level::params
{};

template <typename value_t, typename index_t>
class instance {
    public:
        typedef sparse::matrix<value_t, index_t> matrix;

        // Construct complete multigrid level from system matrix (a),
        // prolongation (p) and restriction (r) operators.
        // The matrices are moved into the local members.
        instance(matrix &&a, matrix &&p, matrix &&r, const params &prm, unsigned nlevel)
            : A(std::move(a)), P(std::move(p)), R(std::move(r))
        {
            if (nlevel) {
                u.resize(A.rows);
                f.resize(A.rows);

                if (prm.kcycle && nlevel % prm.kcycle == 0)
                    for(auto v = cg.begin(); v != cg.end(); ++v)
                        v->resize(A.rows);
            }

            t.resize(A.rows);
        }

        // Construct the coarsest hierarchy level from system matrix (a) and
        // its inverse (ai).
        instance(matrix &&a, matrix &&ai, const params &prm, unsigned nlevel)
            : A(std::move(a)), Ai(std::move(ai)), u(A.rows), f(A.rows), t(A.rows)
        { }

        // Perform one relaxation (smoothing) step.
        template <class vector1, class vector2>
        void relax(const vector1 &rhs, vector2 &x) const {
            const index_t n = A.rows;

#pragma omp parallel for schedule(dynamic, 1024)
            for(index_t i = 0; i < n; ++i) {
                value_t temp = rhs[i];
                value_t diag = 1;

                for(index_t j = A.row[i], e = A.row[i + 1]; j < e; ++j) {
                    index_t c = A.col[j];
                    value_t v = A.val[j];

                    temp -= v * x[c];

                    if (c == i) diag = v;
                }

                t[i] = x[i] + 0.72 * (temp / diag);
            }

            vector_copy(t, x);
        }

        // Compute residual value.
        template <class vector1, class vector2>
        value_t resid(const vector1 &rhs, vector2 &x) const {
            const index_t n = A.rows;
            value_t norm = 0;

#pragma omp parallel for reduction(+:norm) schedule(dynamic, 1024)
            for(index_t i = 0; i < n; ++i) {
                value_t temp = rhs[i];

                for(index_t j = A.row[i], e = A.row[i + 1]; j < e; ++j)
                    temp -= A.val[j] * x[A.col[j]];

                norm += temp * temp;
            }

            return sqrt(norm);
        }

        // Perform one V-cycle. Coarser levels are cycled recursively. The
        // coarsest level is solved directly.
        template <class Iterator, class vector1, class vector2>
        static void cycle(Iterator lvl, Iterator end, const params &prm,
                const vector1 &rhs, vector2 &x)
        {
            const index_t n = lvl->A.rows;
            Iterator nxt = lvl; ++nxt;

            if (nxt != end) {
                const index_t nc = nxt->A.rows;

                for(unsigned j = 0; j < prm.ncycle; ++j) {
                    for(unsigned i = 0; i < prm.npre; ++i) lvl->relax(rhs, x);

                    //lvl->t = rhs - lvl->A * x;
#pragma omp parallel for schedule(dynamic, 1024)
                    for(index_t i = 0; i < n; ++i) {
                        value_t temp = rhs[i];

                        for(index_t j = lvl->A.row[i], e = lvl->A.row[i + 1]; j < e; ++j)
                            temp -= lvl->A.val[j] * x[lvl->A.col[j]];

                        lvl->t[i] = temp;
                    }

                    //nxt->f = lvl->R * lvl->t;
#pragma omp parallel for schedule(dynamic, 1024)
                    for(index_t i = 0; i < nc; ++i) {
                        value_t temp = 0;

                        for(index_t j = lvl->R.row[i], e = lvl->R.row[i + 1]; j < e; ++j)
                            temp += lvl->R.val[j] * lvl->t[lvl->R.col[j]];

                        nxt->f[i] = temp;
                    }

                    std::fill(nxt->u.begin(), nxt->u.end(), static_cast<value_t>(0));

                    if (nxt->cg[0].empty())
                        cycle(nxt, end, prm, nxt->f, nxt->u);
                    else
                        kcycle(nxt, end, prm, nxt->f, nxt->u);

                    //x += lvl->P * nxt->u;
#pragma omp parallel for schedule(dynamic, 1024)
                    for(index_t i = 0; i < n; ++i) {
                        value_t temp = 0;

                        for(index_t j = lvl->P.row[i], e = lvl->P.row[i + 1]; j < e; ++j)
                            temp += lvl->P.val[j] * nxt->u[lvl->P.col[j]];

                        x[i] += temp;
                    }

                    for(unsigned i = 0; i < prm.npost; ++i) lvl->relax(rhs, x);
                }
            } else {
                for(index_t i = 0; i < n; ++i) {
                    value_t temp = 0;
                    for(index_t j = lvl->Ai.row[i], e = lvl->Ai.row[i + 1]; j < e; ++j)
                        temp += lvl->Ai.val[j] * rhs[lvl->Ai.col[j]];
                    x[i] = temp;
                }
            }
        }

        template <class Iterator, class vector1, class vector2>
        static void kcycle(Iterator lvl, Iterator end, const params &prm,
                const vector1 &rhs, vector2 &x)
        {
            const index_t n = lvl->A.rows;
            Iterator nxt = lvl; ++nxt;

            if (nxt != end) {
                auto &r = lvl->cg[0];
                auto &s = lvl->cg[1];
                auto &p = lvl->cg[2];
                auto &q = lvl->cg[3];

                std::copy(&rhs[0], &rhs[0] + n, &r[0]);

                value_t rho1 = 0, rho2 = 0;

                for(int iter = 0; iter < 2; ++iter) {
                    std::fill(&s[0], &s[0] + n, static_cast<value_t>(0));
                    cycle(lvl, end, prm, r, s);

                    rho2 = rho1;
                    rho1 = lvl->inner_prod(r, s);

                    if (iter) {
                        value_t beta = rho1 / rho2;
                        std::transform(
                                &p[0], &p[0] + n,
                                &s[0], &p[0],
                                [beta](value_t pp, value_t ss) {
                                    return ss + beta * pp;
                                });
                    } else {
                        std::copy(&s[0], &s[0] + n, &p[0]);
                    }

#pragma omp parallel for schedule(dynamic, 1024)
                    for(index_t i = 0; i < n; ++i) {
                        value_t temp = 0;

                        for(index_t j = lvl->A.row[i], e = lvl->A.row[i + 1]; j < e; ++j)
                            temp += lvl->A.val[j] * p[lvl->A.col[j]];

                        q[i] = temp;
                    }

                    value_t alpha = rho1 / lvl->inner_prod(q, p);

                    std::transform(
                            &x[0], &x[0] + n,
                            &p[0], &x[0],
                            [alpha](value_t xx, value_t pp) {
                                return xx + alpha * pp;
                            });

                    std::transform(
                            &r[0], &r[0] + n,
                            &q[0], &r[0],
                            [alpha](value_t rr, value_t qq) {
                                return rr - alpha * qq;
                            });
                }
            } else {
                for(index_t i = 0; i < n; ++i) {
                    value_t temp = 0;
                    for(index_t j = lvl->Ai.row[i], e = lvl->Ai.row[i + 1]; j < e; ++j)
                        temp += lvl->Ai.val[j] * rhs[lvl->Ai.col[j]];
                    x[i] = temp;
                }
            }
        }

        index_t size() const {
            return t.size();
        }

        index_t nonzeros() const {
            return sparse::matrix_nonzeros(A);
        }
    private:
        matrix A;
        matrix P;
        matrix R;

        matrix Ai;

        mutable std::vector<value_t> u;
        mutable std::vector<value_t> f;
        mutable std::vector<value_t> t;

        mutable std::array<std::vector<value_t>, 4> cg;

        template <class U>
        inline static void vector_copy(U &u, U &v) {
            std::swap(u, v);
        }

        template <class U, class V>
        inline static void vector_copy(U &u, V &v) {
            std::copy(u.begin(), u.end(), &v[0]);
        }

        template <class vector1, class vector2>
        value_t inner_prod(const vector1 &x, const vector2 &y) const {
            const index_t n = A.rows;

            value_t sum = 0;

#pragma omp parallel for reduction(+:sum) schedule(dynamic, 1024)
            for(index_t i = 0; i < n; ++i)
                sum += x[i] * y[i];

            return sum;
        }
};

};

} // namespace level
} // namespace amgcl

#endif