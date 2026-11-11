// SPDX-License-Identifier: GPL-3.0-or-later
// The subset of the Win32 API that the nxrt runtime (third_party/pokopia-recomp/runtime) uses, for POSIX hosts and the PS5.
// Implemented in runtime/nxrt_port/src/win32_shim.cpp on top of runtime/include/pkp/platform.h. Semantics follow the
// Win32 documentation for what nxrt relies on; anything else is deliberately absent so a new use fails to compile.
//   SRWLOCK, CONDITION_VARIABLE   one pointer each (nxrt static_asserts it), futex based
//   HANDLE                         threads, events, waitable timers, mutexes: objects of the shim
//   Virtual*                       reservations / commits through the platform layer, with a page-state table for VirtualQuery
//   RaiseException                 unwinds to the guest thread's entry frame (thread_port.cpp), else aborts
#pragma once

#include <strings.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#define WINAPI
#define CALLBACK
#define APIENTRY
#define NOMINMAX_SHIM 1

typedef int BOOL;
typedef uint8_t BOOLEAN;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONG64;
typedef int64_t LONGLONG;
typedef uint64_t ULONG64;
typedef uint64_t DWORD64;
typedef uint64_t ULONGLONG;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef intptr_t LONG_PTR;
typedef size_t SIZE_T;
typedef intptr_t SSIZE_T;
typedef unsigned int UINT;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* LPVOID;
typedef void* PVOID;
typedef const void* LPCVOID;
typedef char CHAR;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef wchar_t WCHAR;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef DWORD* LPDWORD;
typedef uintptr_t UINT_PTR;
typedef uintptr_t UINT_PTR_SHIM;
typedef DWORD(WINAPI* LPTHREAD_START_ROUTINE)(LPVOID);

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED 0x80u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260

#define ERROR_SUCCESS 0u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_ADDRESS 487u
#define ERROR_TIMEOUT 1460u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_NOT_SUPPORTED 50u
#define ERROR_WRITE_FAULT 29u
#define ERROR_ACCESS_DENIED 5u

// memory
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define MEM_FREE 0x00010000u
#define MEM_PRIVATE 0x00020000u
#define MEM_MAPPED 0x00040000u
#define MEM_RESET 0x00080000u
#define MEM_TOP_DOWN 0x00100000u
#define MEM_WRITE_WATCH 0x00200000u
#define MEM_LARGE_PAGES 0x20000000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_GUARD 0x100u
#define SEC_RESERVE 0x04000000u
#define SEC_COMMIT 0x08000000u
#define FILE_MAP_ALL_ACCESS 0xF001Fu
#define FILE_MAP_READ 0x4u
#define FILE_MAP_WRITE 0x2u

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _SYSTEM_INFO {
    WORD wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;

// synchronisation
typedef struct _RTL_SRWLOCK { void* Ptr; } SRWLOCK, *PSRWLOCK;
typedef struct _RTL_CONDITION_VARIABLE { void* Ptr; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define SRWLOCK_INIT {nullptr}
#define CONDITION_VARIABLE_INIT {nullptr}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1u

// threads
#define CREATE_SUSPENDED 0x4u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_ALL_ACCESS 0x1FFFFFu
#define DUPLICATE_SAME_ACCESS 0x2u
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2u
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x1u
#define TIMER_ALL_ACCESS 0x1F0003u
#define EXCEPTION_NONCONTINUABLE 0x1u

typedef struct _PROCESSOR_NUMBER { WORD Group; BYTE Number, Reserved; } PROCESSOR_NUMBER;
typedef struct _GROUP_AFFINITY { DWORD_PTR Mask; WORD Group; WORD Reserved[3]; } GROUP_AFFINITY;
typedef enum _LOGICAL_PROCESSOR_RELATIONSHIP { RelationProcessorCore = 0, RelationNumaNode = 1, RelationCache = 2,
                                               RelationProcessorPackage = 3, RelationGroup = 4, RelationAll = 0xffff } LOGICAL_PROCESSOR_RELATIONSHIP;
typedef struct _PROCESSOR_RELATIONSHIP {
    BYTE Flags, EfficiencyClass, Reserved[20];
    WORD GroupCount;
    GROUP_AFFINITY GroupMask[1];
} PROCESSOR_RELATIONSHIP;
typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
    LOGICAL_PROCESSOR_RELATIONSHIP Relationship;
    DWORD Size;
    PROCESSOR_RELATIONSHIP Processor;
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;
#define LTP_PC_SMT 0x1
#define NORMAL_PRIORITY_CLASS 0x20u
#define ABOVE_NORMAL_PRIORITY_CLASS 0x8000u
#define HIGH_PRIORITY_CLASS 0x80u
#define THREAD_SUSPEND_RESUME 0x0002u
#define THREAD_GET_CONTEXT 0x0008u
#define THREAD_QUERY_INFORMATION 0x0040u
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800u

// exceptions: only the types the diagnostics headers name (the port's crash handler uses POSIX signals)
typedef struct _CONTEXT { DWORD64 Rip, Rsp, Rbp, Rax, Rbx, Rcx, Rdx, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15; DWORD EFlags; } CONTEXT, *PCONTEXT;
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct _EXCEPTION_POINTERS { PEXCEPTION_RECORD ExceptionRecord; PCONTEXT ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

extern "C++" {
// ---- synchronisation (win32_shim.cpp)
void InitializeSRWLock(PSRWLOCK l);
void AcquireSRWLockExclusive(PSRWLOCK l);
void AcquireSRWLockShared(PSRWLOCK l);
void ReleaseSRWLockExclusive(PSRWLOCK l);
void ReleaseSRWLockShared(PSRWLOCK l);
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l);
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l);
void InitializeConditionVariable(PCONDITION_VARIABLE cv);
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags);
void WakeConditionVariable(PCONDITION_VARIABLE cv);
void WakeAllConditionVariable(PCONDITION_VARIABLE cv);

// ---- time
void Sleep(DWORD ms);
BOOL SwitchToThread();
BOOL QueryPerformanceCounter(LARGE_INTEGER* v);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* v);
ULONGLONG GetTickCount64();
DWORD GetTickCount();
HANDLE CreateWaitableTimerExW(void* sa, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE t, const LARGE_INTEGER* due, LONG period, void* routine, void* arg, BOOL resume);
BOOL SetWaitableTimerEx(HANDLE t, const LARGE_INTEGER* due, LONG period, void* routine, void* arg, void* reason, ULONG delay);

