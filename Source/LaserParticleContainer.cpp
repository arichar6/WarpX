
#include <limits>
#include <cmath>
#include <algorithm>
#include <numeric>

#include <WarpX.H>
#include <WarpXConst.H>
#include <WarpX_f.H>
#include <ParticleContainer.H>

using namespace amrex;

namespace
{
    Vector<Real> CrossProduct (const Vector<Real>& a, const Vector<Real>& b)
    {
	return { a[1]*b[2]-a[2]*b[1],  a[2]*b[0]-a[0]*b[2],  a[0]*b[1]-a[1]*b[0] };
    }
}

LaserParticleContainer::LaserParticleContainer (AmrCore* amr_core, int ispecies)
    : WarpXParticleContainer(amr_core, ispecies)
{
    charge = 1.0;
    mass = std::numeric_limits<Real>::max();

    if (WarpX::use_laser)
    {
	ParmParse pp("laser");

	// Parse the type of laser profile and set the corresponding flag `profile`
	std::string laser_type_s;
	pp.get("profile", laser_type_s);
	std::transform(laser_type_s.begin(), laser_type_s.end(), laser_type_s.begin(), ::tolower);
	if (laser_type_s == "gaussian") {
	    profile = laser_t::Gaussian;
  } else if(laser_type_s == "harris") {
      profile = laser_t::Harris;
  } else if(laser_type_s == "parse_field_function") {
      profile = laser_t::parse_field_function;
	} else {
	    amrex::Abort("Unknown laser type");
	}

	// Parse the properties of the antenna
	pp.getarr("position", position);
	pp.getarr("direction", nvec);
	pp.getarr("polarization", p_X);
	pp.query("pusher_algo", pusher_algo);
	pp.get("wavelength", wavelength);
	pp.get("e_max", e_max);

	if ( profile == laser_t::Gaussian ) {
	    // Parse the properties of the Gaussian profile
	   pp.get("profile_waist", profile_waist);
	   pp.get("profile_duration", profile_duration);
	   pp.get("profile_t_peak", profile_t_peak);
	   pp.get("profile_focal_distance", profile_focal_distance);
	   pp.query("zeta", zeta);
	   pp.query("beta", beta);
	   pp.query("phi2", phi2);
	}

  if ( profile == laser_t::Harris ) {
    // Parse the properties of the Harris profile
    pp.get("profile_waist", profile_waist);
    pp.get("profile_duration", profile_duration);
    pp.get("profile_focal_distance", profile_focal_distance);
  }

  if ( profile == laser_t::parse_field_function ) {
    // Parse the properties of the parse_field_function profile
    pp.get("field_function(X,Y,t)", field_function);
    // User-defined constants: replace names by value
    my_constants.ReadParameters();
    field_function = my_constants.replaceStringValue(field_function);
    // Pass math expression and list of variables to Fortran as char*
    const std::string s_var = "X,Y,t";
    parser_instance_number = parser_initialize_function(field_function.c_str(),
                                                        field_function.length(),
                                                        s_var.c_str(),
                                                        s_var.length());
  }

	// Plane normal
	Real s = 1.0/std::sqrt(nvec[0]*nvec[0] + nvec[1]*nvec[1] + nvec[2]*nvec[2]);
	nvec = { nvec[0]*s, nvec[1]*s, nvec[2]*s };

    if (WarpX::gamma_boost > 1.) {
        // Check that the laser direction is equal to the boost direction
        AMREX_ALWAYS_ASSERT_WITH_MESSAGE( 
            nvec[0]*WarpX::boost_direction[0]
          + nvec[1]*WarpX::boost_direction[1]
          + nvec[2]*WarpX::boost_direction[2] - 1. < 1.e-12,
          "The Lorentz boost should be in the same direction as the laser propagation");
        // Get the position of the plane, along the boost direction, in the lab frame
        // and convert the position of the antenna to the boosted frame
        Z0_lab = nvec[0]*position[0] + nvec[1]*position[1] + nvec[2]*position[2];
        Real Z0_boost = Z0_lab/WarpX::gamma_boost;
        position[0] += (Z0_boost-Z0_lab)*nvec[0];
        position[1] += (Z0_boost-Z0_lab)*nvec[1];
        position[2] += (Z0_boost-Z0_lab)*nvec[2];
    }

	// The first polarization vector
	s = 1.0/std::sqrt(p_X[0]*p_X[0] + p_X[1]*p_X[1] + p_X[2]*p_X[2]);
	p_X = { p_X[0]*s, p_X[1]*s, p_X[2]*s };

	Real dp = std::inner_product(nvec.begin(), nvec.end(), p_X.begin(), 0.0);
        AMREX_ALWAYS_ASSERT_WITH_MESSAGE(std::abs(dp) < 1.0e-14, 
            "Laser plane vector is not perpendicular to the main polarization vector");

	p_Y = CrossProduct(nvec, p_X);   // The second polarization vector

#if AMREX_SPACEDIM == 3
	u_X = p_X;
	u_Y = p_Y;
#else
	u_X = CrossProduct({0., 1., 0.}, nvec);
	u_Y = {0., 1., 0.};
#endif

        prob_domain = Geometry::ProbDomain();
        {
            Vector<Real> lo, hi;
            if (pp.queryarr("prob_lo", lo)) {
                prob_domain.setLo(lo);
            }
            if (pp.queryarr("prob_hi", hi)) {
                prob_domain.setHi(hi);
            }
        }
    }
}

