! tcrad_flux_interface.F90 - Interface routines for TCRAD fluxes -*- f90 -*-
!
! (C) Copyright 2021- ECMWF.
!
! This software is licensed under the terms of the Apache Licence Version 2.0
! which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
!
! In applying this licence, ECMWF does not waive the privileges and immunities
! granted to it by virtue of its status as an intergovernmental organisation
! nor does it submit to any jurisdiction.
!
! Author:  Robin Hogan
! Email:   r.j.hogan@ecmwf.int
!
!
! This file is included in modules specifying the NREGION parameter
! (typically 2 or 3) which makes this routine use a "doubleclouds" or
! "tripleclouds" assumption.
!

!---------------------------------------------------------------------
! Compute the flux profile including the effects of scattering, either
! using the classic Tripleclouds solver alone, or using it to compute
! the source function for subsequent radiance calculations.
subroutine calc_flux(nspec, nlev, surf_emission, surf_albedo, planck_hl, &
     &  cloud_fraction, &
#if NUM_REGIONS == 3
     &  fractional_std, &
#endif
     &  od_clear, od_cloud, &
     &  ssa_cloud, asymmetry_cloud, &
     &  overlap_param, flux_up, flux_dn, n_angles_per_hem, do_3d_effects, &
     &  cloud_cover)

  use parkind1, only           : jpim, jprb
  use yomhook,  only           : lhook, dr_hook
  use tcrad_layer_solutions, only   : calc_reflectance_transmittance, &
       &  calc_radiance_source, gauss_legendre, &
       &  LW_DIFFUSIVITY, MAX_GAUSS_LEGENDRE_POINTS

  implicit none

  ! Inputs

  ! Number of spectral intervals and levels. Note that all
  ! level-dependent variables count down from top-of-atmosphere.
  integer(jpim), intent(in) :: nspec, nlev

  ! Surface upwards emission, in W m-2 (i.e. emissivity multiplied
  ! by Planck function at the surface skin temperature) integrated
  ! across each spectral interval, and albedo in the same intervals
  real(jprb), intent(in), dimension(nspec) :: surf_emission, surf_albedo

  ! Planck function integrated over each spectral interval at each
  ! half-level, in W m-2 (i.e. the flux emitted by a horizontal
  ! black-body surface)
  real(jprb), intent(in), dimension(nspec,nlev+1) :: planck_hl

  ! Profile of cloud fraction 
  real(jprb), intent(in), dimension(nlev) :: cloud_fraction

#if NUM_REGIONS == 3
  ! Profile of the fractional standard deviation (i.e. standard
  ! deviation divided by mean) of the horizontal in-cloud water
  ! content (or cloud extinction coefficient) distribution.
  real(jprb), intent(in), dimension(nlev) :: fractional_std
