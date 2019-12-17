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

template<class t_neighbor>
ForceEwald<t_neighbor>::ForceEwald(System* system, bool half_neigh_):Force(system,half_neigh_) {
    half_neigh = half_neigh_;
    assert( half_neigh == true );

    std::vector<int> dims( 3 );
    std::vector<int> periods( 3 );

    dims.at( 0 ) = dims.at( 1 ) = dims.at( 2 ) = 0;
    periods.at( 0 ) = periods.at( 1 ) = periods.at( 2 ) = 1;

    int n_ranks;
    MPI_Comm_size( MPI_COMM_WORLD, &n_ranks );

    MPI_Dims_create( n_ranks, 3, dims.data() );

    MPI_Comm cart_comm;
    MPI_Cart_create( MPI_COMM_WORLD, 3, dims.data(), periods.data(), 0,
                         &cart_comm );

    int rank;
    MPI_Comm_rank( cart_comm, &rank );

    std::vector<int> loc_coords( 3 );
    std::vector<int> cart_dims( 3 );
    std::vector<int> cart_periods( 3 );
    MPI_Cart_get( cart_comm, 3, cart_dims.data(), cart_periods.data(),
                  loc_coords.data() );


    //MPI_Topo_test( comm, &comm_type );
    //assert( comm_type == MPI_CART );
    this->comm = comm;

    
}

//initialize Ewald params if given from input deck
template<class t_neighbor>
void ForceEwald<t_neighbor>::init_coeff(T_X_FLOAT neigh_cut_,char** args) {

  double alpha = atof(args[3]);
  double rmax = atof(args[4]);
  double kmax = atof(args[5]);
}
//TODO: overload initialization to include tuning when not given params

//TODO: Use create_neigh_list just like shortrange forces
template<class t_neighbor>
void ForceEwald<t_neighbor>::create_neigh_list(System* system) {
  N_local = system->N_local;

  double grid_min[3] = {system->sub_domain_lo_x - system->sub_domain_x,
                        system->sub_domain_lo_y - system->sub_domain_y,
                        system->sub_domain_lo_z - system->sub_domain_z};
  double grid_max[3] = {system->sub_domain_hi_x + system->sub_domain_x,
                        system->sub_domain_hi_y + system->sub_domain_y,
                        system->sub_domain_hi_z + system->sub_domain_z};

  auto x = Cabana::slice<Positions>(system->xvf);

  t_neighbor list( x, 0, N_local, neigh_cut, 1.0, grid_min, grid_max );
  neigh_list = list;
}

