/* Copyright 2019 Andrew Myers, Aurore Blelly, Axel Huebl
 * Maxence Thevenet, Remi Lehe, Weiqun Zhang
 *
 *
 * This file is part of WarpX.
 *
 * License: BSD-3-Clause-LBNL
 */
#ifndef WARPX_PML_H_
#define WARPX_PML_H_

#include "PML_fwd.H"

#include "Utils/WarpXAlgorithmSelection.H"

#ifdef WARPX_USE_FFT
#   include "FieldSolver/SpectralSolver/SpectralSolver.H"
#endif

#include <AMReX_MultiFab.H>
#include <AMReX_BoxArray.H>
#include <AMReX_Config.H>
#include <AMReX_FabArray.H>
#include <AMReX_FabFactory.H>
#include <AMReX_GpuContainers.H>
#include <AMReX_IntVect.H>
#include <AMReX_REAL.H>
#include <AMReX_Vector.H>

#include <AMReX_BaseFwd.H>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct Sigma : amrex::Gpu::DeviceVector<amrex::Real>
{
    [[nodiscard]] int lo() const { return m_lo; }
    [[nodiscard]] int hi() const { return m_hi; }
    int m_lo, m_hi;
};

struct SigmaBox
{
    SigmaBox (const amrex::Box& box, const amrex::BoxArray& grids,
              const amrex::Real* dx, const amrex::IntVect& ncell, const amrex::IntVect& delta,
              const amrex::Box& regdomain, amrex::Real v_sigma);

    void define_single (const amrex::Box& regdomain, const amrex::IntVect& ncell,
                        const amrex::Array<amrex::Real,AMREX_SPACEDIM>& fac,
                        amrex::Real v_sigma);
    void define_multiple (const amrex::Box& box, const amrex::BoxArray& grids,
                          const amrex::IntVect& ncell,
                          const amrex::Array<amrex::Real,AMREX_SPACEDIM>& fac,
                          amrex::Real v_sigma);

    void ComputePMLFactorsB (const amrex::Real* dx, amrex::Real dt);
    void ComputePMLFactorsE (const amrex::Real* dx, amrex::Real dt);

    using SigmaVect = std::array<Sigma,AMREX_SPACEDIM>;

    using value_type = void; // needed by amrex::FabArray

    SigmaVect sigma;
    SigmaVect sigma_cumsum;
    SigmaVect sigma_star;
    SigmaVect sigma_star_cumsum;
    SigmaVect sigma_fac;
    SigmaVect sigma_cumsum_fac;
    SigmaVect sigma_star_fac;
    SigmaVect sigma_star_cumsum_fac;
    amrex::Real v_sigma;

};

class SigmaBoxFactory
    : public amrex::FabFactory<SigmaBox>
{
public:
    SigmaBoxFactory (const amrex::BoxArray& grid_ba, const amrex::Real* dx,
                     const amrex::IntVect& ncell, const amrex::IntVect& delta,
                     const amrex::Box& regular_domain, const amrex::Real v_sigma_sb)
        : m_grids(grid_ba), m_dx(dx), m_ncell(ncell), m_delta(delta), m_regdomain(regular_domain), m_v_sigma_sb(v_sigma_sb) {}
    ~SigmaBoxFactory () override = default;

    SigmaBoxFactory (const SigmaBoxFactory&) = default;
    SigmaBoxFactory (SigmaBoxFactory&&) noexcept = default;

    SigmaBoxFactory () = delete;
    SigmaBoxFactory& operator= (const SigmaBoxFactory&) = delete;
    SigmaBoxFactory& operator= (SigmaBoxFactory&&) = delete;

    [[nodiscard]] SigmaBox* create (const amrex::Box& box, int /*ncomps*/,
        const amrex::FabInfo& /*info*/, int /*box_index*/) const final
    {
        return new SigmaBox(box, m_grids, m_dx, m_ncell, m_delta, m_regdomain, m_v_sigma_sb);
    }

    void destroy (SigmaBox* fab) const final
    {
        delete fab;
    }

    [[nodiscard]] SigmaBoxFactory*
    clone () const final
    {
        return new SigmaBoxFactory(*this);
    }

private:
    const amrex::BoxArray& m_grids;
    const amrex::Real* m_dx;
    amrex::IntVect m_ncell;
    amrex::IntVect m_delta;
    amrex::Box m_regdomain;
    amrex::Real m_v_sigma_sb;
};

