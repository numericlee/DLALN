// ALN Library
// file train_ops.cpp
// Copyright (C) 2018 William W. Armstrong.
// 
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// Version 3 of the License, or (at your option) any later version.
// 
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
// 
// For further information contact 
// William W. Armstrong
// 3624 - 108 Street NW
// Edmonton, Alberta, Canada  T6J 1B4

#ifdef ALNDLL
#define ALNIMP __declspec(dllexport)
#endif

#include <aln.h>
#include "alnpriv.h"

#ifdef _DEBUG
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// include files
#include <stdafx.h>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <alnpp.h>
#include <dtree.h>
#include <datafile.h>
#include <malloc.h>
#include ".\cmyaln.h" 
#include "alnextern.h"
#include "alnintern.h"
#include <Eigen/Dense>

using namespace Eigen;

//defines used to set up TRfile and VARfile

#define LINEAR_REGRESSION 0
#define TESSELLATION 1
#define APPROXIMATION 2
#define BAGGING 3

// We use dblRespTotal in two ways and the following definition helps.
#define DBLNOISEVARIANCE dblRespTotal

// files used in training operations
CDataFile TRfile;
CDataFile VARfile;

//routines
void ALNAPI doLinearRegression(); // Determines an upper bound on error, and provides a start for other training.
void ALNAPI createNoiseVarianceFile(); // This creates tesselations and the noise variance file VARfile.
void ALNAPI approximate(); // Actually does training avoiding overtraining using samples in VARfile.
void ALNAPI outputTrainingResults();
void ALNAPI trainAverage(); // Takes several ALNs created in approximate() and creates an ALN of their average
void ALNAPI constructDTREE(int nMaxDepth); // Takes the average ALN and turns it into a DTREE
void ALNAPI cleanup(); // Destroys ALNs etc.
void fillvector(double * adblX, CMyAln* paln); // Sends a vector to training from a file, online or averaging.
void ALNAPI createTR_VARfiles(int nChoose);
void createSamples(); // Creates noise variance samples

// ALN pointers
CMyAln * pALN = NULL; // declares a pointer to an ALN used in linear regression
static CMyAln** apALN = NULL;  // an array of pointers to ALNs used in approximate()
static CMyAln* pAvgALN;      // an ALN representing the bagged average of several ALNs trained on the TVfile with different random numbers

// Some global variables
double dblMinRMSE = 0; // stops training when the training error is smaller than this
double dblLearnRate = 0.2;  // roughly, 0.2 corrects most of the error for if we make 15 passes through TRfile
int nNumberEpochs = 10; // if the learnrate is 0.2, then one will need 5 or 10 roughly to almost correct the errors
long nRowsTR; // the number of rows in the current training set loaded into TRfile
long nRowsVAR; // the number of rows in the noise variance file.  When approximation starts, this should be nRowsTV
double dblFlimit = 0 ;// For linear regression can be anything, for overtraining must be zero
int nEpochSize; // the number of input vectors in the current training set
BOOL bALNgrowable = TRUE; // FALSE for linear regression, TRUE otherwise
BOOL bTessellate = FALSE; // Controls setup of training for creating two Delaunay tessellations of a partition of the TVfile.
BOOL bStopTraining; // Set to TRUE and becomes FALSE if any (active) linear piece needs training
int nNotifyMask = AN_TRAIN | AN_EPOCH | AN_VECTORINFO; // used with callbacks
double * adblX = NULL;
ALNNODE** apLFN; // This stores the LFN of the tessellation a sample X belongs to.
long nNoiseVarianceSamples; // The number of noise variance samples in VARfile
double dblFlatten; // This number is the sum of the squares of domain coordinates
   // which forms a paraboloid for tessallation.  Smoothing 0.01 is 1% of the depth.

