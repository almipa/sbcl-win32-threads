/*
 * the Win32 incarnation of OS-dependent routines.  See also
 * $(sbcl_arch)-win32-os.c
 *
 * This file (along with os.h) exports an OS-independent interface to
 * the operating system VM facilities. Surprise surprise, this
 * interface looks a lot like the Mach interface (but simpler in some
 * places). For some operating systems, a subset of these functions
 * will have to be emulated.
 */

/*
 * This software is part of the SBCL system. See the README file for
 * more information.
 *
 * This software is derived from the CMU CL system, which was
 * written at Carnegie Mellon University and released into the
 * public domain. The software is in the public domain and is
 * provided with absolutely no warranty. See the COPYING and CREDITS
 * files for more information.
 */

/*
 * This file was copied from the Linux version of the same, and
 * likely still has some linuxisms in it have haven't been elimiated
 * yet.
 */

#define _WIN32_WINNT 0x0500
#define RtlUnwind RtlUnwind_FromSystemHeaders
#include <malloc.h>
#include <stdio.h>
#include <sys/param.h>
#include <sys/file.h>
#include <io.h>
#include "sbcl.h"
#include "os.h"
#include "arch.h"
#include "globals.h"
#include "sbcl.h"
#include "interrupt.h"
#include "interr.h"
#include "lispregs.h"
#include "runtime.h"
#include "alloc.h"
#include "genesis/primitive-objects.h"
#include "dynbind.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <math.h>
#include <float.h>

#include <excpt.h>

#include "validate.h"
#include "thread.h"
size_t os_vm_page_size;

#include "gc.h"
#include "gencgc-internal.h"
#include <Winsock2.h>

#if 0
int linux_sparc_siginfo_bug = 0;
int linux_supports_futex=0;
#endif

#include <stdarg.h>

#undef  RtlUnwind
/* missing definitions for modern mingws */
#ifndef EH_UNWINDING
#define EH_UNWINDING 0x02
#endif
#ifndef EH_EXIT_UNWIND
#define EH_EXIT_UNWIND 0x04
#endif

void odprint(const char * msg)
{
  char buf[1024];
  DWORD lastError = GetLastError();
  #if defined(LISP_FEATURE_SB_THREAD)
  sprintf(buf, "[0x%p] %s\n", pthread_self(), msg);
  OutputDebugString(buf);
  #else
  OutputDebugString(msg);
  #endif
  SetLastError(lastError);
}
const char * t_nil_s(lispobj symbol);

void odprintf_(const char * fmt, ...)
{
  char buf[1024];
  va_list args;
  int n;
  DWORD lastError = GetLastError();
  struct thread * self = arch_os_get_current_thread();
  #if defined(LISP_FEATURE_SB_THREAD)
  if (self) {
    sprintf(buf, "[0x%p] %s, %s, %s, %s ", pthread_self(), t_nil_s(GC_SAFE), t_nil_s(GC_INHIBIT), t_nil_s(INTERRUPTS_ENABLED), t_nil_s(IN_SAFEPOINT));
  } else {
    sprintf(buf, "[0x%p] (arch_os_get_current_thread() is NULL) ", pthread_self());
  }
  #else
  buf[0] = 0;
  #endif
  n = strlen(buf);
  va_start(args, fmt);
  vsprintf(buf + n, fmt, args);
  va_end(args);
  n = strlen(buf);
  buf[n] = '\n';
  buf[n + 1] = 0;
  OutputDebugString(buf);
  SetLastError(lastError);
}

unsigned long block_deferrables_and_return_mask()
{
  sigset_t sset;
  block_deferrable_signals(0, &sset);
  return (unsigned long)sset;
}

#if defined(LISP_FEATURE_SB_THREAD)
void apply_sigmask(unsigned long sigmask)
{
  sigset_t sset = (sigset_t)sigmask;
  pthread_sigmask(SIG_SETMASK, &sset, 0);
}
#endif

/* The exception handling function looks like this: */
EXCEPTION_DISPOSITION handle_exception(EXCEPTION_RECORD *,
                                       struct lisp_exception_frame *,
                                       CONTEXT *,
                                       void *);

void *base_seh_frame;

static void *get_seh_frame(void)
{
    void* retval;
    asm volatile ("movl %%fs:0,%0": "=r" (retval));
    return retval;
}

static void set_seh_frame(void *frame)
{
    asm volatile ("movl %0,%%fs:0": : "r" (frame));
}

