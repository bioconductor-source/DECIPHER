/****************************************************************************
 *               Utilities for Searching an Inverted Index                  *
 *                           Author: Erik Wright                            *
 ****************************************************************************/

// for OpenMP parallel processing
#ifdef _OPENMP
#include <omp.h>
#endif

/*
 * Rdefines.h is needed for the SEXP typedef, for the error(), INTEGER(),
 * GET_DIM(), LOGICAL(), NEW_INTEGER(), PROTECT() and UNPROTECT() macros,
 * and for the NA_INTEGER constant symbol.
 */
#include <Rdefines.h>

/*
 * R_ext/Rdynload.h is needed for the R_CallMethodDef typedef and the
 * R_registerRoutines() prototype.
 */
#include <R_ext/Rdynload.h>

/* for R_CheckUserInterrupt */
#include <R_ext/Utils.h>

// for calloc/free
#include <stdlib.h>

// for math functions
#include <math.h>

// for time and difftime
#include <time.h>

// DECIPHER header file
#include "DECIPHER.h"

// returns hits between queries and targets in an inverted index
SEXP searchIndex(SEXP query, SEXP wordSize, SEXP stepSize, SEXP logFreqs, SEXP count, SEXP location, SEXP index, SEXP positions, SEXP sepC, SEXP gapC, SEXP output, SEXP total, SEXP minScore, SEXP scoreOnly, SEXP verbose, SEXP pBar, SEXP nThreads)
{
	int i, j, k, p, c;
	int n = length(query); // number of sequences in the query
	int K = asInteger(wordSize); // k-mer length
	int step = asInteger(stepSize); // separation between k-mers
	double *freqs = REAL(logFreqs); // -log of normalized letter frequencies
	int size = length(logFreqs); // alphabet size
	int *num = INTEGER(count); // count of each k-mer in target
	int *loc = INTEGER(location); // location of k-mer in target
	int *ind = INTEGER(index); // index of target sequence
	int *pos = INTEGER(positions); // number of matchable positions in target
	int L = length(count); // number of possible of k-mers
	int type = asInteger(output); // 1 = all hits, 2 = one hit per target, 3 = top hit
	double tot = asReal(total); // total size of target database
	double minS = asReal(minScore); // minimum score or NA to calculate
	int sO = asInteger(scoreOnly); // FALSE to output anchor positions
	int nthreads = asInteger(nThreads);
	
	// set up a timer to minimize interrupt checks
	time_t start, end;
	double elapsed;
	time(&start);
	
	int before, v, *rPercentComplete;
	double N, soFar;
	SEXP percentComplete, utilsPackage;
	v = asLogical(verbose);
	if (v) { // percent complete variables
		N = 0; // completed iterations
		soFar = 0;
		before = 0;
		PROTECT(percentComplete = NEW_INTEGER(1));
		rPercentComplete = INTEGER(percentComplete);
		// make it possible to access R functions from the utils package for the progress bar
		PROTECT(utilsPackage = eval(lang2(install("getNamespace"), ScalarString(mkChar("utils"))), R_GlobalEnv));
	}
	
	// determine cost for the distance between k-mer matches
	double sC = asReal(sepC); // cost for separation between k-mers
	double gC = asReal(gapC); // cost for minimum gaps between k-mers
	int maxSep = (int)sqrt((double)L); // at least one match expected by chance
	double *sepCost = (double *) malloc((maxSep + 1)*sizeof(double)); // thread-safe on Windows
	double *gapCost = (double *) malloc((maxSep + 1)*sizeof(double)); // thread-safe on Windows
	for (i = 0; i <= maxSep; i++) {
		sepCost[i] = sqrt(i);
		gapCost[i] = gC*sepCost[i];
		sepCost[i] *= sC;
	}
	
	// calculate -log(expected k-mer frequency)
	double *addScores = (double *) calloc(L, sizeof(double)); // initialized to zero (thread-safe on Windows)
	double *scores = (double *) calloc(L, sizeof(double)); // initialized to zero (thread-safe on Windows)
	c = 1;
	for (i = 1; i <= K; i++) {
		k = -1;
		j = 0;
		p = 0;
		while (j < L) {
			if (j == p) {
				p += c;
				if (k == size - 1) {
					k = 0;
				} else {
					k++;
				}
			}
			scores[j] += freqs[k];
			if (i > K - step)
				addScores[j] += freqs[k];
			j++;
		}
		c *= size;
	}
	
	// determine the offset for each k-mer
	R_xlen_t *offset = (R_xlen_t *) malloc(L*sizeof(R_xlen_t)); // thread-safe on Windows
	offset[0] = 0;
	k = 0;
	for (i = 1; i < L; i++) {
		offset[i] = offset[k] + num[k];
		k = i;
	}
	
	// build threadsafe vectors for outputs
	double **vecs = (double **) malloc(n*sizeof(double *)); // thread-safe on Windows
	int **ptrs = (int **) malloc(n*sizeof(int *)); // thread-safe on Windows
	int *l = (int *) malloc(n*sizeof(int)); // thread-safe on Windows
	for (i = 0; i < n; i++) {
		ptrs[i] = INTEGER(VECTOR_ELT(query, i)); // query k-mers
		l[i] = length(VECTOR_ELT(query, i)); // query length
	}
	int ***matrices;
	if (sO == 0)
		matrices = (int ***) malloc(n*sizeof(int **)); // thread-safe on Windows
	
	int abort = 0;
	#ifdef _OPENMP
	#pragma omp parallel for private(i,j,k,p,c) schedule(dynamic) num_threads(nthreads)
	#endif
	for (i = 0; i < n; i++) { // each sequence
		if (abort == 0) {
			int *w = ptrs[i]; // k-mers
			
			// record the number of unmasked positions
			int width = 0; // positions in the query
			k = -2; // last unmasked position
			for (j = 0; j < l[i]; j++) { // each k-mer
				if (w[j] != NA_INTEGER) { // unmasked
					if (k == j - 1) {
						width++; // query is always staggered by one position
					} else {
						// all masked (NA) positions are at least K long
						width += K; // new k-mer
					}
					k = j;
				}
			}
			
			// count target occurrences of each query k-mer
			int s = 0; // total number of k-mers shared with targets
			int *counts = (int *) malloc(l[i]*sizeof(int)); // thread-safe on Windows
			for (j = 0; j < l[i]; j++) { // each query k-mer
				if (w[j] == NA_INTEGER) {
					counts[j] = 0;
				} else {
					counts[j] = num[w[j]];
					s += counts[j];
					if (s < 0) { // signed integer overflow
						abort = i + 1;
						break;
					}
				}
			}
			if (abort != 0 // exit
				|| width == 0 // no query
				|| s == 0) { // no targets
				free(counts);
				
				int *set = (int *) malloc(0*sizeof(int)); // thread-safe on Windows
				double *score = (double *) malloc(0*sizeof(double)); // thread-safe on Windows
				ptrs[i] = set;
				vecs[i] = score;
				l[i] = 0; // number of results
				if (sO == 0) {
					int **anchors = (int **) malloc(0*sizeof(int *)); // thread-safe on Windows
					matrices[i] = anchors;
				}
				continue;
			}
			
			// record target occurrences of each query k-mer
			int *posQuery = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			int *posTarget = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			int *set = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			double *score = (double *) malloc(s*sizeof(double)); // thread-safe on Windows
			double *addScore = (double *) malloc(s*sizeof(double)); // thread-safe on Windows
			s = 0;
			for (j = 0; j < l[i]; j++) { // each query k-mer
				if (counts[j] > 0) { // w[j] != NA_INTEGER
					R_xlen_t P = offset[w[j]]; // position of k-mer in loc and ind
					for (k = 0; k < counts[j]; k++) { // each target instance of k-mer
						posQuery[s] = j + 1;
						posTarget[s] = loc[P];
						set[s] = ind[P];
						score[s] = scores[w[j]];
						addScore[s] = addScores[w[j]];
						P++;
						s++;
					}
				}
			}
			
			// merge sort on set then posTarget
			int *o1 = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			for (j = 0; j < s; j++)
				o1[j] = j; // initial order
			int *o2; // pointer to previous order
			for (j = 1; j < l[i]; j++)
				counts[j] += counts[j - 1]; // batch size
			c = l[i]; // current number of batches
			int group1, group2;
			while (c > 1) {
				o2 = o1; // store previous order
				o1 = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
				p = 0; // position in o1
				j = 0; // position in group 1
				group1 = 0; // group 1 in counts
				group2 = 1; // group 2 in counts
				// sort pairs of groups
				while (group2 < c) {
					k = counts[group1]; // position in group 2
					while (j < counts[group1] && k < counts[group2]) {
						if (set[o2[j]] == set[o2[k]]) { // apply tiebreaker
							if (posTarget[o2[j]] <= posTarget[o2[k]]) {
								o1[p++] = o2[j++];
							} else {
								o1[p++] = o2[k++];
							}
						} else if (set[o2[j]] < set[o2[k]]) {
							o1[p++] = o2[j++];
						} else {
							o1[p++] = o2[k++];
						}
					}
					while (j < counts[group1])
						o1[p++] = o2[j++];
					while (k < counts[group2])
						o1[p++] = o2[k++];
					group1 = group2 + 1;
					group2 = group1 + 1;
					j = k;
				}
				while (p < s) {
					o1[p] = o2[p];
					p++;
				}
				
				// merge pairs of groups
				p = 0;
				k = 0;
				while (k < c - 1) {
					k += 2;
					counts[p++] = counts[k - 1];
				}
				if (k < c)
					counts[p++] = counts[k++];
				c = p;
				free(o2);
			}
			free(counts);
			
			// reorder all vectors
			int *posQuery2 = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			int *posTarget2 = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			int *set2 = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			double *score2 = (double *) malloc(s*sizeof(double)); // thread-safe on Windows
			double *addScore2 = (double *) malloc(s*sizeof(double)); // thread-safe on Windows
			for (j = 0; j < s; j++) {
				posQuery2[j] = posQuery[o1[j]];
				posTarget2[j] = posTarget[o1[j]];
				set2[j] = set[o1[j]];
				score2[j] = score[o1[j]];
				addScore2[j] = addScore[o1[j]];
			}
			free(o1);
			free(posQuery);
			free(posTarget);
			free(set);
			free(score);
			free(addScore);
			posQuery = posQuery2;
			posTarget = posTarget2;
			set = set2;
			score = score2;
			addScore = addScore2;
			
			// collapse adjacent hits
			int *len = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			int *origin = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			char *keep = (char *) malloc(s*sizeof(char)); // thread-safe on Windows
			for (j = 0; j < s; j++) {
				len[j] = K;
				origin[j] = j;
				keep[j] = 1;
			}
			c = 0; // current position
			k = 0; // previous position
			p = s; // remaining vector length
			int deltaQuery, deltaTarget;
			while (c < s - 1) {
				c++; // advance current position
				if (set[k] == set[c]) { // same index
					j = k;
					while (j < c) {
						deltaTarget = posTarget[c] - posTarget[j];
						if (deltaTarget > step) {
							k = j + 1; // advance previous position
						} else if (deltaTarget == step) {
							deltaQuery = posQuery[c] - posQuery[j];
							if (deltaQuery == step) { // merge positions
								p--;
								keep[c] = 0;
								origin[c] = origin[j];
								len[origin[j]] = len[origin[j]] + step;
								score[origin[j]] = score[origin[j]] + addScore[c];
								break; // done merging
							}
						} else { // deltaTarget == 0L
							break; // reached same target position
						}
						j++;
					}
				} else {
					k = c;
				}
			}
			free(addScore);
			free(origin);
			
			// resize all vectors
			posQuery2 = (int *) malloc(p*sizeof(int)); // thread-safe on Windows
			posTarget2 = (int *) malloc(p*sizeof(int)); // thread-safe on Windows
			set2 = (int *) malloc(p*sizeof(int)); // thread-safe on Windows
			int *len2 = (int *) malloc(p*sizeof(int)); // thread-safe on Windows
			score2 = (double *) malloc(p*sizeof(double)); // thread-safe on Windows
			p = 0;
			for (j = 0; j < s; j++) {
				if (keep[j]) {
					posQuery2[p] = posQuery[j];
					posTarget2[p] = posTarget[j];
					set2[p] = set[j];
					len2[p] = len[j];
					score2[p] = score[j];
					p++;
				}
			}
			s = p;
			free(keep);
			free(posQuery);
			free(posTarget);
			free(set);
			free(len);
			free(score);
			posQuery = posQuery2;
			posTarget = posTarget2;
			set = set2;
			len = len2;
			score = score2;
			
			// determine significance of occurrences
			int *chain = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			origin = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			int *cov = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
			for (j = 0; j < s; j++) {
				chain[j] = j;
				origin[j] = j;
				cov[j] = len[j] - 1;
			}
			j = 0; // last hit
			k = 1; // current hit
			double prevScore, tempScore;
			int gap, sep;
			while (k < s) {
				if (set[k] != set[j]) { // switched index
					j = k;
				} else {
					prevScore = score[k];
					p = j;
					while (p < k) {
						deltaTarget = posTarget[k] - posTarget[p] - len[p];
						if (deltaTarget > maxSep) {
							j = p; // limit search space
						} else if (deltaTarget >= 0) {
							deltaQuery = posQuery[k] - posQuery[p] - len[p];
							if (deltaQuery >= 0 && deltaQuery <= maxSep) {
								tempScore = score[p] + prevScore;
								if (tempScore > score[k]) {
									if (deltaQuery > deltaTarget) {
										gap = deltaQuery - deltaTarget;
										sep = deltaTarget;
									} else {
										gap = deltaTarget - deltaQuery;
										sep = deltaQuery;
									}
									tempScore = tempScore + gapCost[gap];
									tempScore = tempScore + sepCost[sep];
									if (tempScore > score[k]) {
										score[k] = tempScore;
										chain[k] = p;
										origin[k] = origin[p];
										cov[k] = len[k] - 1 + cov[p];
									}
								}
							}
						}
						p++;
					}
				}
				k++;
			}
			
			// correct for size of target search space
			for (j = 0; j < s; j++) {
				score[j] -= log(((double)(pos[set[j] - 1] - cov[j]))/((double)step));
				score[j] -= log((double)(width - cov[j]));
			}
			free(cov);
			
			// determine the candidate results set
			int *res; // indices of results
			if (type == 1) { // all hits
				// eliminate lower scoring chains with same origin
				int *maxOrigin; // pointer to maximum origin
				maxOrigin = (int *) malloc(s*sizeof(int)); // thread-safe on Windows
				for (j = 0; j < s; j++) {
					if (origin[j] == j) {
						maxOrigin[j] = j;
					} else if (score[maxOrigin[origin[j]]] < score[j]) {
						maxOrigin[origin[j]] = j;
						maxOrigin[j] = NA_INTEGER;
					} else {
						maxOrigin[j] = NA_INTEGER;
					}
				}
				keep = calloc(s, sizeof(char)); // initialized to zero (thread-safe on Windows)
				c = 0; // count of results
				for (j = 0; j < s; j++) {
					if (maxOrigin[j] != NA_INTEGER) {
						keep[maxOrigin[j]] = 1;
						c++;
					}
				}
				free(maxOrigin);
				res = (int *) malloc(c*sizeof(int)); // thread-safe on Windows
				p = 0;
				for (j = 0; j < s; j++)
					if (keep[j])
						res[p++] = j;
				free(keep);
			} else if (type == 2) { // one hit per target
				keep = calloc(s, sizeof(char)); // initialized to zero (thread-safe on Windows)
				p = NA_INTEGER; // current set
				k = 0; // position of max per set
				c = 0; // count of results
				for (j = 0; j < s; j++) {
					if (p != set[j]) {
						p = set[j];
						k = j;
						keep[k] = 1;
						c++;
					} else {
						if (score[j] > score[k]) {
							keep[k] = 0;
							k = j;
							keep[k] = 1;
						}
					}
				}
				res = (int *) malloc(c*sizeof(int)); // thread-safe on Windows
				p = 0;
				for (j = 0; j < s; j++)
					if (keep[j])
						res[p++] = j;
				free(keep);
			} else { // top hit
				c = 1; // count of results
				res = (int *) malloc(c*sizeof(int)); // thread-safe on Windows
				res[0] = 0;
				for (j = 1; j < s; j++)
					if (score[j] > score[res[0]])
						res[0] = j;
			}
			free(origin);
			
			// determine the minimum score per target
			k = 0;
			if (ISNA(minS)) {
				double mS;
				for (j = 0; j < c; j++) {
					mS = log((tot - (double)pos[set[res[j]] - 1])/(double)step);
					if (score[res[j]] >= mS)
						res[k++] = res[j];
				}
			} else {
				for (j = 0; j < c; j++) {
					if (score[res[j]] >= minS)
						res[k++] = res[j];
				}
			}
			c = k; // count passing minScore
			set2 = (int *) malloc(c*sizeof(int)); // thread-safe on Windows
			score2 = (double *) malloc(c*sizeof(double)); // thread-safe on Windows
			for (j = 0; j < c; j++) {
				set2[j] = set[res[j]];
				score2[j] = score[res[j]];
			}
			free(set);
			free(score);
			set = set2;
			score = score2;
			
			ptrs[i] = set;
			vecs[i] = score;
			l[i] = c; // number of results
			
			if (sO == 0) { // include anchor positions with output
				int **anchors = (int **) malloc(c*sizeof(int *)); // thread-safe on Windows
				for (j = 0; j < c; j++) {
					// measure length of chain
					p = res[j];
					k = 1; // length of chain
					while (p != chain[p]) {
						p = chain[p]; // traceback
						k++; // lengthen chain
					}
					
					// record anchor positions
					int *anchor = (int *) malloc((4*k + 1)*sizeof(int)); // thread-safe on Windows
					anchor[0] = k; // store number of anchors in first position
					p = res[j];
					k = 1;
					anchor[k++] = posQuery[p];
					anchor[k++] = posQuery[p] + len[p] - 1;
					anchor[k++] = posTarget[p];
					anchor[k++] = posTarget[p] + len[p] - 1;
					while (p != chain[p]) {
						p = chain[p]; // traceback
						anchor[k++] = posQuery[p];
						anchor[k++] = posQuery[p] + len[p] - 1;
						anchor[k++] = posTarget[p];
						anchor[k++] = posTarget[p] + len[p] - 1;
					}
					anchors[j] = anchor;
				}
				matrices[i] = anchors;
			}
			
			free(posQuery);
			free(posTarget);
			free(len);
			free(chain);
			free(res);
			
			if (v) {
				#ifdef _OPENMP
				#pragma omp critical
				{
					N++;
				}
				#else
				N++;
				#endif
			}
			
			#ifdef _OPENMP
			int master = omp_get_thread_num();
			#else
			int master = 0;
			#endif
			
			if (master == 0) { // master thread
				time(&end);
				elapsed = difftime(end, start);
				
				if (elapsed >= 1) { // at least 1 second has elapsed
					start = end;
					abort = checkInterrupt(); // call back to R
					if (abort == 0 && v) {
						soFar = N/n;
						*rPercentComplete = floor(100*soFar);
						if (*rPercentComplete > before) { // when the percent has changed
							// tell the progress bar to update in the R console
							eval(lang4(install("setTxtProgressBar"), pBar, percentComplete, R_NilValue), utilsPackage);
							before = *rPercentComplete;
						}
					}
				}
			}
		} else {
			int *set = (int *) malloc(0*sizeof(int)); // thread-safe on Windows
			double *score = (double *) malloc(0*sizeof(double)); // thread-safe on Windows
			ptrs[i] = set;
			vecs[i] = score;
			l[i] = 0; // number of results
			if (sO == 0) {
				int **anchors = (int **) malloc(0*sizeof(int *)); // thread-safe on Windows
				matrices[i] = anchors;
			}
		}
	}
	
	free(addScores);
	free(scores);
	free(offset);
	free(sepCost);
	free(gapCost);
	
	int **anchors; // pointers to anchor positions
	if (abort != 0) { // release memory
		for (i = 0; i < n; i++) {
			int *set = ptrs[i];
			double *score = vecs[i];
			if (sO == 0) {
				anchors = matrices[i];
				for (j = 0; j < l[i]; j++) {
					int *anchor = anchors[j];
					free(anchor);
				}
				free(anchors);
			}
			free(set);
			free(score);
		}
		
		free(vecs);
		free(ptrs);
		free(l);
		if (sO == 0)
			free(matrices);
		
		if (abort < 0) {
			error("Received user interrupt.");
		} else {
			error("Too many target k-mer hits for myXStringSet[%d].", abort);
		}
	}
	
	c = 0;
	for (i = 0; i < n; i++)
		c += l[i];
	
	SEXP ans, ans0, ans1, ans2, ans3, ret_list;
	PROTECT(ans0 = allocVector(INTSXP, c));
	int *rans0 = INTEGER(ans0);
	PROTECT(ans1 = allocVector(INTSXP, c));
	int *rans1 = INTEGER(ans1);
	PROTECT(ans2 = allocVector(REALSXP, c));
	double *rans2 = REAL(ans2);
	if (sO == 0)
		PROTECT(ans3 = allocVector(VECSXP, c));
	
	k = 0; // position in output vectors
	for (i = 0; i < n; i++) {
		int *set = ptrs[i];
		double *score = vecs[i];
		if (sO == 0)
			anchors = matrices[i];
		for (j = 0; j < l[i]; j++) {
			rans0[k] = i + 1;
			rans1[k] = set[j];
			rans2[k] = score[j];
			if (sO == 0) {
				int *anchor = anchors[j];
				c = anchor[0]; // number of anchors
				PROTECT(ans = allocMatrix(INTSXP, 4, c));
				int *rans = INTEGER(ans);
				p = 0;
				while (c > 0) { // reverse anchor order
					rans[p++] = anchor[4*c - 3];
					rans[p++] = anchor[4*c - 2];
					rans[p++] = anchor[4*c - 1];
					rans[p++] = anchor[4*c];
					c--;
				}
				free(anchor);
				SET_VECTOR_ELT(ans3, k, ans);
				UNPROTECT(1);
			}
			k++;
		}
		free(set);
		free(score);
		if (sO == 0)
			free(anchors);
	}
	
	free(vecs);
	free(ptrs);
	free(l);
	if (sO == 0) {
		PROTECT(ret_list = allocVector(VECSXP, 4));
		free(matrices);
	} else { // score only
		PROTECT(ret_list = allocVector(VECSXP, 3));
	}
	
	SET_VECTOR_ELT(ret_list, 0, ans0);
	SET_VECTOR_ELT(ret_list, 1, ans1);
	SET_VECTOR_ELT(ret_list, 2, ans2);
	
	if (sO == 0) {
		SET_VECTOR_ELT(ret_list, 3, ans3);
		UNPROTECT(5);
	} else { // score only
		UNPROTECT(4);
	}
	if (v)
		UNPROTECT(2);
	
	return ret_list;
}