void ALNAPI doLinearRegression() // routine
{
  fprintf(fpProtocol,"\n****** Linear regression begins: we want an upper bound on error plus starting weights *****\n");
	// This iterative algorithm is not an accepted method in general, but works well here.
	// The reason for using it for ALN approximation is that the linear regression
	// problem for a linear piece (leaf node = LFN) is constantly changing. A piece gains or loses input vectors
	// for which it determines the output value via the ALN tree of max and min operators.
	// This constantly changes the samples which are to be least-squares fitted for the linear piece.
	// Linear regression helps get the centroid and weights of a linear piece in neighborhood of
	// good values to start other training later.
	adblX = (double *)malloc((nDim) * sizeof(double));

	// Set up the ALN
	pALN = new CMyAln;
	if (!(pALN->Create(nDim, nDim-1))) // nDim is the number of inputs to the ALN plus one for
		//the ALN output. The default output variable is nDim-1.
	{
		fprintf(fpProtocol,"Stopping: linear regression ALN creation failed!\n");
    fflush(fpProtocol);
		exit(0);
	}
  fflush(fpProtocol); // If something goes wrong, we can perhaps see it in the protocol file if we flush before the crash.

	// Set constraints on variables for the ALN	
	for( int m = 0; m < nDim-1; m++) // Note that loops that have m < nDim-1 omit the output variable nDim - 1.
  // TO DO. ALN input number nDim -1 is the last (rightmost) ALN input. It need not be connected
	// to the rightmost data file column. Some steps have been taken to make nOutput specifiable as
  // other than nDim - 1.  We can invert ALNs that have a monotonic variable as input so that input
  // becomes the output. This should be fully implemented.
	{
		pALN->SetEpsilon(adblEpsilon[m],m); // adblEpsilon suggests the size along axis m of a box
		// such that the boxes just cover the domain. It can be used to compute jitter, or to
		// estimate the maximum absolute slope of linear pieces that makes sense, given the data.
		// We input all but the output adblEpsilon (index nDim-1) into this ALN. These Epsilons
		// are set in analyzeTV(). The values of adblEpsilon show up in the constraints.
    if(adblEpsilon[m] == 0)
    {
      fprintf(fpProtocol,"Stopping: Variable %d appears to be constant. Try removing it.\n",m);
      fflush(fpProtocol);
			exit(0);
    }
		pALN->SetMin(adblMinVar[m] - 0.1 * adblStdevVar[m],m); // We extend the domain a bit beyond what the data demands.
		pALN->SetMax(adblMaxVar[m] + 0.1 * adblStdevVar[m],m); // An evaluation file could have a slighly bigger domain (fingers crossed).
		// Next we bound weights (partial derivatives) by noting it makes no sense to have a slope that jumps a huge amount between two input data samples.
		pALN->SetWeightMin(- pow(2.0,0.5) * adblStdevVar[nDim - 1]/adblEpsilon[m],m);  
		pALN->SetWeightMax(  pow(2.0,0.5) * adblStdevVar[nDim - 1]/adblEpsilon[m],m); 
		// impose the a priori bounds on weights
    if(dblMinWeight[m] > pALN->GetWeightMin(m))
    {
		  pALN->SetWeightMin(dblMinWeight[m],m);
    }
    if(dblMaxWeight[m] < pALN->GetWeightMax(m))
    {
		  pALN->SetWeightMax(dblMaxWeight[m],m);
    }
	}
  // createTR_VARfiles(0); see ALNfitDeepView; for linear regression, training set TRfile is  about 50% of the TVfile.
	// The rest is for noise variance samples in VARfile, not used until the approximation phase below.
	// NB The region concept has not been completed.  It allows the user to impose constraints
	// e.g. on slopes(weights) which differ in different parts of the domain.  All we have is region 0 now.

	bALNgrowable = FALSE; // The ALN consists of one leaf node LFN for linear regression.
	bTessellate = FALSE; // TRUE only during creation of tesselations.
	bTrainingAverage = FALSE; // Switch to tell fillvector whether get a training vector or compute an average.
	int nEpochSize = nRowsTV; // nEpochsize gives the number of training samples. Later nRowsTR=nRowsTV.
	(pALN->GetRegion(0))->dblSmoothEpsilon = 0.0; // Linear regression: nothing to smooth.
	nNumberEpochs = 200; // We may have to do tests to see what is sufficient
	dblMinRMSE = 0; // Don't stop early because of low training error.
	dblLearnRate = 0.15;  // This rate seems ok for now.

	// Set up the data
	nRowsTR = nRowsVAR = nRowsTV; // Create the training and noise variance files
	TRfile.Create(nRowsTR, nALNinputs);  // We use this as a buffer for *all* training.
	VARfile.Create(nRowsVAR, nALNinputs); // Both files contain samples of TVfile
	fprintf(fpProtocol, "TRfile and VARfile created, we start linear regression\n");
	fflush(fpProtocol);
	createTR_VARfiles(LINEAR_REGRESSION);
	int nSamples = TRfile.RowCount();
	int nColumns = TRfile.ColumnCount();
	const double* adblData = TRfile.GetDataPtr();
	// The following could also set NULL instead of adblData which leads
	// to fillvector() controlling the input vectors to the ALN.  That allows the
	// system to choose training vectors more flexibly, even online.
	// The advantage of giving the pointer adblData is that epochs consistently
	// permute the order of the samples and go through all samples exactly once.
	pALN->SetDataInfo(nSamples, nColumns, adblData, NULL);
	// TRAIN FOR LINEAR REGRESSION   vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
	// The reason for iterations is so that we can monitor progress in the ... TrainProtocol.txt file,
	// and set new parameters for future training.
	for (int iteration = 0; iteration < 10; iteration++)  // experimental
	{
		if (!pALN->Train(nNumberEpochs, dblMinRMSE, dblLearnRate, FALSE, nNotifyMask))
		{
			fprintf(fpProtocol, "Linear regression training failed!\n");
			fflush(fpProtocol);
			exit(0);
		}
		fprintf(fpProtocol, "\nIteration %d of linear regression completed. Training RMSE = %f \n",iteration, dblTrainErr);
	}
	fprintf(fpProtocol, "Linear regression training succeeded!\n");
	fflush(fpProtocol);
  // We should now have a good linear regression fit.
  // Find the weights on the linear piece using an evaluation at the 0 vector (could be any other place!).
	
	ALNNODE* pActiveLFN;
  for(int m=0; m < nDim; m++)
	{
		adblX[m] = 0;
	}
	double dummy = pALN->QuickEval(adblX, &pActiveLFN); // this finds a pointer to the linear piece
	fprintf(fpProtocol,"Linear regression weights on ALN\n");
	for (int m = 0; m < nDim-1; m++)
	{
    if(nLag[m] == 0) // any data we are looking at can have nonzero lags
    {
			// Note that adblW is stored in a different way, the weight on axis 0 is in adblW[1] etc.
  		fprintf(fpProtocol,"Linear regression weight on %s is %f centroid is %f\n",\
			varname[nInputCol[m]] , ((pActiveLFN)->DATA.LFN.adblW)[m+1], ((pActiveLFN)->DATA.LFN.adblC)[m]); 
		}
    else
    {
  		fprintf(fpProtocol,"Linear regression weight on ALN input %s@lag%d is %f centroid is %f\n",\
			varname[nInputCol[m]], nLag[m] , ((pActiveLFN)->DATA.LFN.adblW)[m+1], ((pActiveLFN)->DATA.LFN.adblC)[m]); 
    }
		adblLRC[m] = ((pActiveLFN)->DATA.LFN.adblC)[m]; // centroid of linear piece for axis m
		adblLRW[m+1] = ((pActiveLFN)->DATA.LFN.adblW)[m+1]; // linear regression weight for axis m
  }
	fprintf(fpProtocol,"Value of the linear piece at the origin is %f; the value at the centroid is %f\n",\
		((pActiveLFN)->DATA.LFN.adblW)[0], ((pActiveLFN)->DATA.LFN.adblC)[nDim - 1] ); 
	fflush(fpProtocol);
	// Put the negative sum of weighted centroids into the 0 position
	// compress the weighted centroid info into W[0], i.e. take the centroids out of all terms like
	// ... + weight_m*(x_m-centroid_m) + ... to have just one number in W[0]
	adblLRW[0] = adblLRC[nDim -1] = ((pActiveLFN)->DATA.LFN.adblC)[nDim -1];
  for (int m = 0; m < nDim - 1; m++)
  {
    adblLRW[0] -= adblLRW[m+1] * adblLRC[m];
  }
	adblLRW[nDim] = -1.0;  // the -1 weight on the ALN output may make a difference somewhere, so we set it here.
	// The idea of weight = -1 is that the hyperplane equation can be written 
	// a*x + b*y -1*z = 0 like an equation instead of z = a*x + b*y like a function.
	// This representation is good if we want to invert the ALN, i.e. get x as a function of y and z.
	// How to do it? Just multiply the first equation by -1/a in all linear pieces. The weight on the output becomes -1.0
	// This is not fully implemented yet.  The output of the ALN is now always the highest index ALN variable nDim -1.

	double desired, predict, se;
	int nCount; 
	nCount = 0;
	for (int j = 0; j < nRowsTR; j++)
	{
		for (int i = 0; i < nDim; i++)
		{
			adblX[i] = TRfile.GetAt(j, i, 0);
		}
		desired = adblX[nDim - 1]; // get the desired result
		adblX[nDim - 1] = 0; // not used in evaluation by QuickEval
		predict = pALN->QuickEval(adblX, &pActiveLFN);
		se = (predict - desired) * (predict - desired);
		nCount++;
		dblLinRegErr +=se;
	} // end loop over  the training file
	dblLinRegErr = sqrt(dblLinRegErr / nCount); 
	fprintf(fpProtocol, "Root Mean Square Linear Regression Training Error is %f \n", dblLinRegErr);
	fprintf(fpProtocol, "The number of data points used in determining linear regression error is %d \n",nCount);
	fflush(fpProtocol);
	pALN->Destroy();
	free(adblX);
	// We are finished with that ALN and have destroyed it
	fprintf(fpProtocol,"Linear regression complete\n");
	fflush(fpProtocol);
}