#if 0
static struct lisp_exception_frame *find_our_seh_frame(void)
{
    struct lisp_exception_frame *frame = get_seh_frame();

    while (frame->handler != handle_exception)
        frame = frame->next_frame;

    return frame;
}

inline static void *get_stack_frame(void)
{
    void* retval;
    asm volatile ("movl %%ebp,%0": "=r" (retval));
    return retval;
}
#endif

#if defined(LISP_FEATURE_SB_THREAD)
void alloc_gc_page()
{
  void* addr = VirtualAlloc(GC_SAFEPOINT_PAGE_ADDR, 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!addr) {
    DWORD lastError = GetLastError();
    lose("in alloc_gc_page, VirtualAlloc returned NULL");
  }
}

void map_gc_page()
{
  DWORD oldProt;
  if (!VirtualProtect(GC_SAFEPOINT_PAGE_ADDR, 4, PAGE_READWRITE, &oldProt)) {
    DWORD lastError = GetLastError();
    lose("in map_gc_page, VirtualProtect returned FALSE");
  }
}

void unmap_gc_page()
{
  DWORD oldProt;
  if (!VirtualProtect(GC_SAFEPOINT_PAGE_ADDR, 4, PAGE_NOACCESS, &oldProt)) {
    DWORD lastError = GetLastError();
    lose("in unmap_gc_page, VirtualProtect returned FALSE");
  }
}
#endif

#if defined(LISP_FEATURE_SB_THREAD)
/* We want to get a slot in TIB that (1) is available at constant
   offset, (2) is our private property, so libraries wouldn't legally
   override it, (3) contains something predefined for threads created
   out of our sight.

   Low 64 TLS slots are adressable directly, starting with
   FS:[#xE10]. When SBCL runtime is initialized, some of the low slots
   may be already in use by its prerequisite DLLs, as DllMain()s and
   TLS callbacks have been called already. But slot 63 is unlikely to
   be reached at this point: one slot per DLL that needs it is the
   common practice, and many system DLLs use predefined TIB-based
   areas outside conventional TLS storage and don't need TLS slots.
   With our current dependencies, even slot 2 is observed to be free
   (as of WinXP and wine).

   Now we'll call TlsAlloc() repeatedly until slot 63 is officially
   assigned to us, then TlsFree() all other slots for normal use. TLS
   slot 63, alias FS:[#.(+ #xE10 (* 4 63))], now belongs to us.

   To summarize, let's list the assumptions we make:

   - TIB, which is FS segment base, contains first 64 TLS slots at the
     offset #xE10 (i.e. TIB layout compatibility);
   - TLS slots are allocated from lower to higher ones;
   - All libraries together with CRT startup have not requested 64
     slots yet.

   All these assumptions together don't seem to be less warranted than
   the availability of TIB arbitrary data slot for our use. There are
   some more reasons to prefer slot 63 over TIB arbitrary data: (1) if
   our assumptions for slot 63 are violated, it will be detected at
   startup instead of causing some system-specific unreproducible
   problems afterwards, depending on OS and loaded foreign libraries;
   (2) if getting slot 63 reliably with our current approach will
   become impossible for some future Windows version, we can add TLS
   callback directory to SBCL binary; main image TLS callback is
   started before _any_ TLS slot is allocated by libraries, and
   some C compiler vendors rely on this fact. */

void os_preinit()
{
    DWORD slots[TLS_MINIMUM_AVAILABLE];
    DWORD key;
    int n_slots = 0, i;
    for (i=0; i<TLS_MINIMUM_AVAILABLE; ++i) {
        key = TlsAlloc();
        if (key == OUR_TLS_INDEX) {
            if (TlsGetValue(key)!=NULL)
                lose("TLS slot assertion failed: fresh slot value is not NULL");
            TlsSetValue(OUR_TLS_INDEX, (intptr_t)0xFEEDBAC4);
            if ((intptr_t)(void*)arch_os_get_current_thread()!=(intptr_t)0xFEEDBAC4)
                lose("TLS slot assertion failed: TIB layout change detected");
            TlsSetValue(OUR_TLS_INDEX, (intptr_t)0xDEADBEAD);
            if ((intptr_t)(void*)arch_os_get_current_thread()!=(intptr_t)0xDEADBEAD)
                lose("TLS slot assertion failed: TIB content unstable");
            TlsSetValue(OUR_TLS_INDEX, NULL);
            break;
        }
        slots[n_slots++]=key;
    }
    for (i=0; i<n_slots; ++i) {
        TlsFree(slots[i]);
    }
    if (key!=OUR_TLS_INDEX) {
        lose("TLS slot assertion failed: slot 63 is unavailable "
             "(last TlsAlloc() returned %u)",key);
    }
}
#endif

