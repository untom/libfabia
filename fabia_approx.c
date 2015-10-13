#include <stdlib.h>
#include <stdio.h>  
#include <limits.h> 
#include <math.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <omp.h>


#include "util.h"
#include "hdf5tools.h"


#include "f2c.h"
#define integer int
#include "clapack.h"
#include "blas.h"

// change the "s" to "d" when switching from float to double
#define F77(x) s ## x ## _

#define STARTTIMER(x) clock_gettime(CLOCK_MONOTONIC, &tmpT);
#define ENDTIMER(tt)  tt += calculateElapsedTime(tmpT);

static const float MACHINE_EPS = 1e-7;

static inline void invertCholesky(float* a, int n) { 
	int info;
	F77(potrf)("l", &n, a, &n, &info);
	assert(!info);
	F77(potri)("l", &n, a, &n, &info);
	assert(!info);
	// dpotri only solves the lower triangle of the symmetric matrix
	for (int i1 = 0; i1 < n; ++i1) {
		for (int i2 = i1+1; i2 < n; ++i2) {
			//a[i2 + i1*n] = a[i1 + i2*n];
			a[i1 + i2*n] = a[i2 + i1*n];
		}
	}
}



void updateUI(int iter, float elapsedTime, int k, int n, int l, float* L, float* Z, float* Psi, float* lapla);

float calculateElapsedTime(struct timespec t0);


// needed for BLAS/LAPACK calls
static const float ONE = 1;
static const float ZERO = 0;
static const int ONE_INT = 1;
	

/**
 * Updates the estimate E(z|x) for a given datapoint x, using the current LPsi/LPsiL matrix.
 * Uses the work-matrices tLPsiL and iLPsiLX, whose contents will be destroyed by this call.
 * 
 * If sum1 != NULL, then the estimates for lapla, as well as sum1 and sum2 will also be 
 * calculated.
 */
static inline void approx_estimateZ(int n, int k, float* x, float* z, float* lapla, float* LPsi, float* LPsiL, float* iLPsiL, float* iLPsiLX,
	float* sum1, float* sum2, float spz, float lap)
{
	for (int i = 0; i < k; ++i) {
		iLPsiL[i] = 1.0f / (LPsiL[i] + lapla[i] + MACHINE_EPS);
		z[i] = 0.0;
	}

	for (int i = 0; i < n; ++i) {
		for (int j = 0; j < k; ++j) {
			iLPsiLX[i*k+j] = LPsi[i*k+j] * iLPsiL[j];
			z[j] += iLPsiLX[i*k+j] * x[i];
		}
	}
	
	if (!sum1 || !sum2)
		return;
	
	F77(ger)(&n, &k, &ONE, x, &ONE_INT,  z, &ONE_INT, sum1, &n);
			
	for (int i = 0; i < k; ++i) {
		for (int j = 0; j < k; ++j) {
			sum2[i*k +j] += z[i] * z[j];
		}
		sum2[i*k + i] += iLPsiL[i];
		iLPsiL[i] += z[i]*z[i];
	
		float s = pow(MACHINE_EPS+iLPsiL[i],-spz);
		lapla[i] = (s < lap) ? lap : s;
	}
}


/**
 * Runs the approximate FABIA algorithm. Expects all  matrices in column-major layout
 * and operates on float matrices.
 * 
 * @param X	matrix of n * l, with nn datapoints in its columns
 * @param Psi	vector of n 
 * @param L	matrix of n * k
 * @param Z	matrix of k * l
 * @param lapla	matrix of k * l   (NOTE: this is the transpose of what it is in R!)
 * @param cyc	the number of EM cycles
 * @param alpha	parameter for the Laplace prior
 * @param eps	epsilon used for regularization
 * @param spl	parameter to tune extra-sparseness of l
 * @param spz	parameter to tune extra-sparseness of z
 * @param scale	scale parameter, as in original FABIA
 * @param lap	minimal value of the variational parameter
 * @param verbose  if non-zero, print status information every $verbose iterations.
 * @param nthreads the number of threads that will be used
 */