void ALNAPI createNoiseVarianceFile() // routine
	// This routine splits creates a tessellation Tess of the domain points of TRfile.
	// This is probably not a Delaunay tessellation, but good enough for our purpose.
	// Using it we create noise variance samples in VARfile.
	// The domain points of TRfile are turned into a convex-down set in one-higher dimension
	// by replacing the sample values with the squares of the domain components.
	// When leaf nodes split, they turn into MAX nodes only. They should be overdetermined before splitting
	// i.e. have more than nDim training points, except that the points at the corners of the simplices
	// of the tessellation are shared among up to nDim other simplices, so the total responsibility of
	// a simplex is about 1. We don't know how to  make sure there are exactly nDim different samples on each piece
	// being points of a Delaunay tessellation. The tessellation, instead, functions to break up the domain
	// into pieces whose points are close to each other. This is good for sampling.
{
	fprintf(fpProtocol,"\n*** Tessellation creation for use with noise variance estimation begins***\n");
	fflush(fpProtocol);
	// Allocate space for samples
	adblX = (double *)malloc((nDim) * sizeof(double));
	CMyAln * pTess = NULL; 
	pTess = new CMyAln;
	if (!(pTess->Create(nDim, nDim - 1))) //create a tessellation ALN
	{
		fprintf(fpProtocol, "ALN creation for tessellation failed!\n");
		fflush(fpProtocol);
		exit(0);
	}
	if (!pTess->SetGrowable(pTess->GetTree()))
	{
		fprintf(fpProtocol,"Setting tessellation ALN for growable failed!\n");
		fflush(fpProtocol);
		exit(0);
	}
	// Set constraints on variables for the ALN
	for( int m = 0; m < nDim-1; m++) // NB Excludes the output variable of the ALN, index nDim -1.
	{
		pTess->SetEpsilon(adblEpsilon[m],m);
		pTess->SetMin(adblMinVar[m] - 0.1 * adblStdevVar[m],m); // the minimum value of the domain is a bit smaller than the min of the data points in TVfile
		pTess->SetMax(adblMaxVar[m] + 0.1 * adblStdevVar[m],m); // the max is a bit larger
		pTess->SetWeightMin(- pow(2.0,0.5) * adblStdevVar[nDim - 1]/adblEpsilon[m],m); // The range of output (for a uniform dist.) divided by...
		pTess->SetWeightMax(  pow(2.0,0.5) * adblStdevVar[nDim - 1]/adblEpsilon[m],m); // ...the likely distance between samples in axis m.
		// Impose the a priori bounds on weights.
		if(dblMinWeight[m] > pTess->GetWeightMin(m))
		{
			pTess->SetWeightMin(dblMinWeight[m],m);
		}
		if(dblMaxWeight[m] < pTess->GetWeightMax(m))
		{
			pTess->SetWeightMax(dblMaxWeight[m],m);
		}
	}
	fprintf(fpProtocol, "\nStart new tessellation **************************************\n");
	fflush(fpProtocol);
	ALNNODE* pActiveLFN;
	pActiveLFN = pTess->GetTree(); // Initially the ALN is a leaf node.
	// Start training using the weights and centroid from linear regression to save time.
	for(int m = 0; m < nDim -1; m++)
	{
		((pActiveLFN)->DATA.LFN.adblC)[m] = adblLRC[m]; // LR refers to Linear Regression.
		((pActiveLFN)->DATA.LFN.adblW)[m+1] = adblLRW[m+1];
		//fprintf(fpProtocol,"Weight on %s is %f centroid is %f\n", varname[nInputCol[m]] , adblLRW[m+1] ,adblLRC[m] );
	}
	((pActiveLFN)->DATA.LFN.adblC)[nDim - 1] = adblLRC[nDim - 1];
	((pActiveLFN)->DATA.LFN.adblW)[0] = adblLRW[0];
	((pActiveLFN)->DATA.LFN.adblW)[nDim] = -1.0;
	//fprintf(fpProtocol,"Weight 0 is %f centroid %d is %f\n", adblLRW[0], nDim -1, adblLRC[nDim - 1] );

	nNumberLFNs = 1;  // Starts at 1 and grows as pieces approach the convex hull of sample points in our paraboloid.
	// Training parameters
	bALNgrowable = TRUE; // Used in rest of program
	bStopTraining = FALSE; // This becomes TRUE and stops training when all leaf nodes fit well enough.
	bTessellate = TRUE; // This creates a tesselation of the TVfile domain points.
	bTrainingAverage = FALSE; // Switch to tell fillvector whether get a training vector or compute an average
	bJitter = FALSE;
	if (bJitter)
	{
		fprintf(fpProtocol, "Jitter is used during noise variance estimation\n");
	}
	else
	{
		fprintf(fpProtocol, "Jitter is not used during noise variance estimation\n");
	}
	fflush(fpProtocol);
	nEpochSize = nRowsTR; // splitting occurs after nEpochsBeforeSplit epochs, see alntrain.cpp -- OnEpochEnd.
	(pTess->GetRegion(0))->dblSmoothEpsilon = 0.001; // A bit of smoothing so pieces share points (maybe?)
	  // is better than none here. The paraboloid is , 1.0 deep so smoothing is 1/10000 of this. 
	dblMinRMSE = 0.0; // We don't stop overtraining upon reaching even a very low training error.
	dblLearnRate = 0.15; // This rate needs experimentation.
	// if dblFlimit is <= 1.0, then its *square* is compared to training MSE to decide on splitting.
	dblFlimit = 0.001; // This allows splitting of eligible pieces until the error is very small
	// Rationale: the ALNs fitting the convex surface need to fit well enough to partition the
	// data points but must stop unlimited splitting when they fit nDim points (shared).
	// This amounts to just one sample per piece of the tessellation.
	nNumberEpochs = 40; // TO DO: We have to do tests to see what is sufficient
	fprintf(fpProtocol, "Tess learning rate %f, Smoothing %f, split if piece MSE > %f\n", dblLearnRate,
		(pTess->GetRegion(0))->dblSmoothEpsilon, dblFlimit*dblFlimit);
	fflush(fpProtocol);
	createTR_VARfiles(TESSELLATION);
	nRowsTR = TRfile.RowCount();
	int nColumns = TRfile.ColumnCount();
	const double* adblData = TRfile.GetDataPtr();
	pTess->SetDataInfo(nRowsTR, nColumns, adblData, NULL);
	// The reason for iterations is so that we can monitor progress in the ...TrainProtocol.txt file,
	// and set new parameters for future training.
	for(int iteration = 0; iteration < 15; iteration++)  // experimentation required!
	{
		// Train a tessellation to divide up the domain of all samples by activity of the linear pieces.
		// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
		if(!pTess->Train(nNumberEpochs, dblMinRMSE, dblLearnRate, FALSE, nNotifyMask))
		{
			fprintf(fpProtocol,"Tessellation failed!\n");
			fflush(fpProtocol);
			exit(0);
		}
		// We need more than nDim samples in a tessellation LFN for measuring noise variance.
		// But these have to be shared so it amounts to one sample per LFN.
		if (nNumberLFNs  >= nRowsTR) 
		{
			fprintf(fpProtocol, "The tessellation is now fine enough to estimate noise\n");
			fflush(fpProtocol);
			break;
		}
		fflush(fpProtocol);

		if (bStopTraining == TRUE)
		{
			fprintf(fpProtocol, "Iterations stopped at %d because all leaf nodes have stopped changing!\n", iteration);
			bStopTraining = FALSE;
			break;
		}
	}
	fprintf(fpProtocol,"\nCreation of tesselation completed. Training RMSE = %f \n", dblTrainErr);
	fflush(fpProtocol);
	// We now restore the TRfile from the TVfile and add new information to
	// a parallel file that tracks which piece of which tesselation it belongs to.
	apLFN = (ALNNODE**)malloc((nRowsTR) * sizeof(ALNNODE*)); // apLFN declared at file scope
	double dblValue = 0;
	for (long i = 0; i < nRowsTR; i++)
	{
		for (int j = 0; j < nDim; j++)
		{
			adblX[j] = TVfile.GetAt(i, j, 0); // We stored the values of the original TRfile in VARfile.
			TRfile.SetAt(i, j, adblX[j], 0);   // We now restore the TRfile to its original state.
		}
		double dummy = pTess->QuickEval(adblX, &pActiveLFN); // this finds a pointer to the linear piece
		apLFN[i] = pActiveLFN; // assign to the X vector index its linear piece.
	}	// This ends the assignment of vectors X to LFNs of their tessellation.
	// We don't need the tessellation any more.
	pTess->Destroy();
	createSamples();
	free(adblX);
	free(apLFN);
	// We keep the VARfile for the noise level F-tests determining piece-splitting during approximation.
  // In future versions of the program we will create a weight-bounded ALN to learn the noise variance
  //  as an ALN function and store it for the present and future evaluations.
}

