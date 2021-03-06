/*
 * Author: Konstantin Schauwecker
 * Year:   2012
 */

#ifndef SPARSESTEREO_SPARSESTEREO_INL_H
#define SPARSESTEREO_SPARSESTEREO_INL_H

#include "sparsestereo.h"
#include <boost/scoped_array.hpp>
#include <limits>
#include <algorithm>
#include "exception.h"
#include "stereorectification.h"
#include "simd.h"
 #include <boost/date_time/posix_time/posix_time.hpp>

using namespace boost;
using namespace boost::posix_time;

namespace sparsestereo {
	using namespace std;

	template <class CORRELATION, typename COST_TYPE>
	SparseStereo<CORRELATION, COST_TYPE>::SparseStereo(int maxDisparity,  float yTolerance, float uniqueness, 
		StereoRectification* rect, bool subpixelFeatures, bool storeUnmatched, int leftRightStep)
		: maxDisparity(maxDisparity), yTolerance(yTolerance), uniqueness(uniqueness), rect(rect),
		storeUnmatched(storeUnmatched), leftRightStep(leftRightStep), precompEpilinesStart(0, 0),
		sparseRect(subpixelFeatures, rect) {
	}
	
	template <class CORRELATION, typename COST_TYPE>
	SparseStereo<CORRELATION, COST_TYPE>::~SparseStereo() {
	}
	
	template <class CORRELATION, typename COST_TYPE>
	void SparseStereo<CORRELATION, COST_TYPE>::match(const cv::Mat& left/*#*/, const cv::Mat& right/*#*/,
		const std::vector<cv::KeyPoint>& leftFeat, const std::vector<cv::KeyPoint>& rightFeat,
		std::vector<SparseMatch>* matches, int combination) {
		if(left.size() != right.size() || (rect != NULL && left.size() != rect->getCalibrationResult().imageSize))
			throw Exception("Mismatching image sizes");
		if(leftFeat.size() == 0 || rightFeat.size() == 0)
			return; // Can't match anything
		
		// For the first time, or when the image size changes, compute epiline lookup table
		if(left.cols != precompEpilinesStart.cols || left.rows != precompEpilinesStart.rows)
			sparseRect.precomputeEpilinesStart(left.cols, left.rows, &precompEpilinesStart);
		
		// Rectify feature points
		sparseRect.rectify(leftFeat, rightFeat, &leftFeatures, &rightFeatures);
	
		// Features are now sorted from top left to bottom right
		boost::shared_array<unsigned int> offsets = SIMD::alignedNew<unsigned int>(left.rows);
		int maxRowLen __attribute__((unused)) = getRowOffsets(rightFeatures, offsets.get(), left.rows);
		
		minimumMatches.resize(leftFeatures.size());
		//ptime lastTime = microsec_clock::local_time();

		// Perform matching
		
		calcCosts(left, right, offsets.get(), combination);
		// time_duration elapsed = (microsec_clock::local_time() - lastTime);
		// cout << "Calc cost time 1x stereo matching: " << elapsed.total_microseconds()/1.0e6 << "s" << endl;
		// Perform left/right consistency check
		//ptime lastTime2 = microsec_clock::local_time();
		
		denseConsistencyCheck(left, right, combination);

		//time_duration elapsed2 = (microsec_clock::local_time() - lastTime2);
		//cout << "denseConsistencyCheck time 1x stereo matching: " << elapsed2.total_microseconds()/1.0e6 << "s" << endl;
		// Compose sparse disparity list
		CORRELATION correlation;
		correlation.setReferenceImage(left);
		correlation.setComparisonImage(right);
		for(int i=0; i<(int)leftFeatures.size(); i++) {
			const cv::KeyPoint* rightFeature = NULL;
			cv::Point2f rightRect = leftFeatures[i].rectPoint + cv::Point2f(1, 0); //Disparity -1
			short cost = 0;
			cv::Point2f rightMatchedPoint;

			if(minimumMatches[i].first != -1) {
				rightFeature = rightFeatures[minimumMatches[i].first].imgPoint;
				rightMatchedPoint = rightFeatures[minimumMatches[i].first].rectPoint;
				rightRect = rightFeatures[minimumMatches[i].first].rectPoint;
				cost = minimumMatches[i].second;
			}
			
			if(rightFeature != NULL || storeUnmatched){
				rightRect = subPixelTestFunction( correlation, left,right, combination,leftFeatures[i].rectPoint,rightMatchedPoint,cost) ;
				matches->push_back(SparseMatch(leftFeatures[i].imgPoint, rightFeature, leftFeatures[i].rectPoint, rightRect, cost));
				
				// if(abs(leftFeatures[i].rectPoint.x - rightRect.x )>70 ){
				// 	cout<<"X1 = "<<leftFeatures[i].rectPoint.x<<endl;
				// 	cout<<"X2 = "<<rightRect.x<<endl;

				// 	cout<<"Disp = "<<leftFeatures[i].rectPoint.x - rightRect.x<<endl;
				// }
			}
		}
	}
	/*Try for subPixel accuracy before the dense consistency check*/