void approx_fabia_cm_f(const int k, const int n, const int l, 
	float* restrict X, float* restrict Psi, float* restrict L, float* restrict Z, float* restrict lapla,
	int cyc, float alpha, float eps, float spl, float spz, int scale, float lap, int verbose, int nthreads)
{
	float *sum1_ = (float*) malloc(nthreads * n * k * sizeof(float));
	float *sum2_ = (float*) malloc(nthreads * k * k * sizeof(float));
	float *XX = (float*) malloc(n * sizeof(float)); 
	float *LPsi = (float*) malloc(k * n * sizeof(float)); 
	float *LPsiL = (float*) malloc(k * sizeof(float));              // only the diagonal!
	float *iLPsiL_ = (float*) malloc(nthreads * k * sizeof(float)); // only the diagonal!
	float *iLPsiLX_ = (float*) malloc(nthreads * n * k * sizeof(float));

	// Note: LPsi is transposed wrt Sepp's implementation!
	// This is because then we can make use of the fact that inv(LPsiL) is
	// symmetric when multiplying it with LPsi in the inner (j) loop	
	
	if (!iLPsiLX_) { 	// if the last allocation failed, we ran out of memory
		fprintf(stderr, "Out of memory\n");
		free(iLPsiL_);
		free(LPsi);
		free(LPsiL);
		free(XX);
		free(sum2_);
		free(sum1_);
		memset(L, 0, n*k*sizeof(float));
		memset(Z, 0, k*l*sizeof(float));
		return;
	}
	
	// these were parameters in the original FABIA, but we never use them.
	//const int nL = 0;
	//const int lL = 0;
	//const int bL = 0;
	const int non_negative = 0;
	
	omp_set_num_threads(nthreads);
	
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	
	struct timespec tmpT;
	float tLoop = 0.0, tChol = 0.0, tRest = 0.0;


	if (lap<eps)
		lap = eps;
	
	memset(XX, 0, n*sizeof(float));
	for (int i1 = 0; i1 < l; ++i1) {
		for (int i2 = 0; i2 < n; ++i2)
			XX[i2] += X[i2 + i1*n] * X[i2 + i1*n];
	}
	for (int i2 = 0; i2 < n; ++i2)
		XX[i2] /= l;
		
	memset(iLPsiL_, 0, nthreads * k * sizeof(float));

	float t = 0.0;
	for (int iter = 1; iter <= cyc; ++iter) {

		for (int i1 = 0; i1 < k; i1++) { //LPsi<-diag(1/Psi)%*%L
			LPsiL[i1] = 0.0;
			for (int i2=0;i2<n;i2++) {
				LPsi[i2*k + i1] =  L[i2 + i1*n]/Psi[i2]; // LPsi gets transposed here
				LPsiL[i1] += L[i2 + i1*n]*LPsi[i2*k + i1];
			}
		}
 		
 		// zero out all sums (TODO: maybe faster if ran in parallel?)
		memset(sum1_, 0, nthreads*n*k*sizeof(float));
		memset(sum2_, 0, nthreads*k*k*sizeof(float));
		
		// the first k*k elements of sum2 will be used to aggregate, so initialize diagonal
		for (int i=0; i < k; ++i)
			sum2_[i*k + i] = eps;
		
		STARTTIMER(tLoop);
		#pragma omp parallel for
		for (int j = 0; j < l; ++j) {
			int tid = omp_get_thread_num();
			float* iLPsiL = iLPsiL_ + (tid*k);
			float* iLPsiLX = iLPsiLX_ + (tid*n*k);
			float* sum1 = sum1_ + (tid*n*k);
			float* sum2 = sum2_ + (tid*k*k);
			approx_estimateZ(n, k, X+n*j, Z+j*k, lapla+j*k, LPsi, LPsiL, iLPsiL,iLPsiLX, sum1, sum2, spz, lap);
		}
		ENDTIMER(tLoop);
		
		// aggregate/reduce sum1/sum2 into their first sub-arrays
		#pragma omp parallel for
		for(int i = 0; i < n*k; ++i)
			for (int tid = 1; tid < nthreads; ++tid)
				sum1_[i] += sum1_[i + tid*n*k];
		#pragma omp parallel for
		for(int i = 0; i < k*k; ++i)
			for (int tid = 1; tid < nthreads; ++tid)
				sum2_[i] += sum2_[i + tid*k*k];
		STARTTIMER(tChol);				
		invertCholesky(sum2_, k);  // sll <- chol2inv(chol(sum2))
		ENDTIMER(tChol);
		STARTTIMER(tRest);
		F77(gemm)("n","n",&n,&k,&k,&ONE,sum1_,&n, sum2_,&k,&ZERO,L,&n); // L <- sum1%*%sll
		
		//L <- L - alpha*Psi*sign(L)*abs(nk_one*eps+L)^{-spl}
		for (int i2 = 0; i2 < k; ++i2) {
			for (int i1 = 0; i1 < n; ++i1) {
				float s = L[i1+ n*i2];
				float sgn = (s > 0) - (s < 0);
				if ((non_negative<=0) || (sgn>0)) {
					float t = fabs(Psi[i1]*alpha*pow(MACHINE_EPS+fabs(s), -spl));
					if (fabs(s) > t)
						L[i1+ n*i2] -= sgn * t;
					else
						L[i1+ n*i2] = 0.0;
				} else
					L[i1+ n*i2] = 0.0;
			}
		}
		
		// TODO: deal with nL here! (check in the original code ~ line 470)
		// TODO: deal with lL here! (check in the original code ~ line 504)
		
		//Psi <- epsn+abs(XX - diag(tcrossprod(L,sum1)/n))
		t = 0.0;
		for (int i1 = 0; i1 < n ;++i1) {
			float s = 0.0;
			for (int i2 = 0; i2 < k; ++i2) {
				s += L[i1 + i2*n] * sum1_[i1 + i2*n];
			}
			if (fabs(s)>t)
				t = fabs(s);
			Psi[i1] = XX[i1] - s/l;
			if (Psi[i1] < eps) {
				Psi[i1] = eps;
			}
		}
		if (t < eps ) {
			for (int i1=0;i1<n;i1++) {
				Psi[i1] = eps;
			}
			for (int j=0; j < l; j++) {
				for (int i1=0; i1 < k; i1++)
					lapla[j*k+i1] = eps; 
			}
			printf("Last update was %f, which is smaller than %f, so I'm bailing out\n", t, eps);
			break;
		}
		
		if (scale) {
			for (int i = 0; i < k; i++){
				float s = 0.0;
				for (int j=0;j<n;j++) {
					s+=L[i*n + j]*L[i*n + j];
				}
				s= 1.0 / (sqrt(s/n)+MACHINE_EPS);
				for (int j=0;j<n;j++) {
					L[i*n + j]*=s;
				}
				s = pow(s*s,-spz);
				for (int j=0;j<l;j++) {
					lapla[j*k + i]*=s;
				}
			}
		}
		
		// check (and re-initialize) "all-zeroes" bicluster
		int nreset = 0;
		for (int i = 0; i < k; ++i) {
			int isZero = 1;
			for (int j = 0; j < n; ++j) {
				if (L[j + i*n] != 0) {
					isZero = 0;
					break;
				}
			}
			
			if (isZero) {
				++nreset;
				for (int j = 0; j < n; ++j)
					L[i*n + j] = (float) rand_normal();
				for (int j = 0; j < l; ++j)
					lapla[j*k + i] = 1.0;
			}
		}
		if (nreset)
			printf("iter %d: reset %d clusters\n", iter, nreset);
		
		if (verbose && (iter % verbose == 0)) {
			updateUI(iter, calculateElapsedTime(t0), k, n, l, L, Z, Psi, lapla);
		}
		ENDTIMER(tRest);
	}
	
	// last update
	if (t >= eps ) {
		for (int i1 = 0; i1 < k; i1++) { //LPsi<-diag(1/Psi)%*%L
			LPsiL[i1] = 0.0;
			for (int i2=0;i2<n;i2++) {
				LPsi[i2*k + i1] =  L[i2 + i1*n]/Psi[i2]; // LPsi gets transposed here
				LPsiL[i1] += L[i2 + i1*n]*LPsi[i2*k + i1];
			}
		}

		#pragma omp parallel for
		for (int j = 0; j < l; ++j) {
			int tid = omp_get_thread_num();
			float* iLPsiL = iLPsiL_ + (tid*k);
			float* iLPsiLX = iLPsiLX_ + (tid*n*k);

			approx_estimateZ(n, k, X+n*j, Z+j*k, lapla+j*k, LPsi, LPsiL, iLPsiL,iLPsiLX, NULL, NULL, spz, lap);
		}
	}
	else
		memset(Z, 0, k*l*sizeof(float));
		
	
	float tot = calculateElapsedTime(t0);
	printf("loop:   %5.2f (%.3f)\n"
	       "Chol:   %5.2f (%.3f)\n"
	       "Rest:   %5.2f (%.3f)\n"
	       "---------------------\nTotal:  %5.2f (%.3f)\n", tLoop, tLoop/tot, tChol, tChol / tot, tRest, tRest / tot, tot, tot/tot);

	free(XX); 
	free(sum1_);
	free(sum2_);
	free(iLPsiLX_);
	free(iLPsiL_);
	free(LPsiL);
	free(LPsi);
	return;
}