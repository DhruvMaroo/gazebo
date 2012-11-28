/*************************************************************************
*                                                                       *
* Open Dynamics Engine, Copyright (C) 2001-2003 Russell L. Smith.       *
* All rights reserved.  Email: russ@q12.org   Web: www.q12.org          *
*                                                                       *
* This library is free software; you can redistribute it and/or         *
* modify it under the terms of EITHER:                                  *
*   (1) The GNU Lesser General Public License as published by the Free  *
*       Software Foundation; either version 2.1 of the License, or (at  *
*       your option) any later version. The text of the GNU Lesser      *
*       General Public License is included with this library in the     *
*       file LICENSE.TXT.                                               *
*   (2) The BSD-style license that is included with this library in     *
*       the file LICENSE-BSD.TXT.                                       *
*                                                                       *
* This library is distributed in the hope that it will be useful,       *
* but WITHOUT ANY WARRANTY; without even the implied warranty of        *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the files    *
* LICENSE.TXT and LICENSE-BSD.TXT for more details.                     *
*                                                                       *
*************************************************************************/
#undef NDEBUG
#include <ode/common.h>
#include <ode/odemath.h>
#include <ode/rotation.h>
#include <ode/timer.h>
#include <ode/error.h>
#include <ode/matrix.h>
#include <ode/misc.h>
#include "config.h"
#include "objects.h"
#include "joints/joint.h"
#include "lcp.h"
#include "util.h"

#include <sys/time.h>

#ifdef SSE
#include <xmmintrin.h>
#define Kf(x) _mm_set_pd((x),(x))
#endif


#undef REPORT_THREAD_TIMING
#define USE_TPROW
#undef TIMING
#undef REPORT_MONITOR
#undef SHOW_CONVERGENCE
#undef RECOMPUTE_RMS
#undef USE_1NORM
//#define LOCAL_STEPPING  // not yet implemented
//#define PENETRATION_JVERROR_CORRECTION
//#define POST_UPDATE_CONSTRAINT_VIOLATION_CORRECTION

#define USE_JOINT_DAMPING

#ifdef USE_TPROW
// added for threading per constraint rows
#include <boost/thread/recursive_mutex.hpp>
#include <boost/bind.hpp>
#include "ode/odeinit.h"
#endif

typedef const dReal *dRealPtr;
typedef dReal *dRealMutablePtr;

//***************************************************************************
// configuration

// for the SOR and CG methods:
// uncomment the following line to use warm starting. this definitely
// help for motor-driven joints. unfortunately it appears to hurt
// with high-friction contacts using the SOR method. use with care

//#define WARM_STARTING 1


// for the SOR method:
// uncomment the following line to determine a new constraint-solving
// order for each iteration. however, the qsort per iteration is expensive,
// and the optimal order is somewhat problem dependent.
// @@@ try the leaf->root ordering.

//#define REORDER_CONSTRAINTS 1


// for the SOR method:
// uncomment the following line to randomly reorder constraint rows
// during the solution. depending on the situation, this can help a lot
// or hardly at all, but it doesn't seem to hurt.

// #define RANDOMLY_REORDER_CONSTRAINTS 1
#undef LOCK_WHILE_RANDOMLY_REORDER_CONSTRAINTS


// structure for passing variable pointers in SOR_LCP
struct dxSORLCPParameters {
    dxQuickStepParameters *qs;
    int nStart;   // 0
    int nChunkSize;
    int m; // m
    int nb;
    dReal stepsize;
    int* jb;
    const int* findex;
    dRealPtr hi;
    dRealPtr lo;
    dRealPtr I;
    dRealPtr Adcfm;
    dRealPtr Adcfm_precon;
    dRealMutablePtr rhs;
    dRealMutablePtr rhs_erp;
    dRealMutablePtr J;
    dRealMutablePtr caccel;
    dRealMutablePtr caccel_erp;
    dRealMutablePtr lambda;
    dRealMutablePtr lambda_erp;
    dRealMutablePtr iMJ;
    dRealMutablePtr delta_error ;
    dRealMutablePtr rhs_precon ;
    dRealMutablePtr J_precon ;
    dRealMutablePtr J_orig ;
    dRealMutablePtr cforce ;
#ifdef PENETRATION_JVERROR_CORRECTION
    dRealMutablePtr vnew ;
#endif
#ifdef REORDER_CONSTRAINTS
    dRealMutablePtr last_lambda ;
    dRealMutablePtr last_lambda_erp ;
#endif
#ifdef USE_JOINT_DAMPING
    dRealMutablePtr b_damp ;
    dRealMutablePtr f_damp ;
    dRealPtr JiM ;
    dRealPtr invI;
    dRealPtr Ad;
    dRealMutablePtr v_damp ;
    dxBody * const *body;
    dRealMutablePtr J_damp ;
    dRealPtr coeff_damp ;
    int* jb_damp ;
    dRealMutablePtr v_joint_damp ;
    dRealMutablePtr a_joint_damp ;
    int m_damp;
#endif
};


//****************************************************************************
// special matrix multipliers

// multiply block of B matrix (q x 6) with 12 dReal per row with C vektor (q)
static void Multiply1_12q1 (dReal *A, const dReal *B, const dReal *C, int q)
{
  dIASSERT (q>0 && A && B && C);

  dReal a = 0;
  dReal b = 0;
  dReal c = 0;
  dReal d = 0;
  dReal e = 0;
  dReal f = 0;
  dReal s;

  for(int i=0, k = 0; i<q; k += 12, i++)
  {
    s = C[i]; //C[i] and B[n+k] cannot overlap because its value has been read into a temporary.

    //For the rest of the loop, the only memory dependency (array) is from B[]
    a += B[  k] * s;
    b += B[1+k] * s;
    c += B[2+k] * s;
    d += B[3+k] * s;
    e += B[4+k] * s;
    f += B[5+k] * s;
  }

  A[0] = a;
  A[1] = b;
  A[2] = c;
  A[3] = d;
  A[4] = e;
  A[5] = f;
}

//***************************************************************************
// testing stuff

#ifdef TIMING
#define IFTIMING(x) x
#else
#define IFTIMING(x) ((void)0)
#endif

//***************************************************************************
// various common computations involving the matrix J

// compute iMJ = inv(M)*J'

static void compute_invM_JT (int m, dRealPtr J, dRealMutablePtr iMJ, int *jb,
  dxBody * const *body, dRealPtr invI)
{
  dRealMutablePtr iMJ_ptr = iMJ;
  dRealPtr J_ptr = J;
  for (int i=0; i<m; J_ptr += 12, iMJ_ptr += 12, i++) {
    int b1 = jb[i*2];
    int b2 = jb[i*2+1];
    dReal k1 = body[b1]->invMass;
    for (int j=0; j<3; j++) iMJ_ptr[j] = k1*J_ptr[j];
    const dReal *invIrow1 = invI + 12*b1;
    dMultiply0_331 (iMJ_ptr + 3, invIrow1, J_ptr + 3);
    if (b2 >= 0) {
      dReal k2 = body[b2]->invMass;
      for (int j=0; j<3; j++) iMJ_ptr[j+6] = k2*J_ptr[j+6];
      const dReal *invIrow2 = invI + 12*b2;
      dMultiply0_331 (iMJ_ptr + 9, invIrow2, J_ptr + 9);
    }
  }
}

// compute out = inv(M)*J'*in.
//#ifdef WARM_STARTING
/*static void multiply_invM_JT (int m, int nb, dRealMutablePtr iMJ, int *jb,
  dRealPtr in, dRealMutablePtr out)
{
  dSetZero (out,6*nb);
  dRealPtr iMJ_ptr = iMJ;
  for (int i=0; i<m; i++) {
    int b1 = jb[i*2];
    int b2 = jb[i*2+1];
    const dReal in_i = in[i];
    dRealMutablePtr out_ptr = out + b1*6;
    for (int j=0; j<6; j++) out_ptr[j] += iMJ_ptr[j] * in_i;
    iMJ_ptr += 6;
    if (b2 >= 0) {
      out_ptr = out + b2*6;
      for (int j=0; j<6; j++) out_ptr[j] += iMJ_ptr[j] * in_i;
    }
    iMJ_ptr += 6;
  }
}*/
//#endif

// compute out = J*in.

static void multiply_J (int m, dRealPtr J, int *jb,
  dRealPtr in, dRealMutablePtr out)
{
  dRealPtr J_ptr = J;
  for (int i=0; i<m; i++) {
    int b1 = jb[i*2];
    int b2 = jb[i*2+1];
    dReal sum = 0;
    dRealPtr in_ptr = in + b1*6;
    for (int j=0; j<6; j++) sum += J_ptr[j] * in_ptr[j];
    J_ptr += 6;
    if (b2 >= 0) {
      in_ptr = in + b2*6;
      for (int j=0; j<6; j++) sum += J_ptr[j] * in_ptr[j];
    }
    J_ptr += 6;
    out[i] = sum;
  }
}


// compute out = (J*inv(M)*J' + cfm)*in.
// use z as an nb*6 temporary.
#ifdef WARM_STARTING
static void multiply_J_invM_JT (int m, int nb, dRealMutablePtr J, dRealMutablePtr iMJ, int *jb,
  dRealPtr cfm, dRealMutablePtr z, dRealMutablePtr in, dRealMutablePtr out)
{
  multiply_invM_JT (m,nb,iMJ,jb,in,z);
  multiply_J (m,J,jb,z,out);

  // add cfm
  for (int i=0; i<m; i++) out[i] += cfm[i] * in[i];
}
#endif

//***************************************************************************
// conjugate gradient method with jacobi preconditioner
// THIS IS EXPERIMENTAL CODE that doesn't work too well, so it is ifdefed out.
//
// adding CFM seems to be critically important to this method.

#ifdef USE_CG_LCP

static inline dReal dot (int n, dRealPtr x, dRealPtr y)
{
  dReal sum=0;
  for (int i=0; i<n; i++) sum += x[i]*y[i];
  return sum;
}


// x = y + z*alpha

static inline void add (int n, dRealMutablePtr x, dRealPtr y, dRealPtr z, dReal alpha)
{
  for (int i=0; i<n; i++) x[i] = y[i] + z[i]*alpha;
}

static void CG_LCP (dxWorldProcessContext *context,
  int m, int nb, dRealMutablePtr J, int *jb, dxBody * const *body,
  dRealPtr invI, dRealMutablePtr lambda, dRealMutablePtr cforce, dRealMutablePtr rhs,
  dRealMutablePtr lo, dRealMutablePtr hi, dRealPtr cfm, int *findex,
  dxQuickStepParameters *qs)
{
  const int num_iterations = qs->num_iterations;

  // precompute iMJ = inv(M)*J'
  dReal *iMJ = context->AllocateArray<dReal> (m*12);
  compute_invM_JT (m,J,iMJ,jb,body,invI);

  dReal last_rho = 0;
  dReal *r = context->AllocateArray<dReal> (m);
  dReal *z = context->AllocateArray<dReal> (m);
  dReal *p = context->AllocateArray<dReal> (m);
  dReal *q = context->AllocateArray<dReal> (m);

  // precompute 1 / diagonals of A
  dReal *Ad = context->AllocateArray<dReal> (m);
  dRealPtr iMJ_ptr = iMJ;
  dRealPtr J_ptr = J;
  for (int i=0; i<m; i++) {
    dReal sum = 0;
    for (int j=0; j<6; j++) sum += iMJ_ptr[j] * J_ptr[j];
    if (jb[i*2+1] >= 0) {
      for (int j=6; j<12; j++) sum += iMJ_ptr[j] * J_ptr[j];
    }
    iMJ_ptr += 12;
    J_ptr += 12;
    Ad[i] = REAL(1.0) / (sum + cfm[i]);
  }

#ifdef WARM_STARTING
  // compute residual r = rhs - A*lambda
  multiply_J_invM_JT (m,nb,J,iMJ,jb,cfm,cforce,lambda,r);
  for (int k=0; k<m; k++) r[k] = rhs[k] - r[k];
#else
  dSetZero (lambda,m);
  memcpy (r,rhs,m*sizeof(dReal));    // residual r = rhs - A*lambda
#endif

  for (int iteration=0; iteration < num_iterations; iteration++) {
    for (int i=0; i<m; i++) z[i] = r[i]*Ad[i];  // z = inv(M)*r
    dReal rho = dot (m,r,z);    // rho = r'*z

    // @@@
    // we must check for convergence, otherwise rho will go to 0 if
    // we get an exact solution, which will introduce NaNs into the equations.
    if (rho < 1e-10) {
      printf ("CG returned at iteration %d\n",iteration);
      break;
    }

    if (iteration==0) {
      memcpy (p,z,m*sizeof(dReal));  // p = z
    }
    else {
      add (m,p,z,p,rho/last_rho);  // p = z + (rho/last_rho)*p
    }

    // compute q = (J*inv(M)*J')*p
    multiply_J_invM_JT (m,nb,J,iMJ,jb,cfm,cforce,p,q);

    dReal alpha = rho/dot (m,p,q);    // alpha = rho/(p'*q)
    add (m,lambda,lambda,p,alpha);    // lambda = lambda + alpha*p
    add (m,r,r,q,-alpha);      // r = r - alpha*q
    last_rho = rho;
  }

  // compute cforce = inv(M)*J'*lambda
  multiply_invM_JT (m,nb,iMJ,jb,lambda,cforce);

#if 0
  // measure solution error
  multiply_J_invM_JT (m,nb,J,iMJ,jb,cfm,cforce,lambda,r);
  dReal error = 0;
  for (int i=0; i<m; i++) error += dFabs(r[i] - rhs[i]);
  printf ("lambda error = %10.6e\n",error);
#endif
}

