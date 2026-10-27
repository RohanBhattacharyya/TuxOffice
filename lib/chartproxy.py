"""Read Office's NDR chart proxy metadata and write its COM registration.

CHART.DLL's DllRegisterServer initializes Office before registering its proxies;
that initialization fails in regsvr32 under Wine. Read the same constant proxy
tables without loading or executing Office code. No binary offsets are pinned.
Only the x64 compiler's RIP-relative ProxyFileList argument is supported; an
unrecognized layout fails before generating registry changes.
"""
import json
from pathlib import Path
import re
import struct
import sys
import uuid


class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        pe = self.unpack("<I", 0x3c)[0]
        if self.data[pe:pe+4] != b"PE\0\0":
            raise ValueError("Not a PE image")
        machine, count = self.unpack("<HH", pe+4)
        size = self.unpack("<H", pe+20)[0]
        opt = pe+24
        if machine != 0x8664 or self.unpack("<H", opt)[0] != 0x20b:
            raise ValueError("Chart proxy registration currently supports x64 Office")
        self.base = self.unpack("<Q", opt+24)[0]
        self.sections = [self.unpack("<IIII", opt+size+i*40+8) for i in range(count)]
        self.exports = self.unpack("<II", opt+112)

    def unpack(self, fmt, offset):
        return struct.unpack_from(fmt, self.data, offset)

    def offset(self, rva, length=1):
        for virtual_size, address, raw_size, raw in self.sections:
            if address <= rva and rva+length <= address+raw_size:
                offset = rva-address+raw
                if offset+length <= len(self.data):
                    return offset
        raise ValueError("Chart metadata points outside the file")

    def values(self, fmt, rva):
        return self.unpack(fmt, self.offset(rva, struct.calcsize(fmt)))

    def pointer(self, rva):
        value = self.values("<Q", rva)[0]
        return value-self.base if value else None

    def string(self, rva):
        offset = self.offset(rva)
        end = self.data.index(b"\0", offset, min(offset+256, len(self.data)))
        self.offset(rva, end-offset+1)
        return self.data[offset:end].decode("ascii")

    def exported_function(self, name):
        export, size = self.exports
        count, functions, names, ordinals = self.values("<IIII", export+24)
        for index in range(count):
            if self.string(self.values("<I", names+index*4)[0]) != name:
                continue
            ordinal = self.values("<H", ordinals+index*2)[0]
            function = self.values("<I", functions+ordinal*4)[0]
            if export <= function < export+size:
                raise ValueError("Forwarded chart factory export")
            return function
        raise ValueError("Chart factory export missing")


def read_proxy_list(pe, address):
    interfaces = []
    for index in range(16):
        info = pe.pointer(address+index*8)
        if info is None:
            if not interfaces:
                raise ValueError("Empty proxy metadata")
            return interfaces
        stubs, names = pe.pointer(info+8), pe.pointer(info+16)
        count, version = pe.values("<HH", info+40)
        if not stubs or not names or not 1 <= count <= 256 or version not in (1, 2):
            raise ValueError("Unrecognized proxy table")
        for item in range(count):
            stub = pe.pointer(stubs+item*8)
            iid = pe.pointer(stub)
            name = pe.string(pe.pointer(names+item*8))
            methods = pe.values("<I", stub+16)[0]
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", name) or not 3 <= methods <= 256:
                raise ValueError("Invalid chart interface metadata")
            offset = pe.offset(iid, 16)
            guid = "{"+str(uuid.UUID(bytes_le=pe.data[offset:offset+16])).upper()+"}"
            interfaces.append({"iid":guid, "name":name, "methods":methods})
    raise ValueError("Unterminated proxy list")


def chart_interfaces(path):
    pe = PE(path)
    function = pe.exported_function("DllGetClassObject")
    offset = pe.offset(function, 128)
    code = pe.data[offset:offset+128]
    candidates = []
    # NdrDllGetClassObject's fourth x64 argument is the ProxyFileList in R9.
    for match in re.finditer(b"\x4c\x8d\x0d", code):
        displacement = struct.unpack_from("<i", code, match.start()+3)[0]
        address = function+match.start()+7+displacement
        try:
            interfaces = read_proxy_list(pe, address)
            if any(i["name"] == "ChDataSourceFactory" and
                   i["iid"] == "{F07F61DB-0EE4-478D-8052-4C2E04BA33C5}" for i in interfaces):
                candidates.append(interfaces)
        except (ValueError, TypeError, struct.error):
            continue
    if len(candidates) != 1:
        raise ValueError("Could not identify Office's chart proxy registration table")
    return candidates[0]


def registry(interfaces, windows_path):
    factory = interfaces[0]["iid"]  # NdrDllRegisterProxy's default factory CLSID.
    lines = ["Windows Registry Editor Version 5.00", ""]
    for interface in interfaces:
        key = "HKEY_CLASSES_ROOT\\Interface\\"+interface["iid"]
        lines.extend(["["+key+"]", "@="+json.dumps(interface["name"]),
                      "["+key+"\\ProxyStubClsid32]", "@="+json.dumps(factory),
                      "["+key+"\\NumMethods]", "@="+json.dumps(str(interface["methods"])), ""])
    key = "HKEY_CLASSES_ROOT\\CLSID\\"+factory
    lines.extend(["["+key+"]", '@="PSFactoryBuffer"', "["+key+"\\InprocServer32]",
                  "@="+json.dumps(windows_path, ensure_ascii=False), '"ThreadingModel"="Both"', ""])
    return "\n".join(lines)


if __name__ == "__main__":
    try:
        interfaces = chart_interfaces(sys.argv[1])
        Path(sys.argv[3]).write_text(registry(interfaces, sys.argv[2]), encoding="utf-16")
        print(f"Prepared {len(interfaces)} Office chart proxy interfaces")
    except (IndexError, OSError, ValueError, struct.error) as error:
        sys.exit(f"Chart proxy registration: {error}")
