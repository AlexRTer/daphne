/*
 * Copyright 2024 The DAPHNE Consortium
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

#pragma once

#include "mlir/ExecutionEngine/CRunnerUtils.h"
#include "runtime/local/context/DaphneContext.h"
#include "runtime/local/datastructures/CSRMatrix.h"

template <typename T>
inline StridedMemRefType<size_t, 1> convertCSRMatrixToRowOffsetsMemRef(const CSRMatrix<T> *input, DCTX(ctx)) {
    StridedMemRefType<size_t, 1> rowOffsetsMemRef{};

    rowOffsetsMemRef.basePtr = input->getRowOffsetsSharedPtr().get();
    rowOffsetsMemRef.data = rowOffsetsMemRef.basePtr;
    rowOffsetsMemRef.offset = 0;
    rowOffsetsMemRef.sizes[0] = input->getNumRows() + 1;
    rowOffsetsMemRef.strides[0] = 1;

    input->increaseRefCounter();

    return rowOffsetsMemRef;
}