void
LaserParticleContainer::InitData ()
{
    InitData(maxLevel());
}

void
LaserParticleContainer::InitData (int lev)
{
    // spacing of laser particles in the laser plane.
    // has to be done after geometry is set up.
    Real S_X, S_Y;
    ComputeSpacing(lev, S_X, S_Y);
    ComputeWeightMobility(S_X, S_Y);

    auto Transform = [&](int i, int j) -> Vector<Real>
    {
#if (AMREX_SPACEDIM == 3)
	return { position[0] + (S_X*(i+0.5))*u_X[0] + (S_Y*(j+0.5))*u_Y[0],
		 position[1] + (S_X*(i+0.5))*u_X[1] + (S_Y*(j+0.5))*u_Y[1],
		 position[2] + (S_X*(i+0.5))*u_X[2] + (S_Y*(j+0.5))*u_Y[2] };
#else
	return { position[0] + (S_X*(i+0.5))*u_X[0],
		 0.0,
		 position[2] + (S_X*(i+0.5))*u_X[2] };
#endif
    };

    // Given the "lab" frame coordinates, return the real coordinates in the laser plane coordinates
    auto InverseTransform = [&](const Vector<Real>& pos) -> Vector<Real>
    {
#if (AMREX_SPACEDIM == 3)
	return {u_X[0]*(pos[0]-position[0])+u_X[1]*(pos[1]-position[1])+u_X[2]*(pos[2]-position[2]),
		u_Y[0]*(pos[0]-position[0])+u_Y[1]*(pos[1]-position[1])+u_Y[2]*(pos[2]-position[2])};
#else
	return {u_X[0]*(pos[0]-position[0])+u_X[2]*(pos[2]-position[2]), 0.0};
#endif
    };

    Vector<int> plane_lo(2, std::numeric_limits<int>::max());
    Vector<int> plane_hi(2, std::numeric_limits<int>::min());
    {
	auto compute_min_max = [&](Real x, Real y, Real z)
	{
	    const Vector<Real>& pos_plane = InverseTransform({x, y, z});
	    int i = pos_plane[0]/S_X;
	    int j = pos_plane[1]/S_Y;
	    plane_lo[0] = std::min(plane_lo[0], i);
	    plane_lo[1] = std::min(plane_lo[1], j);
	    plane_hi[0] = std::max(plane_hi[0], i);
	    plane_hi[1] = std::max(plane_hi[1], j);
	};

	const Real* prob_lo = prob_domain.lo();
	const Real* prob_hi = prob_domain.hi();
#if (AMREX_SPACEDIM == 3)
	compute_min_max(prob_lo[0], prob_lo[1], prob_lo[2]);
	compute_min_max(prob_hi[0], prob_lo[1], prob_lo[2]);
	compute_min_max(prob_lo[0], prob_hi[1], prob_lo[2]);
	compute_min_max(prob_hi[0], prob_hi[1], prob_lo[2]);
	compute_min_max(prob_lo[0], prob_lo[1], prob_hi[2]);
	compute_min_max(prob_hi[0], prob_lo[1], prob_hi[2]);
	compute_min_max(prob_lo[0], prob_hi[1], prob_hi[2]);
	compute_min_max(prob_hi[0], prob_hi[1], prob_hi[2]);
#else
	compute_min_max(prob_lo[0], 0.0, prob_lo[1]);
	compute_min_max(prob_hi[0], 0.0, prob_lo[1]);
	compute_min_max(prob_lo[0], 0.0, prob_hi[1]);
	compute_min_max(prob_hi[0], 0.0, prob_hi[1]);
#endif
    }

    const int nprocs = ParallelDescriptor::NProcs();
    const int myproc = ParallelDescriptor::MyProc();

#if (AMREX_SPACEDIM == 3)
    const Box plane_box {IntVect(plane_lo[0],plane_lo[1],0),
                         IntVect(plane_hi[0],plane_hi[1],0)};
    BoxArray plane_ba {plane_box};
    {
	IntVect chunk(plane_box.size());
	const int min_size = 8;
	while (plane_ba.size() < nprocs && chunk[0] > min_size && chunk[1] > min_size)
	{
	    for (int j = 1; j >= 0 ; j--)
	    {
		chunk[j] /= 2;

		if (plane_ba.size() < nprocs) {
		    plane_ba.maxSize(chunk);
		}
	    }
	}
    }
#else
    BoxArray plane_ba { Box {IntVect(plane_lo[0],0), IntVect(plane_hi[0],0)} };
#endif

    RealVector particle_x, particle_y, particle_z, particle_w;

    const DistributionMapping plane_dm {plane_ba, nprocs};
    const Vector<int>& procmap = plane_dm.ProcessorMap();
    for (int i = 0, n = plane_ba.size(); i < n; ++i)
    {
	if (procmap[i] == myproc)
	{
	    const Box& bx = plane_ba[i];
	    for (IntVect cell = bx.smallEnd(); cell <= bx.bigEnd(); bx.next(cell))
	    {
		const Vector<Real>& pos = Transform(cell[0], cell[1]);
#if (AMREX_SPACEDIM == 3)
		const Real* x = pos.dataPtr();
#else
		const Real x[2] = {pos[0], pos[2]};
#endif
		if (prob_domain.contains(x))
		{
		    for (int k = 0; k<2; ++k) {
			particle_x.push_back(pos[0]);
			particle_y.push_back(pos[1]);
			particle_z.push_back(pos[2]);
		    }
		    particle_w.push_back( weight);
		    particle_w.push_back(-weight);
		}
	    }
	}
    }
    const int np = particle_z.size();
    RealVector particle_ux(np, 0.0);
    RealVector particle_uy(np, 0.0);
    RealVector particle_uz(np, 0.0);

    if (Verbose()) amrex::Print() << "Adding laser particles\n";
    AddNParticles(lev,
                  np, particle_x.dataPtr(), particle_y.dataPtr(), particle_z.dataPtr(),
		  particle_ux.dataPtr(), particle_uy.dataPtr(), particle_uz.dataPtr(),
		  1, particle_w.dataPtr(), 1);
}