class MultiSigmaBox
    : public amrex::FabArray<SigmaBox>
{
public:
    MultiSigmaBox(const amrex::BoxArray& ba, const amrex::DistributionMapping& dm,
                  const amrex::BoxArray& grid_ba, const amrex::Real* dx,
                  const amrex::IntVect& ncell, const amrex::IntVect& delta,
                  const amrex::Box& regular_domain, amrex::Real v_sigma_sb);
    void ComputePMLFactorsB (const amrex::Real* dx, amrex::Real dt);
    void ComputePMLFactorsE (const amrex::Real* dx, amrex::Real dt);
private:
    amrex::Real dt_B = -1.e10;
    amrex::Real dt_E = -1.e10;
};

class PML
{
public:
    PML (int lev, const amrex::BoxArray& ba,
         const amrex::DistributionMapping& dm, bool do_similar_dm_pml,
         const amrex::Geometry* geom, const amrex::Geometry* cgeom,
         int ncell, int delta, amrex::IntVect ref_ratio,
         amrex::Real dt, int nox_fft, int noy_fft, int noz_fft, short grid_type,
         int do_moving_window, int pml_has_particles, int do_pml_in_domain,
         int psatd_solution_type, int J_in_time, int rho_in_time,
         bool do_pml_dive_cleaning, bool do_pml_divb_cleaning,
         const amrex::IntVect& fill_guards_fields,
         const amrex::IntVect& fill_guards_current,
         int max_guard_EB, amrex::Real v_sigma_sb,
         amrex::IntVect do_pml_Lo = amrex::IntVect::TheUnitVector(),
         amrex::IntVect do_pml_Hi = amrex::IntVect::TheUnitVector());

    void ComputePMLFactors (amrex::Real dt);

    std::array<amrex::MultiFab*,3> GetE_fp ();
    std::array<amrex::MultiFab*,3> GetB_fp ();
    std::array<amrex::MultiFab*,3> Getj_fp ();
    std::array<amrex::MultiFab*,3> GetE_cp ();
    std::array<amrex::MultiFab*,3> GetB_cp ();
    std::array<amrex::MultiFab*,3> Getj_cp ();
    std::array<amrex::MultiFab*,3> Get_edge_lengths ();
    std::array<amrex::MultiFab*,3> Get_face_areas ();

    // Used when WarpX::do_pml_dive_cleaning = true
    amrex::MultiFab* GetF_fp ();
    amrex::MultiFab* GetF_cp ();

    // Used when WarpX::do_pml_divb_cleaning = true
    amrex::MultiFab* GetG_fp ();
    amrex::MultiFab* GetG_cp ();

    [[nodiscard]] const MultiSigmaBox& GetMultiSigmaBox_fp () const
    {
        return *sigba_fp;
    }

    [[nodiscard]] const MultiSigmaBox& GetMultiSigmaBox_cp () const
    {
        return *sigba_cp;
    }

#ifdef WARPX_USE_FFT
    void PushPSATD (int lev);
#endif

    void CopyJtoPMLs (const std::array<amrex::MultiFab*,3>& j_fp,
                    const std::array<amrex::MultiFab*,3>& j_cp);

    void Exchange (const std::array<amrex::MultiFab*,3>& mf_pml,
                   const std::array<amrex::MultiFab*,3>& mf,
                   const PatchType& patch_type,
                   int do_pml_in_domain);

    void CopyJtoPMLs (PatchType patch_type,
                    const std::array<amrex::MultiFab*,3>& jp);

    void ExchangeF (amrex::MultiFab* F_fp, amrex::MultiFab* F_cp, int do_pml_in_domain);
    void ExchangeF (PatchType patch_type, amrex::MultiFab* Fp, int do_pml_in_domain);

    void ExchangeG (amrex::MultiFab* G_fp, amrex::MultiFab* G_cp, int do_pml_in_domain);
    void ExchangeG (PatchType patch_type, amrex::MultiFab* Gp, int do_pml_in_domain);

    void FillBoundaryE (PatchType patch_type, std::optional<bool> nodal_sync=std::nullopt);
    void FillBoundaryB (PatchType patch_type, std::optional<bool> nodal_sync=std::nullopt);
    void FillBoundaryF (PatchType patch_type, std::optional<bool> nodal_sync=std::nullopt);
    void FillBoundaryG (PatchType patch_type, std::optional<bool> nodal_sync=std::nullopt);

