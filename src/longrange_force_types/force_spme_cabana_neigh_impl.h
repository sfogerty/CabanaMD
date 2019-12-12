/****************************************************************************
 * Copyright (c) 2018-2019 by the Cabana authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the Cabana library. Cabana is distributed under a   *
 * BSD 3-clause license. For the licensing terms see the LICENSE file in    *
 * the top-level directory.                                                 *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#include<force_ewald_cabana_neigh.h>

#ifdef CabanaMD_ENABLE_Cuda
#include <cufft.h>
#include <cufftw.h>
#else
#include <fftw3.h>
#endif

/* Smooth particle mesh Ewald (SPME) solver
 * - This method, from Essman et al. (1995) computes long-range Coulombic forces
 *   with O(nlogN) scaling by using 3D FFT and interpolation to a mesh for the
 *   reciprocal space part of the Ewald sum.
 * - Here the method is used to compute electrostatic energies from an example
 *   arrangement of charged particles. Currently, we assume periodic boundary
 * conditions and a cubic mesh and arrangement of particles in 3 dimensions.
 * - Future versions will allow for assymetric meshes and non-uniform particle
 *   distributions, as well as 1 or 2 dimensions.
 */

constexpr double PI(3.141592653589793238462643);
constexpr double PI_SQRT(1.772453850905516);
constexpr double PI_SQ(PI*PI);// 9.869604401089359
constexpr double PI_DIV_SQ(1.0/PI_SQ);//0.101321183642338


template<class t_neighbor>
ForceSPME<t_neighbor>::ForceSPME(double accuracy, System* system, bool half_neigh_):Force(system,half_neigh_) {
    half_neigh = half_neigh_;
    assert( half_neigh == true );

    _r_max = 0.0;
    tune( accuracy, system );

}

template<class t_neighbor>
ForceSPME<t_neighbor>::ForceSPME(double alpha, double r_max, bool half_neigh_):Force(system,half_neigh_) {
    half_neigh = half_neigh_;
    assert( half_neigh == true );

    _r_max = r_max;
    _alpha = alpha;
}

// Tune to a given accuracy
template<class t_neighbor>
ForceSPME<t_neighbor>::tune( double accuracy, System* system ) {
    if ( system->domain_x != system->domain_y or system->domain_x != system->domain_z )
    {
        throw std::runtime_error( "SPME needs symmetric system size for now." );
    }
    //auto q = Cabana::slice<Charge>( particles );

    const int N = system->N;

    // Fincham 1994, Optimisation of the Ewald Sum for Large Systems
    // only valid for cubic systems (needs adjustement for non-cubic systems)
    constexpr double EXECUTION_TIME_RATIO_K_R = 2.0;
    double p = -log( accuracy );
    _alpha = pow( EXECUTION_TIME_RATIO_K_R, 1.0 / 6.0 ) * sqrt( p / PI ) *
             pow( N, 1.0 / 6.0 ) / lx;
    _k_max = pow( EXECUTION_TIME_RATIO_K_R, 1.0 / 6.0 ) * sqrt( p / PI ) *
             pow( N, 1.0 / 6.0 ) / lx * 2.0 * PI;
    _r_max = pow( EXECUTION_TIME_RATIO_K_R, 1.0 / 6.0 ) * sqrt( p / PI ) /
             pow( N, 1.0 / 6.0 ) * lx;
    _alpha = sqrt( p ) / _r_max;
    _k_max = 2.0 * sqrt( p ) * _alpha;
}

// Compute a 1D cubic cardinal B-spline value, used in spreading charge to mesh
//   Given the distance from the particle (x) in units of mesh spaces, this
//   computes the fraction of that charge to place at a mesh point x mesh spaces
//   away The cubic B-spline used here is shifted so that it is symmetric about
//   zero All cubic B-splines are smooth functions that go to zero and are
//   defined piecewise
//TODO: replace use of this with Cajita functions
KOKKOS_INLINE_FUNCTION
double TPME::oneDspline( double x )
{
    if ( x >= 0.0 and x < 1.0 )
    {
        return ( 1.0 / 6.0 ) * x * x * x;
    }
    else if ( x >= 1.0 and x <= 2.0 )
    {
        return -( 1.0 / 2.0 ) * x * x * x + 2.0 * x * x - 2.0 * x +
               ( 2.0 / 3.0 );
    }
    // Using the symmetry here, only need to define function between 0 and 2
    // Beware: This means all input to this function should be made positive
    {
        return 0.0; // Zero if distance is >= 2 mesh spacings
    }
}


