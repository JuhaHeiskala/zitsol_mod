/*-----------------------------------------------------------------------*
 * Crout version of ILU                                                  *
 *-----------------------------------------------------------------------*
 * Na Li, Apr 09, 2002 -- YS - August 22nd, 2002                         *
 *                                                                       *
 * Report bugs / send comments to: saad@cs.umn.edu, nli@cs.umn.edu       *
 *-----------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <complex.h>

#include "zheads.h"
#include "zprotos.h"


#ifndef MAXFLOAT
#define MAXFLOAT (1e30)
#endif
#define NZ_DIAG 1         /* whether or not to replace zero diagonal    */
                          /* entries by small terms                     */
#define DELAY_DIAG_UPD 1  /* whether to update diagonals AFTER dropping */
#define BLEND 0.0         /* defines how to blend dropping by diagonal  */
                /* and other strategies. Element is always dropped when */
                /* (for Lij) : Lij < B*tol*D[i]+(1-B)*Norm (inv(L)*e_k) */
static int Lnnz, *Lfirst, *Llist, *Lid, Unnz, *Ufirst, *Ulist, *Uid;
static double Mnorm,*mt_u_norm, *w;
static complex double  *wL, *wU, *D, *milu_csum, *milu_rsum;
static csptr L;
static csptr U; 

/*-------------------- protos */
int update_diagonals(iluptr lu, int i ); 
int comp(const void *fst, const void *snd ); 
int std_drop(int lfil, int i, double tolL, double tolU, double toldiag, int milu) ;
int zlumsolC(complex double *y, complex double *x, iluptr lu );
/*-------------------- end protos */

