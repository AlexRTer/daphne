/*
 * Copyright 2021 The DAPHNE Consortium
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_RUNTIME_LOCAL_KERNELS_SEQ_H
#define SRC_RUNTIME_LOCAL_KERNELS_SEQ_H

#include <runtime/local/datastructures/DataObjectFactory.h>
#include <runtime/local/datastructures/DenseMatrix.h>

#include <stdlib.h>
#include <cmath>
#include <cassert>
#include <iomanip>
#include <stdexcept>

// ****************************************************************************
// Struct for partial template specialization
// ****************************************************************************

template<class DT>
struct Seq{
    static void apply(DT *& res, typename DT::VT start,typename DT::VT end, typename DT::VT inc) = delete;
};

// ****************************************************************************
// Convenience function
// ****************************************************************************

template<class DT>
void seq(DT *& res, typename DT::VT start, typename DT::VT end, typename DT::VT inc) {
    Seq<DT>::apply(res, start, end, inc);
}

// ****************************************************************************
// (Partial) template specializations for different data/value types
// ****************************************************************************

template<typename VT>
struct Seq<DenseMatrix<VT>> {
        static void apply(DenseMatrix<VT> *& res, VT start, VT end, VT inc) {
            assert(inc == inc && "inc cannot be NaN");   
            assert(start == start && "start cannot be NaN");
            assert(end == end && "end cannot be NaN");
            assert(inc != 0 && "inc should not be zero"); // setp 0 can not make any progress to any given boundary
	    if( (start<end && inc<0) || (start>end && inc>0)){// error
		   throw std::runtime_error("The inc cannot lead to the boundary of the sequence"); 
	    }
            
	    VT initialDistanceToEnd= abs(end-start);
            const size_t expectedNumRows= ceil((initialDistanceToEnd/abs(inc)))+1; // number of steps = expectedNumRows and numRows might = expectedNumRows -1 ot expectedNumRows
            const size_t numCols=1;
            // should the kernel do such a check or reallocate res matrix directly?
            if(res == nullptr) 
                res = DataObjectFactory::create<DenseMatrix<VT>>(expectedNumRows, numCols, false);
            else
                assert(res->getNumRows()==expectedNumRows  && "input matrix is not null and may not fit the sequence");

            VT * allValues= res->getValues();

            VT accumulatorValue= start;

            for(size_t i =0; i<expectedNumRows; i++){
              allValues[i]= accumulatorValue;
              accumulatorValue+=inc;
            }

            VT lastValue=allValues[expectedNumRows-1];

            VT eps = 1.0e-13;

            // on my machine the difference is (1.7e-15) greater  than epsilon std::numeric_limits<VT>::epsilon() 
            if ( (end < start) && end-lastValue>eps ) { // reversed sequence
                res->shrinkNumRows(expectedNumRows-1);
            }
            else if ( (end > start) && lastValue-end> eps ){ // normal sequence
                res->shrinkNumRows(expectedNumRows-1);
            }
    }
};

#endif //SRC_RUNTIME_LOCAL_KERNELS_SEQ_H