// Compute derivative of 1D cubic cardinal B-spline
//TODO: replace use of this with Cajita functions
KOKKOS_INLINE_FUNCTION
double TPME::oneDsplinederiv( double origx )
{
    double x = 2.0 - std::abs( origx );
    double forcedir = 1.0;
    if ( origx < 0.0 )
    {
        forcedir = -1.0;
    }
    if ( x >= 0.0 and x < 1.0 )
    {
        return ( 1.0 / 2.0 ) * x * x * forcedir;
    }
    else if ( x >= 1.0 and x <= 2.0 )
    {
        return ( -( 3.0 / 2.0 ) * x * x + 4.0 * x - 2.0 ) * forcedir;
    }
    // Using the symmetry here, only need to define function between 0 and 2
    // Beware: This means all input to this function should be made positive
    else
    {
        return 0.0; // Zero if distance is >= 2 mesh spacings
    }
}


// Compute a 1-D Euler spline. This function is part of the "lattice structure
// factor" and is given by:
//   b(k, meshwidth) = exp(2*PI*i*3*k/meshwidth) / SUM_{l=0,2}(1Dspline(l+1) *
//   exp(2*PI*i*k*l/meshwidth)) when using a non-shifted cubic B-spline in the
//   charge spread, where meshwidth is the number of mesh points in that
//   dimension and k is the scaled fractional coordinate
KOKKOS_INLINE_FUNCTION
double TPME::oneDeuler( int k, int meshwidth )
{
    double denomreal = 0.0;
    double denomimag = 0.0;
    // Compute the denominator sum first, splitting the complex exponential into
    // sin and cos
    for ( int l = 0; l < 3; l++ )
    {
        denomreal += TPME::oneDspline( fmin( 4.0 - ( l + 1.0 ), l + 1.0 ) ) *
                     cos( 2.0 * PI * double( k ) * l / double( meshwidth ) );
        denomimag += TPME::oneDspline( fmin( 4.0 - ( l + 1.0 ), l + 1.0 ) ) *
                     sin( 2.0 * PI * double( k ) * l / double( meshwidth ) );
    }
    // Compute the numerator, again splitting the complex exponential
    double numreal = cos( 2.0 * PI * 3.0 * double( k ) / double( meshwidth ) );
    double numimag = sin( 2.0 * PI * 3.0 * double( k ) / double( meshwidth ) );
    // Returning the norm of the 1-D Euler spline
    return ( numreal * numreal + numimag * numimag ) /
           ( denomreal * denomreal + denomimag * denomimag );
}