template<class t_neighbor>
void ForceEwald<t_neighbor>::compute(System* system) {

  double Ur = 0.0, Uk = 0.0, Uself = 0.0, Udip = 0.0;
  double Udip_vec[3];
  double N_local, N_max;

  N_local = system->N_local;
  N_max = system->N_max;
  x = Cabana::slice<Positions>(system->xvf);
  f = Cabana::slice<Forces>(system->xvf);
  //f_a = Cabana::slice<Forces>(system->xvf);
  id = Cabana::slice<IDs>(system->xvf);
  //type = Cabana::slice<Types>(system->xvf);
  q = Cabana::slice<Charges>(system->xvf);
  p = Cabana::slice<Potentials>(system->xvf);//TODO: Add potentials as part of the system AoSoA?

  //This seems awkward. Could remove?
  Kokkos::View<double *, MemorySpace> domain_length( "domain length", 6 );
  domain_length( 0 ) = system->sub_domain_lo_x;
  domain_length( 1 ) = system->sub_domain_hi_x;
  domain_length( 2 ) = system->sub_domain_lo_y;
  domain_length( 3 ) = system->sub_domain_hi_y;
  domain_length( 4 ) = system->sub_domain_lo_z;
  domain_length( 5 ) = system->sub_domain_hi_z;

  // compute subdomain size and make it available in the kernels
  Kokkos::View<double *, MemorySpace> sub_domain_size( "sub_domain size", 3);
  sub_domain_size( 0 ) = system->sub_domain_x;
  sub_domain_size( 1 ) = system->sub_domain_y;
  sub_domain_size( 2 ) = system->sub_domain_z;
  //sub_domain_size( 0 ) = (system->sub_domain_hi_x + system->sub_domain_x) - (system_sub_domain_lo_x - system_sub_domain_x);
  //sub_domain_size( 1 ) = (system->sub_domain_hi_y + system->sub_domain_y) - (system_sub_domain_lo_y - system_sub_domain_y);
  //sub_domain_size( 2 ) = (system->sub_domain_hi_z + system->sub_domain_z) - (system_sub_domain_lo_z - system_sub_domain_z);
  //sub_domain_size( 1 ) = system->subdomain_hi_y - system_subdomain_y;
  //sub_domain_size( 2 ) = system->subdomain_hi_z - system_subdomain_z;
 
  // compute domain size and make it available in the kernels
  Kokkos::View<double *, MemorySpace> domain_size( "domain size", 3 );
  domain_size( 0 ) = system->domain_x;//TODO: Seems very nnecessary. Remove
  domain_size( 1 ) = system->domain_y;
  domain_size( 2 ) = system->domain_z;
  //domain_size( 0 ) = (system->domain_hi_x + system->domain_x) - (system->domain_lo_x - system->domain_x);
  //domain_size( 1 ) = (system->domain_hi_y + system->domain_y) - (system->domain_lo_y - system->domain_y);
  //domain_size( 2 ) = (system->domain_hi_z + system->domain_z) - (system->domain_lo_z - system->domain_z);
 
  //get the solver parameters
  double alpha = _alpha;
  double r_max = _r_max;
  double eps_r = _eps_r;
  double k_max = _k_max;
/*
  // store MPI information
  int rank, n_ranks;
  std::vector<int> loc_dims( 3 );
  std::vector<int> cart_dims( 3 );
  std::vector<int> cart_periods( 3 );
  MPI_Comm_rank( comm, &rank );
  MPI_Comm_size( comm, &n_ranks );
  MPI_Cart_get( comm, 3, cart_dims.data(), cart_periods.data(),
                loc_dims.data() );

  // neighbor information
  std::vector<int> neighbor_low( 3 );
  std::vector<int> neighbor_up( 3 );

  // get neighbors in parallel decomposition
  for ( int dim = 0; dim < 3; ++dim )
  {
      MPI_Cart_shift( comm, dim, 1, &neighbor_low.at( dim ),
                      &neighbor_up.at( dim ) );
  }
*/
  // initialize potential and force to zero
  auto init_parameters = KOKKOS_LAMBDA( const int idx )
    {
        p( idx ) = 0.0;
        f( idx, 0 ) = 0.0;
        f( idx, 1 ) = 0.0;
        f( idx, 2 ) = 0.0;
    }; 
  Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, N_max ), init_parameters );
  Kokkos::fence();

  // In order to compute the k-space contribution in parallel
  // first the following sums need to be created for each
  // k-vector:
  //              sum(1<=i<=N_part) sin/cos (dot(k,r_i))
  // This can be achieved by computing partial sums on each
  // MPI process, reducing them over all processes and
  // afterward using the pre-computed values to compute
  // the forces and potentials acting on the particles
  // in parallel independently again.
 
  // determine number of required sine / cosine values  
  int k_int = std::ceil( k_max ) + 1;
  int n_kvec = ( 2 * k_int + 1 ) * ( 2 * k_int ) * ( 2 * k_int + 1 );

  //allocate View to store them
  Kokkos::View<double *, MemorySpace> U_trigonometric(
      "sine and cosine contributions", 2 * n_kvec );

  //set all values to zero
  Kokkos::parallel_for( 2 * n_kvec, KOKKOS_LAMBDA( const int idx ) {
      U_trigonometric( idx ) = 0.0;
  } );
  Kokkos::fence();
  
  double lx = domain_size(0);
  double ly = domain_size(1);
  double lz = domain_size(2);

  //Compute partial sums
  auto partial_sums = KOKKOS_LAMBDA( const int idx )
  {
    double qi = q( idx );

        for ( int kz = -k_int; kz <= k_int; ++kz )
        {
            // compute wave vector component
            double _kz = 2.0 * PI / lz * (double)kz;
            for ( int ky = -k_int; ky <= k_int; ++ky )
            {
                // compute wave vector component
                double _ky = 2.0 * PI / ly * (double)ky;
                for ( int kx = -k_int; kx <= k_int; ++kx )
                {
                    // no values required for the central box
                    if ( kx == 0 && ky == 0 && kz == 0 )
                       continue;
                    // compute index in contribution array
                    int kidx =
                        ( kz + k_int ) * ( 2 * k_int + 1 ) * ( 2 * k_int + 1 ) * ( 2 * k_int + 1 ) +
                        ( ky + k_int ) * ( 2 * k_int + 1 ) + ( kx + k_int );
                    // compute wave vector component
                    double _kx = 2.0 * PI / lx * (double)kx;
                    // compute dot product with local particle and wave
                    // vector
                    double kr = _kz * x( idx, 0 ) + _ky * x( idx, 1 ) + _kz * x( idx, 2 );
                    //add contributions
                    Kokkos::atomic_add( &U_trigonometric( 2 * kidx ), qi * cos( kr ) );
                    Kokkos::atomic_add( &U_trigonometric( 2 * kidx + 1 ), qi * sin( kr ) );
                }
            }
        }
    };
    Kokkos::parallel_for( N_max, partial_sums );
    Kokkos::fence();

    //reduce the partial results