// update count of k-mers in a list of k-mers
SEXP countIndex(SEXP num, SEXP query, SEXP step)
{
	if (MAYBE_SHARED(num))
		error(".Call function 'countIndex' called in incorrect context.");
	
	int i, j, n, *kmers;
	int *rans = INTEGER(num);
	int l = length(query); // number of sequences
	int s = asInteger(step); // step size between k-mers
	
	// set up a timer to minimize interrupt checks
	time_t start, end;
	double elapsed;
	time(&start);
	
	for (i = 0; i < l; i++) {
		kmers = INTEGER(VECTOR_ELT(query, i)); // query k-mers
		n = length(VECTOR_ELT(query, i)); // query length
		for (j = 0; j < n; j += s)
			if (kmers[j] != NA_INTEGER) // unmasked position
				rans[kmers[j]]++; // increment count
		
		time(&end);
		elapsed = difftime(end, start);
		if (elapsed >= 1) { // at least 1 second has elapsed
			start = end;
			int abort = checkInterrupt(); // call back to R
			
			if (abort != 0)
				error("Received user interrupt.");
		}
	}
	
	return R_NilValue;
}

// update record of k-mers in a list of k-mers
SEXP updateIndex(SEXP offset, SEXP query, SEXP wordSize, SEXP step, SEXP location, SEXP index, SEXP positions, SEXP count)
{
	if (MAYBE_SHARED(offset))
		error(".Call function 'updateIndex' called in incorrect context.");
	if (MAYBE_SHARED(location))
		error(".Call function 'updateIndex' called in incorrect context.");
	if (MAYBE_SHARED(index))
		error(".Call function 'updateIndex' called in incorrect context.");
	if (MAYBE_SHARED(positions))
		error(".Call function 'updateIndex' called in incorrect context.");
	
	int i, j, k, n, *kmers;
	double *off = REAL(offset);
	int l = length(query); // number of sequences
	int K = asInteger(wordSize); // k-mer length
	int s = asInteger(step); // step size between k-mers
	int *loc = INTEGER(location);
	int *ind = INTEGER(index);
	int *pos = INTEGER(positions);
	int c = asInteger(count); // processed count
	pos += c; // offset starting position by count
	
	// set up a timer to minimize interrupt checks
	time_t start, end;
	double elapsed;
	time(&start);
	
	for (i = 0; i < l; i++) {
		c++;
		kmers = INTEGER(VECTOR_ELT(query, i)); // query k-mers
		n = length(VECTOR_ELT(query, i)); // query length
		
		// record the number of unmasked positions
		k = -1*s - 1; // last hit
		for (j = 0; j < n; j++) {
			if (kmers[j] != NA_INTEGER) {
				if (k == j - s) {
					pos[i] += s;
				} else {
					pos[i] += K;
				}
				k = j;
			}
		}
		
		// update the inverted index
		for (j = 0; j < n; j += s) {
			k = kmers[j];
			if (k != NA_INTEGER) { // unmasked position
				ind[(R_xlen_t)off[k]] = c;
				loc[(R_xlen_t)off[k]] = j + 1;
				off[k]++;
			}
		}
		
		time(&end);
		elapsed = difftime(end, start);
		if (elapsed >= 1) { // at least 1 second has elapsed
			start = end;
			int abort = checkInterrupt(); // call back to R
			
			if (abort != 0)
				error("Received user interrupt.");
		}
	}
	
	return R_NilValue;
}

// determine offset and approximate letter frequencies
SEXP approxFreqs(SEXP offset, SEXP freqs, SEXP count)
{
	if (MAYBE_SHARED(offset))
		error(".Call function 'approxFreqs' called in incorrect context.");
	if (MAYBE_SHARED(freqs))
		error(".Call function 'approxFreqs' called in incorrect context.");
	
	int l = length(offset);
	double *off = REAL(offset);
	double *freq = REAL(freqs);
	int s = length(freqs);
	int *num = INTEGER(count);
	int binSize = l/s;
	int n = binSize;
	
	int j;
	int i = 0; // index in num
	int k = 0; // index in freqs
	freq[0] = num[0];
	while (i < l - 1) {
		j = i + 1;
		off[j] = off[i] + (double)num[i];
		i = j;
		if (i >= n) {
			k++;
			n += binSize;
		}
		freq[k] += num[i];
	}
	
	return R_NilValue;
}