void
LaserParticleContainer::Evolve (int lev,
				const MultiFab&, const MultiFab&, const MultiFab&,
				const MultiFab&, const MultiFab&, const MultiFab&,
				MultiFab& jx, MultiFab& jy, MultiFab& jz,
                                MultiFab* cjx, MultiFab* cjy, MultiFab* cjz,
                                MultiFab* rho, MultiFab* crho,
                                const MultiFab*, const MultiFab*, const MultiFab*,
                                const MultiFab*, const MultiFab*, const MultiFab*,
                                Real t, Real dt)
{
    BL_PROFILE("Laser::Evolve()");
    BL_PROFILE_VAR_NS("Laser::Evolve::Copy", blp_copy);
    BL_PROFILE_VAR_NS("PICSAR::LaserParticlePush", blp_pxr_pp);
    BL_PROFILE_VAR_NS("PICSAR::LaserCurrentDepo", blp_pxr_cd);
    BL_PROFILE_VAR_NS("Laser::Evolve::Accumulate", blp_accumulate);

    Real t_lab = t;
    if (WarpX::gamma_boost > 1) {
        // Convert time from the boosted to the lab-frame
        // (in order to later calculate the amplitude of the field,
        // at the position of the antenna, in the lab-frame)
        t_lab = 1./WarpX::gamma_boost*t + WarpX::beta_boost*Z0_lab/PhysConst::c;
    }

    BL_ASSERT(OnSameGrids(lev,jx));

    MultiFab* cost = WarpX::getCosts(lev);

#ifdef _OPENMP
#pragma omp parallel
#endif
    {
#ifdef _OPENMP
      int thread_num = omp_get_thread_num();
#else
      int thread_num = 0;
#endif

      if (local_rho[thread_num] == nullptr) local_rho[thread_num].reset( new amrex::FArrayBox());
      if (local_jx[thread_num]  == nullptr) local_jx[thread_num].reset( new amrex::FArrayBox());
      if (local_jy[thread_num]  == nullptr) local_jy[thread_num].reset(  new amrex::FArrayBox());
      if (local_jz[thread_num]  == nullptr) local_jz[thread_num].reset(  new amrex::FArrayBox());
      
        Cuda::DeviceVector<Real> plane_Xp, plane_Yp, amplitude_E;

        for (WarpXParIter pti(*this, lev); pti.isValid(); ++pti)
	{
            Real wt = amrex::second();

	    const Box& box = pti.validbox();

            auto& attribs = pti.GetAttribs();

            auto&  wp = attribs[PIdx::w ];
            auto& uxp = attribs[PIdx::ux];
            auto& uyp = attribs[PIdx::uy];
            auto& uzp = attribs[PIdx::uz];

	    const long np  = pti.numParticles();
            // For now, laser particles do not take the current buffers into account
            const long np_current = np;

            m_giv[thread_num].resize(np);

               plane_Xp.resize(np);
               plane_Yp.resize(np);
            amplitude_E.resize(np);

	    //
	    // copy data from particle container to temp arrays
	    //
	    BL_PROFILE_VAR_START(blp_copy);
            pti.GetPosition(m_xp[thread_num], m_yp[thread_num], m_zp[thread_num]);
	    BL_PROFILE_VAR_STOP(blp_copy);

            if (rho) DepositCharge(pti, wp, rho, crho, 0, np_current, np, thread_num, lev);

	    //
	    // Particle Push
	    //
	    BL_PROFILE_VAR_START(blp_pxr_pp);
            // Find the coordinates of the particles in the emission plane
            calculate_laser_plane_coordinates( &np,
                m_xp[thread_num].dataPtr(),
                m_yp[thread_num].dataPtr(),
                m_zp[thread_num].dataPtr(),
                plane_Xp.dataPtr(), plane_Yp.dataPtr(),
                &u_X[0], &u_X[1], &u_X[2], &u_Y[0], &u_Y[1], &u_Y[2],
                &position[0], &position[1], &position[2] );
	    // Calculate the laser amplitude to be emitted,
	    // at the position of the emission plane
	    if (profile == laser_t::Gaussian) {
		warpx_gaussian_laser( &np, plane_Xp.dataPtr(), plane_Yp.dataPtr(),
				      &t_lab, &wavelength, &e_max, &profile_waist, &profile_duration,
				      &profile_t_peak, &profile_focal_distance, amplitude_E.dataPtr(),
				      &zeta, &beta, &phi2 );
	    }

            if (profile == laser_t::Harris) {
		warpx_harris_laser( &np, plane_Xp.dataPtr(), plane_Yp.dataPtr(),
                                    &t, &wavelength, &e_max, &profile_waist, &profile_duration,
                                    &profile_focal_distance, amplitude_E.dataPtr() );
	    }

            if (profile == laser_t::parse_field_function) {
		parse_function_laser( &np, plane_Xp.dataPtr(), plane_Yp.dataPtr(), &t,
				      amplitude_E.dataPtr(), parser_instance_number );
	    }
	    // Calculate the corresponding momentum and position for the particles
            update_laser_particle(
               &np,
               m_xp[thread_num].dataPtr(),
               m_yp[thread_num].dataPtr(),
               m_zp[thread_num].dataPtr(),
               uxp.dataPtr(), uyp.dataPtr(), uzp.dataPtr(),
               m_giv[thread_num].dataPtr(),
               wp.dataPtr(), amplitude_E.dataPtr(), &p_X[0], &p_X[1], &p_X[2],
               &nvec[0], &nvec[1], &nvec[2], &mobility, &dt,
               &PhysConst::c, &WarpX::beta_boost, &WarpX::gamma_boost );
	    BL_PROFILE_VAR_STOP(blp_pxr_pp);

	    //
	    // Current Deposition
	    //
            DepositCurrent(pti, wp, uxp, uyp, uzp, jx, jy, jz,
                           cjx, cjy, cjz, np_current, np, thread_num, lev, dt);

	    //
	    // copy particle data back
	    //
	    BL_PROFILE_VAR_START(blp_copy);
            pti.SetPosition(m_xp[thread_num], m_yp[thread_num], m_zp[thread_num]);
            BL_PROFILE_VAR_STOP(blp_copy);

            if (rho) DepositCharge(pti, wp, rho, crho, 1, np_current, np, thread_num, lev);
            
            if (cost) {
                const Box& tbx = pti.tilebox();
                wt = (amrex::second() - wt) / tbx.d_numPts();
                (*cost)[pti].plus(wt, tbx);
            }
	}
    }
}