	template <class CORRELATION, typename COST_TYPE>
	cv::Point2f SparseStereo<CORRELATION, COST_TYPE>:: subPixelTestFunction(CORRELATION correlation,const cv::Mat& left, const cv::Mat& right,int combination, cv::Point2f referencePoint,cv::Point2f rightFeature, float cost) 
	{
				correlation.setReferencePoint(cv::Point2i(int(referencePoint.x + 0.5),int(referencePoint.y + 0.5)));
				COST_TYPE subCost = correlation.match(cv::Point2i(int(rightFeature.x + 0.5), int(rightFeature.y + 0.5)) ,combination);
				COST_TYPE subCost1 = correlation.match(cv::Point2i(int(rightFeature.x + 0.5-1), int(rightFeature.y + 0.5)) ,combination);
				COST_TYPE subCost2 = correlation.match(cv::Point2i(int(rightFeature.x + 0.5+1), int(rightFeature.y + 0.5)) ,combination);

				float x=0,denominator = (4*subCost - 2*(subCost2 + subCost1));
				if(denominator != 0 )
					x = (subCost2 - subCost1)/denominator ; 
				
				return(Point2f(x+int(rightFeature.x + 0.5), int(rightFeature.y + 0.5)  ));



	}
	template <class CORRELATION, typename COST_TYPE>
	void SparseStereo<CORRELATION, COST_TYPE>::calcCosts(const cv::Mat& left , const cv::Mat& right, unsigned int* rowOffsets, int combination) {
		
		int lastRow = -1e9; //Invalid value
		CORRELATION correlation;
		correlation.setReferenceImage(left);
		correlation.setComparisonImage(right);
		ptime lastTime = microsec_clock::local_time();
		
		for(int l=0; l<(int)leftFeatures.size(); l++) {
			// Find row start and end points
			int ly = (int)(leftFeatures[l].rectPoint.y + 0.5);
			int rightStart = rowOffsets[min(left.rows-1, max(0, int(ly - yTolerance)))];
			int rightEnd = rowOffsets[min(left.rows-1, max(0, int(ly + yTolerance) + 2))];
		
			COST_TYPE minCost = numeric_limits<COST_TYPE>::max();
			int minRightFeature = -1;

			correlation.setReferencePoint(cv::Point2i(int(leftFeatures[l].imgPoint->pt.x + 0.5),
			int(leftFeatures[l].imgPoint->pt.y + 0.5)));
			for(int r=rightStart; r<rightEnd; r++) {
				// First test if this would be a valid match
				if(	rightFeatures[r].rectPoint.x <= leftFeatures[l].rectPoint.x &&
					rightFeatures[r].rectPoint.x >= leftFeatures[l].rectPoint.x - maxDisparity &&
					fabs(leftFeatures[l].rectPoint.y - rightFeatures[r].rectPoint.y) <= yTolerance )
				{
					// It is! Let's compute a cost
					COST_TYPE currentCost = correlation.match(
					cv::Point2i(int(rightFeatures[r].imgPoint->pt.x + 0.5), int(rightFeatures[r].imgPoint->pt.y + 0.5)), combination);
					if(currentCost < minCost) 
					{ // Only store smaller costs
						minCost = currentCost;
						minRightFeature = r;
					}
				}
			}
			//cout<<"SparseStereo-inl.h Line 147"<<endl;
			minimumMatches[l] = std::pair<int, COST_TYPE>(minRightFeature, minCost);
		}
		
	}


	
	template <class CORRELATION, typename COST_TYPE>
	int SparseStereo<CORRELATION, COST_TYPE>::getRowOffsets(const std::vector<SparseRectification::RectifiedFeature>& features,
		unsigned int* offsets, int maxRows) {
		int lastRow=-1, lastOffset = 0, longestRowLen = -1;
		
		for(unsigned int i=0; i<features.size(); i++) {
			int y = (int)(features[i].rectPoint.y + 0.5);
			if(y >= maxRows -1)
				break; // The rectified point is outside the visible image
			else if(y<0)
				continue;
				
			if(y != lastRow) {
				// Save offset
				for(int j=lastRow+1; j<=y; j++)
					offsets[j] = i;
					
				if((int)i - lastOffset > longestRowLen)
					longestRowLen = i - lastOffset;
					
				lastRow = y;
				lastOffset = i;
			}
		}
		
		// Save offset for the remaining items
		for(int i=lastRow+1; i < maxRows; i++)
			offsets[i] = features.size();
		if((int)features.size() - lastOffset > longestRowLen)
			longestRowLen = features.size() - lastOffset;
			
		return longestRowLen; //Return the maximum row length
	}