#endif


struct IndexError {
#ifdef REORDER_CONSTRAINTS
  dReal error;    // error to sort on
  int findex;
#endif
  int index;    // row index
};


#ifdef REORDER_CONSTRAINTS

static int compare_index_error (const void *a, const void *b)
{
  const IndexError *i1 = (IndexError*) a;
  const IndexError *i2 = (IndexError*) b;
  if (i1->findex < 0 && i2->findex >= 0) return -1;
  if (i1->findex >= 0 && i2->findex < 0) return 1;
  if (i1->error < i2->error) return -1;
  if (i1->error > i2->error) return 1;
  return 0;
}

#endif

void computeRHSPrecon(dxWorldProcessContext *context, const int m, const int nb,
                      dRealPtr I, dxBody * const *body,
                      const dReal /*stepsize1*/, dRealMutablePtr /*c*/, dRealMutablePtr J,
                      int *jb, dRealMutablePtr rhs_precon, dRealMutablePtr Mvel)
{
    /************************************************************************************/
    /*                                                                                  */
    /*               compute preconditioned rhs                                         */
    /*                                                                                  */
    /*  J J' lambda = J * ( M * dv / dt + fe )                                          */
    /*                                                                                  */
    /************************************************************************************/
    // mimic computation of rhs, but do it with J*M*inv(J) prefixed for preconditioned case.
    BEGIN_STATE_SAVE(context, tmp2state) {
      IFTIMING (dTimerNow ("compute rhs_precon"));

      // compute the "preconditioned" right hand side `rhs_precon'
      // this is slightly different than non precon, where M is left multiplied by the pre J terms
      //
      // Mvel = M*v/h + fe
      //
      dReal *Mvelcurr = Mvel;
      const dReal *Irow = I;
      dxBody *const *const bodyend = body + nb;
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; Mvelcurr+=6, Irow+=12, bodycurr++) {
        dxBody *b_ptr = *bodycurr;
        // dReal body_mass = b_ptr->mass.mass;
        for (int j=0; j<3; j++)
          Mvelcurr[j] = b_ptr->facc[j]; // +  body_mass * b_ptr->lvel[j] * stepsize1;
        dReal tmpa[3];
        for (int j=0; j<3; j++) tmpa[j] = 0; //b_ptr->avel[j] * stepsize1;
        dMultiply0_331 (Mvelcurr + 3,Irow,tmpa);
        for (int k=0; k<3; k++) Mvelcurr[3+k] += b_ptr->tacc[k];
      }
      //
      // rhs_precon = - J * (M*v/h + fe)
      //
      multiply_J (m,J,jb,Mvel,rhs_precon);

      //
      // no need to add constraint violation correction tterm if we assume acceleration is 0
      //
      for (int i=0; i<m; i++) rhs_precon[i] = - rhs_precon[i];


      /*  DEBUG PRINTOUTS
      printf("\n");
      for (int i=0; i<m; i++) printf("c[%d] = %f\n",i,c[i]);
      printf("\n");
      */
    } END_STATE_SAVE(context, tmp2state);
}

static inline dReal dot6(dRealPtr a, dRealPtr b)
{
#ifdef SSE
  __m128d d = _mm_load_pd(a+0) * _mm_load_pd(b+0) + _mm_load_pd(a+2) * _mm_load_pd(b+2) + _mm_load_pd(a+4) * _mm_load_pd(b+4);
  double r[2];
  _mm_store_pd(r, d);
  return r[0] + r[1];
#else
  return a[0] * b[0] +
         a[1] * b[1] +
         a[2] * b[2] +
         a[3] * b[3] +
         a[4] * b[4] +
         a[5] * b[5];
#endif
}

static inline void sum6(dRealMutablePtr a, dReal delta, dRealPtr b)
{
#ifdef SSE
  __m128d __delta = Kf(delta);
  _mm_store_pd(a + 0, _mm_load_pd(a + 0) + __delta * _mm_load_pd(b + 0));
  _mm_store_pd(a + 2, _mm_load_pd(a + 2) + __delta * _mm_load_pd(b + 2));
  _mm_store_pd(a + 4, _mm_load_pd(a + 4) + __delta * _mm_load_pd(b + 4));
#else
  a[0] += delta * b[0];
  a[1] += delta * b[1];
  a[2] += delta * b[2];
  a[3] += delta * b[3];
  a[4] += delta * b[4];
  a[5] += delta * b[5];
#endif
}