int zilutc(iluptr mt, iluptr lu, int lfil, double tol, int drop, int milu, FILE *fp )
{
/*---------------------------------------------------------------------
 * Column-based ILUT (ILUTC) preconditioner
 * incomplete LU factorization with dropping strategy specified by input
 * paramter drop.
 * NOTE : no pivoting implemented as yet in GE for diagonal elements
 *---------------------------------------------------------------------
 * Parameters
 *---------------------------------------------------------------------
 * on entry:
 * =========
 * mt       = matrix stored in LUSparMat format -- see heads.h for details
 * lu       = pointer to a ILUSpar struct -- see heads.h for details
 * lfil     = integer. The fill-in parameter. Each COLUMN of L and
 *            each ROW of U will have a maximum of lfil elements.
 *            lfil must be .ge. 0.
 * tol      = Sets the threshold for dropping small terms in the
 *            factorization. See below for details on dropping strategy.
 * 
 * drop     = specifies the dropping strategy: 
 *            0 : standard dropping strategy (default) ;
 *            1 : drops a term if corresponding perturbation relative
 *                to diagonal entry is small. For example for U: 
 *                || L(:,i) || * |u(i,j)/D(i,i)| < tol * | D(j,j) | 
 *                
 *            2 : condition number estimator based on: 
 *                rhs = (1, 1, ..., 1 )^T ;
 *            3 : condition number estimator based on: 
 *                rhs = ((+/-)1, ..., (+/-)1 )^T 
 *                + maximizing |v_k| ;
 *            4 : condition number estimator based on: 
 *                rhs = ((+/-)1, ..., (+/-)1 )^T based on ;
 *                + maximizing the norm of (v_{k+1}, ..., v_n)^T );
 *- * 
 * fp       = file pointer for error log (might be stdout )
 *
 * on return:
 * ==========
 * ierr     = return value.
 *            ierr  =  0  --> successful return.
 *            ierr  = -1  --> Illegal value for lfil
 *            ierr  = -2  --> zero diagonal or zero col encountered
 *            if ierr = -2, try to use diagonal scaling
 * lu->n    = dimension of the matrix
 *   ->L    = L part -- stored in SparCol format
 *   ->D    = Diagonals
 *   ->U    = U part -- stored in SparRow format
 *
 * DETAILS on the data structure : 
 * ============================== 
 * 
 *     |  1   0  0 . . . 0 |     | U11 U12 U13 . . . U1n |
 *     | L21  1  0 . . . 0 |     |  0  U22 U23 . . . U2n |
 *     | L31 L32 1 . . . 0 |     |  0   0  U33 . . . U3n |
 * A = |  .   .  . .     . |  *  |  .   .   .  . . .  .  |
 *     |  .   .  .   .   . |     |  .   .   .    .    .  |
 *     |  .   .  .     . . |     |  .   .   .      .  .  |
 *     | Ln1 Ln2 . . . . 1 |     |  0   0   0  . . . Unn |
 * 
 * L - D - U preconditioner : 
 * 
 *     |  0                 |    | 0 U12 U13 . . . U1n |
 *     | L21  0             |    |    0  U23 . . . U2n |
 *     | L31 L32 0          |    |        0  . . . U3n |
 * L = |  .   .  .  .       | U= |           . . .  .  |
 *     |  .   .  .    .     |    |             .    .  |
 *     |  .   .  .      .   |    |               .  .  |
 *     | Ln1 Ln2 . . .  . 0 |    |                   0 | 
 * D = diag( 1/U11, 1/U22, 1/U33, ..., 1/Unn )
 * 
 *   \ - - . . . - - >
 *   | \ - . . . - - >
 *   | | \ - . . . - > U            L: SparCol format
 *   . . | \ . . . . .              U: SparRow format
 *   . . . . \ . . . .
 *   . . . . . \ . . .
 *   | | . . . . \ . .
 *   | | | . . . . \ .
 *   V V V . . . . . \
 *     L               D
 *---------------------------------------------------------------------
 * Workspace
 * =========
 * wL(n)      nonzero values in current column
 * Lid(n)     row numbers of nonzeros in current column
 * Lfirst(n)  At column k
 *            Lfirst(1:k-1), Lfirst(j) points to next value in column j to use
 *            Lfirst(k+1:n), Lfirst(j) indicates if nonzero value exists in
 *                           row j of current column
 * Llist(n)   Llist(j) points to a linked list of columns that will update the
 *            j-th row in U part
 * 
 * wU(n)      nonzero values in current row
 * Uid(n)     column numbers of nonzeros in current row
 * Ufirst(n)  At row k
 *            Ufirst(1:k-1), Ufirst(j) points to next value in row j to use
 *            Ufirst(k+1:n), Ufirst(j) indicates if nonzero value exists in
 *                           column j of current row
 * Ulist(n)   Ulist(j) points to a linked list of rows that will update the
 *            j-th column in L part
 *----------------------------------------------------------------------*/
  int n = mt->n, i, j, k;
  int lfst, ufst, row, col, newrow, newcol, iptr; 
  int nzcount, nnzL;
  complex double lval, uval, t;
  double Lnorm, Unorm, tLnorm, tUnorm, diag, toldiag; 
  int *ia, *ja;
  complex double *ma, *wsym;
  complex double *eL = NULL; /* to estimate the norm of k-th row of L^{-1} */
  complex double *eU = NULL; /* to estimate the norm of k-th col of U^{-1} */
/*-----------------------------------------------------------------------*/
  if(lfil < 0 ) {
    if (fp != NULL)
      fprintf(fp, "ilutc: Illegal value for lfil.\n" );
    return -1;
  }
  zsetupILU(lu, n );
  L = lu->L;
  U = lu->U;
  D = lu->D;

  Lfirst = (int *)Malloc(n*sizeof(int), "ilutc 1");
  Llist  = (int *)Malloc(n*sizeof(int), "ilutc 2" );
  Lid    = (int *)Malloc(n*sizeof(int), "ilutc 3" );
  wL     = (complex double *)Malloc(n*sizeof(complex double), "ilutc 4" );
  Ufirst = (int *)Malloc(n*sizeof(int), "ilutc 5" );
  Ulist  = (int *)Malloc(n*sizeof(int), "ilutc 6" );
  Uid    = (int *)Malloc(n*sizeof(int), "ilutc 7" );
  wU     = (complex double *)Malloc(n*sizeof(complex double), "ilutc 8" );
  w      = (double *)Malloc(n*sizeof(double), "ilutc 9" );
  wsym   = (complex double *)Malloc(n*sizeof(complex double), "ilutc 10" );
  if(drop == 2 || drop == 3 || drop == 4 ) {
    eL = (complex double *)Malloc(n*sizeof(complex double), "ilutc 11" );
    eU = (complex double *)Malloc(n*sizeof(complex double), "ilutc 12" );
  }
  // Modified ILU option added to original ITSOL2, Juha Heiskala
  if (milu>0) {
    milu_csum  = (complex double *)Malloc(n*sizeof(complex double), "ilutc 13" );
    milu_rsum  = (complex double *)Malloc(n*sizeof(complex double), "ilutc 14" );
  }
  mt_u_norm  = (double *)Malloc(n*sizeof(double), "ilutc 15" );

/*-------------------- initialize a few things */
  for(i = 0; i < n; i++ ) {
    D[i] = mt->D[i];
    Lfirst[i] = 0;
    Llist[i] = -1;
    Ufirst[i] = 0;
    Ulist[i] = -1;
    if(drop == 2 ) {
      eL[i] = 1.0;
      eU[i] = 1.0;
    } else if(drop == 3 || drop == 4 ) {
      eL[i] = 0.0;
      eU[i] = 0.0;
    }
    if (milu>0) {
      milu_csum[i] = 0.0;
      milu_rsum[i] = 0.0;
    }
    mt_u_norm[i] = 0.0;
  }

/*-------------------- main loop ------------*/ 
  for(i = 0; i < n; i++ ) {
    tLnorm = tUnorm = 0.0;
/*-------------------- load column i into wL */
    Lnnz = 0;
    nnzL = mt->L->nzcount[i];
    ia = mt->L->ja[i];
    ma = mt->L->ma[i];
    for(j = 0; j < nnzL; j++ ) {
      row = ia[j];
      Lfirst[row] = 1;
      t = ma[j];
      tLnorm += cabs(t);
      wL[row] = t;
      Lid[Lnnz++] = row;
    }
/*-------------------- load row i into wU */
    Unnz = 0;
    nzcount = mt->U->nzcount[i];
    ja = mt->U->ja[i];
    ma = mt->U->ma[i];
    for(j = 0; j < nzcount; j++ ) {
      col = ja[j];
      if(col != i ) {
        Ufirst[col] = 1;
        t = ma[j];
        wU[col] = t;
        tUnorm += cabs(t);
        Uid[Unnz++] = col;
      }
    }
/*-------------------- update U(i) using Llist */
    j = Llist[i];
    while(j >= 0 ) {
      lfst = Lfirst[j];
      lval = L->ma[j][lfst];
      ufst = Ufirst[j];
      nzcount = U->nzcount[j];
      ja = U->ja[j];
      ma = U->ma[j];
      for(k = ufst; k < nzcount; k++ ) {
        col = ja[k];
        uval = ma[k];
	if (col == i) continue; 
	/* DIAG-OPTION: if (col == i) {D[i] -= lval * uval;} else */
	if(Ufirst[col] == 1 ) 
          wU[col] -= lval * uval;
	else {
/*-------------------- fill-in */
          Ufirst[col] = 1;
          Uid[Unnz++] = col;
          wU[col] = -lval * uval;
        }
      }
/*-------------------- update Lfirst and Llist */
      Lfirst[j] = ++lfst;
      if(lfst < L->nzcount[j] ) {
        newrow = L->ja[j][lfst];
        iptr = j;
        j = Llist[iptr];
        Llist[iptr] = Llist[newrow];
        Llist[newrow] = iptr;
      } else {
        j = Llist[j];
      }
    }
/*-------------------- update L(i) using Ulist */
    j = Ulist[i];
    while(j >= 0 ) {
      ufst = Ufirst[j];
      uval = U->ma[j][ufst];
      lfst = Lfirst[j];
      nnzL = L->nzcount[j];
      ia = L->ja[j];
      ma = L->ma[j];
      for(k = lfst; k < nnzL; k++ ) {
        row = ia[k];
        lval = ma[k];
        if(Lfirst[row] == 1 ) {
          wL[row] -= lval * uval;
        } else {
/*-------------------- fill-in */
          Lfirst[row] = 1;
          Lid[Lnnz++] = row;
          wL[row] = -lval * uval;
        }
      }
      Ufirst[j] = ++ufst;
      if(ufst < U->nzcount[j] ) {
        newcol = U->ja[j][ufst];
        iptr = j;
        j = Ulist[iptr];
        Ulist[iptr] = Ulist[newcol];
        Ulist[newcol] = iptr;
      } else {
        j = Ulist[j];
      }
    }
/*-------------------- take care of special case when D[i] == 0 ---------*/
    Mnorm = (tLnorm + tUnorm)/(Lnnz+Unnz) ;
    if(D[i] == 0 ) {
      if(!NZ_DIAG ) {
	if (fp != NULL)
	  fprintf(fp, "zero diagonal encountered.\n" );
        for(j = i; j < n; j++ ) {
          L->ja[j] = NULL;
          L->ma[j] = NULL;
          U->ja[j] = NULL;
          U->ma[j] = NULL;
        }
        return -2;
      } else {
        D[i] = (1.0e-4+tol)*Mnorm; 
        if(D[i] == 0.0 ) {
	  D[i] = 1.0;
        }
      }
    }
/*-------------------- update diagonals [before dropping option]         */
    diag = cabs(D[i]);
    toldiag = cabs(D[i]*tol);
    D[i] = 1.0 / D[i];
    /* DIAG-UPDATE-OPTION: COMMENT THE NEXT LINE */
    if (!DELAY_DIAG_UPD) update_diagonals(lu, i); 
/*-------------------- call different dropping funcs according to 'drop' */
/*-------------------- drop = 0                                          */
    if(drop == 0) {
      std_drop(lfil,i,toldiag,toldiag,0.0, milu);
/*-------------------- drop = 1                                          */
    } else if(drop == 1) {
/*--------------------calculate one norms                                */
      Lnorm = diag;
      for (j=0; j < Lnnz; j++) 
        Lnorm += cabs(wL[Lid[j]]);
     /* compute Unorm now */
      Unorm = diag;
      for (j=0; j < Unnz; j++) 
        Unorm += cabs(wU[Uid[j]]); 
      Lnorm /= (1.0 + Lnnz);
      Lnorm *= tol;
      Unorm /= (1.0 + Unnz) ; 
      Unorm *= tol;
      std_drop(lfil,i,Lnorm,Unorm,0.0, milu);
/*-------------------- drop = 2                                          */
    } else if(drop == 2 ) {
      Lnorm = tol * diag / max(1,cabs(eL[i]));
      eU[i] *= D[i];   
      Unorm = tol / max(1,cabs(eU[i]));
      std_drop(lfil,i,Lnorm,Unorm,toldiag, milu);
/*-------------------- update eL[i+1,...,n] and eU[i+1,...,n]            */
      t = eL[i] * D[i];
      for(j = 0; j < Lnnz; j++ ) {
        row = Lid[j];
        eL[row] -= wL[row] * t;
      }
      t = eU[i];
      for(j = 0; j < Unnz; j++ ) {
        col = Uid[j];
        eU[col] -= wU[col] * t;
      }
/*-------------------- drop = 3 */
    } else if(drop == 3 ) {
      if(cabs(1-eL[i]) > cabs(-1-eL[i]) ) {
        eL[i] = 1 - eL[i];
      } else {
        eL[i] = -1 - eL[i];
      }
      if(cabs(1-eU[i]) > cabs(-1-eU[i]) ) {
        eU[i] = 1 - eU[i];
      } else {
        eU[i] = -1 - eU[i];
      }
      eU[i] *= D[i];
      Lnorm = tol * diag / max(1,cabs(eL[i]));
      Unorm = tol / max(1,cabs(eU[i]));
      std_drop(lfil,i,Lnorm,Unorm,toldiag, milu);
/*-------------------- update eL[i+1,...,n] and eU[i+1,...,n] */
      t = eL[i] * D[i];
      for(j = 0; j < Lnnz; j++ ) {
        row = Lid[j];
        eL[row] += wL[row] * t;
      }
      t = eU[i];
      for(j = 0; j < Unnz; j++ ) {
        col = Uid[j];
        eU[col] += wU[col] * t;
      }
/*-------------------- drop = 4                               */
    } else if(drop == 4 ) {
      complex double x1, x2;
      double s1 = 0, s2 = 0;
      x1 = 1 - eL[i];
      x2 = -1 - eL[i];
      t = x1 * D[i];
      for(j = 0; j < Lnnz; j++ ) {
        row = Lid[j];
        s1 += cabs(eL[row] + wL[row] * t);
      }
      t = x2 * D[i];
      for(j = 0; j < Lnnz; j++ ) {
        row = Lid[j];
        s2 += cabs(eL[row] + wL[row] * t);
      }
      if(s1 > s2 ) {
	eL[i] = x1;
      } else {
	eL[i] = x2;
      }
      Lnorm = tol * diag / max(1,cabs(eL[i]));
      x1 = (1 - eU[i]) * D[i];
      x2 = (-1 - eU[i]) * D[i];
      s1 = s2 = 0.0;
      t  = x1;
      for(j = 0; j < Unnz; j++ ) {
        col = Uid[j];
        s1 += cabs(eU[col] + wU[col] * t);
      }
      t = x2;
      for(j = 0; j < Unnz; j++ ) {
        col = Uid[j];
        s2 += cabs(eU[col] + wU[col] * t);
      }
      if(s1 > s2 ) {
        eU[i] = x1;
      } else {
        eU[i] = x2;
      }
      Unorm = tol / max(1,cabs(eU[i]));
      std_drop(lfil,i,Lnorm,Unorm,toldiag, milu);
/*-------------------- update eL[i+1,...,n] and eU[i+1,...,n] */
      t = eL[i] * D[i];
      for(j = 0; j < Lnnz; j++ ) {
        row = Lid[j];
        eL[row] += wL[row] * t;
      }
      t = eU[i];
      for(j = 0; j < Unnz; j++ ) {
        col = Uid[j];
        eU[col] += wU[col] * t;
      }
    }
    /*-------------------- drop = 5 (added for octave compatibility, 2-norm based tolerance, Juha Heiskala         */
    else if(drop == 5) {
      /*--------------------calculate two norms      */
      double Anorm = diag*diag;
      for (j=0; j < mt->L->nzcount[i]; j++) 
        Anorm += cabs(mt->L->ma[i][j])*cabs(mt->L->ma[i][j]);

      for (j=0; j < mt->U->nzcount[i]; j++) {
	int col = mt->U->ja[i][j];
	mt_u_norm[col] += cabs(mt->U->ma[i][j])*cabs(mt->U->ma[i][j]);
      }
      Anorm += mt_u_norm[i];

      Unorm = sqrt(Anorm);
      Lnorm = Unorm; ///diag;

      Lnorm *= tol;
      Unorm *= tol;
      std_drop(lfil,i,Lnorm,Unorm,0.0, milu);
    }
    else {
      if (fp != NULL)
	fprintf(fp, "Invalid option for dropping ...\n" );
      exit(0);
    }

    if (milu>0) {
      D[i] = 1.0/(1.0/D[i] + milu_rsum[i] + milu_csum[i]);
      for(j = 0; j < L->nzcount[i]; j++ ) {      
	L->ma[i][j] *=  D[i];
      }
    }
  
/*-------------------- update diagonals [after dropping option]        */
/* DIAG-UPDATE-OPTION: COMMENT THE NEXT  LINE */
     if (DELAY_DIAG_UPD) update_diagonals(lu, i);


/*-------------------- reset nonzero indicators [partly reset already] */ 
    for(j = 0; j < Lnnz; j++ )
      Lfirst[Lid[j]] = 0;
    for(j = 0; j < Unnz; j++ ) 
      Ufirst[Uid[j]] = 0;
/*-------------------- initialize linked list for next row of U */
    if(U->nzcount[i] > 0 ) {
      col = Uid[0];
      Ufirst[i] = 0;
      Ulist[i] = Ulist[col];
      Ulist[col] = i;
    }
/*-------------------- initialize linked list for next column of L */
    if(L->nzcount[i] > 0 ) {
      row = Lid[0];
      Lfirst[i] = 0;
      Llist[i] = Llist[row];
      Llist[row] = i;
    }

    D[i] = 1.0/D[i];    
  
}
  
  free(Lfirst );
  free(Llist );
  free(Lid );
  free(wL );
  free(Ufirst );
  free(Ulist );
  free(Uid );
  free(wU );
  free(w );
  free(wsym );
  if(drop == 2 || drop == 3 || drop == 4 ) {
    free(eL );
    free(eU );
  }
  if (milu>0) {
  free(milu_csum);
  free(milu_rsum);
  }
  free(mt_u_norm);
  
  return 0;
}