	template <class CORRELATION, typename COST_TYPE>
	void SparseStereo<CORRELATION, COST_TYPE>::denseConsistencyCheck(const cv::Mat& left, const cv::Mat& right, int combination) {			
		int lastRow = -1e9; //Invalid value
		CORRELATION correlation;
		correlation.setReferenceImage(right);
		correlation.setComparisonImage(left);
		
		for(int l = 0; l < (int)leftFeatures.size(); l++) {
			int ly = (int)(leftFeatures[l].rectPoint.y + 0.5);
			if(ly != lastRow) {		
				// Skip top and bottom
				if(ly < correlation.getWindowSize()/2)
					continue;
				else if(ly >= left.rows - correlation.getWindowSize()/2 -1)
					break;
								
				lastRow = ly;
			}
			
			// Get the minimum match and cost
			int r = minimumMatches[l].first;
			COST_TYPE minCost = minimumMatches[l].second;
			
			if(r == -1)
				continue;
			
			// Get epiline start and end
			float leftStartX = precompEpilinesStart(leftFeatures[l].imgPoint->pt.y, max(0, min(
				precompEpilinesStart.cols, (int)(rightFeatures[r].rectPoint.x+0.5))));
			int startX = max(correlation.getWindowSize()/2, int(leftStartX /*+ 0.5*/));
			int endX = min(precompEpilinesStart.cols - correlation.getWindowSize()/2,
				int(leftStartX + maxDisparity /*+ 0.5*/));
			Epiline epiline = rect!=NULL ? rect->getLeftEpiline(leftFeatures[l].imgPoint->pt) : Epiline(leftFeatures[l].imgPoint->pt.y);
			if(!epiline.isValid())
				continue;
			
			// Preform consistency check
			correlation.setReferencePoint(cv::Point2i(int(rightFeatures[r].imgPoint->pt.x + 0.5), int(rightFeatures[r].imgPoint->pt.y + 0.5)));
			for(int x = startX; x < endX; x+= leftRightStep) {
				int y = (int)(epiline.at(x) + 0.5);
				if(y < correlation.getWindowSize()/2 || y >= left.rows - correlation.getWindowSize()/2)
					continue;
				
				COST_TYPE currentCost = correlation.match(cv::Point2i(x, y),combination);
				
				if(currentCost < minCost/uniqueness && fabs(x - int(leftFeatures[l].imgPoint->pt.x+0.5)) > leftRightStep) {
					minimumMatches[l].first = -1;
					break;
				}
			}
		}
	}	
}

#endif