void ALNAPI approximate() // routine
{
	fprintf(fpProtocol, "\n**************Approximation with one or more ALNs begins ********\n");
	fflush(fpProtocol);
	int nalns = nALNs;  // The number of ALNs over which we average (for "bagging")
	// createTR_VARfiles(APPROXIMATION);  // prepares for using the whole TVfile and the whole VARfile with noise variance samples for training and stopping
	fprintf(fpProtocol,"Training %d approximation ALNs starts, using F-test to stop splitting\n", nalns);
	fflush(fpProtocol);
	if(bJitter)
	{
		fprintf(fpProtocol, "Jitter is used during approximation\n");
	}
	else
	{
		fprintf(fpProtocol, "Jitter is not used during approximation\n");
	}
	fflush(fpProtocol);
// Explanation of dblFlimit
// dblFlimit = 2.59 says that splitting of a linear piece is prevented when the mean square
// training error of a piece becomes less than 2.59 times the average of the noise variance
// samples on it. This value comes from tables of the F-test for d.o.f. > 7 and probability 90%.
// For 90% with 3 d.o.f the value is 5.39, i.e. with fewer d.o.f. training stops sooner
// and the training error will generally be larger than with a lower F-value.
// We have to IMPROVE the program to use d.o.f. of *each* piece for both training error
// and noise variance. In the present setup, the d.o.f of the two are equal.
	const double adblFconstant[13]{ 9.00, 5.39, 4.11, 3.45, 3.05, 2.78, 2.59, 2.44, 2.32, 1.79, 1.61, 1.51, 1.40 };
	int dofIndex;
	dofIndex = nDim - 2; // the lowest possible nDim is 2 for one ALN input and one ALN output
	if(nDim > 10) dofIndex = 8;
	if(nDim > 20) dofIndex = 9;
	if(nDim > 30) dofIndex = 10;
	if(nDim > 40) dofIndex = 11;
	if(nDim > 60) dofIndex = 12;

	dblFlimit = adblFconstant[dofIndex]; // This can determine splitting for linear pieces
	dblFlimit = 1.4; // Override for stopping splitting, but temporary until we can compute the dof for the pieces
	// from the training samples and noise variance samples.
	// Other values for dblFlimit with other numbers of samples, i.e. degrees of freedom, are:
	// n dblFlimit
	// 2 9.00
	// 3 5.39
	// 4 4.11
	// 5 3.45
	// 6 3.05
	// 7 2.78
	// 8 2.59
	// 9 2.44
	// 10 2.32
	// 20 1.79
	// 30 1.61
	// 40 1.51
	// 60 1.40
	// 120 1.26
	// REQUIRED IMPROVEMENT  We have to take into account the actual numbers of samples of TSfile and VARfile per leaf node during an epoch.
	// As training of the approximant progresses, the dof of pieces decreases and dblFlimit should be appropriate.
	fprintf(fpProtocol, "nDim is %d and the F-limit used for stopping splitting is %f \n", nDim, dblFlimit);
	fflush(fpProtocol);
	// ***************** SET UP THE ARRAY OF POINTERS TO ALNS FOR TRAINING ONLY *************************
	if(bTrain)
	{
		apALN = (CMyAln**) malloc(nALNs * sizeof(CMyAln*));
	}
	fflush(fpProtocol);

	for (int n = 0; n < nalns; n++)
	{
		 apALN[n] = new CMyAln; // NULL initialized ALN
	}
	// Set up the sample buffer.
	double * adblX = (double *)malloc((nDim) * sizeof(double));
	// Train all the ALNs
	for (int n = 0; n < nalns; n++)
	{
		// Set up the ALN, index n
		if (!(apALN[n]->Create(nDim, nDim-1)))
		{
	    fprintf(fpProtocol,"ALN creation failed!\n");
      fflush(fpProtocol);
			exit(0);
		}
		// Now make the tree growable
		if (!apALN[n]->SetGrowable(apALN[n]->GetTree()))		
		{
	    fprintf(fpProtocol,"Setting ALN %d growable failed!\n", n);
      fflush(fpProtocol);
      exit(0);
		}
		bALNgrowable = TRUE; // now the nodes can split
		// Set constraints on variables for ALN index n
		for(int m = 0; m < nDim-1; m++)
		{
			apALN[n]->SetEpsilon(adblEpsilon[m],m);
			// Expand the bounds on the variables by adding 10% of their stdevs.
			// These bounds will be needed for creating a multilevel DTREE and for inversion
			apALN[n]->SetMin(adblMinVar[m] - 0.1 * adblStdevVar[m],m);
			apALN[n]->SetMax(adblMaxVar[m] + 0.1 * adblStdevVar[m],m);
			// Bound weights
		  apALN[n]->SetWeightMin(- pow(2.0,0.5)* adblStdevVar[nDim - 1]/adblEpsilon[m],m);
		  apALN[n]->SetWeightMax( pow(2.0,0.5) * adblStdevVar[nDim - 1]/adblEpsilon[m],m);
      // impose the a priori bounds on weights
      if(dblMinWeight[m] > apALN[n]->GetWeightMin(m))
      {
		    apALN[n]->SetWeightMin(dblMinWeight[m],m);
      }
      if(dblMaxWeight[m] < apALN[n]->GetWeightMax(m))
      {
		    apALN[n]->SetWeightMax(dblMaxWeight[m],m);
      }
		} // end of loop over m

		bTrainingAverage = FALSE;
		bTessellate = FALSE;

    if(bClassify)
    {
     // TO DO
    }
		(apALN[n]->GetRegion(0))->dblSmoothEpsilon = 0.0;
		fprintf(fpProtocol, "The smoothing for training each approximation is %f\n", 0.0); 
		if(bEstimateNoiseVariance)
		{
			// use the weights and centroids from linear regression
			ALNNODE* pActiveLFN;
			pActiveLFN = apALN[n]->GetTree();
			for(int m = 0; m < nDim - 1; m++)
			{
				((pActiveLFN)->DATA.LFN.adblC)[m] = adblLRC[m];
				((pActiveLFN)->DATA.LFN.adblW)[m+1] = adblLRW[m+1];
			}
			((pActiveLFN)->DATA.LFN.adblC)[nDim - 1] = adblLRC[nDim - 1];
			((pActiveLFN)->DATA.LFN.adblW)[0] = adblLRW[0];
			((pActiveLFN)->DATA.LFN.adblW)[nDim]= -1.0;
		}
		nNumberEpochs = 100;
		dblMinRMSE = 0.0;
		dblLearnRate = 0.15;
		bStopTraining = FALSE; // Set TRUE in alntrain.cpp. Set FALSE by any piece needing more training. 
    nNumberLFNs = 1;  // initialize at 1
		// Set up the data
		createTR_VARfiles(APPROXIMATION);
		// Tell the training algorithm the way to access the data using fillvector
		int nSamples = TRfile.RowCount();
		int nColumns = TRfile.ColumnCount();
		const double* adblData = TRfile.GetDataPtr();
		apALN[n]->SetDataInfo(nSamples, nColumns, adblData, NULL);
		fprintf(fpProtocol,"----------  Training approximation ALN %d ------------------\n",n);
		for(int iteration = 0; iteration < 40; iteration++) // is 40 iterations enough?
		{
			fprintf(fpProtocol, "\nStart iteration %d of approximation with ALN %d, learning rate %f\n", iteration, n, dblLearnRate);
			fflush(fpProtocol);

			// TRAIN ALNS WITHOUT OVERTRAINING   vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
			if (!apALN[n]->Train(nNumberEpochs, dblMinRMSE, dblLearnRate, bJitter, nNotifyMask))
			{
			  fprintf(fpProtocol,"Training failed!\n");
        exit(0);
			}
			if(bEstimateNoiseVariance)
      {
				fprintf(fpProtocol,"Training RMSE = %f\n", dblTrainErr);
			}
			fprintf(fpProtocol,"Number of active LFNs = %d. Tree growing\n", nNumberLFNs);
			fflush(fpProtocol);
			if (bStopTraining == TRUE)
			{
				fprintf(fpProtocol, "\nTraining of approximation ALN %d completed at iteration %d \n", n, iteration);
				fprintf(fpProtocol, "This training stopped because all leaf nodes have stopped changing!\n");
				bStopTraining = FALSE;
				fflush(fpProtocol);
				break;
			}
			fflush(fpProtocol);
		} // end of loop of training interations over one ALN
  } // end of the loop for n = ALN index
	free(adblX);
	// we don't destroy the ALNs because they are needed for further work in reporting
}

