/* Copyright 2019 Yinjian Zhao
 *
 * This file is part of WarpX.
 *
 * License: BSD-3-Clause-LBNL
 */
#ifndef WARPX_PARTICLES_COLLISION_ELASTIC_COLLISION_PEREZ_H_
#define WARPX_PARTICLES_COLLISION_ELASTIC_COLLISION_PEREZ_H_

#include "ComputeTemperature.H"
#include "UpdateMomentumPerezElastic.H"
#include "Particles/WarpXParticleContainer.H"
#include "Utils/WarpXConst.H"

#include <AMReX_Random.H>


/** Prepare information for and call UpdateMomentumPerezElastic().
 *
 * @tparam T_index type of index arguments
 * @tparam T_PR type of particle related floating point arguments
 * @tparam T_R type of other floating point arguments
 * @tparam SoaData_type type of the "struct of array" for the two involved species
 * @param[in] I1s,I2s is the start index for I1,I2 (inclusive).
 * @param[in] I1e,I2e is the stop index for I1,I2 (exclusive).
 * @param[in] I1,I2 the index arrays. They determine all elements that will be used.
 * @param[in,out] soa_1,soa_2 the struct of array for species 1/2
 * @param[in] q1,q2 charge of species 1/2
 * @param[in] m1,m2 mass of species 1/2
 * @param[in] T1 temperature (Joule) of species 1
 *            and will be used if greater than zero,
 *            otherwise will be computed.
 * @param[in] T2 temperature (Joule) of species 2, @see T1
 * @param[in] dt is the time step length between two collision calls.
 * @param[in] L is the Coulomb log and will be used if greater than zero,
 *            otherwise will be computed.
 * @param[in] dV is the volume of the corresponding cell.
 * @param[in] engine the random number generator state & factory
 * @param[in] isSameSpecies whether this is an intra-species collision process
 * @param[in] coll_idx is the collision index offset.
*/