void
LaserParticleContainer::PostRestart ()
{
    Real Sx, Sy;
    const int lev = finestLevel();
    ComputeSpacing(lev, Sx, Sy);
    ComputeWeightMobility(Sx, Sy);
}

void
LaserParticleContainer::ComputeSpacing (int lev, Real& Sx, Real& Sy) const
{
    const std::array<Real,3>& dx = WarpX::CellSize(lev);

    const Real eps = dx[0]*1.e-50;
#if (AMREX_SPACEDIM == 3)
    Sx = std::min(std::min(dx[0]/(std::abs(u_X[0])+eps),
			   dx[1]/(std::abs(u_X[1])+eps)),
		           dx[2]/(std::abs(u_X[2])+eps));
    Sy = std::min(std::min(dx[0]/(std::abs(u_Y[0])+eps),
			   dx[1]/(std::abs(u_Y[1])+eps)),
		           dx[2]/(std::abs(u_Y[2])+eps));
#else
    Sx = std::min(dx[0]/(std::abs(u_X[0])+eps),
		  dx[2]/(std::abs(u_X[2])+eps));
    Sy = 1.0;
#endif
}

void
LaserParticleContainer::ComputeWeightMobility (Real Sx, Real Sy)
{
    constexpr Real eps = 0.01;
    constexpr Real fac = 1.0/(2.0*3.1415926535897932*PhysConst::mu0*PhysConst::c*PhysConst::c*eps);
    weight = fac * wavelength * Sx * Sy / std::min(Sx,Sy) * e_max;

    // The mobility is the constant of proportionality between the field to
    // be emitted, and the corresponding velocity that the particles need to have.
    mobility = (Sx * Sy)/(weight * PhysConst::mu0 * PhysConst::c * PhysConst::c);
    // When running in the boosted-frame, the input parameters (and in particular
    // the amplitude of the field) are given in the lab-frame.
    // Therefore, the mobility needs to be modified by a factor WarpX::gamma_boost.
    mobility = mobility/WarpX::gamma_boost;
}

void
LaserParticleContainer::PushP (int lev, Real dt,
                               const MultiFab&, const MultiFab&, const MultiFab&,
                               const MultiFab&, const MultiFab&, const MultiFab&)
{
    // I don't think we need to do anything.
}