void os_init(char *argv[], char *envp[])
{
    SYSTEM_INFO system_info;

    GetSystemInfo(&system_info);
    os_vm_page_size = system_info.dwPageSize;

    base_seh_frame = get_seh_frame();
#if defined(LISP_FEATURE_SB_THREAD)
    alloc_gc_page();
#endif
}


/*
 * So we have three fun scenarios here.
 *
 * First, we could be being called to reserve the memory areas
 * during initialization (prior to loading the core file).
 *
 * Second, we could be being called by the GC to commit a page
 * that has just been decommitted (for easy zero-fill).
 *
 * Third, we could be being called by create_thread_struct()
 * in order to create the sundry and various stacks.
 *
 * The third case is easy to pick out because it passes an
 * addr of 0.
 *
 * The second case is easy to pick out because it will be for
 * a range of memory that is MEM_RESERVE rather than MEM_FREE.
 *
 * The second case is also an easy implement, because we leave
 * the memory as reserved (since we do lazy commits).
 */

os_vm_address_t
os_validate(os_vm_address_t addr, os_vm_size_t len)
{
    MEMORY_BASIC_INFORMATION mem_info;

    if (!addr) {
        /* the simple case first */
        os_vm_address_t real_addr;
        if (!(real_addr = VirtualAlloc(addr, len, MEM_COMMIT, PAGE_EXECUTE_READWRITE))) {
            fprintf(stderr, "VirtualAlloc: 0x%lx.\n", GetLastError());
            return 0;
        }

        return real_addr;
    }

    if (!VirtualQuery(addr, &mem_info, sizeof mem_info)) {
        fprintf(stderr, "VirtualQuery: 0x%lx.\n", GetLastError());
        return 0;
    }

    if ((mem_info.State == MEM_RESERVE) && (mem_info.RegionSize >=len)) return addr;

    if (mem_info.State == MEM_RESERVE) {
        fprintf(stderr, "validation of reserved space too short.\n");
        fflush(stderr);
    }

    if (!VirtualAlloc(addr, len, (mem_info.State == MEM_RESERVE)? MEM_COMMIT: MEM_RESERVE, PAGE_EXECUTE_READWRITE)) {
        fprintf(stderr, "VirtualAlloc: 0x%lx.\n", GetLastError());
        return 0;
    }

    return addr;
}

/*
 * For os_invalidate(), we merely decommit the memory rather than
 * freeing the address space. This loses when freeing per-thread
 * data and related memory since it leaks address space. It's not
 * too lossy, however, since the two scenarios I'm aware of are
 * fd-stream buffers, which are pooled rather than torched, and
 * thread information, which I hope to pool (since windows creates
 * threads at its own whim, and we probably want to be able to
 * have them callback without funky magic on the part of the user,
 * and full-on thread allocation is fairly heavyweight). Someone
 * will probably shoot me down on this with some pithy comment on
 * the use of (setf symbol-value) on a special variable. I'm happy
 * for them.
 */

void
os_invalidate(os_vm_address_t addr, os_vm_size_t len)
{
    if (!VirtualFree(addr, len, MEM_DECOMMIT)) {
        fprintf(stderr, "VirtualFree: 0x%lx.\n", GetLastError());
    }
}

void
os_invalidate_free(os_vm_address_t addr, os_vm_size_t len)
{
    if (!VirtualFree(addr, 0, MEM_RELEASE)) {
        fprintf(stderr, "VirtualFree: 0x%lx.\n", GetLastError());
    }
}

/*
 * os_map() is called to map a chunk of the core file into memory.
 *
 * Unfortunately, Windows semantics completely screws this up, so
 * we just add backing store from the swapfile to where the chunk
 * goes and read it up like a normal file. We could consider using
 * a lazy read (demand page) setup, but that would mean keeping an
 * open file pointer for the core indefinately (and be one more
 * thing to maintain).
 */