static void ComputeRows(
                int /*thread_id*/,
                IndexError* order,
                dxBody* const * /*body*/,
                dxSORLCPParameters params,
                boost::recursive_mutex* /*mutex*/)
{

  #ifdef REPORT_THREAD_TIMING
  struct timeval tv;
  double cur_time;
  gettimeofday(&tv,NULL);
  cur_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  //printf("thread %d started at time %f\n",thread_id,cur_time);
  #endif

  //boost::recursive_mutex::scoped_lock lock(*mutex); // put in caccel read/writes?
  dxQuickStepParameters *qs    = params.qs;
  int startRow                 = params.nStart;   // 0
  int nRows                    = params.nChunkSize; // m
#ifdef USE_1NORM
  int m                        = params.m; // m used for rms error computation
#endif
#ifdef PENETRATION_JVERROR_CORRECTION
  dReal stepsize               = params.stepsize;
  dRealMutablePtr vnew         = params.vnew;
#endif
  int* jb                      = params.jb;
  const int* findex            = params.findex;
  dRealPtr        hi           = params.hi;
  dRealPtr        lo           = params.lo;
  dRealPtr        Adcfm        = params.Adcfm;
  dRealPtr        Adcfm_precon = params.Adcfm_precon;
  dRealMutablePtr rhs          = params.rhs;
  dRealMutablePtr rhs_erp      = params.rhs_erp;
  dRealMutablePtr J            = params.J;
  dRealMutablePtr caccel       = params.caccel;
  dRealMutablePtr caccel_erp   = params.caccel_erp;
  dRealMutablePtr lambda       = params.lambda;
  dRealMutablePtr lambda_erp   = params.lambda_erp;
  dRealMutablePtr iMJ          = params.iMJ;
#ifdef RECOMPUTE_RMS
  dRealMutablePtr delta_error  = params.delta_error;
#endif
  dRealMutablePtr rhs_precon   = params.rhs_precon;
  dRealMutablePtr J_precon     = params.J_precon;
  dRealMutablePtr J_orig       = params.J_orig;
  dRealMutablePtr cforce       = params.cforce;
#ifdef REORDER_CONSTRAINTS
  dRealMutablePtr last_lambda  = params.last_lambda;
  dRealMutablePtr last_lambda_erp  = params.last_lambda_erp;
#endif
#ifdef USE_JOINT_DAMPING
  dRealMutablePtr b_damp       = params.b_damp;
  dRealMutablePtr f_damp       = params.f_damp;
  dRealPtr JiM                 = params.JiM;
  dRealPtr        invI         = params.invI;
  dRealPtr        Ad           = params.Ad;
  dRealMutablePtr v_damp       = params.v_damp;
  dxBody* const* body          = params.body;
  int nb                       = params.nb;
  dReal stepsize               = params.stepsize;  // watchout for PENETRATION_JVERROR_CORRECTION
  dRealMutablePtr J_damp       = params.J_damp;
  dRealPtr coeff_damp   = params.coeff_damp;
  int*            jb_damp      = params.jb_damp;
  dRealMutablePtr v_joint_damp = params.v_joint_damp;
  dRealMutablePtr a_joint_damp = params.a_joint_damp;
  int m_damp                   = params.m_damp;
#endif

  //printf("iiiiiiiii %d %d %d\n",thread_id,jb[0],jb[1]);
  //for (int i=startRow; i<startRow+nRows; i++) // swap within boundary of our own segment
  //  printf("wwwwwwwwwwwww>id %d start %d n %d  order[%d].index=%d\n",thread_id,startRow,nRows,i,order[i].index);



  /*  DEBUG PRINTOUTS
  // print J_orig
  printf("J_orig\n");
  for (int i=startRow; i<startRow+nRows; i++) {
    for (int j=0; j < 12 ; j++) {
      printf("  %12.6f",J_orig[i*12+j]);
    }
    printf("\n");
  }
  printf("\n");

  // print J, J_precon (already premultiplied by inverse of diagonal of LHS) and rhs_precon and rhs
  printf("J_precon\n");
  for (int i=startRow; i<startRow+nRows; i++) {
    for (int j=0; j < 12 ; j++) {
      printf("  %12.6f",J_precon[i*12+j]);
    }
    printf("\n");
  }
  printf("\n");

  printf("J\n");
  for (int i=startRow; i<startRow+nRows; i++) {
    for (int j=0; j < 12 ; j++) {
      printf("  %12.6f",J[i*12+j]);
    }
    printf("\n");
  }
  printf("\n");

  printf("rhs_precon\n");
  for (int i=startRow; i<startRow+nRows; i++)
    printf("  %12.6f",rhs_precon[i]);
  printf("\n");

  printf("rhs\n");
  for (int i=startRow; i<startRow+nRows; i++)
    printf("  %12.6f",rhs[i]);
  printf("\n");
  */

  double rms_error = 0;
  int num_iterations = qs->num_iterations;
  int precon_iterations = qs->precon_iterations;
  double sor_lcp_tolerance = qs->sor_lcp_tolerance;


  // FIME: preconditioning can be defined insdie iterations loop now, becareful to match last iteration with
  //       velocity update
  bool preconditioning;

#ifdef PENETRATION_JVERROR_CORRECTION
  dReal Jvnew_final = 0;
#endif
  for (int iteration=0; iteration < num_iterations + precon_iterations; iteration++) {

    rms_error = 0;

    if (iteration < precon_iterations) preconditioning = true;
    else                               preconditioning = false;

#ifdef REORDER_CONSTRAINTS //FIXME: do it for lambda_erp and last_lambda_erp
    // constraints with findex < 0 always come first.
    if (iteration < 2) {
      // for the first two iterations, solve the constraints in
      // the given order
      IndexError *ordercurr = order+startRow;
      for (int i = startRow; i != startRow+nRows; ordercurr++, i++) {
        ordercurr->error = i;
        ordercurr->findex = findex[i];
        ordercurr->index = i;
      }
    }
    else {
      // sort the constraints so that the ones converging slowest
      // get solved last. use the absolute (not relative) error.
      for (int i=startRow; i<startRow+nRows; i++) {
        dReal v1 = dFabs (lambda[i]);
        dReal v2 = dFabs (last_lambda[i]);
        dReal max = (v1 > v2) ? v1 : v2;
        if (max > 0) {
          //@@@ relative error: order[i].error = dFabs(lambda[i]-last_lambda[i])/max;
          order[i].error = dFabs(lambda[i]-last_lambda[i]);
        }
        else {
          order[i].error = dInfinity;
        }
        order[i].findex = findex[i];
        order[i].index = i;
      }
    }

    //if (thread_id == 0) for (int i=startRow;i<startRow+nRows;i++) printf("=====> %d %d %d %f %d\n",thread_id,iteration,i,order[i].error,order[i].index);

    qsort (order+startRow,nRows,sizeof(IndexError),&compare_index_error);

    //@@@ potential optimization: swap lambda and last_lambda pointers rather
    //    than copying the data. we must make sure lambda is properly
    //    returned to the caller
    memcpy (last_lambda+startRow,lambda+startRow,nRows*sizeof(dReal));

    //if (thread_id == 0) for (int i=startRow;i<startRow+nRows;i++) printf("-----> %d %d %d %f %d\n",thread_id,iteration,i,order[i].error,order[i].index);

#endif
#ifdef RANDOMLY_REORDER_CONSTRAINTS
    if ((iteration & 7) == 0) {
      #ifdef LOCK_WHILE_RANDOMLY_REORDER_CONSTRAINTS
        boost::recursive_mutex::scoped_lock lock(*mutex); // lock for every swap
      #endif
      //  int swapi = dRandInt(i+1); // swap across engire matrix
      for (int i=startRow+1; i<startRow+nRows; i++) { // swap within boundary of our own segment
        int swapi = dRandInt(i+1-startRow)+startRow; // swap within boundary of our own segment
        //printf("xxxxxxxx>id %d swaping order[%d].index=%d order[%d].index=%d\n",thread_id,i,order[i].index,swapi,order[swapi].index);
        IndexError tmp = order[i];
        order[i] = order[swapi];
        order[swapi] = tmp;
      }

      // {
      //   // verify
      //   boost::recursive_mutex::scoped_lock lock(*mutex); // lock for every row
      //   printf("  random id %d iter %d\n",thread_id,iteration);
      //   for (int i=startRow+1; i<startRow+nRows; i++)
      //     printf(" %5d,",i);
      //   printf("\n");
      //   for (int i=startRow+1; i<startRow+nRows; i++)
      //     printf(" %5d;",(int)order[i].index);
      //   printf("\n");
      // }
    }
#endif

    dRealMutablePtr caccel_ptr1;
    dRealMutablePtr caccel_ptr2;
    dRealMutablePtr caccel_erp_ptr1;
    dRealMutablePtr caccel_erp_ptr2;

#ifdef PENETRATION_JVERROR_CORRECTION
    dRealMutablePtr vnew_ptr1;
    dRealMutablePtr vnew_ptr2;
    const dReal stepsize1 = dRecip(stepsize);
    dReal Jvnew = 0;
#endif
    dRealMutablePtr cforce_ptr1;
    dRealMutablePtr cforce_ptr2;
    for (int i=startRow; i<startRow+nRows; i++) {
      //boost::recursive_mutex::scoped_lock lock(*mutex); // lock for every row

      // @@@ potential optimization: we could pre-sort J and iMJ, thereby
      //     linearizing access to those arrays. hmmm, this does not seem
      //     like a win, but we should think carefully about our memory
      //     access pattern.

      int index = order[i].index;

      dReal delta,delta_erp;
      dReal delta_precon;

      // setup pointers to the constraint accelerations and forces
      {
        int b1 = jb[index*2];
        int b2 = jb[index*2+1];
        caccel_ptr1 = caccel + 6*b1;
        caccel_ptr2 = (b2 >= 0) ? caccel + 6*b2 : NULL;
        caccel_erp_ptr1 = caccel_erp + 6*b1;
        caccel_erp_ptr2 = (b2 >= 0) ? caccel_erp + 6*b2 : NULL;
#ifdef PENETRATION_JVERROR_CORRECTION
        vnew_ptr1 = vnew + 6*b1;
        vnew_ptr2 = (b2 >= 0) ? vnew + 6*b2 : NULL;
#endif
        cforce_ptr1 = cforce + 6*b1;
        cforce_ptr2 = (b2 >= 0) ? cforce + 6*b2 : NULL;
      }

#ifdef USE_JOINT_DAMPING
      /*************************************************************/
      /* compute b_damp                                            */
      /* b is to be modified by b_damp                             */
      /* where b_damp = -J*inv(M)*f_damp / Ad  (since b is rhs/Ad) */
      /*                                                           */
      /* initially f_damp is 0, so motion is undamped on first     */
      /* iteration.                                                */
      /*                                                           */
      /*************************************************************/
      {
        b_damp[index] = 0;
        int b1 = jb[index*2];
        int b2 = jb[index*2+1];
        dRealMutablePtr f_damp_ptr1 = f_damp + 6*b1;
        dRealMutablePtr f_damp_ptr2 = (b2 >= 0) ? f_damp + 6*b2 : NULL;
   
        dRealPtr JiM_ptr = JiM + index*12;

        // compute b_damp = JiM * f_damp, b_damp is preset to zero already
        for (int j=0;j<6;j++) {
          b_damp[index] += JiM_ptr[j] * f_damp_ptr1[j];
          if (b2>=0) b_damp[index] += JiM_ptr[j+6] * f_damp_ptr2[j];
        }

        // and scale JiM by Ad
        b_damp[index] *= Ad[index];
        // FIXME: find some kind of limiters that works as artificial dampers
        // b_damp must make b smaller
        // so b_damp must have opposite sign as b
        // and abs(b_damp) < abs(b)
        //if (b_damp[index]*b[index]>0) b_damp[index]=0;
        //if (dFabs(b_damp[index])>dFabs(b[index])) b_damp[index]=-b[index];
      }
#endif


      dReal old_lambda        = lambda[index];
      dReal old_lambda_erp    = lambda_erp[index];

      //
      // caccel is the constraint accel in the non-precon case
      // cforce is the constraint force in the     precon case
      // J_precon and J differs essentially in Ad and Ad_precon,
      //  Ad is derived from diagonal of J inv(M) J'
      //  Ad_precon is derived from diagonal of J J'
      //
      // caccel_erp is from the non-precon case with erp turned on
      //
      if (preconditioning) {
        // update delta_precon
        delta_precon = rhs_precon[index] - old_lambda*Adcfm_precon[index];

        dRealPtr J_ptr = J_precon + index*12;

        // for preconditioned case, update delta using cforce, not caccel

        delta_precon -= dot6(cforce_ptr1, J_ptr);
        if (cforce_ptr2)
          delta_precon -= dot6(cforce_ptr2, J_ptr + 6);

        // set the limits for this constraint.
        // this is the place where the QuickStep method differs from the
        // direct LCP solving method, since that method only performs this
        // limit adjustment once per time step, whereas this method performs
        // once per iteration per constraint row.
        // the constraints are ordered so that all lambda[] values needed have
        // already been computed.
        dReal hi_act, lo_act;
        if (findex[index] >= 0) {
          hi_act = dFabs (hi[index] * lambda[findex[index]]);
          lo_act = -hi_act;
        } else {
          hi_act = hi[index];
          lo_act = lo[index];
        }

        // compute lambda and clamp it to [lo,hi].
        // @@@ SSE not a win here
#if 1
        dReal new_lambda = old_lambda+ delta_precon;
        if (new_lambda < lo_act) {
          delta_precon = lo_act-old_lambda;
          lambda[index] = lo_act;
        }
        else if (new_lambda > hi_act) {
          delta_precon = hi_act-old_lambda;
          lambda[index] = hi_act;
        }
        else {
          lambda[index] = new_lambda;
        }
#else
        dReal nl = old_lambda+ delta_precon;
        _mm_store_sd(&nl, _mm_max_sd(_mm_min_sd(_mm_load_sd(&nl), _mm_load_sd(&hi_act)), _mm_load_sd(&lo_act)));
        lambda[index] = nl;
        delta_precon = nl - old_lambda;
#endif

        // update cforce (this is strictly for the precon case)
        {
          // for preconditioning case, compute cforce
          J_ptr = J_orig + index*12;

          // update cforce.
          sum6(cforce_ptr1, delta_precon, J_ptr);
          if (cforce_ptr2)
            sum6(cforce_ptr2, delta_precon, J_ptr + 6);
        }

        // record error (for the non-erp version)
        rms_error += delta_precon*delta_precon;
#ifdef RECOMPUTE_RMS
        delta_error[index] = dFabs(delta_precon);
#endif
        old_lambda_erp = old_lambda;
        lambda_erp[index] = lambda[index];
      }
      else // non-precon case
      {
        {
          // FOR erp = 0

          // NOTE:
          // for this update, we need not throw away J*v(n+1)/h term from rhs
          //   ...so adding it back, but remember rhs has already been
          //      scaled by Ad_i, so we need to do the same to J*v(n+1)/h
          //      but given that J is already scaled by Ad_i, we don't have
          //      to do it explicitly here
          delta =
#ifdef PENETRATION_JVERROR_CORRECTION
                 Jvnew_final +
#endif
                rhs[index] - old_lambda*Adcfm[index];

#ifdef USE_JOINT_DAMPING
          /***************************************************************************/
          /* b is to be modified by b_damp = -J*inv(M)*f_damp / Ad since b is rhs/Ad */
          /***************************************************************************/
          delta += b_damp[index];
#endif
          dRealPtr J_ptr = J + index*12;
          delta -= dot6(caccel_ptr1, J_ptr);
          if (caccel_ptr2)
            delta -= dot6(caccel_ptr2, J_ptr + 6);

          // set the limits for this constraint.
          // this is the place where the QuickStep method differs from the
          // direct LCP solving method, since that method only performs this
          // limit adjustment once per time step, whereas this method performs
          // once per iteration per constraint row.
          // the constraints are ordered so that all lambda[] values needed have
          // already been computed.
          dReal hi_act, lo_act;
          if (findex[index] >= 0) {
            hi_act = dFabs (hi[index] * lambda[findex[index]]);
            lo_act = -hi_act;
          } else {
            hi_act = hi[index];
            lo_act = lo[index];
          }

          // compute lambda and clamp it to [lo,hi].
          // @@@ SSE not a win here
#if 1
          dReal new_lambda = old_lambda + delta;
          if (new_lambda < lo_act) {
            delta = lo_act-old_lambda;
            lambda[index] = lo_act;
          }
          else if (new_lambda > hi_act) {
            delta = hi_act-old_lambda;
            lambda[index] = hi_act;
          }
          else {
            lambda[index] = new_lambda;
          }
#else
          dReal nl = old_lambda + delta;
          _mm_store_sd(&nl, _mm_max_sd(_mm_min_sd(_mm_load_sd(&nl), _mm_load_sd(&hi_act)), _mm_load_sd(&lo_act)));
          lambda[index] = nl;
          delta = nl - old_lambda;
#endif

          // update caccel
          {
            // for non-precon case, update caccel
            dRealPtr iMJ_ptr = iMJ + index*12;

            // update caccel.
            sum6(caccel_ptr1, delta, iMJ_ptr);
            if (caccel_ptr2)
              sum6(caccel_ptr2, delta, iMJ_ptr + 6);

#ifdef PENETRATION_JVERROR_CORRECTION
            // update vnew incrementally
            //   add stepsize * delta_caccel to the body velocity
            //   vnew = vnew + dt * delta_caccel
            sum6(vnew_ptr1, stepsize*delta, iMJ_ptr);;
            if (caccel_ptr2)
              sum6(vnew_ptr2, stepsize*delta, iMJ_ptr + 6);

            // COMPUTE Jvnew = J*vnew/h*Ad
            //   but J is already scaled by Ad, and we multiply by h later
            //   so it's just Jvnew = J*vnew here
            if (iteration >= num_iterations-7) {
              // check for non-contact bilateral constraints only
              // I've set findex to -2 for contact normal constraint
              if (findex[index] == -1) {
                dRealPtr J_ptr = J + index*12;
                Jvnew = dot6(vnew_ptr1,J_ptr);
                if (caccel_ptr2)
                  Jvnew += dot6(vnew_ptr2,J_ptr+6);
                //printf("iter [%d] findex [%d] Jvnew [%f] lo [%f] hi [%f]\n",
                //       iteration, findex[index], Jvnew, lo[index], hi[index]);
              }
            }
            //printf("iter [%d] vnew [%f,%f,%f,%f,%f,%f] Jvnew [%f]\n",
            //       iteration,
            //       vnew_ptr1[0], vnew_ptr1[1], vnew_ptr1[2],
            //       vnew_ptr1[3], vnew_ptr1[4], vnew_ptr1[5],Jvnew);
#endif

          }
        }
        // record error (for the non-erp version)
        rms_error += delta*delta;
#ifdef RECOMPUTE_RMS
        delta_error[index] = dFabs(delta);
#endif
        {
          // FOR erp != 0 (i.e. projection)
          // for rhs_erp  note: Adcfm does not have erp because it is on the lhs
          delta_erp = rhs_erp[index] - old_lambda_erp*Adcfm[index];
          dRealPtr J_ptr = J + index*12;
          delta_erp -= dot6(caccel_erp_ptr1, J_ptr);
          if (caccel_erp_ptr2)
            delta_erp -= dot6(caccel_erp_ptr2, J_ptr + 6);

          // for the _erp version
          // set the limits for this constraint.
          // this is the place where the QuickStep method differs from the
          // direct LCP solving method, since that method only performs this
          // limit adjustment once per time step, whereas this method performs
          // once per iteration per constraint row.
          // the constraints are ordered so that all lambda_erp[] values needed have
          // already been computed.
          dReal hi_act, lo_act;
          if (findex[index] >= 0) {
            hi_act = dFabs (hi[index] * lambda_erp[findex[index]]);
            lo_act = -hi_act;
          } else {
            hi_act = hi[index];
            lo_act = lo[index];
          }

          // compute lambda and clamp it to [lo,hi].
          // @@@ SSE not a win here
#if 1
          dReal new_lambda_erp = old_lambda_erp + delta_erp;
          if (new_lambda_erp < lo_act) {
            delta_erp = lo_act-old_lambda_erp;
            lambda_erp[index] = lo_act;
          }
          else if (new_lambda_erp > hi_act) {
            delta_erp = hi_act-old_lambda_erp;
            lambda_erp[index] = hi_act;
          }
          else {
            lambda_erp[index] = new_lambda_erp;
          }
#else
          dReal nl = old_lambda_erp + delta_erp;
          _mm_store_sd(&nl, _mm_max_sd(_mm_min_sd(_mm_load_sd(&nl), _mm_load_sd(&hi_act)), _mm_load_sd(&lo_act)));
          lambda_erp[index] = nl;
          delta_erp = nl - old_lambda_erp;
#endif
          // update caccel_erp
          if (!preconditioning)
          {
            // for non-precon case, update caccel_erp
            dRealPtr iMJ_ptr = iMJ + index*12;

            // update caccel_erp.
            sum6(caccel_erp_ptr1, delta_erp, iMJ_ptr);
            if (caccel_erp_ptr2)
              sum6(caccel_erp_ptr2, delta_erp, iMJ_ptr + 6);

          }
        }
      }



      //@@@ a trick that may or may not help
      //dReal ramp = (1-((dReal)(iteration+1)/(dReal)iterations));
      //delta *= ramp;

    } // end of for loop on m

#ifdef PENETRATION_JVERROR_CORRECTION
    Jvnew_final = Jvnew*stepsize1;
    Jvnew_final = Jvnew_final > 1.0 ? 1.0 : ( Jvnew_final < -1.0 ? -1.0 : Jvnew_final );
#endif

#ifdef USE_JOINT_DAMPING
    /****************************************************************/
    /* compute v_damp per caccel update                             */
    /*   based on all external forces fe, caccel, f_damp            */
    /*   v_damp should have started out same as v(n)                */
    /*   v_damp should end up being v(n+1)                          */
    /*                                                              */
    /*  v_damp = v_current + stepsize * invM * f_all                */
    /*                                                              */
    /****************************************************************/
    {
      IFTIMING (dTimerNow ("velocity update due to f_damp"));
      const dReal *invIrow = invI;
      dRealMutablePtr f_damp_ptr = f_damp;
      dRealMutablePtr v_damp_ptr = v_damp;
      dxBody *const *const bodyend = body + nb;
      const dReal *caccel_ptr = caccel;

      for (dxBody *const *bodycurr = body; bodycurr != bodyend; caccel_ptr+=6, invIrow += 12, f_damp_ptr+=6, v_damp_ptr+=6, bodycurr++) {
        // f_damp should be updated in SOR LCP

        // compute the velocity update:
        // add stepsize * invM * f_damp to the body velocity
        dxBody *b = *bodycurr;
        dReal body_invMass_mul_stepsize = stepsize * b->invMass;
        dReal tmp3[3];
        for (int j=0; j<3; j++) {
          // note that caccel is an acceleration, hence there is
          // no premultiplying of invM here (compare to update due to external force 'facc' below)
          // add stepsize * caccel to the body velocity
          v_damp_ptr[j]   = b->lvel[j] + stepsize * caccel_ptr[j] + body_invMass_mul_stepsize * ( b->facc[j] + f_damp_ptr[j] );
          v_damp_ptr[j+3] = b->avel[j] + stepsize * caccel_ptr[j+3];

          // accumulate step*torques
          tmp3[j] = stepsize*(b->tacc[j] + f_damp_ptr[j+3]);
        }
        // v_damp = invI * f_damp
        dMultiplyAdd0_331 (v_damp_ptr+3, invIrow, tmp3);
      }
    }

    /****************************************************************/
    /* compute f_damp per v_damp update                             */
    /* compute damping force f_damp = J_damp' * B * J_damp * v_damp */
    /*                                                              */
    /*  we probably want to apply some kind of limiter on f_damp    */
    /*  based on changes in v_damp.                                 */
    /*                                                              */
    /*  for starters, ramp up damping to increase stability.        */
    /*                                                              */
    /****************************************************************/
    {
      // dSetZero (f_damp,6*nb); // reset f_damp, following update skips around, so cannot set to 0 inline
      dRealPtr J_damp_ptr = J_damp;
      // compute f_damp and velocity updates
      // first compute v_joint_damp = J_damp * v_damp
      // v_joint_damp is m_damp X 1 single column vector
      for (int j=0; j<m_damp;J_damp_ptr+=12, j++) {
        int b1 = jb_damp[j*2];
        int b2 = jb_damp[j*2+1];

        // ramp-up coefficient for the last quarter of the iteration
        // turn damping off if only 1 inner iteration is requested
        dReal alpha = 0.0;
        if (num_iterations > 1 && iteration >= 3*(num_iterations-1)/4)
          alpha = ((dReal)iteration - 3.0*(num_iterations-1)/4.0) /
                        ((dReal)(num_iterations-1)/4.0);

        v_joint_damp[j] = 0;
        for (int k=0;k<6;k++) v_joint_damp[j] += J_damp_ptr[k] * v_damp[b1*6+k];
        if (b2 >= 0) for (int k=0;k<6;k++) v_joint_damp[j] += J_damp_ptr[k+6] * v_damp[b2*6+k];

        // printf("%d [%d] a=%f v= %f\t",iteration, j, alpha, v_joint_damp[j]);

        // multiply by damping coefficients (B is diagnoal)
        dReal tmpv = v_joint_damp[j] * alpha * coeff_damp[j];

        // so now v_joint_damp = B * J_damp * v_damp
        // update f_damp = J_damp' * v_joint_damp
        for (int k=0; k<6; k++) f_damp[b1*6+k] = -J_damp_ptr[k]*tmpv;
        if (b2 >= 0) for (int k=0; k<6; k++) f_damp[b2*6+k] = -J_damp_ptr[6+k]*tmpv;

        // for (int k=0;k<6;k++)
        //   printf("%f\t", f_damp[b1*6+k]);

#define TRUNCATE_JOINT_DAMPING
#ifdef TRUNCATE_JOINT_DAMPING
        // if a_joint_damp (damping acceleration at the joint:  J * f_damp * invM ) * dt > v_joint_damp,
        // then damping force will likely destabilize the system.
        // try reducing f_damp so a_joint_damp * dt <= v_joint_damp.

        // compute a_joint_damp
        // can consolidate this computation later
        dReal tmpa[3];
        a_joint_damp[j] = 0;
        dReal invM1 = body[b1]->invMass;
        const dReal *invI1 = invI + 12 * b1;
        for (int k=0;k<3;k++) a_joint_damp[j] += J_damp_ptr[k] * f_damp[b1*6+k] * invM1;
        dMultiplyAdd0_331 (tmpa, invI1, f_damp+b1*6+3);
        for (int k=3;k<6;k++) a_joint_damp[j] += J_damp_ptr[k] * tmpa[k-3];
        if (b2 >= 0)
        {
          dReal invM2 = body[b2]->invMass;
          const dReal *invI2 = invI + 12 * b2;
          for (int k=0;k<3;k++) a_joint_damp[j] += J_damp_ptr[k] * f_damp[b2*6+k] * invM2;
          dMultiplyAdd0_331 (tmpa, invI2, f_damp+b2*6+3);
          for (int k=3;k<6;k++) a_joint_damp[j] += J_damp_ptr[k] * tmpa[k-3];
        }

        // if damping force is too large, scale f_damp down by ratio and a safety factor
        if (fabs(v_joint_damp[j]) > 1.0e-6)
        {
          dReal ratio_damp = a_joint_damp[j]*stepsize / v_joint_damp[j];
          // printf("ratio [%f]\tdv [%f]/\tv0 [%f]\t", ratio_damp, a_joint_damp[j]*stepsize , v_joint_damp[j]);
          if (ratio_damp < -1.0)
          {
            for (int k=0; k<6; k++) f_damp[b1*6+k] /= 2.0*fabs(ratio_damp);
            if (b2 >= 0) for (int k=0; k<6; k++) f_damp[b2*6+k] /= 2.0*fabs(ratio_damp);

            // printf("\nf_damp_new\t");
            // for (int k=0;k<6;k++)
            //   printf("%f\t", f_damp[b1*6+k]);
          }
        }
#endif
        // printf("\n");
      }

    }
#endif






    // DO WE NEED TO COMPUTE NORM ACROSS ENTIRE SOLUTION SPACE (0,m)?
    // since local convergence might produce errors in other nodes?
#ifdef RECOMPUTE_RMS
    // recompute rms_error to be sure swap is not corrupting arrays
    rms_error = 0;
    #ifdef USE_1NORM
        //for (int i=startRow; i<startRow+nRows; i++)
        for (int i=0; i<m; i++)
        {
          rms_error = dFabs(delta_error[order[i].index]) > rms_error ? dFabs(delta_error[order[i].index]) : rms_error; // 1norm test
        }
    #else // use 2 norm
        //for (int i=startRow; i<startRow+nRows; i++)
        for (int i=0; i<m; i++)  // use entire solution vector errors
          rms_error += delta_error[order[i].index]*delta_error[order[i].index]; ///(dReal)nRows;
        rms_error = sqrt(rms_error/(dReal)nRows);
    #endif
#else
    rms_error = sqrt(rms_error/(dReal)nRows);
#endif

    //printf("------ %d %d %20.18f\n",thread_id,iteration,rms_error);

    //for (int i=startRow; i<startRow+nRows; i++) printf("debug: %d %f\n",i,delta_error[i]);


    //{
    //  // verify
    //  boost::recursive_mutex::scoped_lock lock(*mutex); // lock for every row
    //  printf("  random id %d iter %d\n",thread_id,iteration);
    //  for (int i=startRow+1; i<startRow+nRows; i++)
    //    printf(" %10d,",i);
    //  printf("\n");
    //  for (int i=startRow+1; i<startRow+nRows; i++)
    //    printf(" %10d;",order[i].index);
    //  printf("\n");
    //  for (int i=startRow+1; i<startRow+nRows; i++)
    //    printf(" %10.8f,",delta_error[i]);
    //  printf("\n%f\n",rms_error);
    //}

#ifdef SHOW_CONVERGENCE
    printf("MONITOR: id: %d iteration: %d error: %20.16f\n",thread_id,iteration,rms_error);
#endif

    if (rms_error < sor_lcp_tolerance)
    {
      #ifdef REPORT_MONITOR
        printf("CONVERGED: id: %d steps: %d rms(%20.18f < %20.18f)\n",thread_id,iteration,rms_error,sor_lcp_tolerance);
      #endif
      if (iteration < precon_iterations) iteration = precon_iterations; // goto non-precon step
      else                               break;                         // finished
    }
    else if (iteration == num_iterations + precon_iterations -1)
    {
      #ifdef REPORT_MONITOR
        printf("WARNING: id: %d did not converge in %d steps, rms(%20.18f > %20.18f)\n",thread_id,num_iterations,rms_error,sor_lcp_tolerance);
      #endif
    }

  } // end of for loop on iterations

  //printf("vnew: ");
  //for (int i=0; i<6*nb; i++) printf(" %f ",vnew[i]);
  //printf("\n");



  qs->rms_error          = rms_error;

  #ifdef REPORT_THREAD_TIMING
  gettimeofday(&tv,NULL);
  double end_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  printf("      quickstep row thread %d start time %f ended time %f duration %f\n",thread_id,cur_time,end_time,end_time - cur_time);
  #endif
}

