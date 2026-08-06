path = '/home/greg/phone/scripts/build-initramfs.sh'
with open(path, 'r') as f:
    content = f.read()

old = '    cp "$OUT/propstub.so" "$ROOT/initramfs/lib64/propstub.so"'
new = '''    cp "$OUT/propstub.so" "$ROOT/initramfs/lib64/propstub.so"
    # MiniOS pm-service stub: compile and install so cnss-daemon does not spin
    # forever on the missing pm-service QMI daemon (MEMORY.md workaround).
    aarch64-linux-gnu-gcc -shared -fPIC -O2 -nostartfiles \\
        -Wl,-soname,libperipheral_client.so \\
        -o "$ROOT/initramfs/lib64/pm_client_stub.so" \\
        "$MINIOS/radio/pm_client_stub.c" -lc 2>&1 || echo "WARN: pm_client_stub compile failed"
    chmod 755 "$ROOT/initramfs/lib64/pm_client_stub.so" 2>/dev/null || true'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, 'w') as f:
        f.write(content)
    print('OK: build-initramfs.sh patched')
else:
    print('ERROR: target line not found')
