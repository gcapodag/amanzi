
module libats
    use iso_c_binding

    private
    public :: ats

    include "ats.f90"

    type ats
        private
        type(c_ptr) :: ptr ! pointer to ats class
    contains
        final :: ats_delete 
        procedure :: setup => ats_setup
        procedure :: initialize => ats_initialize
        procedure :: advance => ats_advance
        procedure :: advance_test => ats_advance_test
        procedure :: set_sources => ats_set_sources
    end type

    ! wow fortran constructor wow
    interface ats
        procedure ats_create
    end interface

contains
    
    function ats_create()
        implicit none
        type(ats) :: ats_create
        ats_create%ptr = ats_create_c()
    end function

    subroutine ats_delete(this)
        implicit none
        type(ats) :: this
        call ats_delete_c(this%ptr)
    end subroutine

    subroutine ats_setup(this, comm, infile)
        implicit none
        class(ats) :: this
        integer, intent(in) :: comm
        character(len=*), intent(in) :: infile
        character(len=1, kind=C_CHAR) :: c_str_infile(len_trim(infile) + 1)
        integer :: n_char, i

        n_char = len_trim(infile)
        do i = 1, n_char
            c_str_infile(i) = infile(i:i)
        end do
        c_str_infile(n_char + 1) = C_NULL_CHAR

       call ats_setup_c(this%ptr, comm, c_str_infile)
    end subroutine

    subroutine ats_initialize(this)
        implicit none
        class(ats) :: this
        call ats_initialize_c(this%ptr)
    end subroutine

    subroutine ats_advance(this, dt)
        implicit none
        class(ats) :: this
        double precision, intent(in) :: dt
        call ats_advance_c(this%ptr, dt)
    end subroutine

    subroutine ats_advance_test(this)
        implicit none
        class(ats) :: this
        call ats_advance_test_c(this%ptr)
    end subroutine

    subroutine ats_set_sources(this, soil_infil, soil_evap, ncols)
        implicit none
        class(ats) :: this
        double precision, dimension(*), intent(in) :: soil_infil
        double precision, dimension(*), intent(in) :: soil_evap
        integer, intent(in) :: ncols
        call ats_set_sources_c(this%ptr, soil_infil, soil_evap, ncols)
    end subroutine

end module
