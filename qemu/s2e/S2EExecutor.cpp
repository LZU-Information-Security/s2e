extern "C" {
#include <qemu-common.h>
#include <cpu-all.h>
#include <tcg-llvm.h>
}

#include "S2EExecutor.h"
#include <s2e/S2E.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/Utils.h>

#include <s2e/s2e_qemu.h>

#include <llvm/Module.h>
#include <llvm/Function.h>
#include <llvm/DerivedTypes.h>
#include <llvm/Instructions.h>
#include <llvm/Target/TargetData.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>

#include <klee/StatsTracker.h>
#include <klee/PTree.h>
#include <klee/Memory.h>

#include <vector>

#include <sys/mman.h>

using namespace std;
using namespace llvm;
using namespace klee;

extern "C" {
    // XXX
    extern volatile void* saved_AREGs[3];
}

namespace s2e {

S2EHandler::S2EHandler(S2E* s2e)
        : m_s2e(s2e)
{
}

std::ostream &S2EHandler::getInfoStream() const
{
    return m_s2e->getInfoStream();
}

std::string S2EHandler::getOutputFilename(const std::string &fileName)
{
    return m_s2e->getOutputFilename(fileName);
}

std::ostream *S2EHandler::openOutputFile(const std::string &fileName)
{
    return m_s2e->openOutputFile(fileName);
}

/* klee-related function */
void S2EHandler::incPathsExplored()
{
    m_pathsExplored++;
}

/* klee-related function */
void S2EHandler::processTestCase(const klee::ExecutionState &state,
                     const char *err, const char *suffix)
{
    m_s2e->getWarningsStream() << "Terminating state '" << (&state)
           << "with error message '" << (err ? err : "") << "'" << std::endl;
}

S2EExecutor::S2EExecutor(S2E* s2e, TCGLLVMContext *tcgLLVMContext,
                    const InterpreterOptions &opts,
                            InterpreterHandler *ie)
        : Executor(opts, ie, tcgLLVMContext->getExecutionEngine()),
          m_s2e(s2e), m_tcgLLVMContext(tcgLLVMContext)
{
    LLVMContext& ctx = m_tcgLLVMContext->getLLVMContext();

    /* Add dummy TB function declaration */
    const PointerType* tbFunctionArgTy =
            PointerType::get(IntegerType::get(ctx, 64), 0);
    FunctionType* tbFunctionTy = FunctionType::get(
            IntegerType::get(ctx, TCG_TARGET_REG_BITS),
            vector<const Type*>(1, PointerType::get(
                    IntegerType::get(ctx, 64), 0)),
            false);

    Function* tbFunction = Function::Create(
            tbFunctionTy, Function::PrivateLinkage, "s2e_dummyTbFunction",
            m_tcgLLVMContext->getModule());

    /* Create dummy main function containing just two instructions:
       a call to TB function and ret */
    Function* dummyMain = Function::Create(
            FunctionType::get(Type::getVoidTy(ctx), false),
            Function::PrivateLinkage, "s2e_dummyMainFunction",
            m_tcgLLVMContext->getModule());

    BasicBlock* dummyMainBB = BasicBlock::Create(ctx, "entry", dummyMain);

    vector<Value*> tbFunctionArgs(1, ConstantPointerNull::get(tbFunctionArgTy));
    CallInst::Create(tbFunction, tbFunctionArgs.begin(), tbFunctionArgs.end(),
            "tbFunctionCall", dummyMainBB);
    ReturnInst::Create(m_tcgLLVMContext->getLLVMContext(), dummyMainBB);

    // XXX: this will not work without creating JIT
    // XXX: how to get data layout without without ExecutionEngine ?
    m_tcgLLVMContext->getModule()->setDataLayout(
            m_tcgLLVMContext->getExecutionEngine()
                ->getTargetData()->getStringRepresentation());

    /* Set module for the executor */
    ModuleOptions MOpts(KLEE_LIBRARY_DIR,
                    /* Optimize= */ false, /* CheckDivZero= */ false);
    setModule(m_tcgLLVMContext->getModule(), MOpts);

    m_dummyMain = kmodule->functionMap[dummyMain];
}

S2EExecutor::~S2EExecutor()
{
    if(statsTracker)
        statsTracker->done();
}

S2EExecutionState* S2EExecutor::createInitialState()
{
    assert(!processTree);

    /* Create initial execution state */
    S2EExecutionState *state =
        new S2EExecutionState(m_dummyMain);

    state->cpuState = NULL;
    state->cpuPC = 0;

    if(pathWriter)
        state->pathOS = pathWriter->open();
    if(symPathWriter)
        state->symPathOS = symPathWriter->open();

    if(statsTracker)
        statsTracker->framePushed(*state, 0);

    states.insert(state);

    processTree = new PTree(state);
    state->ptreeNode = processTree->root;

    /* Externally accessible global vars */
    /* XXX move away */
    addExternalObject(*state, &tcg_llvm_runtime,
                      sizeof(tcg_llvm_runtime), false);
    addExternalObject(*state, saved_AREGs,
                      sizeof(saved_AREGs), false);

#if 0
    /* Make CPUState instances accessible: generated code uses them as globals */
    for(CPUState *env = first_cpu; env != NULL; env = env->next_cpu) {
        std::cout << "Adding KLEE CPU (addr = " << env
                  << " size = " << sizeof(*env) << ")" << std::endl;
        addExternalObject(*state, env, sizeof(*env), false);
    }

    /* Map physical memory */
    std::cout << "Populating KLEE memory..." << std::endl;

    for(ram_addr_t addr = 0; addr < last_ram_offset; addr += TARGET_PAGE_SIZE) {
        int pd = cpu_get_physical_page_desc(addr) & ~TARGET_PAGE_MASK;
        if(pd > IO_MEM_ROM && !(pd & IO_MEM_ROMD))
            continue;

        void* p = qemu_get_ram_ptr(addr);
        MemoryObject* mo = addExternalObject(
                *state, p, TARGET_PAGE_SIZE, false);
        mo->isUserSpecified = true; // XXX hack

        /* XXX */
        munmap(p, TARGET_PAGE_SIZE);
    }
    std::cout << "...done" << std::endl;
#endif

    return state;
}

void S2EExecutor::initializeExecution(S2EExecutionState* state)
{
    typedef std::pair<uint64_t, uint64_t> _UnusedMemoryRegion;
    foreach(_UnusedMemoryRegion p, m_unusedMemoryRegions) {
        /* XXX */
        /* XXX : use qemu_virtual* */
        munmap((void*) p.first, p.second);
    }

    state->cpuState = first_cpu;
    initializeGlobals(*state);
    bindModuleConstants();

}

void S2EExecutor::registerCpu(S2EExecutionState *initialState,
                              CPUX86State *cpuEnv)
{
    std::cout << std::hex
              << "Adding CPU (addr = 0x" << cpuEnv
              << ", size = 0x" << sizeof(*cpuEnv) << ")"
              << std::dec << std::endl;
    addExternalObject(*initialState, cpuEnv, sizeof(*cpuEnv), false);
    if(!initialState->cpuState) initialState->cpuState = cpuEnv;
}

void S2EExecutor::registerMemory(S2EExecutionState *initialState,
                        uint64_t startAddr, uint64_t size,
                        uint64_t hostAddr, bool isStateLocal)
{
    assert((hostAddr & ~TARGET_PAGE_MASK) == 0);
    assert((startAddr & ~TARGET_PAGE_MASK) == 0);
    assert((size & ~TARGET_PAGE_MASK) == 0);

    std::cout << std::hex
              << "Adding memory block (startAddr = 0x" << startAddr
              << ", size = 0x" << size << ", hostAddr = 0x" << hostAddr
              << ")" << std::dec << std::endl;

    if(isStateLocal) {
        for(uint64_t addr = hostAddr; addr < hostAddr+size;
                     addr += TARGET_PAGE_SIZE) {
            MemoryObject* mo = addExternalObject(
                    *initialState, (void*) addr, TARGET_PAGE_SIZE, false);
            mo->isUserSpecified = true; // XXX hack

            /* XXX */
            /* XXX : use qemu_mprotect */
            mprotect((void*) addr, TARGET_PAGE_SIZE, PROT_NONE);
        }

        m_unusedMemoryRegions.push_back(make_pair(hostAddr, size));
    } else {
        /* TODO */
    }
}

inline uintptr_t S2EExecutor::executeTranslationBlock(
        S2EExecutionState* state,
        TranslationBlock* tb,
        void* volatile* saved_AREGs)
{
    tcg_llvm_runtime.last_tb = tb;
#if 0
    return ((uintptr_t (*)(void* volatile*)) tb->llvm_tc_ptr)(saved_AREGs);
#else
    KFunction *kf;
    typeof(kmodule->functionMap.begin()) it =
            kmodule->functionMap.find(tb->llvm_function);
    if(it != kmodule->functionMap.end()) {
        kf = it->second;
    } else {
        unsigned cIndex = kmodule->constants.size();
        kf = kmodule->updateModuleWithFunction(tb->llvm_function);

        for(unsigned i = 0; i < kf->numInstructions; ++i)
            bindInstructionConstants(kf->instructions[i]);

        kmodule->constantTable.resize(kmodule->constants.size());
        for(unsigned i = cIndex; i < kmodule->constants.size(); ++i) {
            Cell &c = kmodule->constantTable[i];
            c.value = evalConstant(kmodule->constants[i]);
        }
    }

    /* Update state */
    state->cpuState = (CPUX86State*) saved_AREGs[0];
    state->cpuPC = tb->pc;

    assert(state->stack.size() == 1);
    assert(state->pc == m_dummyMain->instructions);

    /* Emulate call to a TB function */
    state->prevPC = state->pc;

    state->pushFrame(state->pc, kf);
    state->pc = kf->instructions;

    if(statsTracker)
        statsTracker->framePushed(*state,
            &state->stack[state->stack.size()-2]);

    /* Pass argument */
    bindArgument(kf, 0, *state,
                 Expr::createPointer((uint64_t) saved_AREGs));

    if (!state->addressSpace.copyInConcretes()) {
        std::cerr << "external modified read-only object" << std::endl;
        exit(1);
    }

    /* Execute */
    while(state->stack.size() != 1) {
        /* XXX: update cpuPC */

        KInstruction *ki = state->pc;
        stepInstruction(*state);
        executeInstruction(*state, ki);

        /* TODO: timers */
        /* TODO: MaxMemory */

        updateStates(state);
        if(states.find(state) == states.end()) {
            std::cerr << "The state was killed !" << std::endl;
            std::cerr << "Last executed instruction was:" << std::endl;
            ki->inst->dump();
            exit(1);
        }
    }

    state->prevPC = 0;
    state->pc = m_dummyMain->instructions;

    ref<Expr> resExpr =
            getDestCell(*state, state->pc).value;
    assert(isa<klee::ConstantExpr>(resExpr));

    state->addressSpace.copyOutConcretes();

    return cast<klee::ConstantExpr>(resExpr)->getZExtValue();
#endif
}

void S2EExecutor::readMemoryConcrete(S2EExecutionState *state,
                    uint64_t address, uint8_t* buf, uint64_t size)
{
    uint64_t page_offset = address & ~TARGET_PAGE_MASK;
    if(page_offset + size <= TARGET_PAGE_SIZE) {
        /* Single-page access */

        ObjectPair op =
            state->addressSpace.findObject(address & TARGET_PAGE_MASK);

        if(op.first) {
            /* KLEE-owned memory */

            assert(op.first->isUserSpecified);

            ObjectState *wos = NULL;
            for(uint64_t i=0; i<size; ++i) {
                if(!op.second->readConcrete8(page_offset+i, buf+i)) {
                    if(!wos) {
                        op.second = wos = state->addressSpace.getWriteable(
                                                        op.first, op.second);
                    }
                    buf[i] = toConstant(*state, wos->read8(page_offset+i),
                                   "concrete memory access")->getZExtValue(8);
                    wos->write8(page_offset+i, buf[i]);
                }
            }
        } else {
            /* QEMU-owned memory */
            memcpy(buf, (void*) address, size);
        }
    } else {
        /* Access spans multiple pages */
        uint64_t size1 = TARGET_PAGE_SIZE - page_offset;
        readMemoryConcrete(state, address, buf, size1);
        readMemoryConcrete(state, address, buf + size1, size - size1);
    }
}

void S2EExecutor::writeMemoryConcrete(S2EExecutionState *state,
            uint64_t address, const uint8_t* buf, uint64_t size)
{
    uint64_t page_offset = address & ~TARGET_PAGE_MASK;
    if(page_offset + size <= TARGET_PAGE_SIZE) {
        /* Single-page access */

        ObjectPair op =
            state->addressSpace.findObject(address & TARGET_PAGE_MASK);

        if(op.first) {
            /* KLEE-owned memory */

            assert(op.first->isUserSpecified);

            ObjectState* wos =
                    state->addressSpace.getWriteable(op.first, op.second);
            for(uint64_t i=0; i<size; ++i) {
                wos->write8(page_offset+i, buf[i]);
            }
        } else {
            /* QEMU-owned memory */
            memcpy((void*) address, buf, size);
        }
    } else {
        /* Access spans multiple pages */
        uint64_t size1 = TARGET_PAGE_SIZE - page_offset;
        writeMemoryConcrete(state, address, buf, size1);
        writeMemoryConcrete(state, address, buf + size1, size - size1);
    }
}

} // namespace s2e