void ALNAPI outputTrainingResults() // routine
{
	fprintf(fpProtocol, "\n**** Analyzing results on the training/variance set begins ***\n");
	// all the ALNs have been trained, now report results
	int i, j, k, n;
	double desired, average, sum;
	int nalns; // lower case indicates a value on the stack
	nalns = nALNs;
	ALNNODE* pActiveLFN = NULL;
	// test the average of the ALNs against data in the TV set
	double * adblX = (double *)malloc((nDim) * sizeof(double));
	double * adblWAcc = (double *)malloc((nDim) * sizeof(double));
	double * adblAbsWAcc = (double *)malloc((nDim) * sizeof(double));
	for (k = 0; k < nDim; k++)
	{
		adblWAcc[k] = 0; // zero the accumulator for the average weight
		adblAbsWAcc[k] = 0; // zero the accumulator for the average absolute weight
	}
	double se = 0; // square error accumulator

	int	nClassError = 0;  // for classification problems
	for (j = 0; j < nRowsTV; j++)
	{
		sum = 0;

		for (n = 0; n < nalns; n++)
		{
			for (i = 0; i < nDim; i++)
			{
				adblX[i] = TVfile.GetAt(j, i, 0);
			}
			double dblValue = apALN[n]->QuickEval(adblX, &pActiveLFN);
			sum += dblValue;
			for (int k = 0; k < nDim; k++)
			{
				adblWAcc[k] += ((pActiveLFN)->DATA.LFN.adblW)[k + 1]; //the adblW vector has the bias in it
																												// so the components are shifted
				adblAbsWAcc[k] += fabs(((pActiveLFN)->DATA.LFN.adblW)[k + 1]);
			}
		}
		average = sum / (double)nalns; // this is the result of averaging [0,1]-limited ALNs
		desired = TVfile.GetAt(j, nDim - 1, 0); // get the desired result	
		se += (average - desired) * (average - desired);
		if (((desired > 0.5) && (average < 0.5)) || ((desired < 0.5) && (average > 0.5))) nClassError++;
	}
	double rmse = sqrt(se / ((double)nRowsTV - 1.0)); // frees se for use below.
	// get the average weight on all variables k
	for (k = 0; k < nDim; k++)
	{
		adblWAcc[k] /= (nRowsTV * nalns);
		adblAbsWAcc[k] /= (nRowsTV * nalns);
	}
	fprintf(fpProtocol, "Size of datasets PP TV Test %d  %d  %d \n", nRowsPP, nRowsTV, nRowsTS);
	fprintf(fpProtocol, "Root mean square error of the average over %d ALNS is %f \n", nalns, rmse);
	fprintf(fpProtocol, "Warning: the above result is optimistic, see results on the test set below\n");
	fprintf(fpProtocol, "Importance of each input variable:\n");
	fprintf(fpProtocol, "Abs imp = stdev(input var) * average absolute weight / stdev(output var) \n");
	fprintf(fpProtocol, "Abs imp is numerical and indicates ups and downs in output when the given input varies.\n");
	fprintf(fpProtocol, "For example a sawtooth function with six teeth would have importance 12.\n");
	fprintf(fpProtocol, "First we have to compute the standard deviation of the output variable.\n");
	fflush(fpProtocol);
	//compute the average of the output variable in the TVset
	k = nDim - 1;
	desired = 0;
	for (j = 0; j < nRowsTV; j++)
	{
		desired += TVfile.GetAt(j, k, 0);
	}
	desired /= nRowsTV; // now desired holds the average for variable k

	// compute the standard deviation of the output variable in the TVset
	se = 0;
	double temp;
	for (j = 0; j < nRowsTV; j++)
	{
		temp = TVfile.GetAt(j, k, 0);
		se += (temp - desired) * (temp - desired);
	}
	se /= ((double)nRowsTV - 1.0); // sample variance of the output variable
	double stdevOutput = sqrt(se);
	fprintf(fpProtocol, "\nStandard deviation of the output in the TVfile %f\n", stdevOutput);
	if (fabs(stdevOutput) < 1e-10)
	{
		fprintf(fpProtocol, "\nStopping: The standard deviation of the output on the TV set is near 0.\n");
		fclose(fpProtocol);
		exit(0);
	}
	// we compute the variance of each column of TV
	se = 0;
	for (k = 0; k < nDim - 1; k++) // do each variable k
	{
		//compute the average of variable k in TVset
		desired = 0;
		for (j = 0; j < nRowsTV; j++)
		{
			desired += TVfile.GetAt(j, k, 0);
		}
		desired /= nRowsTV; // now desired holds the average for variable k

		// compute the standard deviation of variable k in TVset
		se = 0;
		double temp;
		for (j = 0; j < nRowsTV; j++)
		{
			temp = TVfile.GetAt(j, k, 0);
			se += (temp - desired) * (temp - desired);
		}
		se /= ((double)nRowsTV - 1.0); // sample variance of variable k
		dblImportance[k] = sqrt(se) * adblAbsWAcc[k] / stdevOutput;
		if (nLag[k] == 0)
		{
			fprintf(fpProtocol, "Variable %s: stdev = \t%f; avg.wt = \t%f; abs imp = \t%f\n",
				varname[nInputCol[k]], sqrt(se), adblWAcc[k], dblImportance[k]);
		}
		else
		{
			fprintf(fpProtocol, "Variable %s@lag%d: stdev = \t%f; avg.wt = \t%f; abs imp = \t%f\n",
				varname[nInputCol[k]], nLag[k], sqrt(se), adblWAcc[k], dblImportance[k]);
		}
		// we use the product of the variance of k and the average absolute weight as a measure of importance
	}
	if (bClassify)
	{
		fprintf(fpProtocol, "Number of TV file cases misclassified = %d out of %d\n", nClassError, nRowsTV);
		fprintf(fpProtocol, "Percentage of TV file cases misclassified = %f", 100.0*(double)nClassError / (double)nRowsTV);
	}
	free(adblX);
	free(adblWAcc);
	free(adblAbsWAcc);
	fflush(fpProtocol);
}