template <typename T_index, typename T_PR, typename T_R, typename SoaData_type>
AMREX_GPU_HOST_DEVICE AMREX_INLINE
void ElasticCollisionPerez (
    T_index const I1s, T_index const I1e,
    T_index const I2s, T_index const I2e,
    T_index const* AMREX_RESTRICT I1,
    T_index const* AMREX_RESTRICT I2,
    SoaData_type soa_1, SoaData_type soa_2,
    T_PR const  q1, T_PR const  q2,
    T_PR const  m1, T_PR const  m2,
    T_PR const  T1, T_PR const  T2,
    T_R const  dt, T_PR const   L, T_R const dV,
    amrex::RandomEngine const& engine,
    bool const isSameSpecies, T_index coll_idx)
{
    const T_index NI1 = I1e - I1s;
    const T_index NI2 = I2e - I2s;
    const T_index max_N = amrex::max(NI1,NI2);
    const T_index min_N = amrex::min(NI1,NI2);

    T_PR * const AMREX_RESTRICT w1 = soa_1.m_rdata[PIdx::w];
    T_PR * const AMREX_RESTRICT u1x = soa_1.m_rdata[PIdx::ux];
    T_PR * const AMREX_RESTRICT u1y = soa_1.m_rdata[PIdx::uy];
    T_PR * const AMREX_RESTRICT u1z = soa_1.m_rdata[PIdx::uz];

    T_PR * const AMREX_RESTRICT w2 = soa_2.m_rdata[PIdx::w];
    T_PR * const AMREX_RESTRICT u2x = soa_2.m_rdata[PIdx::ux];
    T_PR * const AMREX_RESTRICT u2y = soa_2.m_rdata[PIdx::uy];
    T_PR * const AMREX_RESTRICT u2z = soa_2.m_rdata[PIdx::uz];

    // get local T1t and T2t
    T_PR T1t; T_PR T2t;
    if ( T1 <= T_PR(0.0) && L <= T_PR(0.0) )
    {
        T1t = ComputeTemperature(I1s,I1e,I1,u1x,u1y,u1z,m1);
    }
    else { T1t = T1; }
    if ( T2 <= T_PR(0.0) && L <= T_PR(0.0) )
    {
        T2t = ComputeTemperature(I2s,I2e,I2,u2x,u2y,u2z,m2);
    }
    else { T2t = T2; }

    // local density
    T_PR n1  = T_PR(0.0);
    T_PR n2  = T_PR(0.0);
    T_PR n12 = T_PR(0.0);
    for (T_index i1=I1s; i1<I1e; ++i1) { n1 += w1[ I1[i1] ]; }
    for (T_index i2=I2s; i2<I2e; ++i2) { n2 += w2[ I2[i2] ]; }
    // Intra-species: the density is in fact the sum of the density of
    // each sub-group of particles
    if (isSameSpecies) {
        n1 = n1 + n2;
        n2 = n1;
    }
    n1 = n1 / dV; n2 = n2 / dV;
    {
      T_index i1 = I1s; T_index i2 = I2s;
      for (T_index k = 0; k < max_N; ++k)
      {
        n12 += amrex::min( w1[ I1[i1] ], w2[ I2[i2] ] );
        ++i1; if ( i1 == I1e ) { i1 = I1s; }
        ++i2; if ( i2 == I2e ) { i2 = I2s; }
      }
      n12 = n12 / dV;
    }
    // Intra-species: Apply correction in collision rate
    if (isSameSpecies) { n12 *= T_PR(2.0); }

    // compute Debye length lmdD
    T_PR lmdD;
    if ( T1t < T_PR(0.0) || T2t < T_PR(0.0) ) {
        lmdD = T_PR(0.0);
    }
    else {
        lmdD = T_PR(1.0)/std::sqrt( n1*q1*q1/(T1t*PhysConst::ep0) +
                                    n2*q2*q2/(T2t*PhysConst::ep0) );
    }
    T_PR rmin = std::pow( T_PR(4.0) * MathConst::pi / T_PR(3.0) *
               amrex::max(n1,n2), T_PR(-1.0/3.0) );
    lmdD = amrex::max(lmdD, rmin);

#if (defined WARPX_DIM_RZ)
    T_PR * const AMREX_RESTRICT theta1 = soa_1.m_rdata[PIdx::theta];
    T_PR * const AMREX_RESTRICT theta2 = soa_2.m_rdata[PIdx::theta];
#endif

    // call UpdateMomentumPerezElastic()
    {
      T_index i1 = I1s + coll_idx;
      T_index i2 = I2s + coll_idx;

      // we will start from collision number = coll_idx and then add
      // stride (smaller set size) until we do all collisions (larger set size)
      for (T_index k = coll_idx; k < max_N; k += min_N)
      {
#if (defined WARPX_DIM_RZ)
          /* In RZ geometry, macroparticles can collide with other macroparticles
           * in the same *cylindrical* cell. For this reason, collisions between macroparticles
           * are actually not local in space. In this case, the underlying assumption is that
           * particles within the same cylindrical cell represent a cylindrically-symmetry
           * momentum distribution function. Therefore, here, we temporarily rotate the
           * momentum of one of the macroparticles in agreement with this cylindrical symmetry.
           * (This is technically only valid if we use only the m=0 azimuthal mode in the simulation;
           * there is a corresponding assert statement at initialization.) */
          T_PR const theta = theta2[I2[i2]]-theta1[I1[i1]];
          T_PR const u1xbuf = u1x[I1[i1]];
          u1x[I1[i1]] = u1xbuf*std::cos(theta) - u1y[I1[i1]]*std::sin(theta);
          u1y[I1[i1]] = u1xbuf*std::sin(theta) + u1y[I1[i1]]*std::cos(theta);
#endif

          UpdateMomentumPerezElastic(
              u1x[ I1[i1] ], u1y[ I1[i1] ], u1z[ I1[i1] ],
              u2x[ I2[i2] ], u2y[ I2[i2] ], u2z[ I2[i2] ],
              n1, n2, n12,
              q1, m1, w1[ I1[i1] ], q2, m2, w2[ I2[i2] ],
              dt, L, lmdD,
              engine);

#if (defined WARPX_DIM_RZ)
          T_PR const u1xbuf_new = u1x[I1[i1]];
          u1x[I1[i1]] = u1xbuf_new*std::cos(-theta) - u1y[I1[i1]]*std::sin(-theta);
          u1y[I1[i1]] = u1xbuf_new*std::sin(-theta) + u1y[I1[i1]]*std::cos(-theta);
#endif

          if (max_N == NI1) {
            i1 += min_N;
          }
          if (max_N == NI2) {
            i2 += min_N;
          }
      }
    }

}

#endif // WARPX_PARTICLES_COLLISION_ELASTIC_COLLISION_PEREZ_H_