    [[nodiscard]] bool ok () const { return m_ok; }

    void CheckPoint (const std::string& dir) const;
    void Restart (const std::string& dir);

    static void Exchange (amrex::MultiFab& pml, amrex::MultiFab& reg, const amrex::Geometry& geom, int do_pml_in_domain);

private:
    bool m_ok;

    bool m_dive_cleaning;
    bool m_divb_cleaning;

    const amrex::IntVect m_fill_guards_fields;
    const amrex::IntVect m_fill_guards_current;

    const amrex::Geometry* m_geom;
    const amrex::Geometry* m_cgeom;

    std::array<std::unique_ptr<amrex::MultiFab>,3> pml_E_fp;
    std::array<std::unique_ptr<amrex::MultiFab>,3> pml_B_fp;
    std::array<std::unique_ptr<amrex::MultiFab>,3> pml_j_fp;

    std::array<std::unique_ptr<amrex::MultiFab>,3> pml_edge_lengths;

    std::array<std::unique_ptr<amrex::MultiFab>,3> pml_E_cp;
    std::array<std::unique_ptr<amrex::MultiFab>,3> pml_B_cp;
    std::array<std::unique_ptr<amrex::MultiFab>,3> pml_j_cp;

    // Used when WarpX::do_pml_dive_cleaning = true
    std::unique_ptr<amrex::MultiFab> pml_F_fp;
    std::unique_ptr<amrex::MultiFab> pml_F_cp;

    // Used when WarpX::do_pml_divb_cleaning = true
    std::unique_ptr<amrex::MultiFab> pml_G_fp;
    std::unique_ptr<amrex::MultiFab> pml_G_cp;

    std::unique_ptr<MultiSigmaBox> sigba_fp;
    std::unique_ptr<MultiSigmaBox> sigba_cp;

#ifdef WARPX_USE_FFT
    std::unique_ptr<SpectralSolver> spectral_solver_fp;
    std::unique_ptr<SpectralSolver> spectral_solver_cp;
#endif

    // Factory for field data
    std::unique_ptr<amrex::FabFactory<amrex::FArrayBox> > pml_field_factory;

    [[nodiscard]] amrex::FabFactory<amrex::FArrayBox> const& fieldFactory () const noexcept
    {
        return *pml_field_factory;
    }

#ifdef AMREX_USE_EB
    amrex::EBFArrayBoxFactory const& fieldEBFactory () const noexcept {
        return static_cast<amrex::EBFArrayBoxFactory const&>(*pml_field_factory);
    }
#endif

    static amrex::BoxArray MakeBoxArray (bool single_box_domain,
                                         const amrex::Box& regular_domain,
                                         const amrex::Geometry& geom,
                                         const amrex::BoxArray& grid_ba,
                                         const amrex::IntVect& ncell,
                                         int do_pml_in_domain,
                                         const amrex::IntVect& do_pml_Lo,
                                         const amrex::IntVect& do_pml_Hi);

    static amrex::BoxArray MakeBoxArray_single (const amrex::Box& regular_domain,
                                                const amrex::BoxArray& grid_ba,
                                                const amrex::IntVect& ncell,
                                                const amrex::IntVect& do_pml_Lo,
                                                const amrex::IntVect& do_pml_Hi);

    static amrex::BoxArray MakeBoxArray_multiple (const amrex::Geometry& geom,
                                                  const amrex::BoxArray& grid_ba,
                                                  const amrex::IntVect& ncell,
                                                  int do_pml_in_domain,
                                                  const amrex::IntVect& do_pml_Lo,
                                                  const amrex::IntVect& do_pml_Hi);

    static void CopyToPML (amrex::MultiFab& pml, amrex::MultiFab& reg, const amrex::Geometry& geom);
};

#ifdef WARPX_USE_FFT
void PushPMLPSATDSinglePatch( int lev,
    SpectralSolver& solver,
    std::array<std::unique_ptr<amrex::MultiFab>,3>& pml_E,
    std::array<std::unique_ptr<amrex::MultiFab>,3>& pml_B,
    std::unique_ptr<amrex::MultiFab>& pml_F,
    std::unique_ptr<amrex::MultiFab>& pml_G,
    const amrex::IntVect& fill_guards);
#endif

#endif