void ALNAPI trainAverage() // routine
{
  int nalns;
  nalns = nALNs;
	bTrainingAverage = TRUE;
	// For training the average, we use the TV set to
	// define the region where the data points are located.
	// The values of those points are automatically jittered to cover that region.
	fprintf(fpProtocol,"\n**** Training an ALN by resampling the average of approximations ******\n");
	fprintf(fpProtocol, "The F-limit used for stopping splitting of the average ALN is %f \n", dblFlimit);
	// The noise variance values in VARfile are the previous ones divided by nalns.
	createTR_VARfiles(BAGGING);
	pAvgALN = new CMyAln; // NULL initialized ALN
	if (!(pAvgALN->Create(nDim, nDim-1) &&
				pAvgALN->SetGrowable(pAvgALN->GetTree())))
	{
    fprintf(fpProtocol,"Stopping: Growable average ALN creation failed!\n");
    exit(0);
	}
	bALNgrowable = TRUE;
	// Quickstart: get centroids and weights in neighborhood of good values
	for(int k = 0; k < nDim - 1; k++) // do each variable k except the output
	{
		pAvgALN->SetEpsilon(adblEpsilon[k],k);
    // set the boundaries of the DTREE a bit beyond where the data samples are
		pAvgALN->SetMin(adblMinVar[k] - 0.1 * adblStdevVar[k],k);
		pAvgALN->SetMax(adblMaxVar[k] + 0.1 * adblStdevVar[k],k);
    // first put in the maximum and minimum weights that make sense
    // we expect about 10 samples across the box in dimension k, so
    // we use a factor of 0.1, which could be questioned
		pAvgALN->SetWeightMin(- pow(2.0,0.5) * adblStdevVar[nDim - 1]/adblEpsilon[k],k);
		pAvgALN->SetWeightMax( pow(2.0,0.5) * adblStdevVar[nDim - 1]/adblEpsilon[k],k);
    // now impose the a priori bounds on weights
    if(dblMinWeight[k] > pAvgALN->GetWeightMin(k))
    {
		  pAvgALN->SetWeightMin(dblMinWeight[k],k);
    }
    if(dblMaxWeight[k] < pAvgALN->GetWeightMax(k))
    {
		  pAvgALN->SetWeightMax(dblMaxWeight[k],k);
    }
	}
	fprintf(fpProtocol,"Smoothing epsilon same as for approximation\n\n");
	fflush(fpProtocol);
	// Tell the training algorithm the way to access the data using fillvector
	nEpochSize = nRowsTR;
	pAvgALN->SetDataInfo(nEpochSize,nDim ,NULL,NULL);
	dblMinRMSE = 0; // Stopping splitting uses the F-test
	dblLearnRate = 0.2;
	//*********
	if(bEstimateNoiseVariance)
	{
		nNumberEpochs = 10;
		// TRAIN AVERAGE ALN vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
		if(!pAvgALN->Train(nNumberEpochs, dblMinRMSE, dblLearnRate, FALSE, nNotifyMask)) // fast change to start, dblLearnRate 1.0 for 2 epochs
		{
			 fprintf(fpProtocol,"Training failed!\n");
		}
	}
	else // we have opened a .fit file
	{
		// use the weights and centroids from linear regression to start
		ALNNODE* pActiveLFN;
		pActiveLFN = pAvgALN->GetTree();
		for(int m = 0; m < nDim - 1; m++)
		{
			((pActiveLFN)->DATA.LFN.adblC)[m] = adblLRC[m];
			((pActiveLFN)->DATA.LFN.adblW)[m+1] = adblLRW[m+1];
		}
		((pActiveLFN)->DATA.LFN.adblC)[nDim - 1] = adblLRC[nDim - 1];
		((pActiveLFN)->DATA.LFN.adblW)[0] = adblLRW[0];
		((pActiveLFN)->DATA.LFN.adblW)[nDim] = -1.0;
	}
	//**********	// at this point we have no further use for the starting values from linear regression
	if(bEstimateNoiseVariance)
	{
		delete [] adblLRC; // delete these from free store
		delete [] adblLRW;
		adblLRC = NULL; // set the pointers to NULL
		adblLRW = NULL;
	}
	dblLearnRate = 0.2;
	nEpochSize = nRowsTV; //training average ALN
  nNumberLFNs = 1;  // initialize at 1
  for(int iteration = 0; iteration < 20; iteration++) // is 20 iterations enough?
	{
	  // Call the training function
		bStopTraining = FALSE;
	  if (!pAvgALN->Train(nNumberEpochs, dblMinRMSE, dblLearnRate, FALSE, nNotifyMask)) // average
	  {
		  fprintf(fpProtocol,"Average ALN training failed!\n");
      fflush(fpProtocol);
      exit(0);
	  }
		if (bStopTraining == TRUE)
		{
			fprintf(fpProtocol, "This training stopped because all leaf nodes have stopped changing!\n");
			bStopTraining = FALSE;
			fprintf(fpProtocol, "\nTraining of the average ALN completed at iteration %d \n", iteration);
			fflush(fpProtocol);
			break;
		}
		fprintf(fpProtocol,"\nIteration %d of training average ALN, RMSE = %f\n", iteration, dblTrainErr);
		fflush(fpProtocol);
		/* this may not be useful
		// we don't need to worry about overtraining because there is no noise
		if((double) nNumberLFNs < (double)nOldNumberLFNs * 0.8)
		{
			fprintf(fpProtocol,"Stopping training average ALN, number %d of active LFNs has shrunk too much\n", nNumberLFNs);
			fflush(fpProtocol);
			break;  // if active LFN growth has almost completely stopped, stop iterating
		}
		nOldNumberLFNs = nNumberLFNs; */
	}
	/*
	// polish the average ALN
  // keep the final value of nNumberLFNs and hence the same nEpochSize
	dblLearnRate = 0.05;
	if (!pAvgALN->Train(nNumberEpochs, dblMinRMSE, dblLearnRate,FALSE, nNotifyMask)) // polish average
	{
		fprintf(fpProtocol,"Polishing average ALN training failed!\n");
    fflush(fpProtocol);
    exit(0);
	}	
	else
	{
		fprintf(fpProtocol,"Polishing average ALN (ie at learning rate 0.05): RMSE = %f\n", dblTrainErr);
	}
	// now we have an average ALN
	*/
	fflush(fpProtocol);
}

void ALNAPI constructDTREE(int nMaxDepth) // routine
{
	// ******************  CONSTRUCT A DTREE FOR THE AVERAGE ALN *******************************
  fprintf(fpProtocol,"\n***** Constructing an ALN decision tree from the average ALN *****\n");
	DTREE* pAvgDTR;

 // create a single-layer average DTREE directly with ConvertDtree and without splitting
	pAvgDTR = pAvgALN->ConvertDtree(nMaxDepth);
  if(pAvgDTR == NULL)
	{
		fprintf(fpProtocol,"No DTREE was generated from the average ALN. Stopping. \n");
    exit(0);
	}
	else
	{
		WriteDtree(szDTREEFileName,pAvgDTR);
		fprintf(fpProtocol,"The DTREE of the average of ALNs %s  was written.\n",szDTREEFileName );
	  fflush(fpProtocol);
  }
}




void ALNAPI cleanup() // routine
{
  if(bTrain)
  {
		// cleanup just what was allocated for training
		for (int n = 0; n < nALNs; n++)
		{
			apALN[n]->Destroy();
		}
		free(apALN);
		pAvgALN->Destroy();
    // the TV file is not created for evaluation
		TVfile.Destroy(); 
    TRfile.Destroy();
    TSfile.Destroy();
    VARfile.Destroy();
    free(adblEpsilon);
	}
  
  // clean up the rest
	PreprocessedDataFile.Destroy();                                       
	free(adblMinVar);
	free(adblMaxVar);
	free(adblStdevVar);
  fclose(fpOutput);
  fclose(fpProtocol);
}

// file task.cpp