// Compute the energy and forces
//TODO: replace this mesh with a Cajita mesh
double TPME::compute( System &system, ParticleList &mesh )
{
    // For now, force symmetry
    if ( system->domain_x != system->domain_y or system->domain_x != system->domain_z )
    {
        throw std::runtime_error( "SPME needs symmetric system size for now." );
    }

    // Initialize energies: real-space, k-space (reciprocal space), self-energy
    double Ur = 0.0, Uk = 0.0, Uself = 0.0;

    // Particle slices
    auto x = Cabana::slice<Position>( system->xvf );
    auto q = Cabana::slice<Charge>( system->xvf );
    auto p = Cabana::slice<Potential>( system->xvf );
    auto f = Cabana::slice<Force>( system->xvf );

    // Mesh slices
    // TODO: replace with Cajita mesh
    auto meshr = Cabana::slice<Position>( mesh );
    auto meshq = Cabana::slice<Charge>( mesh );

    // Number of particles
    int n_max = system->N;

    // Number of mesh points
    size_t meshsize = mesh.size();//TODO: replace with Cajita

    double total_energy = 0.0;

    // Set the potential of each particle to zero
    auto init_p = KOKKOS_LAMBDA( const int idx )
    {
        p( idx ) = 0.0;
        f( idx, 0 ) = 0.0;
        f( idx, 1 ) = 0.0;
        f( idx, 2 ) = 0.0;
    };
    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, n_max ),
                          init_p );
    Kokkos::fence();

    double alpha = _alpha;
    double r_max = _r_max;
    double eps_r = _eps_r;

    // computation real-space contribution
    // plain all to all comparison
    Kokkos::parallel_reduce(
        Kokkos::TeamPolicy<>( n_max, Kokkos::AUTO ),
        KOKKOS_LAMBDA( Kokkos::TeamPolicy<>::member_type member, double &Ur_i ) {
        int i = member.league_rank();
            if ( i < n_max )
            {
                double Ur_inner = 0.0;
                int per_shells = std::ceil( r_max / lx );
                Kokkos::parallel_reduce(
                    Kokkos::ThreadVectorRange( member, n_max ),
                    [&]( int &j, double &Ur_j ) {
                        for ( int pz = -per_shells; pz <= per_shells; ++pz )
                            for ( int py = -per_shells; py <= per_shells;
                                  ++py )
                                for ( int px = -per_shells;
                                      px <= per_shells; ++px )
                                {
                                    double dx =
                                        r( i, 0 ) -
                                        ( r( j, 0 ) + (double)px * lx );
                                    double dy =
                                        r( i, 1 ) -
                                        ( r( j, 1 ) + (double)py * ly );
                                    double dz =
                                        r( i, 2 ) -
                                        ( r( j, 2 ) + (double)pz * lz );
                                    double d =
                                        sqrt( dx * dx + dy * dy + dz * dz );
                                    double contrib =
                                        ( d <= r_max &&
                                          std::abs( d ) >= 1e-12 )
                                            ? 0.5 * q( i ) * q( j ) *
                                                  erfc( alpha * d ) / d
                                            : 0.0;
                                    double f_fact =
                                        ( d <= r_max &&
                                          std::abs( d ) >= 1e-12 ) *
                                        q( i ) * q( j ) *
                                        ( 2.0 * sqrt( alpha / PI ) *
                                              exp( -alpha * d * d ) +
                                          erfc( sqrt( alpha ) * d ) ) /
                                        ( d * d +
                                          ( std::abs( d ) <= 1e-12 ) );
                                    Kokkos::atomic_add( &f( i, 0 ),
                                                        f_fact * dx );
                                    Kokkos::atomic_add( &f( i, 1 ),
                                                        f_fact * dy );
                                    Kokkos::atomic_add( &f( i, 2 ),
                                                        f_fact * dz );
                                    Kokkos::single(
                                        Kokkos::PerThread( member ),
                                        [&] { Ur_j += contrib; } );
                                }
                    },
                    Ur_inner );
                Kokkos::single( Kokkos::PerTeam( member ), [&] {
                    p( i ) += Ur_inner;
                    Ur_i += Ur_inner;
                } );
            }
        },
        Ur );

    // computation reciprocal-space contribution

    // First, spread the charges onto the mesh


//TODO: Cajita mesh
    // how far apart the mesh points are (assumed uniform cubic)
    double spacing = meshr( 1, 0 ) - meshr( 0, 0 ); 
    auto spread_q = KOKKOS_LAMBDA( const int idx )
    {
        double xdist, ydist, zdist;
        for ( size_t pidx = 0; pidx < particles.size(); ++pidx )
        {
            // x-distance between mesh point and particle
            xdist = fmin(
                fmin( std::abs( meshr( idx, 0 ) - r( pidx, 0 ) ),
                      std::abs( meshr( idx, 0 ) - ( r( pidx, 0 ) + 1.0 ) ) ),
                std::abs(
                    meshr( idx, 0 ) -
                    ( r( pidx, 0 ) - 1.0 ) ) ); // account for periodic bndry
            // y-distance between mesh point and particle
            ydist = fmin(
                fmin( std::abs( meshr( idx, 1 ) - r( pidx, 1 ) ),
                      std::abs( meshr( idx, 1 ) - ( r( pidx, 1 ) + 1.0 ) ) ),
                std::abs(
                    meshr( idx, 1 ) -
                    ( r( pidx, 1 ) - 1.0 ) ) ); // account for periodic bndry
            // z-distance between mesh point and particle
            zdist = fmin(
                fmin( std::abs( meshr( idx, 2 ) - r( pidx, 2 ) ),
                      std::abs( meshr( idx, 2 ) - ( r( pidx, 2 ) + 1.0 ) ) ),
                std::abs(
                    meshr( idx, 2 ) -
                    ( r( pidx, 2 ) - 1.0 ) ) ); // account for periodic bndry

            if ( xdist <= 2.0 * spacing and ydist <= 2.0 * spacing and
                 zdist <= 2.0 * spacing ) 
            {
                // add charge to mesh point according to spline
                meshq( idx ) += q( pidx ) *
                                TPME::oneDspline( 2.0 - ( xdist / spacing ) ) *
                                TPME::oneDspline( 2.0 - ( ydist / spacing ) ) *
                                TPME::oneDspline( 2.0 - ( zdist / spacing ) );
            }
        }
    };
    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, meshsize ),
                          spread_q );
    Kokkos::fence();