#endif

  ! Layer optical depth of gas and aerosol
  real(jprb), intent(in), dimension(nspec,nlev) :: od_clear

  ! Layer optical depth of cloud averaged only over the cloudy part
  ! of the gridbox, single scattering albedo and asymmetry
  ! factor. If delta-Eddington scaling is required then this should
  ! already have been done.
  real(jprb), intent(in), dimension(nspec,nlev) :: od_cloud, &
       &  ssa_cloud, asymmetry_cloud

  ! Overlap parameter governing how clouds in adjacent layers are
  ! overlapped - this is the "alpha" of Hogan and Illingworth
  ! (2000). It is defined only between layers, hence the nlev-1
  ! elements.
  real(jprb), intent(in), dimension(nlev-1) :: overlap_param

  ! Outputs

  ! Upwelling and downwelling fluxes in each spectral interval at
  ! each half-level (W m-2)
  real(jprb), intent(out), dimension(nspec,nlev+1) :: flux_up, flux_dn

  ! Optional inputs

  ! Number of angles to compute radiances per hemisphere, for
  ! example, 2 results in the delta-2-plus-4 algorithm recommended
  ! by Fu et al. (1997). A value of 0 (the default) indicates to use
  ! the output from the two-stream Tripleclouds flux calculation
  ! directly.
  integer,    intent(in), optional :: n_angles_per_hem

  ! Do we represent 3D effects in the radiance calculations?
  logical,    intent(in), optional :: do_3d_effects

  ! Return cloud cover computed from cloud fraction profile and
  ! overlap rules
  real(jprb), intent(out), optional :: cloud_cover

  ! Local variables

  ! Combined gas/aerosol/cloud optical depth in each region
  real(jprb), dimension(nspec,NREGION,nlev)   :: od

  ! Single scattering albedo of the cloudy regions (ssa=0 in the
  ! clear region)
  real(jprb), dimension(nspec,2:NREGION,nlev) :: ssa

  ! Reflectance and transmittance of each layer and region
  real(jprb), dimension(nspec,NREGION,nlev) :: reflectance, transmittance

  ! Rate of emission up from the top or down through the base of
  ! each layer and region (W m-2)
  real(jprb), dimension(nspec,NREGION,nlev) :: source_up, source_dn

  ! Which layers are cloud-free?  Dummy cloud-free layers are added
  ! above TOA (level 0) and below the ground (level nlev+1).
  logical :: is_cloud_free_layer(0:nlev+1)

  ! Upward and downward overlap matrices - see Hogan et al. (JGR
  ! 2016) for definitions
  real(jprb), dimension(NREGION,NREGION,nlev+1) :: u_overlap, v_overlap

  ! Upwelling and downwelling fluxes at the top and base of each
  ! layer in each region, in W m-2
  real(jprb), dimension(nspec,NREGION,nlev) :: flux_up_base, flux_dn_base
  real(jprb), dimension(nspec,NREGION,nlev) :: flux_up_top, flux_dn_top

  ! Cloud optical depth scaling in each cloudy region
  real(jprb) :: od_scaling(2:NREGION,nlev)

  ! Fractional area coverage of each region
  real(jprb) :: region_fracs(1:NREGION,nlev)

  ! Cloud fractions below this are ignored
  real(jprb), parameter :: cloud_fraction_threshold = 1.0e-6

  ! Gauss-Legendre points and weights for sampling cosine of zenith
  ! angle distribution
  real(jprb), dimension(MAX_GAUSS_LEGENDRE_POINTS) :: mu_list, weight_list

  ! Actual weight used accounts for projection into horizontal area
  real(jprb) ::  weight

  ! Local versions of optional arguments
  integer(jpim) :: n_angles_per_hem_local
  logical :: do_3d_effects_local

  ! Loop indices for region and stream
  integer(jpim) :: jreg, jstream

  real(jprb) :: hook_handle

  if (lhook) call dr_hook('tcradr:calc_flux',0,hook_handle)

  ! Store local values for optional variables
  if (present(n_angles_per_hem)) then
    n_angles_per_hem_local = min(n_angles_per_hem, MAX_GAUSS_LEGENDRE_POINTS)
  else
    n_angles_per_hem_local = 0
  end if

  if (present(do_3d_effects)) then
    do_3d_effects_local = do_3d_effects
  else
    do_3d_effects_local = .false.
  end if

  ! Compute the wavelength-independent region fractions and
  ! optical-depth scalings
  call calc_region_properties(nlev, cloud_fraction, &
#if NUM_REGIONS == 3
       &  .true., fractional_std, &
#endif
       &  region_fracs, &
       &  od_scaling, cloud_fraction_threshold)

  ! Compute wavelength-independent overlap matrices u_overlap and
  ! v_overlap
  call calc_overlap_matrices(nlev, &
       &  region_fracs, overlap_param, &
       &  u_overlap, v_overlap, &
       &  0.5_jprb, &
       &  cloud_fraction_threshold, &
       &  cloud_cover)

  ! Average gas and cloud properties noting that: (1) region 1 is
  ! cloud-free so we copy over the gas optical depth; (2) gases only
  ! absorb so the single scattering albedo (ssa) of region is
  ! implicitly zero and we don't even have an array dimension for
  ! it; (3) since the gases don't scatter, the asymmetry factor of
  ! the gas-cloud mixture is equal to the value for clouds,
  ! regardless of the optical depth scaling (od_scaling) so we
  ! simply use the asymmetry_cloud variable when calculating
  ! reflectance and transmittance.
  od(:,1,:) = od_clear
  do jreg = 2,NREGION
    od(:,jreg,:) = od_clear + od_cloud*spread(od_scaling(jreg,:),1,nspec)
    ssa(:,jreg,:) = ssa_cloud(:,:)*od_cloud(:,:) &
         &  * spread(od_scaling(jreg,:),1,nspec) / od(:,jreg,:)
  end do

  ! Identify cloud-free layers
  is_cloud_free_layer(0) = .true.
  is_cloud_free_layer(1:nlev) = (region_fracs(1,:) == 1.0_jprb)
  is_cloud_free_layer(nlev+1) = .true.

  ! Compute layer-wise properties
  call calc_reflectance_transmittance(nspec, nlev, NREGION, &
       &  region_fracs, planck_hl, od, ssa, asymmetry_cloud, &
       &  reflectance, transmittance, source_up, source_dn)

  ! Classic Tripleclouds method to compute flux profile
  call calc_two_stream_flux(nspec, nlev, surf_emission, surf_albedo, &
       &  reflectance, transmittance, source_up, source_dn, &
       &  is_cloud_free_layer, u_overlap, v_overlap, &
       &  flux_up_base, flux_dn_base, flux_up_top, flux_dn_top)

  if (n_angles_per_hem_local > 0) then
    ! Fu et al. (1997) method: pass N beams through the
    ! atmosphere using the two-stream solution as the scattering
    ! source function
    if (n_angles_per_hem_local == 1) then
      ! Two-stream special case
      weight_list(1) = 1;
      mu_list = 1.0_jprb / LW_DIFFUSIVITY
    else
      call gauss_legendre(n_angles_per_hem_local, mu_list, weight_list)
    end if

    flux_up = 0.0_jprb
    flux_dn = 0.0_jprb
    do jstream = 1,n_angles_per_hem_local
      weight = weight_list(jstream)*mu_list(jstream) &
           &  / sum(weight_list(1:n_angles_per_hem_local) &
           &          * mu_list(1:n_angles_per_hem_local))
      ! Radiances are computed in pairs: up and down with same
      ! absolute zenith angle
      call calc_radiance_source(nspec, nlev, NREGION, &
           &  mu_list(jstream), &
           &  region_fracs, planck_hl, od, ssa, asymmetry_cloud, &
           &  flux_up_base, flux_dn_base, flux_up_top, flux_dn_top, &
           &  transmittance, source_up, source_dn)
      call calc_radiance_dn(nspec, nlev, &
           &  weight, &
           &  transmittance, source_dn, v_overlap, flux_dn)
      call calc_radiance_up(nspec, nlev, &
           &  weight, flux_up_base(:,:,nlev), &
           &  transmittance, source_up, u_overlap, flux_up)
    end do

  else ! n_angles_per_hem_local == 0
    ! Simply take the existing two-stream fluxes
    flux_up(:,1:nlev) = sum(flux_up_top,2)
    flux_up(:,nlev+1) = sum(flux_up_base(:,:,nlev),2)
    flux_dn(:,1:nlev) = sum(flux_dn_top,2)
    flux_dn(:,nlev+1) = sum(flux_dn_base(:,:,nlev),2)
  end if

  if (lhook) call dr_hook('tcrad:calc_flux',1,hook_handle)