/*
    double *U_trigon_array = new double[2 * n_kvec];
    for ( int idx = 0; idx < 2 * n_kvec; ++idx )
        U_trigon_array[idx] = U_trigonometric( idx );
//TODO: remove MPI call here
    MPI_Allreduce( MPI_IN_PLACE, U_trigon_array, 2 * n_kvec, MPI_DOUBLE,
                   MPI_SUM, comm );

    for ( int idx = 0; idx < 2 * n_kvec; ++idx )
        U_trigonometric( idx ) = U_trigon_array[idx];

    delete[] U_trigon_array;
*/

    MPI_Allreduce( MPI_IN_PLACE, U_trigonometric.data(), 2 * n_kvec, MPI_DOUBLE,
                   MPI_SUM, comm );




    // In orig Ewald this was reduction to Uk
    // Now, it's a parallel_for to update each p(idx)
    auto kspace_potential = KOKKOS_LAMBDA( const int idx ) {
        // general coefficient
        double coeff = 4.0 * PI / ( lx * ly * lz );
        double k[3];
        
        double qi = q( idx );

        for ( int kz = -k_int; kz <= k_int; ++kz )
        {
            // compute wave vector component
            k[2] = 2.0 * PI / lz * (double)kz;
            for ( int ky = -k_int; ky <= k_int; ++ky )
            {
                // compute wave vector component
                k[1] = 2.0 * PI / ly * (double)ky;
                for ( int kx = -k_int; kx <= k_int; ++kx )
                {
                    // no values required for the central box
                    if ( kx == 0 && ky == 0 && kz == 0 )
                        continue;
                        // compute index in contribution array
                        int kidx = ( kz + k_int ) * ( 2 * k_int + 1 ) *
                                       ( 2 * k_int + 1 ) +
                                   ( ky + k_int ) * ( 2 * k_int + 1 ) +
                                   ( kx + k_int );
                        // compute wave vector component
                        k[0] = 2.0 * PI / lx * (double)kx;
                        // compute dot product of wave vector with itself
                        double kk = k[0] * k[0] + k[1] * k[1] + k[2] * k[2];
                        ;
                        // compute dot product with local particle and wave
                        // vector
                        double kr = k[0] * x( idx, 0 ) + k[1] * x( idx, 1 ) +
                                    k[2] * x( idx, 2 );

                        // coefficient dependent on wave vector
                        double k_coeff =
                            exp( -kk / ( 4 * alpha * alpha ) ) / kk;

                        // contribution to potential energy
                        double contrib =
                            coeff * k_coeff *
                            ( U_trigonometric( 2 * kidx ) *
                                  U_trigonometric( 2 * kidx ) +
                              U_trigonometric( 2 * kidx + 1 ) *
                                  U_trigonometric( 2 * kidx + 1 ) );
                        p( idx ) += contrib;
                        //Uk_part += contrib;

                        for ( int dim = 0; dim < 3; ++dim )
                            f( idx, dim ) +=
                              k_coeff * 2.0 * qi * k[dim] *
                                ( U_trigonometric( 2 * kidx + 1 ) * cos( kr ) -
                                  U_trigonometric( 2 * kidx ) * sin( kr ) );
                }
            }
        }
    };
    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, N_max ), kspace_potential );
    Kokkos::fence();

    //MPI_Allreduce( MPI_IN_PLACE, &Uk, 1, MPI_DOUBLE, MPI_SUM, comm );

    // computation real-space contribution
    //
    // In order to compute the real-space contribution to potentials and
    // forces the Cabana implementation of halos and Verlet lists is
    // used. The halos are used to communicate particles along the
    // borders of MPI domains to their respective neighbors, so that
    // complete Verlet lists can be created. To save computation time
    // the half shell variant is used, that means that Newton's third
    // law of motion is used: F(i,j) = -F(j,i). The downside of this
    // is that the computed partial forces and potentials of the
    // ghost particles need to be communicated back to the source
    // process, which is done by using the 'scatter' implementation
    // of Cabana.
    // TODO: Enable re-use of CabanaMD neighborlist from short-range forces

    
    // store the number of local particles
    //int n_local = n_max;

    // communicate particles along the edges of the system

    auto force_contribs = KOKKOS_LAMBDA( const int idx ) 
    {
        int num_n = Cabana::NeighborList<t_neighbor>::numNeighbor(neigh_list, idx); 

        double rx = x( idx, 0 );
        double ry = x( idx, 1 );
        double rz = x( idx, 2 );
        double qi = q( idx );

        for ( int ij = 0; ij < num_n; ++ij )
        {
            int j = Cabana::NeighborList<t_neighbor>::getNeighbor( neigh_list,
                                                                 idx, ij );
            double dx = x( j, 0 ) - rx;
            double dy = x( j, 1 ) - ry;
            double dz = x( j, 2 ) - rz;
            double d = sqrt( dx * dx + dy * dy + dz * dz );
            double qj = q( j );

            // potential computation
            double contrib = 0.5 * qi * qj * erfc( alpha * d ) / d;
            Kokkos::atomic_add( &p( idx ), contrib );
            Kokkos::atomic_add( &p( j ), contrib );

            // force computation
            double f_fact = qi * qj *
                            ( 2.0 * sqrt( alpha / PI ) * exp( -alpha * d * d ) +
                              erfc( sqrt( alpha ) * d ) ) /
                            ( d * d );
            Kokkos::atomic_add( &f( idx, 0 ), f_fact * dx );
            Kokkos::atomic_add( &f( idx, 1 ), f_fact * dy );
            Kokkos::atomic_add( &f( idx, 2 ), f_fact * dz );
            Kokkos::atomic_add( &f( j, 0 ), -f_fact * dx );
            Kokkos::atomic_add( &f( j, 1 ), -f_fact * dy );
            Kokkos::atomic_add( &f( j, 2 ), -f_fact * dz );
        }
    };
    Kokkos::parallel_for( N_local, force_contribs );
    Kokkos::fence();