//TODO: Again replace with Cajita
    int meshwidth =
        std::round( std::pow( meshsize, 1.0 / 3.0 ) ); // Assuming cubic mesh

// Calculating the values of the BC array involves first shifting the fractional
// coords then compute the B and C arrays as described in the paper This can be
// done once at the start of a run if the mesh stays constant

#ifdef CabanaMD_ENABLE_Cuda
    cufftDoubleComplex *BC;
    cudaMallocManaged( (void **)&BC, sizeof( cufftDoubleComplex ) * meshsize );
#else
    fftw_complex *BC;
    BC = (fftw_complex *)fftw_malloc( sizeof( fftw_complex ) * meshsize );
#endif

    auto BC_functor = KOKKOS_LAMBDA( const int kx )
    {
        int ky, kz, mx, my, mz, idx;
        for ( ky = 0; ky < meshwidth; ky++ )
        {
            for ( kz = 0; kz < meshwidth; kz++ )
            {
                idx = kx + ( ky * meshwidth ) + ( kz * meshwidth * meshwidth );
                if ( kx + ky + kz > 0 )
                {
                    // Shift the C array
                    mx = kx;
                    my = ky;
                    mz = kz;
                    if ( mx > meshwidth / 2.0 )
                    {
                        mx = kx - meshwidth;
                    }
                    if ( my > meshwidth / 2.0 )
                    {
                        my = ky - meshwidth;
                    }
                    if ( mz > meshwidth / 2.0 )
                    {
                        mz = kz - meshwidth;
                    }
                    double m2 = ( mx * mx + my * my +
                                  mz * mz );
// Calculate BC.
#ifdef CabanaMD_ENABLE_Cuda
                    BC[idx].x = TPME::oneDeuler( kx, meshwidth ) *
                                TPME::oneDeuler( ky, meshwidth ) *
                                TPME::oneDeuler( kz, meshwidth ) *
                                exp( -PI * PI * m2 / ( alpha * alpha ) ) /
                                ( PI * lx * ly * lz * m2 );
                    BC[idx].y = 0.0; // imag part
#else
                    BC[idx][0] = TPME::oneDeuler( kx, meshwidth ) *
                                 TPME::oneDeuler( ky, meshwidth ) *
                                 TPME::oneDeuler( kz, meshwidth ) *
                                 exp( -PI * PI * m2 / ( alpha * alpha ) ) /
                                 ( PI * lx * ly * lz * m2 );
                    BC[idx][1] = 0.0; // imag part
#endif
                }
                else
                {
#ifdef CabanaMD_ENABLE_Cuda
                    BC[idx].x = 0.0;
                    BC[idx].y = 0.0; // set origin element to zero
#else
                    BC[idx][0] = 0.0;
                    BC[idx][1] = 0.0; // set origin element to zero
#endif
                }
            }
        }
    };
    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, meshwidth ),
                          BC_functor );
    Kokkos::fence();


// Next, solve Poisson's equation taking some FFTs of charges on mesh grid
// The plan here is to perform an inverse FFT on the mesh charge, then multiply
//  the norm of that result (in reciprocal space) by the BC array

