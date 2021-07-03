#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../LIB/zheads.h"
#include "../LIB/zdefs.h" 
#include "../LIB/zprotos.h"  
#include "../ios.h" 
#include <sys/time.h>
#include <string.h>
#include <complex.h>
/*---------------------------------------------------------------------------*
 * main test driver for the ILUT preconditioner
 *---------------------------------------------------------------------------*/

int main () { 
/*-------------------- protos */
  void output_header( io_t *pio );
  void output_result( int lfil, io_t *pio, int iparam );
  int zread_inputs( char *in_file, io_t *pio );
  int zget_matrix_info( FILE *fmat, io_t *pio );
  int zread_coo(complex double **VAL, int **COL, int **ROW, io_t *pio,
		complex double **rhs, complex double **sol); 
  void zrandvec (complex double *v, int n);
/*-------------------- end protos                                  */
  int ierr = 0;
/*-------------------------------------------------------------------
 * OPTIONS:
 * Plotting  = whether or not to dump gmres iteration infor (its, res)
 * output_lu = whether or not to dump the pattern of the LU factors
 *             e.g. for plotting with matlab.. 
 * diagscal  = diagonal scaling or not 
 *-----------------------------------------------------------------*/
  int plotting = 0, output_lu = 0, diagscal = 1;
  char pltfile[256];
  FILE *fits = NULL;
  csptr csmat = NULL;
  SMatptr MAT;
  SPreptr PRE; 
  iluptr lu = NULL;
  complex double *sol = NULL, *x = NULL, *rhs = NULL;
  int lfil;
  double tol; 
 /*-------------------- COO -- related arrays */
  complex double *VAL;
  int *ROW, *COL;
  int n, nnz;
 /*-------------------- end COO related stuff */
  FILE *flog = stdout, *fmat = NULL;
  io_t io;       
  double tm1=0.0, tm2=0.0;
  int mat, numat, iparam, i;
  double terr;
  char line[MAX_LINE];
/*-------------------- allocates structs for preconditoiner and matrix */
  MAT = (SMatptr)Malloc( sizeof(SMat), "main:MAT" );
  PRE = (SPreptr)Malloc( sizeof(SPre), "main:PRE" );
/*-------------------- read parameters and other inputs     */
  memset(&io, 0, sizeof(io) );
  if(zread_inputs( "inputs", &io ) != 0 ) {
    fprintf( flog, "Invalid inputs file...\n" );
    goto ERROR_HANDLE;
  }
/*-------------------- matfile  contains  paths to matrices */
  if( NULL == ( fmat = fopen( "matfile_coo", "r" ) ) ) {
    fprintf( flog, "Can't open matfile_coo...\n" );
    goto ERROR_HANDLE;
  }
  memset( line, 0, MAX_LINE );
  fgets( line, MAX_LINE, fmat );
  if( ( numat = atoi( line ) ) <= 0 ) {
    fprintf( flog, "Invalid count of matrices...\n" );
    goto ERROR_HANDLE;
  } 
/*-------------------- open file OUT/ILUT.out for all performance
                       results of this run (all matrices and params) 
                       also set io->PrecMeth */
    /* sprintf( io.outfile, "OUT/%s_ILUT.out", io.HBnameF );*/
    strcpy(io.outfile,"OUT/ILUT.out");
    strcpy(io.PrecMeth,"ILUT");
    if( NULL == ( io.fout = fopen( io.outfile, "w" ) ) ) {
      fprintf(flog,"Can't open output file %s...\n", io.outfile);
      goto ERROR_HANDLE;
   }
/*-------------------- LOOP THROUGH MATRICES */
  for( mat = 1; mat <= numat; mat++ ) {
    if( zget_matrix_info( fmat, &io ) != 0 ) {
      fprintf( flog, "Invalid format in matfile...\n" );
      goto ERROR_HANDLE;
    }
    fprintf( flog, "MATRIX: %s...\n", io.HBnameF );
/*-------------------- Read matrix in COO format*/
    csmat = (csptr)Malloc( sizeof(zSparMat), "main" );
    zread_coo(&VAL, &COL, &ROW, &io, &rhs, &sol) ;
    n = io.ndim;
    nnz = io.nnz;
    fprintf(stdout,"  matrix read successfully \n");
/*-------------------- diag scaling ---  */

    if (diagscal ==1) {  
      int nrm=1;
      double *diag;
      diag = ( double *)Malloc( sizeof(double)*n, "mainILUC:diag" );
      ierr = zroscalC( csmat, diag, nrm);
      if( ierr != 0 ) {
	fprintf( stderr, "main-ilut: roscal: a zero row...\n" );
	return ierr;
      }
      ierr = zcoscalC( csmat, diag, nrm );
      if( ierr != 0 ) {
	fprintf( stderr, "main-ilut: roscal: a zero col...\n" );
	return ierr;
      }
      free(diag);
    }
/*-------------------- conversion from COO to CS format */
    if( ( ierr = zCOOcs( n, nnz, VAL, COL, ROW, csmat ) ) != 0 ) {
      fprintf( stderr, "ILUT: zCOOcs error\n" );
      return ierr;
    }
/*-------------------- COO arrays no longer needed -- free */    
    free(ROW); ROW = NULL;
    free(VAL); VAL = NULL;
    free(COL); COL = NULL;
/*----------------------------------------------------------------------
|  The right-hand side is generated by assuming the solution is
|  a vector of ones.  To be changed if rhs is available from data.
|---------------------------------------------------------------------*/
    fprintf(stdout,"  ******* ndim = %d  \n",io.ndim);
    x   = (complex double *)Malloc( n*sizeof(complex double), "main" );
    for( i = 0; i < n; i++ ) 
      x[i] = 1.0 + 0.0*I; 
    zmatvec( csmat, x, rhs );
    fprintf(stdout,"right-hand side generated \n");
    output_header( &io );
/*-------------------- set initial lfil and tol */ 
    lfil = io.lfil0;
    tol  = io.tol0;
/*-------------------- LOOP through parameters */
    for( iparam = 1; iparam <= io.nparam; iparam++ ) {
      fprintf( flog, "iparam = %d\n", iparam );
      lu = (iluptr)Malloc( sizeof(zILUSpar), "main" );
      fprintf( flog, "begin ilut\n" );
      /*timers */
      tm1 = sys_timer();
/*--------------------  call zilut */
      ierr = zilut( csmat, lu, lfil, tol, flog );
      tm2 = sys_timer();
      if( ierr != 0 ) {
	fprintf( io.fout, " *** ILUT error - code %d \n", ierr);
	io.its = -1;
	io.tm_i = -1;
	io.enorm = -1;
	io.rnorm = -1;
	goto NEXT_PARA;
      }
      
      if(output_lu )       {
	char matdata[MAX_LINE];
	sprintf( matdata, "OUT/%s.dat", io.HBnameF );
	outputLU( lu, matdata );
      } 
      io.tm_p = tm2 - tm1;
      io.fillfact = (double)znnz_ilu( lu, flog )/(double)(io.nnz + 1);
      fprintf( flog, "ilut ends, fill factor (mem used) = %f\n", io.fillfact );

      if( zcondestLU( lu, sol, x, flog ) != 0 ) {
	fprintf( flog, "Not attempting iterative solution.\n" );
	fprintf( io.fout, "Not attempting iterative solution.\n" );
	io.its = -1;
	io.tm_i = -1;
	io.enorm = -1;
	io.rnorm = -1;
	goto NEXT_PARA;
      }

 /*-------------------- initial guess */     
      zrandvec(x, n);
/*       for( i = 0; i < io.ndim; i++ ) 	x[i] = 0.0 + 0.0*I;  */
/*-------------------- create a file for printing
                       'its -- time -- res' info from fgmres */
     if(plotting ) { 
       sprintf( pltfile, "OUT/%s_ILUT_F%05d_T%08.6f", io.HBnameF, lfil,tol);
       if( NULL == ( fits = fopen( pltfile, "w" ) ) ) {
	 fprintf( flog, "Can't open output file %s...\n", pltfile );
	 goto ERROR_HANDLE;
       }
     } else 
       fits  =NULL;
/*-------------------- set up the structs before calling fgmr */
      MAT->n = n;
      MAT->CSR = csmat;
      MAT->zmatvec = zmatvecCSR; 
      PRE->ILU = lu;
      PRE->zprecon = zpreconILU;
/*-------------------- call fgmr */
      io.its = io.maxits;
      tm1 = sys_timer();

      zfgmres(MAT, PRE, rhs, x, io.tol, io.im, &io.its, fits );
      tm2 = sys_timer();
      io.tm_i = tm2 - tm1;
      if( io.its < io.maxits ) 
 fprintf( flog, "param %03d OK: converged in %d steps...\n\n", iparam, io.its );
      else 
 fprintf( flog, "not converged in %d steps...\n\n", io.maxits );
      if( fits ) fclose( fits );

/*-------------------- calculate error and res norms */
      terr = 0.0;
      for( i = 0; i < io.ndim; i++ )
	      terr += pow(cabs( x[i] - 1.0 ),2);
      io.enorm = sqrt(terr);
      zmatvec( csmat, x, sol );
      terr = 0.0;
      for( i = 0; i < io.ndim; i++ )
	       terr += pow(cabs( rhs[i] - sol[i] ),2);
      io.rnorm = sqrt(terr);
/*-------------------- Test with next params   */     
   NEXT_PARA:
      output_result( lfil, &io, iparam );
      lfil += io.lfilInc;
      tol  *= io.tolMul;
      zcleanILU(lu);
   }
/*-------------------- Test with next matrix  NEXT_MAT:*/
   zcleanCS( csmat );
   free( sol );
   free( x );
   free( rhs );
  }
  fclose( io.fout );
  if( flog != stdout ) fclose ( flog );
  fclose( fmat );
  free(MAT) ; 
  free (PRE); 
  return 0;
ERROR_HANDLE:
  exit( -1 );
}
