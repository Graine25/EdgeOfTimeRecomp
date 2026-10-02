import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_ALL_ACCESS = 0x1FFFFF
THREAD_ALL_ACCESS = 0x1FFFFF
TH32CS_SNAPTHREAD = 0x4
CONTEXT_FULL = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_ulonglong), ("High", ctypes.c_longlong)]


class CONTEXT(ctypes.Structure):
    _align_ = 16
    _fields_ = [("P1Home", ctypes.c_ulonglong), ("P2Home", ctypes.c_ulonglong),
                ("P3Home", ctypes.c_ulonglong), ("P4Home", ctypes.c_ulonglong),
                ("P5Home", ctypes.c_ulonglong), ("P6Home", ctypes.c_ulonglong),
                ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD),
                ("SegGs", wt.WORD), ("SegSs", wt.WORD), ("EFlags", wt.DWORD),
                ("Dr0", ctypes.c_ulonglong), ("Dr1", ctypes.c_ulonglong), ("Dr2", ctypes.c_ulonglong),
                ("Dr3", ctypes.c_ulonglong), ("Dr6", ctypes.c_ulonglong), ("Dr7", ctypes.c_ulonglong),
                ("Rax", ctypes.c_ulonglong), ("Rcx", ctypes.c_ulonglong), ("Rdx", ctypes.c_ulonglong),
                ("Rbx", ctypes.c_ulonglong), ("Rsp", ctypes.c_ulonglong), ("Rbp", ctypes.c_ulonglong),
                ("Rsi", ctypes.c_ulonglong), ("Rdi", ctypes.c_ulonglong), ("R8", ctypes.c_ulonglong),
                ("R9", ctypes.c_ulonglong), ("R10", ctypes.c_ulonglong), ("R11", ctypes.c_ulonglong),
                ("R12", ctypes.c_ulonglong), ("R13", ctypes.c_ulonglong), ("R14", ctypes.c_ulonglong),
                ("R15", ctypes.c_ulonglong), ("Rip", ctypes.c_ulonglong),
                ("FltSave", ctypes.c_byte * 512),
                ("VectorRegister", M128A * 26), ("VectorControl", ctypes.c_ulonglong),
                ("DebugControl", ctypes.c_ulonglong), ("LastBranchToRip", ctypes.c_ulonglong),
                ("LastBranchFromRip", ctypes.c_ulonglong), ("LastExceptionToRip", ctypes.c_ulonglong),
                ("LastExceptionFromRip", ctypes.c_ulonglong)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_ulonglong), ("Segment", wt.WORD), ("Mode", ctypes.c_int)]


class KDHELP64(ctypes.Structure):
    _fields_ = [("Thread", ctypes.c_ulonglong), ("ThCallbackStack", wt.DWORD),
                ("ThCallbackBStore", wt.DWORD), ("NextCallback", wt.DWORD), ("FramePointer", wt.DWORD),
                ("KiCallUserMode", ctypes.c_ulonglong), ("KeUserCallbackDispatcher", ctypes.c_ulonglong),
                ("SystemRangeStart", ctypes.c_ulonglong), ("KiUserExceptionDispatcher", ctypes.c_ulonglong),
                ("StackBase", ctypes.c_ulonglong), ("StackLimit", ctypes.c_ulonglong),
                ("BuildVersion", wt.DWORD), ("RetpolineStubFunctionTableSize", wt.DWORD),
                ("RetpolineStubFunctionTable", ctypes.c_ulonglong), ("RetpolineStubOffset", wt.DWORD),
                ("RetpolineStubSize", wt.DWORD), ("Reserved0", ctypes.c_ulonglong * 2)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_ulonglong * 4), ("Far", wt.BOOL), ("Virtual", wt.BOOL),
                ("Reserved", ctypes.c_ulonglong * 3), ("KdHelp", KDHELP64)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_ulonglong * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_ulonglong),
                ("Flags", wt.ULONG), ("Value", ctypes.c_ulonglong), ("Address", ctypes.c_ulonglong),
                ("Register", wt.ULONG), ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG),
                ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_char * 1024)]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbghelp.SymSetOptions.argtypes = [wt.DWORD]
dbghelp.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbghelp.SymFunctionTableAccess64.argtypes = [wt.HANDLE, ctypes.c_ulonglong]
dbghelp.SymGetModuleBase64.restype = ctypes.c_ulonglong
dbghelp.SymGetModuleBase64.argtypes = [wt.HANDLE, ctypes.c_ulonglong]
dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong),
                                ctypes.c_void_p]
FTA = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_ulonglong)
GMB = ctypes.WINFUNCTYPE(ctypes.c_ulonglong, wt.HANDLE, ctypes.c_ulonglong)
RPM = ctypes.WINFUNCTYPE(wt.BOOL, wt.HANDLE, ctypes.c_ulonglong, ctypes.c_void_p, wt.DWORD,
                         ctypes.POINTER(wt.DWORD))
