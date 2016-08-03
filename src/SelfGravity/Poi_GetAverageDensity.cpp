#include "Copyright.h"
#include "GAMER.h"

#ifdef GRAVITY




//-------------------------------------------------------------------------------------------------------
// Function    :  Poi_GetAverageDensity
// Description :  Evaluate the average density for the Poisson solver
//
// Note        :  1. For the Poisson solver with the periodic BC (in both physical and comoving frames,
//                   the average density will be subtracted from the total density at each cell when
//                   solving the Poisson equation at the refined levels in order to be consistent with the
//                   base-level periodic FFT solver
//             :  2. For the Poisson solver with the isolated BC in the comoving frames, the UNITY will be
//                   subtracted from the total density at each cell when solving the Poisson equation at
//                   all levels in order to be consistent with the Poisson eq. in the comoving frame
//                3. For the debug mode, we perform summation in a specific order in order to ensure that
//                   the round-off errors will be the same in runs with different numbers of MPI ranks
//                4. Include both the fluid and particles's mass
//
// Parameter   :  None
//-------------------------------------------------------------------------------------------------------
void Poi_GetAverageDensity()
{

   if ( MPI_Rank == 0 )    Aux_Message( stdout, "%s ... \n", __FUNCTION__ );


// check
#  ifndef DENS
#  error : ERROR : VARIABLE "DENS" IS NOT DEFINED IN THE FUNCTION "Poi_GetAverageDensity" !!
#  endif

   if ( !OPT__INIT_RESTRICT  &&  MPI_Rank == 0 )
      Aux_Message( stderr, "WARNING : option \"%s\" is NOT turned on when evaluating the average density !!\n",
                   "OPT__INIT_RESTRICT" );

// initialize it to zero
   AveDensity_Init = 0.0;


// 1. for OOC computing (no longer useful)
// ==================================================================================================


// 2. for debug mode
// ==================================================================================================
#  ifdef GAMER_DEBUG

// only use rank 0 for summation in the debug mode
   const int NP[3]  = { NX0_TOT[0]/PATCH_SIZE, NX0_TOT[1]/PATCH_SIZE, NX0_TOT[2]/PATCH_SIZE };
   const int PScale = PATCH_SIZE * amr->scale[0];

//###NOTE : actually "int" is enoughly for Cr1D_Local and Cr1D_All. If they do need "long" or "ulong", then
//          at least Cr1D_IdxTable and other looping indices will need "long or ulong" as well since base-level
//          patches fill the entire box
   int     Cr3D[3], NPatch_All[MPI_NRank], Disp[MPI_NRank];
   int     NPatch_Sum    = 0;
   double *Rho_All       = NULL;
   long   *Cr1D_All      = NULL;
   int    *Cr1D_IdxTable = NULL;
   double *Rho_Local     = new double [ amr->NPatchComma[0][1] ];
   long   *Cr1D_Local    = new long   [ amr->NPatchComma[0][1] ];

   MPI_Gather( &amr->NPatchComma[0][1], 1, MPI_INT, NPatch_All, 1, MPI_INT, 0, MPI_COMM_WORLD );

   if ( MPI_Rank == 0 )
   {
      for (int r=0; r<MPI_NRank; r++)  NPatch_Sum += NPatch_All[r];

      Disp[0] = 0;
      for (int r=1; r<MPI_NRank; r++)  Disp[r] = Disp[r-1] + NPatch_All[r-1];

      Rho_All       = new double [NPatch_Sum];
      Cr1D_All      = new long   [NPatch_Sum];
      Cr1D_IdxTable = new int    [NPatch_Sum];
   }

// prepare density and 1D-corner arrays
   for (int PID=0; PID<amr->NPatchComma[0][1]; PID++)
   {
      for (int d=0; d<3; d++)    Cr3D[d] = amr->patch[0][0][PID]->corner[d]/PScale;

      Cr1D_Local[PID] = ( (long)Cr3D[2]*NP[1] + (long)Cr3D[1] )*NP[0] + (long)Cr3D[0];
      Rho_Local [PID] = 0.0;

      for (int k=0; k<PATCH_SIZE; k++)
      for (int j=0; j<PATCH_SIZE; j++)
      for (int i=0; i<PATCH_SIZE; i++)
         Rho_Local[PID] += (double)amr->patch[ amr->FluSg[0] ][0][PID]->fluid[DENS][k][j][i];
   }

// gather data
   MPI_Gatherv( Cr1D_Local, amr->NPatchComma[0][1], MPI_LONG,   Cr1D_All, NPatch_All, Disp, MPI_LONG,
                0, MPI_COMM_WORLD );

   MPI_Gatherv( Rho_Local,  amr->NPatchComma[0][1], MPI_DOUBLE, Rho_All,  NPatch_All, Disp, MPI_DOUBLE,
                0, MPI_COMM_WORLD );

   if ( MPI_Rank == 0 )
   {
//    sort
      Mis_Heapsort( NPatch_Sum, Cr1D_All, Cr1D_IdxTable );

//    get average density
      for (int t=0; t<NPatch_Sum; t++)    AveDensity_Init += Rho_All[ Cr1D_IdxTable[t] ];
      AveDensity_Init /= (double)NX0_TOT[0]*NX0_TOT[1]*NX0_TOT[2];
   }

// add particle mass
#  ifdef PARTICLE
   long    NPar_AcPlusInac_AllRank[MPI_NRank], NPar_AcPlusInac_Sum;
   int     Count[MPI_NRank];
   double  ParMassSum;
   real   *ParMass_AllRank = NULL;

// get the total number of active + inactive particles in each rank
   MPI_Gather( &amr->Par->NPar_AcPlusInac, 1, MPI_LONG, NPar_AcPlusInac_AllRank, 1, MPI_LONG, 0, MPI_COMM_WORLD );

   if ( MPI_Rank == 0 )
   {
      NPar_AcPlusInac_Sum = 0;
      for (int r=0; r<MPI_NRank; r++)  NPar_AcPlusInac_Sum += NPar_AcPlusInac_AllRank[r];

      ParMass_AllRank = new real [NPar_AcPlusInac_Sum];

//    here we have assumed that "integer" type is enough !!
      for (int r=0; r<MPI_NRank; r++)  Count[r] = NPar_AcPlusInac_AllRank[r];

      Disp[0] = 0;
      for (int r=1; r<MPI_NRank; r++)  Disp[r] = Disp[r-1] + Count[r-1];
   }

// collect particle mass from all ranks
#  ifdef FLOAT8
   MPI_Gatherv( amr->Par->Mass, amr->Par->NPar_AcPlusInac, MPI_DOUBLE,
                ParMass_AllRank, Count, Disp, MPI_DOUBLE, 0, MPI_COMM_WORLD );
#  else
   MPI_Gatherv( amr->Par->Mass, amr->Par->NPar_AcPlusInac, MPI_FLOAT,
                ParMass_AllRank, Count, Disp, MPI_FLOAT,  0, MPI_COMM_WORLD );
#  endif

   if ( MPI_Rank == 0 )
   {
//    sort
      Mis_Heapsort( NPar_AcPlusInac_Sum, ParMass_AllRank, NULL );

//    add average particle density
      ParMassSum = 0.0;
      for (long p=0; p<NPar_AcPlusInac_Sum; p++)
      {
//       skip inactive and massless particles
         if ( ParMass_AllRank[p] > (real)0.0 )  ParMassSum += (double)ParMass_AllRank[p];
      }

      AveDensity_Init += ParMassSum / ( amr->BoxSize[0]*amr->BoxSize[1]*amr->BoxSize[2] );

      delete [] ParMass_AllRank;
   }
#  endif // #ifdef PARTICLE

// broadcast
   MPI_Bcast( &AveDensity_Init, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD );

// free memory
   delete [] Rho_Local;
   delete [] Cr1D_Local;
   if ( MPI_Rank == 0 )
   {
      delete [] Rho_All;
      delete [] Cr1D_All;
      delete [] Cr1D_IdxTable;
   }


// 3. for general cases
// ==================================================================================================
#  else // #ifdef GAMER_DEBUG

// evaluate the sum of density (we only use the base-level data because the restriction condition is assumed
// to be fulfilled
   double AveDensity_Init_local = 0.0;

   for (int PID=0; PID<amr->NPatchComma[0][1]; PID++)
   for (int k=0; k<PATCH_SIZE; k++)
   for (int j=0; j<PATCH_SIZE; j++)
   for (int i=0; i<PATCH_SIZE; i++)
      AveDensity_Init_local += amr->patch[ amr->FluSg[0] ][0][PID]->fluid[DENS][k][j][i];

// sum over all MPI ranks
   MPI_Allreduce( &AveDensity_Init_local, &AveDensity_Init, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD );

// average
   AveDensity_Init /= (double)NX0_TOT[0]*NX0_TOT[1]*NX0_TOT[2];

// add particle mass
#  ifdef PARTICLE
   double ParMassSum_local=0.0, ParMassSum_total;

   for (long p=0; p<amr->Par->NPar_AcPlusInac; p++)
   {
//    skip inactive and massless particles
      if ( amr->Par->Mass[p] > (real)0.0 )   ParMassSum_local += (double)amr->Par->Mass[p];
   }

// sum over all MPI ranks
   MPI_Allreduce( &ParMassSum_local, &ParMassSum_total, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD );

   AveDensity_Init += ParMassSum_total / ( amr->BoxSize[0]*amr->BoxSize[1]*amr->BoxSize[2] );
#  endif // #ifdef PARTICLE

#  endif // #ifdef GAMER_DEBUG ... else ...


// 4. output results
   if ( MPI_Rank == 0 )
   {
      Aux_Message( stdout, "NOTE : background density = %20.14e\n", AveDensity_Init );

#     ifdef COMOVING
      const double Deviation = fabs( AveDensity_Init - 1.0 );
      if ( Deviation > 1.0e-5 )
      {
         Aux_Message( stderr, "WARNING : background density deviates from unity by %20.14e ", Deviation );
         Aux_Message( stderr,           "(UNITY is assumed in COMOVING) !!\n" );
      }
#     endif
   }


// check
   if ( AveDensity_Init <= 0.0 )    Aux_Error( ERROR_INFO, "average density (%14.7e) <= 0.0 !!\n", AveDensity_Init );


   if ( MPI_Rank == 0 )    Aux_Message( stdout, "%s ... done\n", __FUNCTION__ );

} // FUNCTION : Poi_GetAverageDensity



#endif // #ifdef GRAVITY