end subroutine calc_flux


!---------------------------------------------------------------------
! Compute the flux profile neglecting the effects of scattering, via
! a number of radiance calculations
subroutine calc_no_scattering_flux(nspec, nlev, surf_emission, surf_albedo, planck_hl, &
     &  cloud_fraction, &
#if NUM_REGIONS == 3
     &  fractional_std, &
#endif
     &  od_clear, od_cloud, &
     &  overlap_param, flux_up, flux_dn, n_angles_per_hem, do_3d_effects, &
     &  cloud_cover)

  use parkind1, only           : jpim, jprb
  use yomhook,  only           : lhook, dr_hook
  use tcrad_layer_solutions, only   : calc_reflectance_transmittance, &
       &  calc_radiance_source, calc_no_scattering_radiance_source, &
       &  gauss_legendre, LW_DIFFUSIVITY, MAX_GAUSS_LEGENDRE_POINTS

  implicit none

  ! Inputs

  ! Number of spectral intervals and levels. Note that all
  ! level-dependent variables count down from top-of-atmosphere.
  integer(jpim), intent(in) :: nspec, nlev

  ! Surface upwards emission, in W m-2 (i.e. emissivity multiplied
  ! by Planck function at the surface skin temperature) integrated
  ! across each spectral interval, and albedo in the same intervals
  real(jprb), intent(in), dimension(nspec) :: surf_emission, surf_albedo

  ! Planck function integrated over each spectral interval at each
  ! half-level, in W m-2 (i.e. the flux emitted by a horizontal
  ! black-body surface)
  real(jprb), intent(in), dimension(nspec,nlev+1) :: planck_hl

  ! Profile of cloud fraction 
  real(jprb), intent(in), dimension(nlev) :: cloud_fraction

