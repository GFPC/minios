path = '/home/greg/phone/minios/radio/cnss.c'

patch = '''    /* MiniOS pm-service stub: override the vendor libperipheral_client.so with our
     * minimal stub immediately after the main staging loop so that cnss-daemon
     * does not spin forever waiting for a pm-service QMI server that does not
     * exist on MiniOS.  The real vendor .so busy-loops inside pm_client_connect()
     * trying to connect to the Peripheral Manager QMI service; our stub grants
     * ACCESS_ALLOWED immediately via a direct callback, allowing cnss-daemon to
     * proceed to the actual WLFW bring-up sequence.
     * The stub .so lives at /lib64/pm_client_stub.so (baked into initramfs by
     * build-minios-hybrid.sh) and is installed here, after the vendor_force[]
     * loop would have just overwritten /lib64/libperipheral_client.so with the
     * broken vendor version. */
    {
        const char *pm_stub = "/lib64/pm_client_stub.so";
        const char *pm_dst  = "/lib64/libperipheral_client.so";
        if (path_exists(pm_stub)) {
            copy_file_bin(pm_stub, pm_dst);
            LOGI("radio", "%s", "pm_client: stub installed over vendor version");
        } else {
            LOGI("radio", "%s", "pm_client: stub not found at /lib64/pm_client_stub.so -- vendor version active");
        }
    }
'''

with open(path, 'r') as f:
    lines = f.readlines()

target = '    unlink("/lib64/libqrtr.so");\n'
idx = next((i for i, l in enumerate(lines) if l == target), None)
if idx is None:
    print('ERROR: target line not found')
else:
    lines.insert(idx, patch + '\n')
    with open(path, 'w') as f:
        f.writelines(lines)
    print(f'OK: inserted stub override before line {idx+1} (now {idx+2})')