int update_diagonals(iluptr lu, int i )
{
/*---------------------------------------------------------------------
 * update diagonals D_{i+1,...,n}
 *---------------------------------------------------------------------*/
  complex double *diag = lu->D;

  complex double scale = diag[i];
  /* By using the expansion arrays, only the shorter one of L(k) and U(k)
   * need to be scaled, so the time complexity = O(min(Lnnz,Unnz)) */
  int j, id;
  
  if(Lnnz < Unnz ) {
    for(j = 0; j < Lnnz; j++ ) {
      id = Lid[j];
      if(Ufirst[id] != 0 ) {
	diag[id] -= wL[id] * wU[id] * scale;
      }
    }
  } else {
    for(j = 0; j < Unnz; j++ ) {
      id = Uid[j];
      if(Lfirst[id] != 0 ) {
	diag[id] -= wL[id] * wU[id] * scale;
      }
    }
  }
  return 0;
}

/*-----------------------------------------------------------------------*/
int CondestC(iluptr lu, FILE *fp ){
  int n = lu->n, i;
  double norm = 0.0;
  complex double *y = (complex double *) Malloc(n*sizeof(complex double), "condestC");
  complex double *x = (complex double *) Malloc(n*sizeof(complex double), "condestC");

  for(i = 0; i < n; i++ ) 
    y[i] = 1.0;
  
  zlumsolC(y, x, lu );
  for(i = 0; i < n; i++ ) {
    norm = max(norm, cabs(x[i]) );
  }
  if (fp != NULL)
    fprintf(fp, "ILU inf-norm lower bound : %16.2f\n", norm );
  free(x); free(y);
  if(norm > 1e30 ) {
    return -1;
  }
  return 0;
}