// Set up the real-space charge and reciprocal-space charge
#ifdef CabanaMD_ENABLE_Cuda
    cufftDoubleComplex *Qr, *Qktest;
    cufftHandle plantest;
    cudaMallocManaged( (void **)&Qr, sizeof( fftw_complex ) * meshsize );
    cudaMallocManaged( (void **)&Qktest, sizeof( fftw_complex ) * meshsize );
    // Copy charges into real input array
    auto copy_charge = KOKKOS_LAMBDA( const int idx )
    {
        Qr[idx].x = meshq( idx );
        Qr[idx].y = 0.0;
    };
    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, meshsize ),
                          copy_charge );
#else
    fftw_complex *Qr, *Qktest;
    fftw_plan plantest, planforward;
    Qr = (fftw_complex *)fftw_malloc( sizeof( fftw_complex ) * meshsize );
    Qktest = (fftw_complex *)fftw_malloc( sizeof( fftw_complex ) * meshsize );
    // Copy charges into real input array
    auto copy_charge = KOKKOS_LAMBDA( const int idx )
    {
        Qr[idx][0] = meshq( idx );
        Qr[idx][1] = 0.0;
    };
    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, meshsize ),
                          copy_charge );
#endif
    Kokkos::fence();

// Plan out that IFFT on the real-space charge mesh
#ifdef CabanaMD_ENABLE_Cuda
    cufftPlan3d( &plantest, meshwidth, meshwidth, meshwidth, CUFFT_Z2Z );
    cufftExecZ2Z( plantest, Qr, Qktest, CUFFT_INVERSE ); // IFFT on Q
    // Update the energy
    Kokkos::parallel_reduce(
        meshsize,
        KOKKOS_LAMBDA( const int idx, double &Uk_part ) {
            Uk_part += BC[idx].x * ( ( Qktest[idx].x * Qktest[idx].x ) +
                                     ( Qktest[idx].y * Qktest[idx].y ) );
        },
        Uk );
    Kokkos::fence();
    cufftDestroy( plantest );
#else
    plantest = fftw_plan_dft_3d( meshwidth, meshwidth, meshwidth, Qr, Qktest,
                                 FFTW_BACKWARD, FFTW_ESTIMATE );
    fftw_execute( plantest ); // IFFT on Q
    // update the energy
    Kokkos::parallel_reduce(
        meshsize,
        KOKKOS_LAMBDA( const int idx, double &Uk_part ) {
            Uk_part += BC[idx][0] * ( ( Qktest[idx][0] * Qktest[idx][0] ) +
                                      ( Qktest[idx][1] * Qktest[idx][1] ) );
        },
        Uk );
    Kokkos::fence();
    fftw_destroy_plan( plantest );
    // update Q for later force calcs
    auto update_q = KOKKOS_LAMBDA( const int idx )
    {
        Qktest[idx][0] *= BC[idx][0];
        Qktest[idx][1] *= BC[idx][0];
    };

    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, n_max ),
                          update_q );
    // FFT forward on altered Q array
    planforward = fftw_plan_dft_3d( meshwidth, meshwidth, meshwidth, Qktest,
                                    Qktest, FFTW_FORWARD, FFTW_ESTIMATE );
    fftw_execute( planforward );
    fftw_destroy_plan( planforward );