dbghelp.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(STACKFRAME64),
                                ctypes.c_void_p, RPM, FTA, GMB, ctypes.c_void_p]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]


def find_pid(name):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV"],
                                  text=True)
    for line in out.splitlines()[1:]:
        cols = [c.strip('"') for c in line.split('","')]
        if cols and cols[0].lower() == name.lower():
            return int(cols[1])
    raise SystemExit(f"{name} is not running")


k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wt.BOOL


def read_mem(hproc, addr, buf, size, read):
    n = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(hproc, addr, buf, size, ctypes.byref(n))
    if read:
        read[0] = n.value
    return 1 if ok else 0


def modules(hproc):
    mods = (wt.HMODULE * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(hproc, mods, ctypes.sizeof(mods), ctypes.byref(needed), 0x3)
    out = []
    for i in range(needed.value // ctypes.sizeof(wt.HMODULE)):
        name = ctypes.create_unicode_buffer(520)
        psapi.GetModuleFileNameExW(hproc, mods[i], name, 520)
        out.append((mods[i] or 0, os.path.basename(name.value)))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    target = args[0] if args else "EdgeOfTimeRecomp.exe"
    depth = int(args[1]) if len(args) > 1 else 24
    pid = int(target) if target.isdigit() else find_pid(target)
    hproc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not hproc:
        raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")
    dbghelp.SymSetOptions(0x4 | 0x2 | 0x10)
    exe_dir = None
    mods = modules(hproc)
    for base, name in mods:
        if name.lower() == "edgeoftimerecomp.exe":
            buf = ctypes.create_unicode_buffer(520)
            psapi.GetModuleFileNameExW(hproc, base, buf, 520)
            exe_dir = os.path.dirname(buf.value)
    if not dbghelp.SymInitializeW(hproc, exe_dir, True):
        raise SystemExit(f"SymInitialize failed: {ctypes.get_last_error()}")
    mods.sort()

    def module_of(addr):
        best = None
        for base, name in mods:
            if base and base <= addr:
                best = (base, name)
        return best

    def symbolize(addr):
        sym = SYMBOL_INFO()
        sym.SizeOfStruct = 88
        sym.MaxNameLen = 1000
        disp = ctypes.c_ulonglong()
        if dbghelp.SymFromAddr(hproc, addr, ctypes.byref(disp), ctypes.byref(sym)):
            m = module_of(addr)
            return f"{m[1] if m else '?'}!{sym.Name.decode(errors='replace')}+0x{disp.value:x}"
        m = module_of(addr)
        return f"{m[1]}+0x{addr - m[0]:x}" if m else f"0x{addr:x}"

    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    tids = []
    if k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                tids.append(te.th32ThreadID)
            if not k32.Thread32Next(snap, ctypes.byref(te)):
                break
    k32.CloseHandle(snap)
    print(f"pid {pid}: {len(tids)} threads, symbols from {exe_dir}")
    rpm = RPM(lambda h, a, b, n, r: read_mem(h, a, b, n, r))
    fta = FTA(lambda h, a: dbghelp.SymFunctionTableAccess64(h, a) or 0)
    gmb = GMB(lambda h, a: dbghelp.SymGetModuleBase64(h, a))
    for tid in tids:
        ht = k32.OpenThread(THREAD_ALL_ACCESS, False, tid)
        if not ht:
            print(f"-- thread {tid}: OpenThread failed")
            continue
        k32.SuspendThread(ht)
        ctx = CONTEXT()
        ctx.ContextFlags = CONTEXT_FULL
        ok = k32.GetThreadContext(ht, ctypes.byref(ctx))
        frames = []
        if ok:
            sf = STACKFRAME64()
            sf.AddrPC.Offset, sf.AddrPC.Mode = ctx.Rip, 3
            sf.AddrFrame.Offset, sf.AddrFrame.Mode = ctx.Rbp, 3
            sf.AddrStack.Offset, sf.AddrStack.Mode = ctx.Rsp, 3
            for _ in range(depth):
                if not dbghelp.StackWalk64(IMAGE_FILE_MACHINE_AMD64, hproc, ht, ctypes.byref(sf),
                                           ctypes.byref(ctx), rpm, fta, gmb, None):
                    break
                if sf.AddrPC.Offset == 0:
                    break
                frames.append(symbolize(sf.AddrPC.Offset))
        k32.ResumeThread(ht)
        k32.CloseHandle(ht)
        ours = any("!" in f and not f.split("!")[0].lower().startswith(("ntdll", "kernel", "win32u",
                                                                        "user32", "combase", "ucrtbase"))
                   for f in frames)
        if not show_all and not ours:
            continue
        print(f"-- thread {tid}" + ("" if ok else " (no context)"))
        for f in frames:
            print(f"   {f}")
    k32.CloseHandle(hproc)


if __name__ == "__main__":
    main()