//***************************************************************************
// SOR_LCP method
//
// nb is the number of bodies in the body array.
// J is an m*12 matrix of constraint rows
// jb is an array of first and second body numbers for each constraint row
// invI is the global frame inverse inertia for each body (stacked 3x3 matrices)
//
// this returns lambda and cforce (the constraint force).
// note: cforce is returned as inv(M)*J'*lambda,
//   the constraint force is actually J'*lambda
//
// rhs, lo and hi are modified on exit
//
static void SOR_LCP (dxWorldProcessContext *context,
  const int m, const int nb, dRealMutablePtr J, dRealMutablePtr J_precon, dRealMutablePtr J_orig,
#ifdef PENETRATION_JVERROR_CORRECTION
  dRealMutablePtr vnew,
#endif
  int *jb, dxBody * const *body,
  dRealPtr invI, dRealPtr I, dRealMutablePtr lambda, dRealMutablePtr lambda_erp,
  dRealMutablePtr caccel, dRealMutablePtr caccel_erp, dRealMutablePtr cforce,
  dRealMutablePtr rhs, dRealMutablePtr rhs_erp, dRealMutablePtr rhs_precon,
  dRealPtr lo, dRealPtr hi, dRealPtr cfm, const int *findex,
  dxQuickStepParameters *qs,
#ifdef USE_TPROW
  boost::threadpool::pool* row_threadpool,
#endif
#ifdef USE_JOINT_DAMPING
  const int m_damp, dRealMutablePtr J_damp, dRealPtr coeff_damp, int *jb_damp,dRealMutablePtr v_damp,
  dRealMutablePtr f_damp,dRealMutablePtr v_joint_damp,dRealMutablePtr a_joint_damp, dRealPtr JiM,
#endif
  const dReal stepsize)
{
#ifdef WARM_STARTING
  {
    // for warm starting, this seems to be necessary to prevent
    // jerkiness in motor-driven joints. i have no idea why this works.
    for (int i=0; i<m; i++) {
      lambda[i] *= 0.9;
      lambda_erp[i] *= 0.9;
    }
  }
#else
  dSetZero (lambda,m);
  dSetZero (lambda_erp,m);
#endif

  // precompute iMJ = inv(M)*J'
  dReal *iMJ = context->AllocateArray<dReal> (m*12);
  compute_invM_JT (m,J,iMJ,jb,body,invI);

  // compute cforce=(inv(M)*J')*lambda. we will incrementally maintain cforce
  // as we change lambda.
#ifdef WARM_STARTING
  multiply_invM_JT (m,nb,J,jb,lambda,cforce);
  multiply_invM_JT (m,nb,iMJ,jb,lambda,caccel);
#else
  dSetZero (caccel,nb*6);
  dSetZero (caccel_erp,nb*6);
  dSetZero (cforce,nb*6);
#endif

  dReal *Ad = context->AllocateArray<dReal> (m);

  {
    const dReal sor_w = qs->w;    // SOR over-relaxation parameter
    // precompute 1 / diagonals of A
    dRealPtr iMJ_ptr = iMJ;
    dRealPtr J_ptr = J;
    for (int i=0; i<m; J_ptr += 12, iMJ_ptr += 12, i++) {
      dReal sum = 0;
      for (int j=0; j<6; j++) sum += iMJ_ptr[j] * J_ptr[j];

      if (jb[i*2+1] >= 0) {
        for (int k=6; k<12; k++) sum += iMJ_ptr[k] * J_ptr[k];
      }
      Ad[i] = sor_w / (sum + cfm[i]);
    }
  }

  // recompute Ad for preconditioned case, Ad_precon is similar to Ad but
  //   whereas Ad is 1 over diagonals of J inv(M) J'
  //    Ad_precon is 1 over diagonals of J J'
  dReal *Ad_precon = context->AllocateArray<dReal> (m);

  {
    const dReal sor_w = qs->w;    // SOR over-relaxation parameter
    // precompute 1 / diagonals of A
    // preconditioned version uses J instead of iMJ
    dRealPtr J_ptr = J;
    for (int i=0; i<m; J_ptr += 12, i++) {
      dReal sum = 0;
      for (int j=0; j<6; j++) sum += J_ptr[j] * J_ptr[j];
      if (jb[i*2+1] >= 0) {
        for (int k=6; k<12; k++) sum += J_ptr[k] * J_ptr[k];
      }
      Ad_precon[i] = sor_w / (sum + cfm[i]);
    }
  }

  /********************************/
  /* allocate for Adcfm           */
  /* which is a mX1 column vector */
  /********************************/
  // compute Adcfm_precon for the preconditioned case
  //   do this first before J gets altered (J's diagonals gets premultiplied by Ad)
  //   and save a copy of J into J_orig
  //   as J becomes J * Ad, J_precon becomes J * Ad_precon
  dReal *Adcfm_precon = context->AllocateArray<dReal> (m);


  {
    // NOTE: This may seem unnecessary but it's indeed an optimization
    // to move multiplication by Ad[i] and cfm[i] out of iteration loop.

    // scale J_precon and rhs_precon by Ad
    // copy J_orig
    dRealMutablePtr J_ptr = J;
    dRealMutablePtr J_precon_ptr = J_precon;
    dRealMutablePtr J_orig_ptr = J_orig;
    for (int i=0; i<m; J_ptr += 12, J_precon_ptr += 12, J_orig_ptr += 12, i++) {
      dReal Ad_precon_i = Ad_precon[i];
      for (int j=0; j<12; j++) {
        J_precon_ptr[j] = J_ptr[j] * Ad_precon_i;
        J_orig_ptr[j] = J_ptr[j]; //copy J
      }
      rhs_precon[i] *= Ad_precon_i;
      // scale Ad by CFM. N.B. this should be done last since it is used above
      Adcfm_precon[i] = Ad_precon_i * cfm[i];
    }
  }

  dReal *Adcfm = context->AllocateArray<dReal> (m);

  {
    // NOTE: This may seem unnecessary but it's indeed an optimization
    // to move multiplication by Ad[i] and cfm[i] out of iteration loop.

    // scale J and rhs by Ad
    dRealMutablePtr J_ptr = J;
    for (int i=0; i<m; J_ptr += 12, i++) {
      dReal Ad_i = Ad[i];
      for (int j=0; j<12; j++) {
        J_ptr[j] *= Ad_i;
      }
      rhs[i] *= Ad_i;
      rhs_erp[i] *= Ad_i;
      // scale Ad by CFM. N.B. this should be done last since it is used above
      Adcfm[i] = Ad_i * cfm[i];
    }
  }


  // order to solve constraint rows in
  IndexError *order = context->AllocateArray<IndexError> (m);

  dReal *delta_error = context->AllocateArray<dReal> (m);

#ifndef REORDER_CONSTRAINTS
  {
    // make sure constraints with findex < 0 come first.
    IndexError *orderhead = order, *ordertail = order + (m - 1);

    // Fill the array from both ends
    for (int i=0; i<m; i++) {
      if (findex[i] < 0) {
        orderhead->index = i; // Place them at the front
        ++orderhead;
      } else {
        ordertail->index = i; // Place them at the end
        --ordertail;
      }
    }
    dIASSERT (orderhead-ordertail==1);
  }
#endif

#ifdef REORDER_CONSTRAINTS
  // the lambda computed at the previous iteration.
  // this is used to measure error for when we are reordering the indexes.
  dReal *last_lambda = context->AllocateArray<dReal> (m);
  dReal *last_lambda_erp = context->AllocateArray<dReal> (m);
#endif

#ifdef USE_JOINT_DAMPING
  dReal *b_damp = context->AllocateArray<dReal> (m);
#endif

  boost::recursive_mutex mutex;

  // number of chunks must be at least 1
  // (single iteration, through all the constraints)
  int num_chunks = qs->num_chunks > 0 ? qs->num_chunks : 1; // min is 1

  // prepare pointers for threads
  dxSORLCPParameters *params = context->AllocateArray<dxSORLCPParameters> (num_chunks);

  // divide into chunks sequentially
  int chunk = m / num_chunks+1;
  chunk = chunk > 0 ? chunk : 1;
  int thread_id = 0;


  #ifdef REPORT_THREAD_TIMING
  // timing
  struct timeval tv;
  double cur_time;
  gettimeofday(&tv,NULL);
  cur_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  //printf("    quickstep start threads at time %f\n",cur_time);
  #endif


  IFTIMING (dTimerNow ("start pgs rows"));
  for (int i=0; i<m; i+= chunk,thread_id++)
  {
    //for (int ijk=0;ijk<m;ijk++) printf("thread_id> id:%d jb[%d]=%d\n",thread_id,ijk,jb[ijk]);

    int nStart = i - qs->num_overlap < 0 ? 0 : i - qs->num_overlap;
    int nEnd   = i + chunk + qs->num_overlap;
    if (nEnd > m) nEnd = m;
    // if every one reorders constraints, this might just work
    // comment out below if using defaults (0 and m) so every thread runs through all joints
    params[thread_id].qs  = qs ;
    params[thread_id].nStart = nStart;   // 0
    params[thread_id].nChunkSize = nEnd - nStart; // m
    params[thread_id].m = m; // m
    params[thread_id].jb = jb;
    params[thread_id].findex = findex;
    params[thread_id].hi = hi;
    params[thread_id].lo = lo;
    params[thread_id].I= I;
    params[thread_id].Adcfm = Adcfm;
    params[thread_id].Adcfm_precon = Adcfm_precon;
    params[thread_id].rhs = rhs;
    params[thread_id].rhs_erp = rhs_erp;
    params[thread_id].J = J;
    params[thread_id].caccel = caccel;
    params[thread_id].caccel_erp = caccel_erp;
    params[thread_id].lambda = lambda;
    params[thread_id].lambda_erp = lambda_erp;
    params[thread_id].iMJ = iMJ;
    params[thread_id].delta_error  = delta_error ;
    params[thread_id].rhs_precon  = rhs_precon ;
    params[thread_id].J_precon  = J_precon ;
    params[thread_id].J_orig  = J_orig ;
    params[thread_id].cforce  = cforce ;
#ifdef REORDER_CONSTRAINTS
    params[thread_id].last_lambda  = last_lambda ;
    params[thread_id].last_lambda_erp  = last_lambda_erp ;
#endif
#ifdef PENETRATION_JVERROR_CORRECTION
    params[thread_id].stepsize = stepsize;
    params[thread_id].vnew  = vnew ;
#endif
#ifdef USE_JOINT_DAMPING
    params[thread_id].body    = body ;
    params[thread_id].b_damp  = b_damp ;
    params[thread_id].f_damp  = f_damp ;
    params[thread_id].JiM     = JiM    ;
    params[thread_id].invI    = invI;
    params[thread_id].Ad      = Ad;
    params[thread_id].v_damp  = v_damp ;
    params[thread_id].nb = nb;
    params[thread_id].stepsize = stepsize;
    params[thread_id].J_damp = J_damp       ;
    params[thread_id].coeff_damp = coeff_damp   ;
    params[thread_id].jb_damp    = jb_damp ;
    params[thread_id].v_joint_damp = v_joint_damp ;
    params[thread_id].a_joint_damp = a_joint_damp ;
    params[thread_id].m_damp = m_damp ;
#endif

#ifdef REPORT_MONITOR
    printf("thread summary: id %d i %d m %d chunk %d start %d end %d \n",thread_id,i,m,chunk,nStart,nEnd);
#endif
#ifdef USE_TPROW
    if (row_threadpool && row_threadpool->size() > 0)
      row_threadpool->schedule(boost::bind(ComputeRows,thread_id,order, body, params[thread_id], &mutex));
    else //automatically skip threadpool if only 1 thread allocated
      ComputeRows(thread_id,order, body, params[thread_id], &mutex);
#else
    ComputeRows(thread_id,order, body, params[thread_id], &mutex);
#endif
  }


  // check time for scheduling, this is usually very quick
  //gettimeofday(&tv,NULL);
  //double wait_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  //printf("      quickstep done scheduling start time %f stopped time %f duration %f\n",cur_time,wait_time,wait_time - cur_time);

#ifdef USE_TPROW
  IFTIMING (dTimerNow ("wait for threads"));
  if (row_threadpool && row_threadpool->size() > 0)
    row_threadpool->wait();
  IFTIMING (dTimerNow ("threads done"));
#endif



  #ifdef REPORT_THREAD_TIMING
  gettimeofday(&tv,NULL);
  double end_time = (double)tv.tv_sec + (double)tv.tv_usec / 1.e6;
  printf("    quickstep threads start time %f stopped time %f duration %f\n",cur_time,end_time,end_time - cur_time);
  #endif
}

