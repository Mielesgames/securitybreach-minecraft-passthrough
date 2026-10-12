"""Read a UE4SS crash dump (crash_*.dmp next to the game's exe): what crashed, and which of our
functions were on the stack.

    python tools/crash_stack.py <crash.dmp> [module.dll=path\\to\\image-with-pdb-next-to-it ...]

Example, from the repository root (the build's dll must be the one that crashed):

    copy build\\sb\\Game__Shipping__Win64\\SecurityBreachMinecraftPassthrough.dll %TEMP%\\sym\\main.dll
    copy build\\sb\\Game__Shipping__Win64\\SecurityBreachMinecraftPassthrough.pdb %TEMP%\\sym\\
    python tools/crash_stack.py crash.dmp main.dll=%TEMP%\\sym\\main.dll ^
        ue4ss.dll=build\\Game__Shipping__Win64\\bin\\UE4SS.dll

The stack is not unwound: every stack slot that points into a module is listed, so stale return
addresses of earlier calls show up too. Read it top down and trust the frames that fit together.
Set ONLY=main.dll,ue4ss.dll to list those modules only.
"""
import ctypes
import os
import struct
import sys
from ctypes import wintypes

ONLY = [name for name in os.environ.get("ONLY", "").lower().split(",") if name]

dump_path = sys.argv[1]
pdb_map = {}  # module file name (lower case) -> image with its PDB next to it
for arg in sys.argv[2:]:
    name, path = arg.split("=", 1)
    pdb_map[name.lower()] = path

data = open(dump_path, "rb").read()
signature, _version, stream_count, directory = struct.unpack_from("<IIII", data, 0)
if signature != 0x504D444D:
    sys.exit("not a minidump")

streams = {}
for i in range(stream_count):
    stream_type, size, rva = struct.unpack_from("<III", data, directory + i * 12)
    streams[stream_type] = (size, rva)


def read_string(rva):
    (length,) = struct.unpack_from("<I", data, rva)
    return data[rva + 4:rva + 4 + length].decode("utf-16-le")


def short_name(path):
    return path.replace("/", "\\").split("\\")[-1]


modules = []  # (base, size, link timestamp, path)
_, rva = streams[4]
(count,) = struct.unpack_from("<I", data, rva)
for i in range(count):
    base, size, _checksum, timestamp, name_rva = struct.unpack_from("<QIIII", data, rva + 4 + i * 108)
    modules.append((base, size, timestamp, read_string(name_rva)))


def find_module(address):
    for base, size, _timestamp, name in modules:
        if base <= address < base + size:
            return base, name
    return None


ranges = []  # (address, size, file offset) of the memory the dump holds
if 9 in streams:
    _, rva = streams[9]
    count, offset = struct.unpack_from("<QQ", data, rva)
    for i in range(count):
        start, size = struct.unpack_from("<QQ", data, rva + 16 + i * 16)
        ranges.append((start, size, offset))
        offset += size
if 5 in streams:
    _, rva = streams[5]
    (count,) = struct.unpack_from("<I", data, rva)
    for i in range(count):
        start, size, offset = struct.unpack_from("<QII", data, rva + 4 + i * 16)
        ranges.append((start, size, offset))


def read_mem(address, length):
    for start, size, offset in ranges:
        if start <= address and address + length <= start + size:
            return data[offset + (address - start):offset + (address - start) + length]
    return None


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wintypes.ULONG), ("Size", wintypes.ULONG), ("ModBase", ctypes.c_uint64),
                ("Flags", wintypes.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64),
                ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
                ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG), ("Name", wintypes.WCHAR * 1024)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wintypes.DWORD),
                ("FileName", wintypes.LPWSTR), ("Address", ctypes.c_uint64)]


dbghelp = ctypes.WinDLL("dbghelp.dll")
PROCESS = wintypes.HANDLE(0x1234)  # any unique value: no live process is involved
dbghelp.SymSetOptions(0x2 | 0x4 | 0x10)  # undecorate names, deferred loads, load line numbers
dbghelp.SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
dbghelp.SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                     ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
dbghelp.SymFromAddrW.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                 ctypes.POINTER(SYMBOL_INFOW)]
dbghelp.SymGetLineFromAddrW64.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(wintypes.DWORD),
                                          ctypes.POINTER(IMAGEHLP_LINEW64)]
dbghelp.SymInitializeW(PROCESS, None, False)


def image_timestamp(path):
    image = open(path, "rb").read(0x400)
    (pe,) = struct.unpack_from("<I", image, 0x3C)
    return struct.unpack_from("<I", image, pe + 8)[0]


with_symbols = set()
for base, size, timestamp, name in modules:
    image = pdb_map.get(short_name(name).lower())
    if image is None:
        continue
    dbghelp.SymLoadModuleExW(PROCESS, None, image, None, base, size, None, 0)
    with_symbols.add(base)
    same = image_timestamp(image) == timestamp
    print(f"{short_name(name)}: {'same build as the dump' if same else 'NOT the build that crashed, names will be wrong'}")


def symbolize(address):
    module = find_module(address)
    if not module:
        return None
    base, name = module
    text = f"{short_name(name)}+{address - base:#x}"
    if base in with_symbols:
        symbol = SYMBOL_INFOW()
        symbol.SizeOfStruct = 88
        symbol.MaxNameLen = 1023
        displacement = ctypes.c_uint64()
        if dbghelp.SymFromAddrW(PROCESS, address, ctypes.byref(displacement), ctypes.byref(symbol)):
            text += f"  {symbol.Name}+{displacement.value:#x}"
        line = IMAGEHLP_LINEW64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINEW64)
        line_displacement = wintypes.DWORD()
        if dbghelp.SymGetLineFromAddrW64(PROCESS, address, ctypes.byref(line_displacement), ctypes.byref(line)):
            text += f"  [{short_name(line.FileName)}:{line.LineNumber}]"
    return text


_, rva = streams[6]
code, _flags, _record, address, _parameter_count, _pad = struct.unpack_from("<IIQQII", data, rva + 8)
parameters = struct.unpack_from("<15Q", data, rva + 8 + 32)
context_size, context_rva = struct.unpack_from("<II", data, rva + 8 + 152)
print(f"\nexception {code:#x} at {address:#x}  {symbolize(address)}")
if code == 0xC0000005:
    kind = {0: "read", 1: "write", 8: "execute"}.get(parameters[0], "?")
    print(f"  access violation: {kind} at {parameters[1]:#x}")

context = data[context_rva:context_rva + context_size]
names = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
         "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"]
regs = dict(zip(names, struct.unpack_from("<17Q", context, 0x78)))
print("  " + "  ".join(f"{name}={value:#x}" for name, value in regs.items()))

print("\nstack (every slot that points into a module):")
shown = 0
for offset in range(0, 0x8000, 8):
    raw = read_mem(regs["rsp"] + offset, 8)
    if raw is None:
        break
    (value,) = struct.unpack("<Q", raw)
    module = find_module(value)
    if not module or (ONLY and short_name(module[1]).lower() not in ONLY):
        continue
    print(f"  rsp+{offset:#06x}  {symbolize(value)}")
    shown += 1
    if shown >= 400:
        break