// ---- threads and processes
HANDLE CreateThread(void* sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid);
DWORD ResumeThread(HANDLE h);
DWORD SuspendThread(HANDLE h);
[[noreturn]] void ExitThread(DWORD code);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE* out, DWORD access, BOOL inherit, DWORD options);
HANDLE GetCurrentThread();
HANDLE GetCurrentProcess();
DWORD GetCurrentThreadId();
DWORD GetCurrentProcessId();
DWORD GetThreadId(HANDLE h);
DWORD GetCurrentProcessorNumber();
void GetCurrentProcessorNumberEx(PROCESSOR_NUMBER* p);
BOOL SetThreadPriority(HANDLE h, int prio);
int GetThreadPriority(HANDLE h);
BOOL SetThreadPriorityBoost(HANDLE h, BOOL disable);
DWORD_PTR SetThreadAffinityMask(HANDLE h, DWORD_PTR mask);
BOOL SetThreadIdealProcessorEx(HANDLE h, PROCESSOR_NUMBER* ideal, PROCESSOR_NUMBER* prev);
BOOL GetThreadIdealProcessorEx(HANDLE h, PROCESSOR_NUMBER* ideal);
BOOL GetThreadTimes(HANDLE h, FILETIME* create, FILETIME* exit, FILETIME* kernel, FILETIME* user);
BOOL SetThreadStackGuarantee(ULONG* size);
HANDLE CreateEventA(void* sa, BOOL manual, BOOL initial, LPCSTR name);
HANDLE CreateEventW(void* sa, BOOL manual, BOOL initial, LPCWSTR name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateMutexA(void* sa, BOOL owner, LPCSTR name);
BOOL ReleaseMutex(HANDLE h);
DWORD GetLastError();
void SetLastError(DWORD e);
void GetSystemInfo(SYSTEM_INFO* si);
[[noreturn]] void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args);
BOOL IsDebuggerPresent();
void OutputDebugStringA(LPCSTR s);
HMODULE GetModuleHandleA(LPCSTR name);
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n);
DWORD GetModuleFileNameW(HMODULE m, wchar_t* out, DWORD n);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
void* GetProcAddress(HMODULE m, LPCSTR name);
DWORD GetFileAttributesA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu

[[noreturn]] void TerminateProcess(HANDLE p, UINT code);
[[noreturn]] void ExitProcess(UINT code);
BOOL IsBadReadPtr(const void* p, UINT_PTR_SHIM n);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf, DWORD* len);
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR* proc, DWORD_PTR* sys);
BOOL SetPriorityClass(HANDLE p, DWORD cls);
DWORD GetPriorityClass(HANDLE p);

// ---- memory
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD* old);
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len);
BOOL FlushInstructionCache(HANDLE p, LPCVOID addr, SIZE_T size);
// sections (pagefile-backed, SEC_RESERVE) and their views: POSIX shared memory, so several views alias the same pages
HANDLE CreateFileMappingW(HANDLE file, void* sa, DWORD prot, DWORD size_high, DWORD size_low, LPCWSTR name);
LPVOID MapViewOfFileEx(HANDLE section, DWORD access, DWORD off_high, DWORD off_low, SIZE_T size, LPVOID at);
LPVOID MapViewOfFile(HANDLE section, DWORD access, DWORD off_high, DWORD off_low, SIZE_T size);
BOOL UnmapViewOfFile(LPCVOID view);
}  // extern "C++"
#define CreateFileMapping CreateFileMappingW