struct dJointWithInfo1
{
  dxJoint *joint;
  dxJoint::Info1 info;
};

void dxQuickStepper (dxWorldProcessContext *context,
  dxWorld *world, dxBody * const *body, int nb,
  dxJoint * const *_joint, int _nj, dReal stepsize)
{
  IFTIMING(dTimerStart("preprocessing"));

  const dReal stepsize1 = dRecip(stepsize);

  {
    // number all bodies in the body list - set their tag values
    for (int i=0; i<nb; i++) body[i]->tag = i;
  }

  // for all bodies, compute the inertia tensor and its inverse in the global
  // frame, and compute the rotational force and add it to the torque
  // accumulator. I and invI are a vertical stack of 3x4 matrices, one per body.
  dReal *invI = context->AllocateArray<dReal> (3*4*nb);
  dReal *I = context->AllocateArray<dReal> (3*4*nb);

  {
    dReal *invIrow = invI;
    dReal *Irow = I;
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; invIrow += 12, Irow += 12, bodycurr++) {
      dMatrix3 tmp;
      dxBody *b_ptr = *bodycurr;

      // compute inverse inertia tensor in global frame
      dMultiply2_333 (tmp,b_ptr->invI,b_ptr->posr.R);
      dMultiply0_333 (invIrow,b_ptr->posr.R,tmp);

      // also store I for later use by preconditioner
      dMultiply2_333 (tmp,b_ptr->mass.I,b_ptr->posr.R);
      dMultiply0_333 (Irow,b_ptr->posr.R,tmp);

      if (b_ptr->flags & dxBodyGyroscopic) {
        // compute rotational force
        dMultiply0_331 (tmp,Irow,b_ptr->avel);
        dSubtractVectorCross3(b_ptr->tacc,b_ptr->avel,tmp);
      }
    }
  }

  // get the masses for every body
  dReal *invM = context->AllocateArray<dReal> (nb);
  {
    dReal *invMrow = invM;
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; invMrow++, bodycurr++) {
      dxBody *b_ptr = *bodycurr;
      //*invMrow = b_ptr->mass.mass;
      *invMrow = b_ptr->invMass;

    }
  }


  {
    // add the gravity force to all bodies
    // since gravity does normally have only one component it's more efficient
    // to run three loops for each individual component
    dxBody *const *const bodyend = body + nb;
    dReal gravity_x = world->gravity[0];
    if (!_dequal(gravity_x, 0.0)) {
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
        dxBody *b_ptr = *bodycurr;
        if ((b_ptr->flags & dxBodyNoGravity)==0) {
          b_ptr->facc[0] += b_ptr->mass.mass * gravity_x;
        }
      }
    }
    dReal gravity_y = world->gravity[1];
    if (!_dequal(gravity_y, 0.0)) {
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
        dxBody *b_ptr = *bodycurr;
        if ((b_ptr->flags & dxBodyNoGravity)==0) {
          b_ptr->facc[1] += b_ptr->mass.mass * gravity_y;
        }
      }
    }
    dReal gravity_z = world->gravity[2];
    if (!_dequal(gravity_z, 0.0)) {
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
        dxBody *b_ptr = *bodycurr;
        if ((b_ptr->flags & dxBodyNoGravity)==0) {
          b_ptr->facc[2] += b_ptr->mass.mass * gravity_z;
        }
      }
    }
  }

  // get joint information (m = total constraint dimension, nub = number of unbounded variables).
  // joints with m=0 are inactive and are removed from the joints array
  // entirely, so that the code that follows does not consider them.
  dJointWithInfo1 *const jointiinfos = context->AllocateArray<dJointWithInfo1> (_nj);
  int nj;

  {
    dJointWithInfo1 *jicurr = jointiinfos;
    dxJoint *const *const _jend = _joint + _nj;
    for (dxJoint *const *_jcurr = _joint; _jcurr != _jend; _jcurr++) {  // jicurr=dest, _jcurr=src
      dxJoint *j = *_jcurr;
      j->getInfo1 (&jicurr->info);
      dIASSERT (jicurr->info.m >= 0 && jicurr->info.m <= 6 && jicurr->info.nub >= 0 && jicurr->info.nub <= jicurr->info.m);
      if (jicurr->info.m > 0) {
        jicurr->joint = j;
        jicurr++;
      }
    }
    nj = jicurr - jointiinfos;
  }

  context->ShrinkArray<dJointWithInfo1>(jointiinfos, _nj, nj);

  int m;
  int mfb; // number of rows of Jacobian we will have to save for joint feedback

  {
    int mcurr = 0, mfbcurr = 0;
    const dJointWithInfo1 *jicurr = jointiinfos;
    const dJointWithInfo1 *const jiend = jicurr + nj;
    for (; jicurr != jiend; jicurr++) {
      int jm = jicurr->info.m;
      mcurr += jm;
      if (jicurr->joint->feedback)
        mfbcurr += jm;
    }

    m = mcurr;
    mfb = mfbcurr;
  }