os_vm_address_t
os_map(int fd, int offset, os_vm_address_t addr, os_vm_size_t len)
{
    os_vm_size_t count;

#if 0
    fprintf(stderr, "os_map: %d, 0x%x, %p, 0x%x.\n", fd, offset, addr, len);
    fflush(stderr);
#endif

    if (!VirtualAlloc(addr, len, MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
        fprintf(stderr, "VirtualAlloc: 0x%lx.\n", GetLastError());
        lose("os_map: VirtualAlloc failure");
    }

    if (lseek(fd, offset, SEEK_SET) == -1) {
        lose("os_map: Seek failure.");
    }

    count = read(fd, addr, len);
    if (count != len) {
        fprintf(stderr, "expected 0x%x, read 0x%x.\n", len, count);
        lose("os_map: Failed to read enough bytes.");
    }

    return addr;
}

static DWORD os_protect_modes[8] = {
    PAGE_NOACCESS,
    PAGE_READONLY,
    PAGE_READWRITE,
    PAGE_READWRITE,
    PAGE_EXECUTE,
    PAGE_EXECUTE_READ,
    PAGE_EXECUTE_READWRITE,
    PAGE_EXECUTE_READWRITE,
};

void
os_protect(os_vm_address_t address, os_vm_size_t length, os_vm_prot_t prot)
{
    DWORD old_prot;

    if (!VirtualProtect(address, length, os_protect_modes[prot], &old_prot)) {
        fprintf(stderr, "VirtualProtect failed, code 0x%lx.\n", GetLastError());
        fflush(stderr);
    }
}

/* FIXME: Now that FOO_END, rather than FOO_SIZE, is the fundamental
 * description of a space, we could probably punt this and just do
 * (FOO_START <= x && x < FOO_END) everywhere it's called. */
static boolean
in_range_p(os_vm_address_t a, lispobj sbeg, size_t slen)
{
    char* beg = (char*)((long)sbeg);
    char* end = (char*)((long)sbeg) + slen;
    char* adr = (char*)a;
    return (adr >= beg && adr < end);
}

boolean
is_linkage_table_addr(os_vm_address_t addr)
{
    return in_range_p(addr, LINKAGE_TABLE_SPACE_START, LINKAGE_TABLE_SPACE_END);
}

boolean
is_valid_lisp_addr(os_vm_address_t addr)
{
    struct thread *th;
    if(in_range_p(addr, READ_ONLY_SPACE_START, READ_ONLY_SPACE_SIZE) ||
       in_range_p(addr, STATIC_SPACE_START   , STATIC_SPACE_SIZE) ||
       in_range_p(addr, DYNAMIC_SPACE_START  , dynamic_space_size))
        return 1;
    for_each_thread(th) {
        if(((os_vm_address_t)th->control_stack_start <= addr) && (addr < (os_vm_address_t)th->control_stack_end))
            return 1;
        if(in_range_p(addr, (unsigned long)th->binding_stack_start, BINDING_STACK_SIZE))
            return 1;
    }
    return 0;
}

/* A tiny bit of interrupt.c state we want our paws on. */
extern boolean internal_errors_enabled;

#ifdef LISP_FEATURE_UD2_BREAKPOINTS
#define IS_TRAP_EXCEPTION(exception_record, context) \
    (((exception_record)->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) && \
     (((unsigned short *)((context.win32_context)->Eip))[0] == 0x0b0f))
#define TRAP_CODE_WIDTH 2
#else
#define IS_TRAP_EXCEPTION(exception_record, context) \
    ((exception_record)->ExceptionCode == EXCEPTION_BREAKPOINT)
#define TRAP_CODE_WIDTH 1
#endif

/*
 * A good explanation of the exception handling semantics is
 * http://win32assembly.online.fr/Exceptionhandling.html .
 */

EXCEPTION_DISPOSITION
handle_exception(EXCEPTION_RECORD *exception_record,
                 struct lisp_exception_frame *exception_frame,
                 CONTEXT *context,
                 void *dispatcher_context)
{
    DWORD lasterror = GetLastError();
    os_context_t ctx;
    ctx.win32_context = context;
#if defined(LISP_FEATURE_SB_THREAD)
    struct thread * self = arch_os_get_current_thread();
    pthread_sigmask(SIG_SETMASK, NULL, &ctx.sigmask);
    pthread_sigmask(SIG_BLOCK, &blockable_sigset, NULL);
#endif
    /* For EXCEPTION_ACCESS_VIOLATION only. */
    void *fault_address = (void *)exception_record->ExceptionInformation[1];
    odprintf("handle exception, EIP = 0x%p, code = 0x%p (addr = 0x%p)", context->Eip, exception_record->ExceptionCode, fault_address);
    if (exception_record->ExceptionFlags & (EH_UNWINDING | EH_EXIT_UNWIND)) {
        /* If we're being unwound, be graceful about it. */

#if defined(LISP_FEATURE_SB_THREAD)
        pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
#endif
        /* Undo any dynamic bindings, including *gc-safe*. */
        unbind_to_here(exception_frame->bindstack_pointer,
                       arch_os_get_current_thread());
#if defined(LISP_FEATURE_SB_THREAD)
        pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
        gc_safepoint();
        SetLastError(lasterror);
#endif
        return ExceptionContinueSearch;
    }


    if (single_stepping &&
        exception_record->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        /* We are doing a displaced instruction. At least function
         * end breakpoints uses this. */
        restore_breakpoint_from_single_step(&ctx);
        #if defined(LISP_FEATURE_SB_THREAD)
        pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
        gc_safepoint();
        #endif
        SetLastError(lasterror);
        return ExceptionContinueExecution;
    }

    if (IS_TRAP_EXCEPTION(exception_record, ctx)) {
        unsigned char trap;

        /* This is just for info in case the monitor wants to print an
         * approximation. */
        access_control_stack_pointer(self) =
            (lispobj *)*os_context_sp_addr(&ctx);
        /* Unlike some other operating systems, Win32 leaves EIP
         * pointing to the breakpoint instruction. */
        ctx.win32_context->Eip += TRAP_CODE_WIDTH;
        /* Now EIP points just after the INT3 byte and aims at the
         * 'kind' value (eg trap_Cerror). */
        trap = *(unsigned char *)(*os_context_pc_addr(&ctx));
        handle_trap(&ctx, trap);
        #if defined(LISP_FEATURE_SB_THREAD)
        pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
        gc_safepoint();
        #endif
        SetLastError(lasterror);
        /* Done, we're good to go! */
        return ExceptionContinueExecution;
    }
    #if defined(LISP_FEATURE_SB_THREAD)
    else if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && fault_address == GC_SAFEPOINT_PAGE_ADDR) {
      pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
      gc_safepoint();
      SetLastError(lasterror);
      return ExceptionContinueExecution;
    }
    #endif
    else if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
             (is_valid_lisp_addr(fault_address) ||
              is_linkage_table_addr(fault_address))) {
        /* Pick off GC-related memory fault next. */
        MEMORY_BASIC_INFORMATION mem_info;

        if (!VirtualQuery(fault_address, &mem_info, sizeof mem_info)) {
            fprintf(stderr, "VirtualQuery: 0x%lx.\n", GetLastError());
            lose("handle_exception: VirtualQuery failure");
        }

        if (mem_info.State == MEM_RESERVE) {
            /* First use new page, lets get some memory for it. */
            if (!VirtualAlloc(mem_info.BaseAddress, os_vm_page_size,
                              MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                fprintf(stderr, "VirtualAlloc: 0x%lx.\n", GetLastError());
                lose("handle_exception: VirtualAlloc failure");

            } else {
                /*
                 * Now, if the page is supposedly write-protected and this
                 * is a write, tell the gc that it's been hit.
                 *
                 * FIXME: Are we supposed to fall-through to the Lisp
                 * exception handler if the gc doesn't take the wp violation?
                 */
                if (exception_record->ExceptionInformation[0]) {
                    int index = find_page_index(fault_address);
                    if ((index != -1) && (page_table[index].write_protected)) {
                        gencgc_handle_wp_violation(fault_address);
                    }
                }
                #if defined(LISP_FEATURE_SB_THREAD)
                pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
                gc_safepoint();
                #endif
                SetLastError(lasterror);
                return ExceptionContinueExecution;
            }

        } else {
            if (gencgc_handle_wp_violation(fault_address)) {
              #if defined(LISP_FEATURE_SB_THREAD)
              pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
              gc_safepoint();
              #endif
              SetLastError(lasterror);
              /* gc accepts the wp violation, so resume where we left off. */
              return ExceptionContinueExecution;
          }
        }

        /* All else failed, drop through to the lisp-side exception handler. */
    }

    /*
     * If we fall through to here then we need to either forward
     * the exception to the lisp-side exception handler if it's
     * set up, or drop to LDB.
     */

    if (internal_errors_enabled) {
        lispobj context_sap;
        lispobj exception_record_sap;

        /* We're making the somewhat arbitrary decision that having
         * internal errors enabled means that lisp has sufficient
         * marbles to be able to handle exceptions, but exceptions
         * aren't supposed to happen during cold init or reinit
         * anyway. */

        fake_foreign_function_call(&ctx);
	
        #if defined(LISP_FEATURE_SB_THREAD)
        pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
        #endif
        
        /* Allocate the SAP objects while the "interrupts" are still
         * disabled. */
        context_sap = alloc_sap(&ctx);
        exception_record_sap = alloc_sap(exception_record);

        /* The exception system doesn't automatically clear pending
         * exceptions, so we lose as soon as we execute any FP
         * instruction unless we do this first. */
        _clearfp();

        /* Call into lisp to handle things. */
        funcall2(StaticSymbolFunction(HANDLE_WIN32_EXCEPTION), context_sap,
                 exception_record_sap);

        /* If Lisp doesn't nlx, we need to put things back. */
        undo_fake_foreign_function_call(&ctx);
        #if defined(LISP_FEATURE_SB_THREAD)
        gc_safepoint();
        #endif
        SetLastError(lasterror);

        /* FIXME: HANDLE-WIN32-EXCEPTION should be allowed to decline */
        return ExceptionContinueExecution;
    }

    fprintf(stderr, "Exception Code: 0x%lx.\n", exception_record->ExceptionCode);
    fprintf(stderr, "Faulting IP: 0x%lx.\n", (DWORD)exception_record->ExceptionAddress);
    if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        MEMORY_BASIC_INFORMATION mem_info;

        if (VirtualQuery(fault_address, &mem_info, sizeof mem_info)) {
            fprintf(stderr, "page status: 0x%lx.\n", mem_info.State);
        }

        fprintf(stderr, "Was writing: %ld, where: 0x%lx.\n",
                exception_record->ExceptionInformation[0],
                (DWORD)fault_address);
    }

    fflush(stderr);

    fake_foreign_function_call(&ctx);
    lose("Exception too early in cold init, cannot continue.");

    /* FIXME: WTF? How are we supposed to end up here? */
    #if defined(LISP_FEATURE_SB_THREAD)
    pthread_sigmask(SIG_SETMASK, &ctx.sigmask, NULL);
    gc_safepoint();
    SetLastError(lasterror);
    #endif
    return ExceptionContinueSearch;
}