int comp(const void *fst, const void *snd )
{
/*-------------------------------------------------------------------
 * compares two integers
 * a callback function used by qsort
---------------------------------------------------------------------*/
  int *i = (int *)fst, *j = (int *)snd;
  if(*i > *j ) return 1;
  if(*i < *j ) return -1;
  return 0;
}

int std_drop(int lfil, int i, double tolL, double tolU, double toldiag, int milu) 
{
/*---------------------------------------------------------------------
 * Standard Dual drop-off strategy 
 * =============================== 
 * 1) Theresholding in L and U as set by tol. Any element whose size
 *    is less than some tolerance (relative to the norm of current
 *    row in u or current column of L) is dropped.
 * 2) Keeping only the largest lfil elements in the i-th column of L
 *    and the largest lfil elements in the i-th row of U.
 * lfil    = number of elements to keep in L and U 
 * tolL    = tolerance parameter used to the L factor
 * tolU    = tolerance parameter used to the U factor
 * toldiag = used for blended dropping.  
 *                                       
 *  L(i,j) is dropped if | L(i,j) | < toldiag*BLEND + tolL*(1-BLEND) 
 *  U(i,j) is dropped if | U(i,j) | < toldiag*BLEND + tolU*(1-BLEND) 
 * 
 *---------------------------------------------------------------------*/ 
  int j, len, col, row, ipos;
  int *ia, *ja;
  complex double *ma, t;
  t = D[i];
/*-------------------- drop U elements                                 */
  len  = 0;
  tolU = BLEND*toldiag + (1.0-BLEND)*tolU;
  tolL = BLEND*toldiag + (1.0-BLEND)*tolL;
/*---------------------------------------------------------------------*/
  for(j = 0; j < Unnz; j++) {
    col = Uid[j];
    if(cabs(wU[col]) > tolU) 
      Uid[len++] = col; 
    else {
      Ufirst[col] = 0;
      if (milu==1) // row option
	milu_rsum[i] += wU[col];
      else if (milu==2) // column option
	milu_rsum[col] += wU[col];
    }
  }

/*-------------------- find the largest lfil elements in row k */
  Unnz = len; 
  len = min(Unnz, lfil );
  for(j = 0; j < Unnz; j++ ) 
    w[j]  = cabs(wU[Uid[j]]);
  qsplit(w, Uid, &Unnz, &len );
  qsort(Uid, len, sizeof(int), comp );
/*-------------------- update U */
  U->nzcount[i] = len;
  if(len > 0 ) {
    ja = U->ja[i] = (int *)Malloc(len*sizeof(int), "std_drop 1" );
    ma = U->ma[i] = (complex double *)Malloc(len*sizeof(complex double), "std_drop 2" );
  }
  for(j = 0; j < len; j++ ) {
    ipos = Uid[j];
    ja[j] = ipos;
    ma[j] = wU[ipos];
  }
  for(j = len; j < Unnz; j++ ) {
    Ufirst[Uid[j]] = 0; /* important: otherwise, delay_update_diagonals may
                         * not work correctly in case U->nzcount[i] < Unnz */
  }
  Unnz = len;
/*-------------------- drop L elements                                    */
  len = 0;
  for(j = 0; j < Lnnz; j++ ) {
    row = Lid[j];
    if (cabs(wL[row]) > tolL)
      Lid[len++] = row;
    else {
      Lfirst[row] = 0;
      if (milu==1) // row option
	milu_csum[row] += wL[row];
      else if (milu==2)
	milu_csum[i] += wL[row];
    }
  }
/*-------------------- find the largest lfil elements in column k         */
  Lnnz = len;
  len = min(Lnnz, lfil );
  for(j = 0; j < Lnnz; j++ ) 
    w[j] = cabs(wL[Lid[j]]);
  qsplit(w, Lid, &Lnnz, &len );
  qsort(Lid, len, sizeof(int), comp );
/*-------------------- update L                                           */
  L->nzcount[i] = len;
  if(len > 0 ) {
    ia = L->ja[i] = (int *)Malloc(len*sizeof(int), "std_drop 3" );
    ma = L->ma[i] = (complex double *)Malloc(len*sizeof(complex double), "std_drop 4" );
  }
  for(j = 0; j < len; j++ ) {
    ipos = Lid[j];
    ia[j] = ipos;
    if (milu == 0) // scaling by t done here as original ITSOL code, Juha Heiskala
      ma[j] = wL[ipos]*t;
    else if (milu >0) // scaling delayed until row/column sum is added to the diagonal for MILU, Juha Heiskala
      ma[j] = wL[ipos];
  }
  for(j = len; j < Lnnz; j++ ) {
    Lfirst[Lid[j]] = 0; /* important: otherwise, delay_update_diagonals may
                         * not work correctly in case L->nzcount[i] < Lnnz */
  }
  Lnnz = len;
  return 0;
}

/*----------------------------------------------------------------------
 *    performs a forward followed by a backward solve
 *    for LU matrix as produced by iluc
 *    y  = right-hand-side
 *    x  = solution on return
 *    lu = LU matrix as produced by iluc.
 *--------------------------------------------------------------------*/

int zlumsolC(complex double *y, complex double *x, iluptr lu )
{
    int n = lu->n, i, j, nzcount, nnzL, *ia, *ja;
    complex double *D = lu->D, *ma;
    csptr L = lu->L;
    csptr U = lu->U;

    for (i = 0; i < n; i++)
        x[i] = y[i];
    /*-------------------- L solve */
    for (i = 0; i < n; i++) {
        nnzL = L->nzcount[i];
        ia = L->ja[i];
        ma = L->ma[i];
        for (j = 0; j < nnzL; j++) {
            x[ia[j]] -= ma[j] * x[i];
        }
    }
    /*-------------------- U solve */
    for (i = n - 1; i >= 0; i--) {
        nzcount = U->nzcount[i];
        ja = U->ja[i];
        ma = U->ma[i];
        for (j = 0; j < nzcount; j++) {
            x[i] -= ma[j] * x[ja[j]];
        }
        x[i] *= D[i];
    }

    return 0;
}