void fillvector(double * adblX, CMyAln* paln) // routine
// This is called by the callback in CMyAln to fill in a data vector for training.
// If adblData is set by TRfile.GetDataPtr() and used in paln.SetDataInfo(nPoints,nCols,adblData)
// then the callback does not need to call this routine to get a training vector.
// It uses FillInputVector(...) instead, a routine that can't do the average.
{
	long nRow;
	nRow = (long)floor(ALNRandFloat() * (double) nRowsTR); // This is where the TRfile is indicated for training.
	for(int i = 0; i < nDim; i++)
	{
		adblX[i] = TRfile.GetAt(nRow,i,0); // Notice that TRfile is fixed. Global nRowsTR is not.
		// To get data in real time, you likely have to write new code.
	}
	if(bTrainingAverage) // In this case, we don't use the output component from above. 
	{
		// To average several ALNs, we create new samples around those in TRfile and average the ALN outputs
		// The average ALN is created with negligible smoothing
		const ALNCONSTRAINT* pConstr;
		int nalns = nALNs;
		double dblValue;
		ALNNODE* pActiveLFN;
		for(int i = 0; i < nDim -1; i++) 
		{
			// The idea here is the same as jitter, and gives  much better sampling.
			// The chosen point is triangularly distributed around the initial point
			pConstr = paln->GetConstraint(i, 0);
			ASSERT(pConstr != NULL);
			adblX[i] += (ALNRandFloat() - ALNRandFloat()) * pConstr->dblEpsilon;  // this dblEpsilon is the size of a box "belonging to" a point in the i-th axis
		}
		double sum = 0;
		for (int n = 0; n < nalns; n++)
		{
			dblValue = apALN[n]->QuickEval(adblX, &pActiveLFN);
			sum += dblValue;
		}
		// put the bagging result into the training vector
		adblX[nDim-1] = sum / (double) nalns; // this is the result of averaging ALNs for vector j
	}
}


void ALNAPI createTR_VARfiles(int nChoose) // routine
{
	// The TVfile is all of the PreprocessedDataFile which is not used for testing.
	// This routine is used to set up TRfile and VARfile in various ways.
	// 
	// nChoose = 0: LinearRegression. The TVfile  is copied, in the same order,
	// into both TRfile and TVfile. It is done only once prior to linear regression.
	// 
	// nChoose = 1: Create Noise Variance Samples does a tesselation of the domain
	// points of the samples in TRfile. The nDim-1 components in the TRfile samples are
	// replaced by the L2 distance from the global centroid found by linear regression. This
	// creates the dataset for a convex-down surface which the ALN can easily
	// train on by constraining all non-leaf nodes to be maximum nodes. 
	// This is similar to the idea of finding the convex hull to generate a
	// Delaunay tessellation. However, in our case, the groups of samples on the LFNs
	// are used to do a generalized cross-validation for the noise. We can't do a Delaunay
	// tessellation because we can't control well enough how many points are on each LFN
	// and their sharing between LFNs. (Future ALN research??)
	// After this routine, VARfile contains all of the noise variance samples, used
	// in the F-test to decide whether or not the training error requires splitting a piece.
	//
	// nChoose = 2: Approximation uses the original TVfile copied into TRfile
	// and the noise variance samples in VARfile. For subsequent averaging of several ALNs, the
	// values of the noise variance samples are divided by the number of ALNs averaged.
	// For the averaging, we must use the fillvector routine, which requires setting
	// two of the parameters to NULL as in SetDataInfo(...,..., NULL, NULL)..
	
	double dblValue;
	long i;
	int j;
	ASSERT(nRowsTR == nRowsTV);
	if (nChoose == 0) // LINEAR_REGRESSION
	{
		// First we fill TRfile and VARfile equally with samples of TVfile.
		// We tried randomly sending about half the TVfile to the front
		// of TRfile & VARfile and half to the back. We did this in order to
		// have two tessellations, each on half the samples, but now it is not needed.
		ASSERT((nRowsTV == nRowsTR) && (nRowsTR == nRowsVAR));
		long tmp0 = 0; // Index for the next sample going to the front
		long tmp1 = nRowsTR - 1;  // Index for the next sample going to the back.
		BOOL bSwitch;
		for (i = 0; i < nRowsTV; i++)
		{
			bSwitch = TRUE; // (ALNRandFloat() < 0.5) ? FALSE : TRUE; //  Where does  the i-th
																								// row of TVfile go? It goes ...
			// This is a test for checking noise variance values. Everything goes to the front.
			if (bSwitch)
			{
				for (j = 0; j < nDim; j++) // ... to the front of TRfile and VARfile, or ...
				{
					dblValue = TVfile.GetAt(i, j, 0);
					TRfile.SetAt(tmp0, j, dblValue, 0);
					VARfile.SetAt(tmp0, j, dblValue, 0);
				}
				tmp0++;
			}
			else
			{
				for (j = 0; j < nDim; j++) // ... to the back.
				{
					dblValue = TVfile.GetAt(i, j, 0);
					TRfile.SetAt(tmp1, j, dblValue, 0);
					VARfile.SetAt(tmp1, j, dblValue, 0);
				}
				tmp1--;
			} //end of if (bSwitch)
		} // end of i loop
		ASSERT(tmp1 == tmp0 - 1); // invariant: tmp1-tmp0 + <rows filled> = nRowsTR - 1
		// At this point the TRfile is set up for linear regression (VARfile stores a changed order).
		if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfileLR.txt");
		if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileLR.txt");
	}	// end if(nChoose == 0) LINEAR_REGRESSION

	if (nChoose == 1) // TESSELLATION
	{
		ASSERT((nRowsTV == nRowsTR) && (nRowsTR == nRowsVAR));
		double sumSq;
		double temp;
		// We want a scale factor so our paraboloid has about depth 1.0 all the time
		dblFlatten = 0;
		for (j = 0; j < nDim - 1; j++)
		{
			temp = fabs(adblMaxVar[j] - adblMinVar[j])/2; // max distance from the middle of the data
			temp = pow(temp,2); // The likely maximal square of the j- component
			dblFlatten += temp;
		}
		// dblFlatten is turned into a rough upper bound on the depth of the paraboloid
		dblFlatten = 1.0/dblFlatten;
		// This also allows us to use dblFlimit <= 1.0 to control splitting for the paraboloid fit.
		for (i = 0; i < nRowsTR; i++)
		{
			// We change *all* values in TRfile to values on a paraboloid.
			sumSq = 0;
			for (j = 0; j < nDim - 1; j++) // we add up the squares of the domain components
			{
				temp = TRfile.GetAt(i, j, 0)- adblLRC[j]; // Use the centroid of the data from LR

				sumSq += temp * temp;
			}
			sumSq *= dblFlatten;  // This controls the paraboloid depth. The RMS error will
			// give a better idea of how the tessellation is fitting the paraboloid.
			TRfile.SetAt(i, nDim - 1, sumSq, 0); // the output value in TRfile defines the paraboloid.
			// We are going to train to fit the convex-down paraboloid! \_/
		}
		if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfileTESS.txt");
		if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileTESS.txt");

	} // This ends if(nChoose == 1) TESSELLATION. trainops.cpp will now create noise variance samples.

	if (nChoose == 2) // APPROXIMATION
	{
		ASSERT((nRowsTV == nRowsTR) && (nRowsTR == nRowsVAR));
		// We copy the original TVfile, without reordering, into TRfile.
		// The VARfile is processed separately to get samples on the same piece as the training point.
		for (i = 0; i < nRowsTV; i++)
		{
			for (j = 0; j < nDim; j++)
			{
				dblValue = TVfile.GetAt(i, j, 0);
				TRfile.SetAt(i, j, dblValue, 0);
			} // end of j loop
		} // end of i loop
		if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfileAP.txt");
		if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileAP.txt");
	} // This ends if (nChoose == 2) APPROXIMATION

	if (nChoose == 3) // BAGGING
	{
		ASSERT((nRowsTV == nRowsTR) && (nRowsTR == nRowsVAR));
		for (i = 0; i < nRowsVAR; i++)
		{
			dblValue = VARfile.GetAt(i, nDim - 1, 0);
			VARfile.SetAt(i, nDim - 1, dblValue/nALNs, 0);
		} // end of i loop
		if (bPrint && bDiagnostics) TRfile.Write("DiagnoseTRfileAP.txt");
		if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileAP.txt");
	} // This ends if (nChoose == 2) BAGGING
} // This ends createTR_VARfiles