/*
    // send the force and potential contributions of the
    // ghost particles back to the origin processes
    for ( int n_halo = 5; n_halo >= 0; --n_halo )
    {
        Cabana::scatter( *( halos.at( n_halo ) ), p );
        Cabana::scatter( *( halos.at( n_halo ) ), f );

        N_max -= halos.at( n_halo )->numGhost();
        system->xvf.resize( N_max );
        // update slices
        x = Cabana::slice<Positions>(system->xvf);
        f = Cabana::slice<Forces>(system->xvf);
        //f_a = Cabana::slice<Forces>(system->xvf);
        id = Cabana::slice<IDs>(system->xvf);
        //type = Cabana::slice<Types>(system->xvf);
        q = Cabana::slice<Charges>(system->xvf);
        p = Cabana::slice<Potentials>(system->xvf);
    }
*/

    // check if the particle array was reduced to the correct size again
    assert( N_max == N_local );
    //TODO: Fix translations between N_max,N_local here and in Rene's code


    // computation of self-energy contribution
    auto calc_Uself = KOKKOS_LAMBDA( int idx )
    {
        p( idx ) += -alpha / PI_SQRT * q( idx ) * q( idx );
    };
    Kokkos::parallel_for( Kokkos::RangePolicy<ExecutionSpace>( 0, N_max ), calc_Uself );
    Kokkos::fence();

    //Not including dipole correction (usually unnecessary)

}


template<class t_neighbor>
T_V_FLOAT ForceEwald<t_neighbor>::compute_energy(System* system) {

  //step++;
  return 0.0;
}

template<class t_neighbor>
const char* ForceEwald<t_neighbor>::name() {return "Ewald";}