#ifdef USE_JOINT_DAMPING
  /************************************************************************/
  /* for joint damping, get the total number of rows for damping jacobian */
  /************************************************************************/
  int m_damp; // number of rows for damped joint jacobian
  {
    int mcurr = 0;
    const dJointWithInfo1 *jicurr = jointiinfos; // info1 stored in jointiinfos
    const dJointWithInfo1 *const jiend = jicurr + nj;
    for (; jicurr != jiend; jicurr++)
      if (jicurr->joint->use_damping)
        mcurr ++;

    m_damp = mcurr;
  }
#endif



  // if there are constraints, compute the constraint force
  dReal *J = NULL;
  dReal *J_precon = NULL;
  dReal *J_orig = NULL;
  int *jb = NULL;

#ifdef USE_JOINT_DAMPING
  /*********************************/
  /* do the same for damped joints */
  /*********************************/
  dReal *v_damp;
  dReal *J_damp = NULL;
  dReal *v_joint_damp = NULL;
  dReal *a_joint_damp = NULL;
  dReal* f_damp = NULL;
  dReal *JiM = NULL;
  int *jb_damp = NULL;
  dReal *coeff_damp = NULL;
#endif

#ifdef PENETRATION_JVERROR_CORRECTION
  dReal *vnew = NULL;
  vnew = context->AllocateArray<dReal> (nb*6);
  // allocate and populate vnew with v(n+1) due to non-constraint forces as the starting value
  {
    dRealMutablePtr vnewcurr = vnew;
    dxBody* const* bodyend = body + nb;
    const dReal *invIrow = invI;
    dReal tmp_tacc[3];
    for (dxBody* const* bodycurr = body; bodycurr != bodyend;
         invIrow += 12, vnewcurr += 6, bodycurr++) {
      dxBody *b_ptr = *bodycurr;

      // add stepsize * invM * fe to the body velocity
      dReal body_invMass_mul_stepsize = stepsize * b_ptr->invMass;
      for (int j=0; j<3; j++) {
        vnewcurr[j]   = b_ptr->lvel[j] + body_invMass_mul_stepsize * b_ptr->facc[j];
        vnewcurr[j+3] = b_ptr->avel[j];
        tmp_tacc[j]   = b_ptr->tacc[j]*stepsize;
      }
      dMultiplyAdd0_331 (vnewcurr+3, invIrow, tmp_tacc);

    }
  }
#endif

  dReal *cforce = context->AllocateArray<dReal> (nb*6);
  dReal *caccel = context->AllocateArray<dReal> (nb*6);
  dReal *caccel_erp = context->AllocateArray<dReal> (nb*6);
#ifdef POST_UPDATE_CONSTRAINT_VIOLATION_CORRECTION
  dReal *caccel_corr = context->AllocateArray<dReal> (nb*6);
#endif

  if (m > 0) {
    dReal *cfm, *lo, *hi, *rhs, *rhs_erp, *rhs_precon, *Jcopy;
    dReal *Mvel;
    dReal *c_v_max;
    int *findex;

    {
      int mlocal = m;

      const unsigned jelements = mlocal*12;
      J = context->AllocateArray<dReal> (jelements);
      dSetZero (J,jelements);
      J_precon = context->AllocateArray<dReal> (jelements);
      J_orig = context->AllocateArray<dReal> (jelements);

      // create a constraint equation right hand side vector `c', a constraint
      // force mixing vector `cfm', and LCP low and high bound vectors, and an
      // 'findex' vector.
      cfm = context->AllocateArray<dReal> (mlocal);
      dSetValue (cfm,mlocal,world->global_cfm);

      lo = context->AllocateArray<dReal> (mlocal);
      dSetValue (lo,mlocal,-dInfinity);

      hi = context->AllocateArray<dReal> (mlocal);
      dSetValue (hi,mlocal, dInfinity);

      findex = context->AllocateArray<int> (mlocal);
      for (int i=0; i<mlocal; i++) findex[i] = -1;

      c_v_max = context->AllocateArray<dReal> (mlocal);
      for (int i=0; i<mlocal; i++) c_v_max[i] = world->contactp.max_vel; // init all to world max surface vel

      const unsigned jbelements = mlocal*2;
      jb = context->AllocateArray<int> (jbelements);

      rhs = context->AllocateArray<dReal> (mlocal);
      rhs_erp = context->AllocateArray<dReal> (mlocal);
      rhs_precon = context->AllocateArray<dReal> (mlocal);
      Mvel = context->AllocateArray<dReal> (nb*6);

      Jcopy = context->AllocateArray<dReal> (mfb*12);

#ifdef USE_JOINT_DAMPING
      JiM = context->AllocateArray<dReal> (mlocal*12); // for computing b_damp
      dSetZero (JiM,jelements);
#endif

    }


#ifdef USE_JOINT_DAMPING
    /*********************************/
    /* for damped joints             */
    /*********************************/
    {
      int mlocal = m_damp;

      const unsigned jelements = mlocal*12;
      J_damp = context->AllocateArray<dReal> (jelements);
      dSetZero (J_damp,jelements);

      // v_joint_damp = J_damp * v
      // v_joint_damp is the velocity of the joint in joint space
      // (relative angular rates of attached bodies)
      const unsigned v_joint_damp_elements = mlocal;
      v_joint_damp = context->AllocateArray<dReal> (v_joint_damp_elements);
      dSetZero (v_joint_damp,v_joint_damp_elements);

      // a_joint_damp = dt * J_damp * invM * f
      // a_joint_damp is the damping force of the joint in joint space
      // (relative angular rates of attached bodies)
      const unsigned a_joint_damp_elements = mlocal;
      a_joint_damp = context->AllocateArray<dReal> (a_joint_damp_elements);
      dSetZero (a_joint_damp,a_joint_damp_elements);

      // jb is the body index for each jacobian
      const unsigned jbelements = mlocal*2;
      jb_damp = context->AllocateArray<int> (jbelements);

      const unsigned f_damp_elements = nb*6;
      f_damp = context->AllocateArray<dReal> (f_damp_elements);
      dSetZero (f_damp,f_damp_elements);

      const unsigned v_damp_elements = nb*6;
      v_damp = context->AllocateArray<dReal> (v_damp_elements);
      dSetZero (v_damp,v_damp_elements);

      const unsigned coeffelements = mlocal;
      coeff_damp = context->AllocateArray<dReal> (coeffelements);
      dSetZero (coeff_damp,coeffelements);
    }
#endif

    BEGIN_STATE_SAVE(context, cstate) {
      dReal *c = context->AllocateArray<dReal> (m);
      dSetZero (c, m);

      {
        IFTIMING (dTimerNow ("create J"));
        // get jacobian data from constraints. an m*12 matrix will be created
        // to store the two jacobian blocks from each constraint. it has this
        // format:
        //
        //   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 \    .
        //   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2  )-- jacobian for joint 0, body 1 and body 2 (3 rows)
        //   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 /
        //   l1 l1 l1 a1 a1 a1 l2 l2 l2 a2 a2 a2 )--- jacobian for joint 1, body 1 and body 2 (3 rows)
        //   etc...
        //
        //   (lll) = linear jacobian data
        //   (aaa) = angular jacobian data
        //
        dxJoint::Info2 Jinfo;
        Jinfo.rowskip = 12;
        Jinfo.fps = stepsize1;
        Jinfo.erp = world->global_erp;

        dReal *Jcopyrow = Jcopy;
        unsigned ofsi = 0;
#ifdef USE_JOINT_DAMPING
        unsigned ofsi_damp = 0; // for joint damping
#endif
        const dJointWithInfo1 *jicurr = jointiinfos;
        const dJointWithInfo1 *const jiend = jicurr + nj;
        for (; jicurr != jiend; jicurr++) {
          dReal *const Jrow = J + ofsi * 12;
          Jinfo.J1l = Jrow;
          Jinfo.J1a = Jrow + 3;
          Jinfo.J2l = Jrow + 6;
          Jinfo.J2a = Jrow + 9;
          Jinfo.c = c + ofsi;
          Jinfo.cfm = cfm + ofsi;
          Jinfo.lo = lo + ofsi;
          Jinfo.hi = hi + ofsi;
          Jinfo.findex = findex + ofsi;
          Jinfo.c_v_max = c_v_max + ofsi;

#ifdef USE_JOINT_DAMPING
          /*******************************************************/
          /*  allocate space for damped joint Jacobians          */
          /*******************************************************/
          if (jicurr->joint->use_damping)
          {
            // damping coefficient is in jicurr->info.damping_coefficient);
            coeff_damp[ofsi_damp] = jicurr->joint->damping_coefficient;

            // setup joint damping pointers so getinfo2 will fill in J_damp
            dReal *const Jrow_damp = J_damp + ofsi_damp * 12;
            Jinfo.J1ld = Jrow_damp;
            Jinfo.J1ad = Jrow_damp + 3;
            Jinfo.J2ld = Jrow_damp + 6;
            Jinfo.J2ad = Jrow_damp + 9;
            // one row of constraint per joint
            ofsi_damp ++;
          }
#endif
          // now write all information into J
          dxJoint *joint = jicurr->joint;
          joint->getInfo2 (&Jinfo);

          const int infom = jicurr->info.m;

          // we need a copy of Jacobian for joint feedbacks
          // because it gets destroyed by SOR solver
          // instead of saving all Jacobian, we can save just rows
          // for joints, that requested feedback (which is normally much less)
          if (joint->feedback) {
            const int rowels = infom * 12;
            memcpy(Jcopyrow, Jrow, rowels * sizeof(dReal));
            Jcopyrow += rowels;
          }

          // adjust returned findex values for global index numbering
          int *findex_ofsi = findex + ofsi;
          for (int j=0; j<infom; j++) {
            int fival = findex_ofsi[j];
            if (fival >= 0)
              findex_ofsi[j] = fival + ofsi;
          }

          ofsi += infom;
        }
      }

      {
        // create an array of body numbers for each joint row
        int *jb_ptr = jb;
        const dJointWithInfo1 *jicurr = jointiinfos;
        const dJointWithInfo1 *const jiend = jicurr + nj;
        for (; jicurr != jiend; jicurr++) {
          dxJoint *joint = jicurr->joint;
          const int infom = jicurr->info.m;

          int b1 = (joint->node[0].body) ? (joint->node[0].body->tag) : -1;
          int b2 = (joint->node[1].body) ? (joint->node[1].body->tag) : -1;
          for (int j=0; j<infom; j++) {
            jb_ptr[0] = b1;
            jb_ptr[1] = b2;
            jb_ptr += 2;
          }
        }
        dIASSERT (jb_ptr == jb+2*m);
        //printf("jjjjjjjjj %d %d\n",jb[0],jb[1]);
      }


#ifdef USE_JOINT_DAMPING
      {
        /*************************************************************/
        /* create an array of body numbers for each damped joint row */
        /*************************************************************/
        int *jb_damp_ptr = jb_damp;
        const dJointWithInfo1 *jicurr = jointiinfos;
        const dJointWithInfo1 *const jiend = jicurr + nj;
        for (; jicurr != jiend; jicurr++) {
          if (jicurr->joint->use_damping)
          {
            dxJoint *joint = jicurr->joint;
            const int infom = 1; // one damping jacobian row per hinge joint

            int b1 = (joint->node[0].body) ? (joint->node[0].body->tag) : -1;
            int b2 = (joint->node[1].body) ? (joint->node[1].body->tag) : -1;
            for (int j=0; j<infom; j++) {
              jb_damp_ptr[0] = b1;
              jb_damp_ptr[1] = b2;
              jb_damp_ptr += 2;
            }
          }
        }
        dIASSERT (jb_damp_ptr == jb_damp+2*m_damp);
      }
#endif


      BEGIN_STATE_SAVE(context, tmp1state) {
        IFTIMING (dTimerNow ("compute rhs"));
        // compute the right hand side `rhs'
        dReal *tmp1 = context->AllocateArray<dReal> (nb*6);
        dSetZero(tmp1,nb*6);
        // put v/h + invM*fe into tmp1
        dReal *tmp1curr = tmp1;
        const dReal *invIrow = invI;
        dxBody *const *const bodyend = body + nb;
        for (dxBody *const *bodycurr = body;
             bodycurr != bodyend;
             tmp1curr+=6, invIrow+=12, bodycurr++) {
          dxBody *b_ptr = *bodycurr;
          dReal body_invMass = b_ptr->invMass;
          for (int j=0; j<3; j++)
            tmp1curr[j] = b_ptr->facc[j]*body_invMass + b_ptr->lvel[j]*stepsize1;
          dMultiply0_331 (tmp1curr + 3,invIrow,b_ptr->tacc);
          for (int k=0; k<3; k++) tmp1curr[3+k] += b_ptr->avel[k] * stepsize1;
        }

        // put J*tmp1 into rhs
        multiply_J (m,J,jb,tmp1,rhs);

#ifdef USE_JOINT_DAMPING
        /*************************************************************/
        /* compute J*inv(M) here JiM, it does not change             */
        /* where b_damp = -J*inv(M)*f_damp / Ad  (since b is rhs/Ad) */
        /* and b is to be modified by b_damp                         */
        /*************************************************************/
        {
          dRealPtr J_ptr = J;
          dRealMutablePtr JiM_ptr = JiM; // intermediate solution storage
          for (int i=0; i<m;J_ptr+=12,JiM_ptr+=12, i++) {

            // compute JiM = J * invM
            int b1 = jb[i*2];
            int b2 = jb[i*2+1];
            dReal k1 = body[b1]->invMass;

            for (int j=0; j<3 ; j++) JiM_ptr[j] = J_ptr[j]*k1;


            const dReal *invI_ptr1 = invI + 12*b1;
            for (int j=0;j<3;j++) for (int k=0;k<3;k++){
              JiM_ptr[3+j] += J_ptr[3+k]*invI_ptr1[k*4+j];
            }

            if (b2 >= 0){
              dReal k2 = body[b2]->invMass;
              for (int j=0; j<3 ; j++) JiM_ptr[j+6] += k2*J_ptr[j+6];
              const dReal *invI_ptr2 = invI + 12*b2;
              for (int j=0;j<3;j++) for (int k=0;k<3;k++) JiM_ptr[9+j] += J_ptr[9+k]*invI_ptr2[k*4+j];
            }
          }
        }
#endif

      } END_STATE_SAVE(context, tmp1state);

      // complete rhs
      for (int i=0; i<m; i++) {
        rhs_erp[i] =      c[i]*stepsize1 - rhs[i];
        if (dFabs(c[i]) > c_v_max[i])
          rhs[i]   =  c_v_max[i]*stepsize1 - rhs[i];
        //if (dFabs(c[i]) > world->contactp.max_vel)
        //  rhs[i]   =  world->contactp.max_vel*stepsize1 - rhs[i];
        else
          rhs[i]   = c[i]*stepsize1 - rhs[i];
      }

      // compute rhs_precon
      computeRHSPrecon(context,m,nb,I,body,stepsize1,c,J,jb,rhs_precon,Mvel);

      // scale CFM
      for (int j=0; j<m; j++) cfm[j] *= stepsize1;

    } END_STATE_SAVE(context, cstate);

#ifdef USE_JOINT_DAMPING
    /***************************************************************************/
    /* create a nb*6 by 1 vector (v_damp) to store estimated implicit velocity */
    /*  as it is updated in the iterative loop                                 */
    /***************************************************************************/
    {
      // allocate v_damp
      dRealMutablePtr v_damp_ptr = v_damp;
      dxBody *const *const bodyend = body + nb;
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; v_damp_ptr+=6, bodycurr++) {
        dxBody *b = *bodycurr;
        v_damp_ptr[0] = b->lvel[0];
        v_damp_ptr[1] = b->lvel[1];
        v_damp_ptr[2] = b->lvel[2];
        v_damp_ptr[3] = b->avel[0];
        v_damp_ptr[4] = b->avel[1];
        v_damp_ptr[5] = b->avel[2];
      }
    }