void createSamples()  // routine
{
	// We have the TRfile used to create a tesselation, the VARfile which is equal to the original TRfile
	// and the array apLFN of pointers to LFNs corresponding to X's in those two files.
	long i; // loop indices
	int nX, nRow; // nX will indicate the current size of various Eigen dynamic matrices using nRow
	long nGoodSamples = 0; // This many samples are on pieces where the noise can be estimated
	long nBadSamples = 0;  // This many are on pieces with too few samples
	long nGoodLFNs = 0;
	long nBadLFNs = 0;
	ALNNODE * pRemember; // This holds a pointer to the leaf node being processed
	nRowsTR = TRfile.RowCount(); // Recall the TRfile has values of the paraboloid, not the data.
	nRowsVAR = VARfile.RowCount();
	ASSERT((nRowsTR == nRowsVAR) && (nRowsVAR == nRowsTV));
	// apLFN[i] is the active LFN for row i of the TRfile.
	// Find the current first non-NULL pointer to an LFN in the array apLFN
	for (i = 0; i < nRowsVAR; i++)
	{
		// We are looking for the next non-NULL pointer to an LFN
		if (apLFN[i] != NULL)
		{
			pRemember = apLFN[i]; // Remember value of that pointer to an LFN
			nX = 0; // We'll first count all the samples on that LFN
			for (int ii = i; ii < nRowsVAR; ii++)// Count all the pointers equal to pRemember
			{
				if (apLFN[ii] == pRemember)
				{
					nX++;
				}
			}
			double dblnX = (float)nX; // We need this constant for the LFN, explained below
			if (nX > nDim + 1) // The bare minimum to get stats.
			{
				// There are nX samples, enough to estimate noise.
				// Now we know the size of matrices we need
				MatrixXd Xmat(nX, nDim); // domain points of samples on the LFN with constant 1.0 column at right
				VectorXd yvec(nX); // values of those samples
				MatrixXd Hmat(nX, nX); // the matrix X times inverse(X'X) times X'
				MatrixXd Temp(nDim, nDim);
				MatrixXd Temp1(nDim, nDim);
				VectorXd yhatvec(nX); // The linear fit based on all nX samples
				double dblVarCorrect = (nDim + 1.0) / (nDim + 3.0); // to get correct variance
				nRow = 0; // We don't want to disturb nX
				for (int ii = i; ii < nRowsVAR; ii++)// Now process all the pointers equal to pRemember
				{
					if (apLFN[ii] == pRemember)
					{
						// Put the domain components of the corresponding sample into Xmat
						for (int j = 0; j < nDim - 1; j++)
						{
							Xmat(nRow, j) = VARfile.GetAt(ii, j, 0); // Get the components at row ii of VARfile
						}
						// Put a 1.0 in the nDim - 1 position
						Xmat(nRow, nDim - 1) = 1.0;
						// put the sample value into yvec
						yvec(nRow) = VARfile.GetAt(ii, nDim - 1, 0);
						nRow++;
					}
				} // look at the next ii for this pRemember
				ASSERT(nRow == nX);
				// We have found all the nX samples on the current piece.
				// Do matrix operations to compute the value of a noise variance sample
				Temp = Xmat.transpose()*Xmat;
				Temp1 = Temp.inverse();
				Hmat = Xmat * Temp1 * Xmat.transpose();
				yhatvec = Hmat * yvec;
				// Create a new noise variance sample in VARfile
				// at every X on the piece pointed at by pRemember.
				nRow = 0;
				for (int ii = i; ii < nRowsVAR; ii++)
				{
					if (apLFN[ii] == pRemember)
					{
						ASSERT(nRow < nX);
						double dblNoiseSample;
						// There are two choices for the next two lines. Leave-one-out and Breiman simplification
						//dblNoiseSample = pow((yhatvec(nRow) - yvec(nRow)) / (1.0 - Hmat(nRow, nRow)), 2)*dblVarCorrect;
						dblNoiseSample = pow((yhatvec(nRow) - yvec(nRow)) / (1.0 - (float)nDim/(float)nX), 2)*dblVarCorrect;
						VARfile.SetAt(ii, nDim - 1, dblNoiseSample, 0); 
						// The correction factor calculation: The variance of yvec is sigma squared. The *average* variance
						//  over a simplex with nDim sample values at corners is 2 sigma squared divided by nDim + 1.
						// The variance of the difference of yhat and y is the sum (nDim + 3)/(nDim +3).
						// Actually yhat is based on nX - 1 points (when one is "left out").
						// So the variance samples have to be multiplied by (nX + 1)/(nX + 3)
						// to achieve a sample with variance sigma squared.
						apLFN[ii] = NULL; // Set the values of all pRemember pointers to NULL
						nRow++;
					}
				}
				ASSERT(nRow == nX);
				nGoodSamples += nX;
				nGoodLFNs++;
			} // end when there are enough samples; automatic deallocation of matrices and vectors
			else
			{
				// These samples are too few to analyze with a linear fit, so just use the sample variance.
				nRow = 0;
				double dblVal,dblSum, dblSq;
				dblVal = dblSum = dblSq = 0.0;
				for (int ii = i; ii < nRowsVAR; ii++)
				{
					if (apLFN[ii] == pRemember)
					{
						dblVal = VARfile.GetAt(ii, nDim - 1, 0);
						dblSum += dblVal;
						dblSq += dblVal * dblVal;
						nRow++;
					}
				}
				ASSERT(nRow == nX);
				// If there is just one sample on a linear piece, we can't assign a noise variance to the X
				dblVal = (nX > 1)?(dblSq - dblSum * dblSum / dblnX) / (dblnX - 1.0): 0.0;
				for (int ii = i; ii < nRowsVAR; ii++)
				{
					if (apLFN[ii] == pRemember)
					{
						apLFN[ii] = NULL; // Set the values of all pRemember pointers to NULL
						VARfile.SetAt(ii, nDim - 1, dblVal, 0); // We just use the mean if there are too few points
					}
				}
				ASSERT(nRow == nX);
				// Increment the counter of samples which are too few on the piece to estimate the noise
				nBadSamples += nX;
				nBadLFNs++;
			}
			// Finished the processing of pointer pRemember apLFN[i]
		} // end if apLFN[i] != NULL; keep looking for a non-NULL pointer beyond i
	} // end for (i = 0; i < nRowsTR; i++)
	// Finished all the clusters of X's and assigned noise variance samples to ones with enough samples
	// Now check to see the global noise variance (You can comment out what follows if it's proven OK)
	// Check a case of known constant noise variance!
	fprintf(fpProtocol, "Good samples %d, bad samples %d, total samples %d\n",
		nGoodSamples, nBadSamples, nRowsVAR);
	fprintf(fpProtocol, "Good LFNs %d, bad LFNs %d, total LFNs %d\n",nGoodLFNs, nBadLFNs, nGoodLFNs + nBadLFNs);
	double dblValue = 0;
	for (long i = 0; i < nRowsVAR; i++)
	{
		dblValue += VARfile.GetAt(i, nDim - 1, 0);
	}
	fprintf(fpProtocol, "Average of noise variance samples = %f\n", dblValue / nRowsVAR);
	fflush(fpProtocol);
	if (bPrint && bDiagnostics) VARfile.Write("DiagnoseVARfileNV.txt");
	if (bPrint && bDiagnostics) fprintf(fpProtocol, "Diagnose VARfileNV.txt written\n");
}