/******************************/
/* Functions called from QEMU */

S2EExecutionState* s2e_create_initial_state(S2E *s2e)
{
    return s2e->getExecutor()->createInitialState();
}

void s2e_initialize_execution(S2E *s2e, S2EExecutionState *initial_state)
{
    s2e->getExecutor()->initializeExecution(initial_state);
}

void s2e_register_cpu(S2E *s2e, S2EExecutionState *initial_state,
                      CPUX86State *cpu_env)
{
    s2e->getExecutor()->registerCpu(initial_state, cpu_env);
}

void s2e_register_memory(S2E* s2e, S2EExecutionState *initial_state,
        uint64_t start_addr, uint64_t size,
        uint64_t host_addr, int is_state_local)
{
    s2e->getExecutor()->registerMemory(initial_state,
        start_addr, size, host_addr, is_state_local);
}

uintptr_t s2e_qemu_tb_exec(S2E* s2e, S2EExecutionState* state,
                           struct TranslationBlock* tb,
                           void* volatile* saved_AREGs)
{
    return s2e->getExecutor()->executeTranslationBlock(state, tb, saved_AREGs);
}

void s2e_read_memory_concrete(S2E *s2e, S2EExecutionState *state,
                        uint64_t address, uint8_t* buf, uint64_t size)
{
    s2e->getExecutor()->readMemoryConcrete(state, address, buf, size);
}

void s2e_write_memory_concrete(S2E *s2e, S2EExecutionState *state,
                        uint64_t address, const uint8_t* buf, uint64_t size)
{
    s2e->getExecutor()->writeMemoryConcrete(state, address, buf, size);
}