void
wos_install_interrupt_handlers(struct lisp_exception_frame *handler)
{
    handler->next_frame = get_seh_frame();
    handler->handler = &handle_exception;
    set_seh_frame(handler);
}

void bcopy(const void *src, void *dest, size_t n)
{
    MoveMemory(dest, src, n);
}

/*
 * The stubs below are replacements for the windows versions,
 * which can -fail- when used in our memory spaces because they
 * validate the memory spaces they are passed in a way that
 * denies our exception handler a chance to run.
 */

void *memmove(void *dest, const void *src, size_t n)
{
    if (dest < src) {
        int i;
        for (i = 0; i < n; i++) *(((char *)dest)+i) = *(((char *)src)+i);
    } else {
        while (n--) *(((char *)dest)+n) = *(((char *)src)+n);
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    while (n--) *(((char *)dest)+n) = *(((char *)src)+n);
    return dest;
}

char *dirname(char *path)
{
    static char buf[PATH_MAX + 1];
    size_t pathlen = strlen(path);
    int i;

    if (pathlen >= sizeof(buf)) {
        lose("Pathname too long in dirname.\n");
        return NULL;
    }

    strcpy(buf, path);
    for (i = pathlen; i >= 0; --i) {
        if (buf[i] == '/' || buf[i] == '\\') {
            buf[i] = '\0';
            break;
        }
    }

    return buf;
}

/* This is a manually-maintained version of ldso_stubs.S. */

void __stdcall RtlUnwind(void *, void *, void *, void *); /* I don't have winternl.h */

void scratch(void)
{
    LARGE_INTEGER la = {{0}};
    CloseHandle(0);
    closesocket(0);
    CreateWaitableTimerA(NULL,FALSE,NULL);
    CancelWaitableTimer(NULL);
    DuplicateHandle(0,0,0,0,0,0,0);
    FlushConsoleInputBuffer(0);
    FormatMessageA(0, 0, 0, 0, 0, 0, 0);
    FreeLibrary(0);
    GetACP();
    GetCommTimeouts(0,0);
    SetCommTimeouts(0,0);
    ClearCommError(0,0,0);
    GetConsoleCP();
    GetConsoleOutputCP();
    GetCurrentProcess();
    GetExitCodeProcess(0, 0);
    GetFileSizeEx(0,&la);
    GetFileType(0);
    GetLastError();
    GetOEMCP();
    GetProcAddress(0, 0);
    GetProcessTimes(0, 0, 0, 0, 0);
    GetSystemTimeAsFileTime(0);
    LoadLibrary(0);
    LocalFree(0);
    PeekConsoleInput(0, 0, 0, 0);
    PeekNamedPipe(0, 0, 0, 0, 0, 0);
    ReadFile(0, 0, 0, 0, 0);
    SetWaitableTimer(NULL,NULL,0L,NULL,NULL,FALSE);
    Sleep(0);
    WriteFile(0, 0, 0, 0, 0);
    _get_osfhandle(0);
    _open_osfhandle(0,0);
    _rmdir(0);
    _pipe(0,0,0);
    _lseeki64(0,0,0);
    access(0,0);
    close(0);
    dup(0);
    isatty(0);
    strerror(42);
    write(0, 0, 0);
    RtlUnwind(0, 0, 0, 0);
    SetStdHandle(0,0);
    GetStdHandle(0);
    MapViewOfFile(0,0,0,0,0);
    UnmapViewOfFile(0);
    FlushViewOfFile(0,0);
    #ifndef LISP_FEATURE_SB_UNICODE
      CreateDirectoryA(0,0);
      CreateFileMappingA(0,0,0,0,0,0);
      CreateFileA(0,0,0,0,0,0,0);
      GetComputerNameA(0, 0);
      GetCurrentDirectoryA(0,0);
      GetEnvironmentVariableA(0, 0, 0);
      GetFileAttributesA(0);
      GetVersionExA(0);
      MoveFileA(0,0);
      SHGetFolderPathA(0, 0, 0, 0, 0);
      SetCurrentDirectoryA(0);
      SetEnvironmentVariableA(0, 0);
    #else
      CreateDirectoryW(0,0);
      CreateFileMappingW(0,0,0,0,0,0);
      CreateFileW(0,0,0,0,0,0,0);
      FormatMessageW(0, 0, 0, 0, 0, 0, 0);
      GetComputerNameW(0, 0);
      GetCurrentDirectoryW(0,0);
      GetEnvironmentVariableW(0, 0, 0);
      GetFileAttributesW(0);
      GetVersionExW(0);
      MoveFileW(0,0);
      SHGetFolderPathW(0, 0, 0, 0, 0);
      SetCurrentDirectoryW(0);
      SetEnvironmentVariableW(0, 0);
    #endif
}

char *
os_get_runtime_executable_path(int external)
{
    char path[MAX_PATH + 1];
    DWORD bufsize = sizeof(path);
    DWORD size;

    if ((size = GetModuleFileNameA(NULL, path, bufsize)) == 0)
        return NULL;
    else if (size == bufsize && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        return NULL;

    return copied_string(path);
}

// 0 - not a socket or other error, 1 - has input, 2 - has no input
int
socket_input_available(HANDLE socket)
{
  unsigned long count = 0, count_size = 0;
  int wsaErrno = WSAGetLastError();
  int err = WSAIoctl((SOCKET)socket, FIONREAD, NULL, 0,
                     &count, sizeof(count), &count_size, NULL, NULL);

  int ret;

  if (err == 0) {
    ret = (count > 0) ? 1 : 2;
  } else
    ret = 0;
  WSASetLastError(wsaErrno);
  return ret;
}

int win32_unix_write(int fd, void * buf, int count)
{
  HANDLE handle;
  DWORD written_bytes;
  OVERLAPPED overlapped;
  struct thread * self = arch_os_get_current_thread();
  BOOL waitInGOR;
  LARGE_INTEGER file_position, nooffset = {{0}};

  odprintf("write(%d, 0x%p, %d)", fd, buf, count);
  handle =(HANDLE)_get_osfhandle(fd);
  odprintf("handle = 0x%p", handle);
  overlapped.hEvent = self->private_events.events[0];
  if (SetFilePointerEx(handle,nooffset,&file_position, FILE_CURRENT)) {
    overlapped.Offset = file_position.LowPart;
    overlapped.OffsetHigh = file_position.HighPart;
  } else {
    overlapped.Offset = 0;
    overlapped.OffsetHigh = 0;
  }
  if (WriteFile(handle, buf, count, &written_bytes,
                &overlapped)) {
    odprintf("write(%d, 0x%p, %d) immeditately wrote %d bytes",
             fd, buf, count, written_bytes);
    goto done_something;
  } else {
    if (GetLastError()!=ERROR_IO_PENDING) {
      errno = EIO;
      return -1;
    } else {
      if(WaitForMultipleObjects(2,self->private_events.events,
                                   FALSE,INFINITE) != WAIT_OBJECT_0) {
        /* Something happened. Interrupt? */
        odprintf("write(%d, 0x%p, %d) EINTR",fd,buf,count);
        CancelIo(handle);
        waitInGOR = TRUE;
      } else {
        waitInGOR = FALSE;
      }
      if (!GetOverlappedResult(handle,&overlapped,&written_bytes,waitInGOR)) {
        if (GetLastError()==ERROR_OPERATION_ABORTED) {
          errno = EINTR;
        } else {
          errno = EIO;
        }
        return -1;
      } else {
        goto done_something;
      }
    }
  }
 done_something:
  file_position.QuadPart += written_bytes;
  SetFilePointerEx(handle,file_position,NULL,FILE_BEGIN);
  return written_bytes;
}

int win32_unix_read(int fd, void * buf, int count)
{
  HANDLE handle;
  OVERLAPPED overlapped = {0};
  DWORD read_bytes = 0;
  struct thread * self = arch_os_get_current_thread();
  DWORD errorCode = 0;
  BOOL waitInGOR = FALSE;
  LARGE_INTEGER file_position, nooffset={{0}};

  odprintf("read(%d, 0x%p, %d)", fd, buf, count);
  handle = (HANDLE)_get_osfhandle(fd);
  odprintf("handle = 0x%p", handle);
  overlapped.hEvent = self->private_events.events[0];
  /* If it has a position, we won't try overlapped */
  if (SetFilePointerEx(handle,nooffset,&file_position, FILE_CURRENT)) {
    overlapped.Offset = file_position.LowPart;
    overlapped.OffsetHigh = file_position.HighPart;
  } else {
    overlapped.Offset = 0;
    overlapped.OffsetHigh = 0;
  }
  if (ReadFile(handle,buf,count,&read_bytes,
               &overlapped)) {
    /* immediately */
    goto done_something;
  } else {
    errorCode = GetLastError();
    if (errorCode == ERROR_HANDLE_EOF || errorCode == ERROR_BROKEN_PIPE) {
      /* it is an `error' for positioned reads! oh wtf */
      goto done_something;
    }
    if (errorCode!=ERROR_IO_PENDING) {
      /* is it some _real_ error? */
      errno = EIO;
      return -1;
    } else {
      if(WaitForMultipleObjects(2,self->private_events.events,
                                FALSE,INFINITE) != WAIT_OBJECT_0) {
        /* Something happened. Interrupt? */
        odprintf("read(%d, 0x%p, %d) EINTR",fd,buf,count);
        CancelIo(handle);
        waitInGOR = TRUE;
        /* Waiting for IO only */
      } else {
        waitInGOR = FALSE;
      }
      if (!GetOverlappedResult(handle,&overlapped,&read_bytes,waitInGOR)) {
        errorCode = GetLastError();
        if (errorCode == ERROR_HANDLE_EOF) {
          goto done_something;
        } else {
          if (errorCode == ERROR_OPERATION_ABORTED) {
            errno = EINTR;      /* that's it. */
          } else {
            errno = EIO;        /* something unspecific */
          }
          return -1;
        }
      } else {
        goto done_something;
      }
    }
  }
 done_something:
  file_position.QuadPart += read_bytes;
  SetFilePointerEx(handle,file_position,NULL,FILE_BEGIN);
  return read_bytes;
}

int win32_wait_object_or_signal(HANDLE waitFor)
{
  struct thread * self = arch_os_get_current_thread();
  HANDLE handles[2];
  handles[0] = waitFor;
  handles[1] = self->private_events.events[1];
  return
    WaitForMultipleObjects(2,handles, FALSE,INFINITE);
}
/* EOF */