#endif

    Uk *= 0.5;

    // computation of self-energy contribution
    Kokkos::parallel_reduce( Kokkos::RangePolicy<ExecutionSpace>( 0, n_max ),
                             KOKKOS_LAMBDA( int idx, double &Uself_part ) {
                                 Uself_part +=
                                     -alpha / PI_SQRT * q( idx ) * q( idx );
                                 p( idx ) += Uself_part;
                             },
                             Uself );
    Kokkos::fence();
    total_energy = Ur + Uk + Uself + Udip;

    // Now, compute forces on each particle
    //
    // For each particle
    // loop through each mesh point
    // 
    // Filter out mesh points where dx > 2gaps, dy > 2gaps, dz > 2 gaps?
    // 
    // calculate B-spline coeffs x,y,z
    // 
    // calculate deriv of B-spline coeffs x,y,z
    // 
    // Fx += q*(deriv_Bx)*By*Bz*Qktest[meshpoint]
    // Fy += q*(deriv_By)*Bx*Bz*Qktest[meshpoint]
    // Fz += q*(deriv_Bz)*Bx*By*Qktest[meshpoint]
    auto gather_f = KOKKOS_LAMBDA( const int pidx )
    {
        double xdist, ydist, zdist, closestpart;
        for ( size_t idx = 0; idx < meshsize; ++idx )
        {
            // x-distance between mesh point and particle
            closestpart = r( pidx, 0 );
            xdist = closestpart - meshr( idx, 0 );
            if ( std::abs( xdist ) >
                 std::abs( meshr( idx, 0 ) - ( r( pidx, 0 ) + 1.0 ) ) )
            {
                closestpart = r( pidx, 0 ) + 1.0;
            }
            if ( std::abs( xdist ) >
                 std::abs( meshr( idx, 0 ) - ( r( pidx, 0 ) - 1.0 ) ) )
            {
                closestpart = r( pidx, 0 ) - 1.0;
            }
            xdist = closestpart - meshr( idx, 0 );

            // y-distance between mesh point and particle
            closestpart = r( pidx, 1 );
            ydist = closestpart - meshr( idx, 1 );
            if ( std::abs( ydist ) >
                 std::abs( meshr( idx, 1 ) - ( r( pidx, 1 ) + 1.0 ) ) )
            {
                closestpart = r( pidx, 1 ) + 1.0;
            }
            if ( std::abs( ydist ) >
                 std::abs( meshr( idx, 1 ) - ( r( pidx, 1 ) - 1.0 ) ) )
            {
                closestpart = r( pidx, 1 ) - 1.0;
            }
            ydist = closestpart - meshr( idx, 1 );

            // z-distance between mesh point and particle
            closestpart = r( pidx, 2 );
            zdist = closestpart - meshr( idx, 2 );
            if ( std::abs( zdist ) >
                 std::abs( meshr( idx, 2 ) - ( r( pidx, 2 ) + 1.0 ) ) )
            {
                closestpart = r( pidx, 2 ) + 1.0;
            }
            if ( std::abs( zdist ) >
                 std::abs( meshr( idx, 2 ) - ( r( pidx, 2 ) - 1.0 ) ) )
            {
                closestpart = r( pidx, 2 ) - 1.0;
            }
            zdist = closestpart - meshr( idx, 2 );

            if ( std::abs( xdist ) <= 2.0 * spacing and
                 std::abs( ydist ) <= 2.0 * spacing and
                 std::abs( zdist ) <=
                     2.0 * spacing ) 
           {
                // Calculate forces on particle from mesh point
                f( pidx, 0 ) +=
                    q( pidx ) * TPME::oneDsplinederiv( xdist / spacing ) *
                    TPME::oneDspline( 2.0 - ( std::abs( ydist ) / spacing ) ) *
                    TPME::oneDspline( 2.0 - ( std::abs( zdist ) / spacing ) ) *
                    Qktest[idx][0];
                f( pidx, 1 ) +=
                    q( pidx ) *
                    TPME::oneDspline( 2.0 - ( std::abs( xdist ) / spacing ) ) *
                    TPME::oneDsplinederiv( ydist / spacing ) *
                    TPME::oneDspline( 2.0 - ( std::abs( zdist ) / spacing ) ) *
                    Qktest[idx][0];
                f( pidx, 2 ) +=
                    q( pidx ) *
                    TPME::oneDspline( 2.0 - ( std::abs( xdist ) / spacing ) ) *
                    TPME::oneDspline( 2.0 - ( std::abs( ydist ) / spacing ) ) *
                    TPME::oneDsplinederiv( zdist / spacing ) * Qktest[idx][0];
                if ( pidx == 0 )
                {
                    std::cout
                        << q( pidx ) *
                               TPME::oneDspline(
                                   2.0 - ( std::abs( xdist ) / spacing ) ) *
                               TPME::oneDsplinederiv( ydist / spacing ) *
                               TPME::oneDspline(
                                   2.0 - ( std::abs( zdist ) / spacing ) ) *
                               Qktest[idx][0]
                        << std::endl;
                }
            }
        }
    };
    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, n_max ),
                          gather_f );

#ifndef CabanaMD_ENABLE_Cuda
    fftw_cleanup();
#endif
    return total_energy;
//TODO: return nothing, just update forces. calc_energy will return this

}
