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

#include "runtime/local/datastructures/IAllocationDescriptor.h"
#ifdef USE_MPI
    #include "runtime/distributed/worker/MPIWorker.h"
#endif
#include <api/cli/StatusCode.h>
#include <api/internal/daphne_internal.h>
#include <api/cli/DaphneUserConfig.h>
#include <api/daphnelib/DaphneLibResult.h>
#include <parser/daphnedsl/DaphneDSLParser.h>
#include "compiler/execution/DaphneIrExecutor.h"
#include <runtime/local/vectorized/LoadPartitioning.h>
#include <parser/config/ConfigParser.h>
#include <util/DaphneLogger.h>

#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/Support/CommandLine.h"

#ifdef USE_CUDA
    #include <runtime/local/kernels/CUDA/HostUtils.h>
#endif

#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>

#include <cstdlib>

[[maybe_unused]] std::unique_ptr<DaphneLogger> logger;

using namespace std;
using namespace mlir;
using namespace llvm::cl;

void parseScriptArgs(const llvm::cl::list<string>& scriptArgsCli, unordered_map<string, string>& scriptArgsFinal) {
    for(const std::string& pair : scriptArgsCli) {
        size_t pos = pair.find('=');
        if(pos == string::npos)
            throw std::runtime_error("script arguments must be specified as name=value, but found '" + pair + "'");
        const string argName = pair.substr(0, pos);
        const string argValue = pair.substr(pos + 1, pair.size());
        if(scriptArgsFinal.count(argName))
            throw runtime_error("script argument: '" + argName + "' was provided more than once");
        scriptArgsFinal.emplace(argName, argValue);
    }
}
void printVersion(llvm::raw_ostream& os) {
    // TODO Include some of the important build flags into the version string.
    os
      << "DAPHNE Version 0.1\n"
      << "An Open and Extensible System Infrastructure for Integrated Data Analysis Pipelines\n"
      << "https://github.com/daphne-eu/daphne\n";
}
    