#if NUM_REGIONS == 3
  ! Profile of the fractional standard deviation (i.e. standard
  ! deviation divided by mean) of the horizontal in-cloud water
  ! content (or cloud extinction coefficient) distribution.
  real(jprb), intent(in), dimension(nlev) :: fractional_std
#endif

  ! Layer optical depth of gas and aerosol
  real(jprb), intent(in), dimension(nspec,nlev) :: od_clear

  ! Layer optical depth of cloud averaged only over the cloudy part
  ! of the gridbox. If Chou scaling is required then this should
  ! have been done already.
  real(jprb), intent(in), dimension(nspec,nlev) :: od_cloud

  ! Overlap parameter governing how clouds in adjacent layers are
  ! overlapped - this is the "alpha" of Hogan and Illingworth
  ! (2000). It is defined only between layers, hence the nlev-1
  ! elements.
  real(jprb), intent(in), dimension(nlev-1) :: overlap_param

  ! Outputs

  ! Upwelling and downwelling fluxes in each spectral interval at
  ! each half-level (W m-2)
  real(jprb), intent(out), dimension(nspec,nlev+1) :: flux_up, flux_dn

  ! Optional inputs

  ! Number of angles to compute radiances per hemisphere, for
  ! example, 2 results in the delta-2-plus-4 algorithm recommended
  ! by Fu et al. (1997). A value of 0 (the default) indicates to use
  ! the output from the two-stream Tripleclouds flux calculation
  ! directly.
  integer,    intent(in), optional :: n_angles_per_hem

  ! Do we represent 3D effects in the radiance calculations?
  logical,    intent(in), optional :: do_3d_effects

  ! Return cloud cover computed from cloud fraction profile and
  ! overlap rules
  real(jprb), intent(out), optional :: cloud_cover

  ! Local variables

  ! Combined gas/aerosol/cloud optical depth in each region
  real(jprb), dimension(nspec,NREGION,nlev)   :: od

  ! Transmittance of each layer and region
  real(jprb), dimension(nspec,NREGION,nlev) :: transmittance

  ! Rate of emission up from the top or down through the base of
  ! each layer and region (W m-2)
  real(jprb), dimension(nspec,NREGION,nlev) :: source_up, source_dn

  ! Which layers are cloud-free?  Dummy cloud-free layers are added
  ! above TOA (level 0) and below the ground (level nlev+1).
  logical :: is_cloud_free_layer(0:nlev+1)

  ! Upward and downward overlap matrices - see Hogan et al. (JGR
  ! 2016) for definitions
  real(jprb), dimension(NREGION,NREGION,nlev+1) :: u_overlap, v_overlap

  ! Surface upwelling flux (W m-2)
  real(jprb), dimension(nspec,NREGION) :: flux_up_surf

  ! Cloud optical depth scaling in each cloudy region
  real(jprb) :: od_scaling(2:NREGION,nlev)

  ! Fractional area coverage of each region
  real(jprb) :: region_fracs(1:NREGION,nlev)

  ! Cloud fractions below this are ignored
  real(jprb), parameter :: cloud_fraction_threshold = 1.0e-6

  ! Gauss-Legendre points and weights for sampling cosine of zenith
  ! angle distribution
  real(jprb), dimension(3) :: mu_list, weight_list

  ! Local versions of optional arguments
  integer(jpim) :: n_angles_per_hem_local
  logical :: do_3d_effects_local

  ! Loop indices for region and stream
  integer(jpim) :: jreg, jstream

  real(jprb) :: hook_handle

  if (lhook) call dr_hook('tcrad:calc_no_scattering_flux',0,hook_handle)

  ! Store local values for optional variables
  if (present(n_angles_per_hem)) then
    n_angles_per_hem_local = min(n_angles_per_hem, MAX_GAUSS_LEGENDRE_POINTS)
  else
    n_angles_per_hem_local = 1
  end if

  if (present(do_3d_effects)) then
    do_3d_effects_local = do_3d_effects
  else
    do_3d_effects_local = .false.
  end if


  ! Compute the wavelength-independent region fractions and
  ! optical-depth scalings
  call calc_region_properties(nlev, cloud_fraction, &
#if NUM_REGIONS == 3
       &  .true., fractional_std, &
#endif
       &  region_fracs, &
       &  od_scaling, cloud_fraction_threshold)

  ! Compute wavelength-independent overlap matrices u_overlap and
  ! v_overlap
  call calc_overlap_matrices(nlev, &
       &  region_fracs, overlap_param, &
       &  u_overlap, v_overlap, &
       &  0.5_jprb, &
       &  cloud_fraction_threshold, &
       &  cloud_cover)

  ! Average gas and cloud properties
  od(:,1,:) = od_clear
  do jreg = 2,NREGION
    od(:,jreg,:) = od_clear + od_cloud*spread(od_scaling(jreg,:),1,nspec)
  end do

  ! Identify cloud-free layers
  is_cloud_free_layer(0) = .true.
  is_cloud_free_layer(1:nlev) = (region_fracs(1,:) == 1.0_jprb)
  is_cloud_free_layer(nlev+1) = .true.

  if (n_angles_per_hem_local <= 1) then
    ! Two-stream special case
    weight_list(1) = 1;
    mu_list = 1.0_jprb / LW_DIFFUSIVITY
  else
    call gauss_legendre(n_angles_per_hem_local, mu_list, weight_list)
  end if

  flux_up = 0.0_jprb
  flux_dn = 0.0_jprb
  flux_up_surf = spread(surf_emission,2,NREGION)*spread(region_fracs(:,nlev),1,nspec)
  do jstream = 1,n_angles_per_hem_local
    call calc_no_scattering_radiance_source(nspec, nlev, NREGION, &
         &  mu_list(jstream), &
         &  region_fracs, planck_hl, od,  &
         &  transmittance, source_up, source_dn)
    ! Radiances are computed in pairs: up and down with same
    ! absolute zenith angle
    call calc_radiance_dn(nspec, nlev, &
         &  weight_list(jstream), &
         &  transmittance, source_dn, v_overlap, flux_dn)
    call calc_radiance_up(nspec, nlev, &
         &  weight_list(jstream), flux_up_surf, &
         &  transmittance, source_up, u_overlap, flux_up)
  end do

  if (lhook) call dr_hook('tcrad:calc_no_scattering_flux',1,hook_handle)

end subroutine calc_no_scattering_flux