// ---- files (win32_files.cpp): POSIX file descriptors behind HANDLE; wide strings are UTF-32 here, converted to UTF-8
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x1u
#define FILE_SHARE_WRITE 0x2u
#define FILE_SHARE_DELETE 0x4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_WRITE_THROUGH 0x8u
#define SYNCHRONIZE 0x00100000u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_HANDLE_EOF 38u
#define ERROR_FILE_EXISTS 80u
#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 0x8u
#define WC_ERR_INVALID_CHARS 0x80u
typedef struct _OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard = 0 } GET_FILEEX_INFO_LEVELS;
typedef WORD LANGID;
#define LANG_CHINESE 0x04
#define LANG_GERMAN 0x07
#define LANG_ENGLISH 0x09
#define LANG_SPANISH 0x0a
#define LANG_FRENCH 0x0c
#define LANG_ITALIAN 0x10
#define LANG_JAPANESE 0x11
#define LANG_KOREAN 0x12
#define LANG_DUTCH 0x13
#define LANG_PORTUGUESE 0x16
#define LANG_RUSSIAN 0x19
#define SUBLANG_ENGLISH_US 0x01
#define SUBLANG_ENGLISH_UK 0x02
#define SUBLANG_ENGLISH_CAN 0x04
#define SUBLANG_FRENCH_CANADIAN 0x03
#define SUBLANG_PORTUGUESE_BRAZILIAN 0x01
#define SUBLANG_SPANISH 0x01
#define SUBLANG_SPANISH_MODERN 0x03
#define SUBLANG_CHINESE_TRADITIONAL 0x01
#define SUBLANG_CHINESE_SIMPLIFIED 0x02
#define SUBLANG_CHINESE_HONGKONG 0x03
#define SUBLANG_CHINESE_MACAU 0x05
#define PRIMARYLANGID(l) ((WORD)(l) & 0x3ff)
#define SUBLANGID(l) ((WORD)(l) >> 10)
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
extern "C++" {
HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, void* sa, DWORD disposition, DWORD flags, HANDLE tmpl);
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, void* sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* pos, DWORD method);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size);
BOOL GetFileAttributesExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL CreateDirectoryW(LPCWSTR path, void* sa);
BOOL CreateDirectoryA(LPCSTR path, void* sa);
BOOL DeleteFileW(LPCWSTR path);
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int n, LPWSTR dst, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int n, LPSTR dst, int cap, LPCSTR def, BOOL* used_def);
DWORD GetFullPathNameA(LPCSTR name, DWORD n, LPSTR out, LPSTR* file_part);
DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD n, LPCSTR file);
LANGID GetUserDefaultUILanguage();
HANDLE CreateMutexW(void* sa, BOOL owner, LPCWSTR name);
BOOL QueryThreadCycleTime(HANDLE h, ULONG64* cycles);
// std::filesystem::path::c_str() is char on POSIX: the W calls nxrt makes with a path take UTF-8 too
HANDLE CreateFileW(const char* name, DWORD access, DWORD share, void* sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL GetFileAttributesExW(const char* name, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL MoveFileExW(const char* from, const char* to, DWORD flags);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL CreateDirectoryW(const char* path, void* sa);
BOOL DeleteFileW(const char* path);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
BOOL GetProcessHandleCount(HANDLE p, DWORD* count);
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryW(const wchar_t* name);   // no DLLs on this port: always nullptr
BOOL FreeLibrary(HMODULE m);
BOOL GetProcessTimes(HANDLE p, FILETIME* create, FILETIME* exit, FILETIME* kernel, FILETIME* user);
DWORD GetGuiResources(HANDLE p, DWORD flags);
void GetSystemTimePreciseAsFileTime(FILETIME* ft);
void GetSystemTimeAsFileTime(FILETIME* ft);
}  // extern "C++"
#define GR_GDIOBJECTS 0u
#define GR_USEROBJECTS 1u
#define CreateFile CreateFileW
#define CreateDirectory CreateDirectoryW
#define DeleteFile DeleteFileW
#define _strtoui64 strtoull
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#include <cstdio>
FILE* _wfopen(const wchar_t* path, const wchar_t* mode);
FILE* _wfopen(const char* path, const wchar_t* mode);   // std::filesystem::path::c_str() on POSIX
#define THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF

#define CreateEvent CreateEventA
#define OutputDebugString OutputDebugStringA
#define YieldProcessor() __builtin_ia32_pause()
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

// MSVC intrinsics and keywords that appear in portable parts of nxrt
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#define _ReturnAddress() __builtin_return_address(0)
#define __debugbreak() __builtin_trap()