#endif

    // load lambda from the value saved on the previous iteration
    dReal *lambda = context->AllocateArray<dReal> (m);
    dReal *lambda_erp = context->AllocateArray<dReal> (m);

#ifdef WARM_STARTING //FIXME: add for lambda_erp
    {
      dReal *lambdscurr = lambda;
      const dJointWithInfo1 *jicurr = jointiinfos;
      const dJointWithInfo1 *const jiend = jicurr + nj;
      for (; jicurr != jiend; jicurr++) {
        int infom = jicurr->info.m;
        memcpy (lambdscurr, jicurr->joint->lambda, infom * sizeof(dReal));
        lambdscurr += infom;
      }
    }
#endif

    BEGIN_STATE_SAVE(context, lcpstate) {
      IFTIMING (dTimerNow ("solving LCP problem"));
      // solve the LCP problem and get lambda and invM*constraint_force
      SOR_LCP (context,m,nb,J,J_precon,J_orig,
#ifdef PENETRATION_JVERROR_CORRECTION
               vnew,
#endif
               jb,body,
               invI,I,lambda,lambda_erp,
               caccel,caccel_erp,cforce,
               rhs,rhs_erp,rhs_precon,
               lo,hi,cfm,findex,
               &world->qs,
#ifdef USE_TPROW
               world->row_threadpool,
#endif
#ifdef USE_JOINT_DAMPING
               m_damp,J_damp,coeff_damp,jb_damp,v_damp,f_damp,v_joint_damp,a_joint_damp,JiM,
#endif
               stepsize);

    } END_STATE_SAVE(context, lcpstate);

#ifdef WARM_STARTING //FIXME: add for lambda_erp
    {
      // save lambda for the next iteration
      //@@@ note that this doesn't work for contact joints yet, as they are
      // recreated every iteration
      const dReal *lambdacurr = lambda;
      const dJointWithInfo1 *jicurr = jointiinfos;
      const dJointWithInfo1 *const jiend = jicurr + nj;
      for (; jicurr != jiend; jicurr++) {
        int infom = jicurr->info.m;
        memcpy (jicurr->joint->lambda, lambdacurr, infom * sizeof(dReal));
        lambdacurr += infom;
      }
    }
#endif

#ifdef USE_JOINT_DAMPING
    /****************************************************************/
    /* perform velocity update due to damping force                 */
    /*  v_new = n_old + stepsize * invM * f_damp                    */
    /****************************************************************/
    {
      const dReal *invIrow = invI;
      IFTIMING (dTimerNow ("velocity update due to f_damp"));

      dRealMutablePtr f_damp_ptr = f_damp;
      dxBody *const *const bodyend = body + nb;
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; invIrow += 12, f_damp_ptr+=6, bodycurr++) {
        // f_damp should be updated in SOR LCP

        // compute the velocity update:
        // add stepsize * invM * f_damp to the body velocity
        dxBody *b = *bodycurr;
        dReal body_invMass_mul_stepsize = stepsize * b->invMass;
        for (int j=0; j<3; j++) {
          b->lvel[j] += body_invMass_mul_stepsize * f_damp_ptr[j];
          f_damp_ptr[3+j] *= stepsize; // multiply torque part by step size
        }
        dMultiplyAdd0_331 (b->avel, invIrow, f_damp_ptr+3);
      }

    }
#endif

    // note that the SOR method overwrites rhs and J at this point, so
    // they should not be used again.

    {
      IFTIMING (dTimerNow ("velocity update due to constraint forces"));
      //
      // update new velocity
      // add stepsize * caccel_erp to the body velocity
      //
      const dReal *caccelcurr = caccel_erp;
      dxBody *const *const bodyend = body + nb;
      for (dxBody *const *bodycurr = body; bodycurr != bodyend; caccelcurr+=6, bodycurr++) {
        dxBody *b_ptr = *bodycurr;
        for (int j=0; j<3; j++) {
          b_ptr->lvel[j] += stepsize * caccelcurr[j];
          b_ptr->avel[j] += stepsize * caccelcurr[3+j];
        }
        // printf("caccel [%f %f %f] [%f %f %f]\n"
        //   ,caccelcurr[0] ,caccelcurr[1] ,caccelcurr[2]
        //   ,caccelcurr[3] ,caccelcurr[4] ,caccelcurr[5]);
        // printf("  vel [%f %f %f] [%f %f %f]\n"
        //   ,b_ptr->lvel[0] ,b_ptr->lvel[1] ,b_ptr->lvel[2]
        //   ,b_ptr->avel[0] ,b_ptr->avel[1] ,b_ptr->avel[2]);
      }
    }

    if (mfb > 0) {
      // force feedback without erp is better
      // straightforward computation of joint constraint forces:
      // multiply related lambdas with respective J' block for joints
      // where feedback was requested
      dReal data[6];
      const dReal *lambdacurr = lambda;
      const dReal *Jcopyrow = Jcopy;
      const dJointWithInfo1 *jicurr = jointiinfos;
      const dJointWithInfo1 *const jiend = jicurr + nj;
      for (; jicurr != jiend; jicurr++) {
        dxJoint *joint = jicurr->joint;
        const int infom = jicurr->info.m;

        if (joint->feedback) {
          dJointFeedback *fb = joint->feedback;
          Multiply1_12q1 (data, Jcopyrow, lambdacurr, infom);
          fb->f1[0] = data[0];
          fb->f1[1] = data[1];
          fb->f1[2] = data[2];
          fb->t1[0] = data[3];
          fb->t1[1] = data[4];
          fb->t1[2] = data[5];

          if (joint->node[1].body)
          {
            Multiply1_12q1 (data, Jcopyrow+6, lambdacurr, infom);
            fb->f2[0] = data[0];
            fb->f2[1] = data[1];
            fb->f2[2] = data[2];
            fb->t2[0] = data[3];
            fb->t2[1] = data[4];
            fb->t2[2] = data[5];
          }

          Jcopyrow += infom * 12;
        }

        lambdacurr += infom;
      }
    }
  }

  {
    IFTIMING (dTimerNow ("compute velocity update"));
    // compute the velocity update:
    // add stepsize * invM * fe to the body velocity
    const dReal *invIrow = invI;
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; invIrow += 12, bodycurr++) {
      dxBody *b_ptr = *bodycurr;
      dReal body_invMass_mul_stepsize = stepsize * b_ptr->invMass;
      for (int j=0; j<3; j++) {
        b_ptr->lvel[j] += body_invMass_mul_stepsize * b_ptr->facc[j];
        b_ptr->tacc[j] *= stepsize;
      }
      dMultiplyAdd0_331 (b_ptr->avel, invIrow, b_ptr->tacc);
      // printf("fe [%f %f %f] [%f %f %f]\n"
      //   ,b_ptr->facc[0] ,b_ptr->facc[1] ,b_ptr->facc[2]
      //   ,b_ptr->tacc[0] ,b_ptr->tacc[1] ,b_ptr->tacc[2]);
      /* DEBUG PRINTOUTS
      printf("uncorrect vel [%f %f %f] [%f %f %f]\n"
        ,b_ptr->lvel[0] ,b_ptr->lvel[1] ,b_ptr->lvel[2]
        ,b_ptr->avel[0] ,b_ptr->avel[1] ,b_ptr->avel[2]);
      */
    }
  }

