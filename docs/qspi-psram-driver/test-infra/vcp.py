import plistlib, subprocess, sys
ser = sys.argv[1]
data = plistlib.loads(subprocess.run(["ioreg","-a","-r","-l","-c","IOUSBHostDevice"],capture_output=True).stdout)
def find_callout(node):
    if "IOCalloutDevice" in node: return node["IOCalloutDevice"]
    for c in node.get("IORegistryEntryChildren",[]):
        r = find_callout(c)
        if r: return r
def walk(node):
    if node.get("USB Serial Number") == ser:
        r = find_callout(node)
        if r: print(r); sys.exit(0)
    for c in node.get("IORegistryEntryChildren",[]): walk(c)
for n in data: walk(n)
print("NOT FOUND", file=sys.stderr); sys.exit(1)