int startDAPHNE(int argc, const char** argv, DaphneLibResult* daphneLibRes, int *id){
    // ************************************************************************
    // Parse command line arguments
    // ************************************************************************
    
    // ------------------------------------------------------------------------
    // Define options
    // ------------------------------------------------------------------------

    // All the variables concerned with the LLVM command line parser (those of
    // type OptionCategory, opt, ...) must be declared static here, because
    // this function may run multiple times in the context of DaphneLib (DAPHNE's
    // Python API). Without static, the second invocation of this function
    // crashes because the options set in the first invocation are still present
    // in some global state. This must be due to the way the LLVM command line
    // library handles its internal state.
    
    // Option categories ------------------------------------------------------
    
    // TODO We will probably subdivide the options into multiple groups later.
    static OptionCategory daphneOptions("DAPHNE Options");
    static OptionCategory schedulingOptions("Advanced Scheduling Knobs");
    static OptionCategory distributedBackEndSetupOptions("Distributed Backend Knobs");


    // Options ----------------------------------------------------------------

    // Distributed backend Knobs
    static opt<ALLOCATION_TYPE> distributedBackEndSetup("dist_backend", cat(distributedBackEndSetupOptions), 
                                            desc("Choose the options for the distribution backend:"),
                                            values(
                                                    clEnumValN(ALLOCATION_TYPE::DIST_MPI, "MPI", "Use message passing interface for internode data exchange"),
                                                    clEnumValN(ALLOCATION_TYPE::DIST_GRPC, "gRPC", "Use remote procedure call for internode data exchange (default)")
                                                ),
                                            init(ALLOCATION_TYPE::DIST_GRPC)
                                            );

    
    // Scheduling options

    static opt<SelfSchedulingScheme> taskPartitioningScheme("partitioning",
            cat(schedulingOptions), desc("Choose task partitioning scheme:"),
            values(
                clEnumVal(STATIC , "Static (default)"),
                clEnumVal(SS, "Self-scheduling"),
                clEnumVal(GSS, "Guided self-scheduling"),
                clEnumVal(TSS, "Trapezoid self-scheduling"),
                clEnumVal(FAC2, "Factoring self-scheduling"),
                clEnumVal(TFSS, "Trapezoid Factoring self-scheduling"),
                clEnumVal(FISS, "Fixed-increase self-scheduling"),
                clEnumVal(VISS, "Variable-increase self-scheduling"),
                clEnumVal(PLS, "Performance loop-based self-scheduling"),
                clEnumVal(MSTATIC, "Modified version of Static, i.e., instead of n/p, it uses n/(4*p) where n is number of tasks and p is number of threads"),
                clEnumVal(MFSC, "Modified version of fixed size chunk self-scheduling, i.e., MFSC does not require profiling information as FSC"),
                clEnumVal(PSS, "Probabilistic self-scheduling")
            ),
            init(STATIC)
    );
    static opt<QueueTypeOption> queueSetupScheme("queue_layout",
            cat(schedulingOptions), desc("Choose queue setup scheme:"),
            values(
                clEnumVal(CENTRALIZED, "One queue (default)"),
                clEnumVal(PERGROUP, "One queue per CPU group"),
                clEnumVal(PERCPU, "One queue per CPU core")
            ),
            init(CENTRALIZED)
    );
	static opt<VictimSelectionLogic> victimSelection("victim_selection",
            cat(schedulingOptions), desc("Choose work stealing victim selection logic:"),
            values(
                clEnumVal(SEQ, "Steal from next adjacent worker (default)"),
                clEnumVal(SEQPRI, "Steal from next adjacent worker, prioritize same NUMA domain"),
                clEnumVal(RANDOM, "Steal from random worker"),
				clEnumVal(RANDOMPRI, "Steal from random worker, prioritize same NUMA domain")
            ),
            init(SEQ)
    );

    static opt<int> numberOfThreads(
            "num-threads", cat(schedulingOptions),
            desc(
                "Define the number of the CPU threads used by the vectorized execution engine "
                "(default is equal to the number of physical cores on the target node that executes the code)"
            )
    );
    static opt<int> minimumTaskSize(
            "grain-size", cat(schedulingOptions),
            desc(
                "Define the minimum grain size of a task (default is 1)"
            ),
            init(1)
    );
    static opt<bool> useVectorizedPipelines(
            "vec", cat(schedulingOptions),
            desc("Enable vectorized execution engine")
    );
    static opt<bool> useDistributedRuntime(
        "distributed", cat(daphneOptions),
        desc("Enable distributed runtime")
    );
    static opt<bool> prePartitionRows(
            "pre-partition", cat(schedulingOptions),
            desc("Partition rows into the number of queues before applying scheduling technique")
    );
    static opt<bool> pinWorkers(
            "pin-workers", cat(schedulingOptions),
            desc("Pin workers to CPU cores")
    );
    static opt<bool> hyperthreadingEnabled(
            "hyperthreading", cat(schedulingOptions),
            desc("Utilize multiple logical CPUs located on the same physical CPU")
    );
    static opt<bool> debugMultiThreading(
            "debug-mt", cat(schedulingOptions),
            desc("Prints debug information about the Multithreading Wrapper")
    );
    
    // Other options
    
    static opt<bool> noObjRefMgnt(
            "no-obj-ref-mgnt", cat(daphneOptions),
            desc(
                    "Switch off garbage collection by not managing data "
                    "objects' reference counters"
            )
    );
    static opt<bool> noIPAConstPropa(
            "no-ipa-const-propa", cat(daphneOptions),
            desc("Switch off inter-procedural constant propagation")
    );
    static opt<bool> noPhyOpSelection(
            "no-phy-op-selection", cat(daphneOptions),
            desc("Switch off physical operator selection, use default kernels for all operations")
    );
    static opt<bool> selectMatrixRepr(
            "select-matrix-repr", cat(daphneOptions),
            desc(
                    "Automatically choose physical matrix representations "
                    "(e.g., dense/sparse)"
            )
    );
    static alias selectMatrixReprAlias( // to still support the longer old form
            "select-matrix-representations", aliasopt(selectMatrixRepr),
            desc("Alias for --select-matrix-repr")
    );
    static opt<bool> cuda(
            "cuda", cat(daphneOptions),
            desc("Use CUDA")
    );
    static opt<bool> fpgaopencl(
            "fpgaopencl", cat(daphneOptions),
            desc("Use FPGAOPENCL")
    );
    static opt<string> libDir(
            "libdir", cat(daphneOptions),
            desc("The directory containing kernel libraries")
    );

    enum ExplainArgs {
      kernels,
      llvm,
      parsing,
      parsing_simplified,
      property_inference,
      select_matrix_repr,
      sql,
      phy_op_selection,
      type_adaptation,
      vectorized,
      obj_ref_mgnt
    };

    static llvm::cl::list<ExplainArgs> explainArgList(
        "explain", cat(daphneOptions),
        llvm::cl::desc("Show DaphneIR after certain compiler passes (separate "
                       "multiple values by comma, the order is irrelevant)"),
        llvm::cl::values(
            clEnumVal(parsing, "Show DaphneIR after parsing"),
            clEnumVal(parsing_simplified, "Show DaphneIR after parsing and some simplifications"),
            clEnumVal(sql, "Show DaphneIR after SQL parsing"),
            clEnumVal(property_inference, "Show DaphneIR after property inference"),
            clEnumVal(select_matrix_repr, "Show DaphneIR after selecting physical matrix representations"),
            clEnumVal(phy_op_selection, "Show DaphneIR after selecting physical operators"),
            clEnumVal(type_adaptation, "Show DaphneIR after adapting types to available kernels"),
            clEnumVal(vectorized, "Show DaphneIR after vectorization"),
            clEnumVal(obj_ref_mgnt, "Show DaphneIR after managing object references"),
            clEnumVal(kernels, "Show DaphneIR after kernel lowering"),
            clEnumVal(llvm, "Show DaphneIR after llvm lowering")),
        CommaSeparated);

    static llvm::cl::list<string> scriptArgs1(
            "args", cat(daphneOptions),
            desc(
                    "Alternative way of specifying arguments to the DaphneDSL "
                    "script; must be a comma-separated list of name-value-pairs, "
                    "e.g., `--args x=1,y=2.2`"
            ),
            CommaSeparated
    );
    const std::string configFileInitValue = "-";
    static opt<string> configFile(
        "config", cat(daphneOptions),
        desc("A JSON file that contains the DAPHNE configuration"),
        value_desc("filename"),
        llvm::cl::init(configFileInitValue)
    );

    static opt<bool> enableProfiling (
            "enable-profiling", cat(daphneOptions),
            desc("Enable profiling support")
    );

    // Positional arguments ---------------------------------------------------
    
    static opt<string> inputFile(Positional, desc("script"), Required);
    static llvm::cl::list<string> scriptArgs2(ConsumeAfter, desc("[arguments]"));

    // ------------------------------------------------------------------------
    // Parse arguments
    // ------------------------------------------------------------------------
    
    static std::vector<const llvm::cl::OptionCategory *> visibleCategories;
    visibleCategories.push_back(&daphneOptions);
    visibleCategories.push_back(&schedulingOptions);
    visibleCategories.push_back(&distributedBackEndSetupOptions);
    
    HideUnrelatedOptions(visibleCategories);

    extrahelp(
            "\nEXAMPLES:\n\n"
            "  daphne example.daphne\n"
            "  daphne --vec example.daphne x=1 y=2.2 z=\"foo\"\n"
            "  daphne --vec --args x=1,y=2.2,z=\"foo\" example.daphne\n"
            "  daphne --vec --args x=1,y=2.2 example.daphne z=\"foo\"\n"
    );
    SetVersionPrinter(&printVersion);
    ParseCommandLineOptions(
            argc, argv,
            "The DAPHNE Prototype.\n\nThis program compiles and executes a DaphneDSL script.\n"
    );

    // ************************************************************************
    // Process parsed arguments
    // ************************************************************************

    // Initialize user configuration.
    DaphneUserConfig user_config{};
    try {
        if (configFile != configFileInitValue && ConfigParser::fileExists(configFile)) {
            ConfigParser::readUserConfig(configFile, user_config);
        }
        else {
//            spdlog::warn("No configuration file provided - using defaults!");
            user_config.loggers.push_back(LogConfig({"default", "daphne-output.txt",
                    static_cast<int>(spdlog::level::warn), "\">>>>>>>>> %H:%M:%S %z %v\""}));
        }
    }
    catch(std::exception & e) {
        std::cerr << "Error while reading user config: " << e.what() << std::endl;
        return StatusCode::PARSER_ERROR;
    }
    
//    user_config.debug_llvm = true;
    user_config.use_vectorized_exec = useVectorizedPipelines;
    user_config.use_distributed = useDistributedRuntime; 
    user_config.use_obj_ref_mgnt = !noObjRefMgnt;
    user_config.use_ipa_const_propa = !noIPAConstPropa;
    user_config.use_phy_op_selection = !noPhyOpSelection;
    user_config.libdir = libDir.getValue();
    user_config.library_paths.push_back(user_config.libdir + "/libAllKernels.so");
    user_config.taskPartitioningScheme = taskPartitioningScheme;
    user_config.queueSetupScheme = queueSetupScheme;
	user_config.victimSelection = victimSelection;
    user_config.numberOfThreads = numberOfThreads; 
    user_config.minimumTaskSize = minimumTaskSize; 
    user_config.pinWorkers = pinWorkers;
    user_config.hyperthreadingEnabled = hyperthreadingEnabled;
    user_config.debugMultiThreading = debugMultiThreading;
    user_config.prePartitionRows = prePartitionRows;
    user_config.distributedBackEndSetup = distributedBackEndSetup;
    if(user_config.use_distributed)
    {
        if(user_config.distributedBackEndSetup!=ALLOCATION_TYPE::DIST_MPI &&  user_config.distributedBackEndSetup!=ALLOCATION_TYPE::DIST_GRPC)
            std::cout<<"No backend has been selected. Wiil use the default 'MPI'\n";
    }
    for (auto explain : explainArgList) {
        switch (explain) {
            case kernels:
                user_config.explain_kernels = true;
                break;
            case llvm:
                user_config.explain_llvm = true;
                break;
            case parsing:
                user_config.explain_parsing = true;
                break;
            case parsing_simplified:
                user_config.explain_parsing_simplified = true;
                break;
            case property_inference:
                user_config.explain_property_inference = true;
                break;
            case select_matrix_repr:
                user_config.explain_select_matrix_repr = true;
                break;
            case sql:
                user_config.explain_sql = true;
                break;
            case phy_op_selection:
                user_config.explain_phy_op_selection = true;
                break;
            case type_adaptation:
                user_config.explain_type_adaptation = true;
                break;
            case vectorized:
                user_config.explain_vectorized = true;
                break;
            case obj_ref_mgnt:
                user_config.explain_obj_ref_mgnt = true;
                break;
        }
    }

    if(user_config.use_distributed && distributedBackEndSetup==ALLOCATION_TYPE::DIST_MPI)
    {
#ifndef USE_MPI
    throw std::runtime_error("you are trying to use the MPI backend. But, Daphne was not build with --mpi option\n");    
#else
        MPI_Init(NULL,NULL);
        MPI_Comm_rank(MPI_COMM_WORLD, id);
        int size=0;
        MPI_Comm_size(MPI_COMM_WORLD, &size);
        if(size<=1)
        {
             throw std::runtime_error("you need to rerun with at least 2 MPI ranks (1 Master + 1 Worker)\n");
        }
        if(*id!=COORDINATOR)
        {
            return *id; 
        }
#endif 
    }
    if(cuda) {
        int device_count = 0;
#ifdef USE_CUDA
        CHECK_CUDART(cudaGetDeviceCount(&device_count));
#endif
        if(device_count < 1)
            std::cerr << "WARNING: CUDA ops requested by user option but no suitable device found" << std::endl;
        else {
            user_config.use_cuda = true;
        }
    }

    if(fpgaopencl) {
        user_config.use_fpgaopencl = true;
    }

    if(enableProfiling) {
        user_config.enable_profiling = true;
    }

    // add this after the cli args loop to work around args order
    if(!user_config.libdir.empty() && user_config.use_cuda)
            user_config.library_paths.push_back(user_config.libdir + "/libCUDAKernels.so");

    // For DaphneLib (Python API).
    user_config.result_struct = daphneLibRes;

    // Extract script args.
    unordered_map<string, string> scriptArgsFinal;
    try {
        parseScriptArgs(scriptArgs2, scriptArgsFinal);
        parseScriptArgs(scriptArgs1, scriptArgsFinal);
    }
    catch(exception& e) {
        std::cerr << "Parser error: " << e.what() << std::endl;
        return StatusCode::PARSER_ERROR;
    }
    
    logger = std::make_unique<DaphneLogger>(user_config);

    // ************************************************************************
    // Compile and execute script
    // ************************************************************************

    // Creates an MLIR context and loads the required MLIR dialects.
    DaphneIrExecutor executor(selectMatrixRepr, user_config);

    // Create an OpBuilder and an MLIR module and set the builder's insertion
    // point to the module's body, such that subsequently created DaphneIR
    // operations are inserted into the module.
    OpBuilder builder(executor.getContext());
    auto loc = mlir::FileLineColLoc::get(builder.getStringAttr(inputFile), 0, 0);
    auto moduleOp = ModuleOp::create(loc);
    auto * body = moduleOp.getBody();
    builder.setInsertionPoint(body, body->begin());

    // Parse the input file and generate the corresponding DaphneIR operations
    // inside the module, assuming DaphneDSL as the input format.
    DaphneDSLParser parser(scriptArgsFinal, user_config);
    try {
        parser.parseFile(builder, inputFile);
    }
    catch(std::exception & e) {
        std::cerr << "Parser error: " << e.what() << std::endl;
        return StatusCode::PARSER_ERROR;
    }

    // Further, process the module, including optimization and lowering passes.
    try{
        if (!executor.runPasses(moduleOp)) {
            return StatusCode::PASS_ERROR;
        }
    }
    catch(std::exception & e){
        std::cerr << "Pass error: " << e.what() << std::endl;
        return StatusCode::PASS_ERROR;
    }

    // JIT-compile the module and execute it.
    // module->dump(); // print the LLVM IR representation
    try{
        auto engine = executor.createExecutionEngine(moduleOp);
        auto error = engine->invoke("main");
        if (error) {
            llvm::errs() << "JIT-Engine invocation failed: " << error;
            return StatusCode::EXECUTION_ERROR;
        }
    }
    catch(std::exception & e){
        std::cerr << "Execution error: " << e.what() << std::endl;
        return StatusCode::EXECUTION_ERROR;
    }

    return StatusCode::SUCCESS;
}


int mainInternal(int argc, const char** argv, DaphneLibResult* daphneLibRes){
    int id=-1; // this  -1 would not change if the user did not select mpi backend during execution
    int res=startDAPHNE(argc, argv, daphneLibRes, &id);

#ifdef USE_MPI    
    if(id==COORDINATOR)
    {
        int size=0;
        MPI_Comm_size(MPI_COMM_WORLD, &size);
        unsigned char terminateMessage=0x00;
        for(int i=1;i<size;i++){
            MPI_Send(&terminateMessage,1, MPI_UNSIGNED_CHAR, i,  DETACH, MPI_COMM_WORLD);
       }
       MPI_Finalize();
    }   
    else if(id>-1){
        MPIWorker worker;
        worker.joinComputingTeam();
        res=StatusCode::SUCCESS;
        MPI_Finalize();
    }
#endif
   
    return res;
}