#ifdef CHECK_VELOCITY_OBEYS_CONSTRAINT
  if (m > 0) {
    BEGIN_STATE_SAVE(context, velstate) {
      dReal *vel = context->AllocateArray<dReal>(nb*6);

      // CHECK THAT THE UPDATED VELOCITY OBEYS THE CONSTRAINT
      //  (this check needs unmodified J)
      //  put residual into tmp
      dRealMutablePtr velcurr = vel;
      //dxBody* const* bodyend = body + nb;
      for (dxBody* const* bodycurr = body; bodycurr != bodyend; velcurr += 6, bodycurr++) {
        dxBody *b_ptr = *bodycurr;
        for (int j=0; j<3; j++) {
          velcurr[j]   = b_ptr->lvel[j];
          velcurr[3+j] = b_ptr->avel[j];
        }
      }
      dReal *tmp = context->AllocateArray<dReal> (m);
      multiply_J (m,J,jb,vel,tmp);

      dReal error = 0;
      for (int i=0; i<m; i++) error += dFabs(tmp[i]);
      printf ("velocity error = %10.6e\n",error);
    }
  }
#endif



  {
    // update the position and orientation from the new linear/angular velocity
    // (over the given timestep)
    IFTIMING (dTimerNow ("update position"));
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
      dxBody *b_ptr = *bodycurr;
      dxStepBody (b_ptr,stepsize);
    }
  }

  // revert lvel and avel with the non-erp version of caccel
  if (m > 0) {
    dReal erp_removal = 1.00;
    IFTIMING (dTimerNow ("velocity update due to constraint forces"));
    // remove caccel_erp
    const dReal *caccel_erp_curr = caccel_erp;
    const dReal *caccel_curr = caccel;
    dxBody *const *const bodyend = body + nb;
    int debug_count = 0;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend;
         caccel_curr+=6, caccel_erp_curr+=6, bodycurr++, debug_count++) {
      dxBody *b_ptr = *bodycurr;
      for (int j=0; j<3; j++) {
        // dReal v0 = b_ptr->lvel[j];
        // dReal a0 = b_ptr->avel[j];
        dReal dv = erp_removal * stepsize * (caccel_curr[j]   - caccel_erp_curr[j]);
        dReal da = erp_removal * stepsize * (caccel_curr[3+j] - caccel_erp_curr[3+j]);

        /* default v removal
        */
        b_ptr->lvel[j] += dv;
        b_ptr->avel[j] += da;
        /* think about minimize J*v somehow without SORLCP...
        */
        /* minimize final velocity test 1,
        if (v0 * dv < 0) {
          if (fabs(v0) < fabs(dv))
            b_ptr->lvel[j] = 0.0;
          else
            b_ptr->lvel[j] += dv;
        }
        if (a0 * da < 0) {
          if (fabs(a0) < fabs(da))
            b_ptr->avel[j] = 0.0;
          else
            b_ptr->avel[j] += da;
        }
        */

        /*  DEBUG PRINTOUTS, total forces/accel on a body
        printf("nb[%d] m[%d] b[%d] i[%d] v[%f] dv[%f] vf[%f] a[%f] da[%f] af[%f] debug[%f - %f][%f - %f]\n"
               ,nb, m, debug_count, j, v0, dv, b_ptr->lvel[j]
                 , a0, da, b_ptr->avel[j]
               ,caccel_curr[j], caccel_erp_curr[j]
               ,caccel_curr[3+j], caccel_erp_curr[3+j]);
        */
      }
      /*  DEBUG PRINTOUTS
      printf("corrected vel [%f %f %f] [%f %f %f]\n",
        b_ptr->lvel[0], b_ptr->lvel[1], b_ptr->lvel[2],
        b_ptr->avel[0], b_ptr->avel[1], b_ptr->avel[2]);
      */
    }

#ifdef POST_UPDATE_CONSTRAINT_VIOLATION_CORRECTION
    // ADD CACCEL CORRECTION FROM VELOCITY CONSTRAINT VIOLATION
    BEGIN_STATE_SAVE(context, velstate) {
      dReal *vel = context->AllocateArray<dReal>(nb*6);

      // CHECK THAT THE UPDATED VELOCITY OBEYS THE CONSTRAINT
      //  (this check needs unmodified J)
      //  put residual into tmp
      dRealMutablePtr velcurr = vel;
      //dxBody* const* bodyend = body + nb;
      for (dxBody* const* bodycurr = body; bodycurr != bodyend; velcurr += 6, bodycurr++) {
        dxBody *b_ptr = *bodycurr;
        for (int j=0; j<3; j++) {
          velcurr[j]   = b_ptr->lvel[j];
          velcurr[3+j] = b_ptr->avel[j];
        }
      }
      dReal *tmp = context->AllocateArray<dReal> (m);
      multiply_J (m,J,jb,vel,tmp);

      // DIRECTLY ADD THE CONSTRAINT VIOLATION TERM (TMP) TO VELOCITY UPDATE
      // add correction term dlambda = J*v(n+1)/dt
      // and caccel += dt*invM*JT*dlambda (dt's cancel out)
      dReal *iMJ = context->AllocateArray<dReal> (m*12);
      compute_invM_JT (m,J,iMJ,jb,body,invI);
      // compute caccel_corr=(inv(M)*J')*dlambda, correction term
      // as we change lambda.
      multiply_invM_JT (m,nb,iMJ,jb,tmp,caccel_corr);

    } END_STATE_SAVE(context, velstate);

    // ADD CACCEL CORRECTION FROM VELOCITY CONSTRAINT VIOLATION
    caccelcurr = caccel;
    const dReal* caccel_corrcurr = caccel_corr;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; caccel_corrcurr+=6, bodycurr++) {
      dxBody *b_ptr = *bodycurr;
      for (int j=0; j<3; j++) {
        b_ptr->lvel[j] += erp_removal * stepsize * caccel_corrcurr[j];
        b_ptr->avel[j] += erp_removal * stepsize * caccel_corrcurr[3+j];
      }
    }
#endif


  }

  {
    IFTIMING (dTimerNow ("tidy up"));
    // zero all force accumulators
    dxBody *const *const bodyend = body + nb;
    for (dxBody *const *bodycurr = body; bodycurr != bodyend; bodycurr++) {
      dxBody *b_ptr = *bodycurr;
      dSetZero (b_ptr->facc,3);
      dSetZero (b_ptr->tacc,3);
    }
  }

  IFTIMING (dTimerEnd());
  IFTIMING (if (m > 0) dTimerReport (stdout,1));
}

#ifdef USE_CG_LCP
static size_t EstimateGR_LCPMemoryRequirements(int m)
{
  size_t res = dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for iMJ
  res += 5 * dEFFICIENT_SIZE(sizeof(dReal) * m); // for r, z, p, q, Ad
  return res;
}
#endif

static size_t EstimateSOR_LCPMemoryRequirements(int m,int /*nb*/
#ifdef USE_JOINT_DAMPING
                                               ,int /*m_damp*/
#endif
)
{
  size_t res = dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for iMJ
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for Ad
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for Ad_precon
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for Adcfm_precon
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for Adcfm
  res += dEFFICIENT_SIZE(sizeof(IndexError) * m); // for order
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for delta_error
#ifdef REORDER_CONSTRAINTS
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for last_lambda
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for last_lambda_erp
#endif
#ifdef USE_JOINT_DAMPING
  res += dEFFICIENT_SIZE(sizeof(dReal) * m); // for b_damp
#endif
  return res;
}

size_t dxEstimateQuickStepMemoryRequirements (
  dxBody * const * /*body*/, int nb, dxJoint * const *_joint, int _nj)
{
  int nj, m, mfb;

  {
    int njcurr = 0, mcurr = 0, mfbcurr = 0;
    dxJoint::SureMaxInfo info;
    dxJoint *const *const _jend = _joint + _nj;
    for (dxJoint *const *_jcurr = _joint; _jcurr != _jend; _jcurr++) {
      dxJoint *j = *_jcurr;
      j->getSureMaxInfo (&info);

      int jm = info.max_m;
      if (jm > 0) {
        njcurr++;

        mcurr += jm;
        if (j->feedback)
          mfbcurr += jm;
      }
    }
    nj = njcurr; m = mcurr; mfb = mfbcurr;
  }

#ifdef USE_JOINT_DAMPING
  int m_damp;
  {
    int m_dampcurr = 0;
    // dxJoint::SureMaxInfo info;
    dxJoint *const *const _jend = _joint + _nj;
    for (dxJoint *const *_jcurr = _joint; _jcurr != _jend; _jcurr++) {
      dxJoint *j = *_jcurr;
      /***************************/
      /* size for damping joints */
      /***************************/
      if (j->use_damping)
        m_dampcurr ++;
    }
    m_damp = m_dampcurr;
  }
#endif

  size_t res = 0;

  res += dEFFICIENT_SIZE(sizeof(dReal) * 3 * 4 * nb); // for invI
  res += dEFFICIENT_SIZE(sizeof(dReal) * 3 * 4 * nb); // for I (inertia) needed by preconditioner
  res += dEFFICIENT_SIZE(sizeof(dReal) * nb); // for invM

  {
    size_t sub1_res1 = dEFFICIENT_SIZE(sizeof(dJointWithInfo1) * _nj); // for initial jointiinfos

    sub1_res1 += dEFFICIENT_SIZE(sizeof(dJointWithInfo1) * nj); // for shrunk jointiinfos
#ifdef PENETRATION_JVERROR_CORRECTION
    sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for vnew
#endif
    sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for cforce
    sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for caccel
    sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for caccel_erp

#ifdef POST_UPDATE_CONSTRAINT_VIOLATION_CORRECTION
    sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for caccel_corr
#endif


    if (m > 0) {
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for J
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for J_precon
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for J_orig
      sub1_res1 += 3 * dEFFICIENT_SIZE(sizeof(dReal) * m); // for cfm, lo, hi
      sub1_res1 += dEFFICIENT_SIZE(sizeof(int) * m); // for findex
      sub1_res1 += dEFFICIENT_SIZE(sizeof(int) * m); // for c_v_max
      sub1_res1 += dEFFICIENT_SIZE(sizeof(int) * 2 * m); // for jb

      sub1_res1 += 2 * dEFFICIENT_SIZE(sizeof(dReal) * m); // for rhs, rhs_erp
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m); // for rhs_precon
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for Mvel

      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * mfb); // for Jcopy

#ifdef USE_JOINT_DAMPING
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 12*m); // for JiM
#endif

#ifdef USE_JOINT_DAMPING
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * m_damp); // for J_damp
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m_damp ); // for v_joint_damp
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m_damp ); // for a_joint_damp
      sub1_res1 += dEFFICIENT_SIZE(sizeof(int) * 2 * m_damp); // for jb_damp
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for f_damp
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for v_damp
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m_damp); // for coeff_damp
#endif

      { // cstate
        size_t sub2_res1 = dEFFICIENT_SIZE(sizeof(dReal) * m); // for c
        { // tmp1state

          size_t sub3_res1 = dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for tmp1
          sub2_res1 += sub3_res1;
        }
        sub1_res1 += sub2_res1;
      }

      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m); // for lambda
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m); // for lambda_erp

      { // lcpstate
        size_t sub2_res1 = EstimateSOR_LCPMemoryRequirements(m,nb
#ifdef USE_JOINT_DAMPING
                                                            ,m_damp
#endif
                                                            ); // for SOR_LCP
        sub1_res1 += sub2_res1;
      }
    } // end of m > 0

#ifdef CHECK_VELOCITY_OBEYS_CONSTRAINT
    { // velstate
      size_t sub2_res1 = dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for vel
      sub2_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m); // for tmp
      sub1_res1 += sub2_res1;
    }
#endif


    if (m > 0) {
#ifdef POST_UPDATE_CONSTRAINT_VIOLATION_CORRECTION
      sub1_res1 = dEFFICIENT_SIZE(sizeof(dReal) * 6 * nb); // for vel
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * m); // for tmp
      sub1_res1 += dEFFICIENT_SIZE(sizeof(dReal) * 12 * m); // for iMJ
#endif
    } // end of m > 0

    res += sub1_res1;
  }

  return res;
}


