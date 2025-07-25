#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <vector>

#include <base.hpp>
#include <consts.hpp>
#include <core.hpp>
#include <boot/bootimg.hpp>

using namespace std;

static void usage(char *arg0) {
    fprintf(stderr,
R"EOF(MagiskBoot - Boot Image Modification Tool

Usage: %s <action> [args...]

Supported actions:
  remount
  unpack [-n] [-h] <bootimg>
    Unpack <bootimg> to, if available, kernel, kernel_dtb, ramdisk.cpio,
    second, dtb, extra, and recovery_dtbo into current directory.
    If '-n' is provided, it will not attempt to decompress kernel or
    ramdisk for MTK style images.
    If '-h' is provided, it will dump header info to 'header',
    which will be parsed when repacking.
    Return values:
    0:valid    1:error    2:chromeos

  repack [-n] <origbootimg> [outbootimg]
    Repack boot image components from current directory
    to [outbootimg], or new-boot.img if not specified.
    If '-n' is provided, it will not attempt to recompress ramdisk
    for MTK style images.

  decompress <comp_type> <infile> [outfile]
    Decompress <infile> with <comp_type> to [outfile], or result to stdout
    if not specified.
    <comp_type> can be one of: gzip, lz4, lz4_legacy, lzma, xz, bzip2, lz4_lg, zstd
    Return values:
    0:valid    1:error

  compress <comp_type> <infile> [outfile]
    Compress <infile> with <comp_type> to [outfile], or result to stdout
    if not specified.
    <comp_type> can be one of: gzip, lz4, lz4_legacy, lzma, xz, bzip2, lz4_lg, zstd
    Return values:
    0:valid    1:error

  dtb <command> <dtb>
    Do various operations with device tree binary.
    Supported commands:
    decode <dtb> [out] : Decode dtb to [out], or stdout if not specified
    encode <dts> [out] : Encode dts to [out], or stdout if not specified
    test <dtb>        : Test the integrity of dtb
    print <dtb>       : Print human readable strings in dtb to stdout
    patch <dtb>       : Search for fstab and remove verity/avb
    patch_skip <dtb>  : Search for fstab and remove dm=
    patch_fstab <dtb> : Search for fstab and add flags
    patch_prop <dtb>  : Search for prop entry and patch value

  hexpatch <file> <hexpattern1> <hexpattern2>
    Search <hexpattern1> in <file> and replace with <hexpattern2>

  cpio <incpio> [commands...]
    Do cpio commands to <incpio> (modifications are done directly)
    Each command is a single argument:
    rm [-r] ENTRY
      Remove ENTRY, specify [-r] to remove recursively
    mkdir MODE ENTRY
      Create directory ENTRY in permissions MODE
    ln TARGET ENTRY
      Create a symlink to TARGET with the name ENTRY
    mv SOURCE DEST
      Move SOURCE to DEST
    add MODE ENTRY INFILE
      Add INFILE as ENTRY in permissions MODE; replaces ENTRY if exists
    extract [ENTRY OUT]
      Extract ENTRY to OUT, or extract all entries to current directory
    test
      Test the current cpio's patch status
      Return values:
      0:stock    1:Magisk    2:unsupported (phh, SuperSU, Xposed)
    patch
      Apply ramdisk patches
    backup ORIG
      Create ramdisk backups from ORIG
    restore
      Restore ramdisk from local backup

  dtb-<cmd> <dtb> [args...]
    Shorthand for dtb <cmd> <dtb> [args...]
    All dtb commands listed above are accessible with shorthand

  compress[-cmd] <infile> [outfile]
    Shorthand for compress <cmd> <infile> [outfile]
    All compression formats are accessible with shorthand

  decompress[-cmd] <infile> [outfile]
    Shorthand for decompress <cmd> <infile> [outfile]
    All compression formats are accessible with shorthand

  sha1 <file>
    Print the SHA1 checksum for <file>

  cleanup
    Cleanup the current working directory

)EOF", arg0);
    exit(1);
}

static __inline__ int unix_open(const char* path, int options,...) {
    if ((options & O_CREAT) == 0) {
        return TEMP_FAILURE_RETRY(open(path, options));
    } else {
        int mode;
        va_list args;
        va_start(args, options);
        mode = va_arg(args, int);
        va_end(args);
        return TEMP_FAILURE_RETRY(open(path, options, mode));
    }
}

/* Returns the device used to mount a directory in /proc/mounts */
static char *find_mount(const char *dir) {
    int fd;
    int res;
    int size;
    char *token = NULL;
    const char delims[] = "\n";
    char buf[4096];
    fd = unix_open("/proc/mounts", O_RDONLY);
    if (fd < 0)
        return NULL;
    buf[sizeof(buf) - 1] = '\0';
    size = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    token = strtok(buf, delims);
    while (token) {
        char mount_dev[256];
        char mount_dir[256];
        int mount_freq;
        int mount_passno;
        res = sscanf(token, "%255s %255s %*s %*s %d %d\n",
                     mount_dev, mount_dir, &mount_freq, &mount_passno);
        mount_dev[255] = 0;
        mount_dir[255] = 0;
        if (res == 4 && (strcmp(dir, mount_dir) == 0))
            return strdup(mount_dev);
        token = strtok(NULL, delims);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    cmdline_logging();
    umask(0);

    if (argc < 2)
        usage(argv[0]);

    // Skip '--' for backwards compatibility
    string_view action(argv[1]);
    if (str_starts(action, "--"))
        action = argv[1] + 2;

    if (action == "cleanup") {
        fprintf(stderr, "Cleaning up...\n");
        unlink(HEADER_FILE);
        unlink(KERNEL_FILE);
        unlink(RAMDISK_FILE);
        unlink(SECOND_FILE);
        unlink(KER_DTB_FILE);
        unlink(EXTRA_FILE);
        unlink(RECV_DTBO_FILE);
        unlink(DTB_FILE);
    } else if (argc >= 2 && action == "remount") {
        char *dev;
        int fd;
        int OFF = 0;
        dev = find_mount("/system");
        if (!dev)
            return -1;
        fd = unix_open(dev, O_RDONLY);
        if (fd < 0)
            return -1;
        ioctl(fd, BLKROSET, &OFF);
        close(fd);
        int system_ro = mount(dev, "/system", "none", MS_REMOUNT, NULL);
        free(dev);
        printf("%d\n",system_ro);
    } else if (argc > 2 && action == "sha1") {
        uint8_t sha1[SHA_DIGEST_SIZE];
        auto m = mmap_data(argv[2]);
        SHA_CTX ctx;
        SHA1_Init(&ctx);
        SHA1_Update(&ctx, m.buf, m.sz);
        SHA1_Final(sha1, &ctx);
        for (uint8_t i : sha1)
            printf("%02x", i);
        printf("\n");
    } else if (action == "unpack") {
        int idx = 2;
        bool hdr = false;
        bool nocomp = false;
        for (;;) {
            if (idx >= argc)
                usage(argv[0]);
            if (argv[idx][0] != '-')
                break;
            for (char *flag = &argv[idx][1]; *flag; ++flag) {
                if (*flag == 'n')
                    nocomp = true;
                else if (*flag == 'h')
                    hdr = true;
            }
            ++idx;
        }
        return unpack(argv[idx], hdr, nocomp);
    } else if (action == "repack") {
        if (argc < 3)
            usage(argv[0]);
        char *orig = argv[2];
        const char *out = argc > 3 ? argv[3] : NEW_BOOT;
        bool nocomp = false;
        if (argv[2] == "-n"sv) {
            nocomp = true;
            orig = argv[3];
            out = argc > 4 ? argv[4] : NEW_BOOT;
        }
        repack(orig, out, nocomp);
    } else if (action == "decompress") {
        if (argc < 4)
            usage(argv[0]);
        decompress(argv[2], argv[3], argv[4] ? argv[4] : "/dev/stdout");
    } else if (action == "compress") {
        if (argc < 4)
            usage(argv[0]);
        compress(argv[2], argv[3], argv[4] ? argv[4] : "/dev/stdout");
    } else if (action == "dtb") {
        if (argc < 4)
            usage(argv[0]);
        auto dtb = mmap_data(argv[3]);
        if (argv[2] == "print"sv)
            dtb_print(dtb);
        else if (argv[2] == "patch"sv)
            dtb_patch(dtb);
        else if (argv[2] == "patch_skip"sv)
            dtb_patch_skip(dtb);
        else if (argv[2] == "patch_fstab"sv)
            dtb_patch_fstab(dtb);
        else if (argv[2] == "patch_prop"sv)
            dtb_patch_prop(dtb);
        else if (argv[2] == "test"sv)
            exit(!dtb_test(dtb));
        else if (argv[2] == "decode"sv)
            dtb_decode(dtb, argv[4] ? argv[4] : "/dev/stdout");
        else if (argv[2] == "encode"sv)
            dtb_encode(dtb, argv[4] ? argv[4] : "/dev/stdout");
        else
            usage(argv[0]);
    } else if (action == "cpio") {
        if (argc < 3)
            usage(argv[0]);
        if (cpio_commands(argc - 2, argv + 2))
            usage(argv[0]);
    } else if (str_starts(action, "compress-")) {
        if (argc < 3)
            usage(argv[0]);
        compress(action.substr(9), argv[2], argv[3] ? argv[3] : "/dev/stdout");
    } else if (str_starts(action, "decompress-")) {
        if (argc < 3)
            usage(argv[0]);
        decompress(action.substr(11), argv[2], argv[3] ? argv[3] : "/dev/stdout");
    } else if (str_starts(action, "dtb-")) {
        if (argc < 3)
            usage(argv[0]);
        char *cmd = argv[1] + 4;
        auto dtb = mmap_data(argv[2]);
        if (cmd == "print"sv)
            dtb_print(dtb);
        else if (cmd == "patch"sv)
            dtb_patch(dtb);
        else if (cmd == "patch_skip"sv)
            dtb_patch_skip(dtb);
        else if (cmd == "patch_fstab"sv)
            dtb_patch_fstab(dtb);
        else if (cmd == "patch_prop"sv)
            dtb_patch_prop(dtb);
        else if (cmd == "test"sv)
            exit(!dtb_test(dtb));
        else if (cmd == "decode"sv)
            dtb_decode(dtb, argv[3] ? argv[3] : "/dev/stdout");
        else if (cmd == "encode"sv)
            dtb_encode(dtb, argv[3] ? argv[3] : "/dev/stdout");
        else
            usage(argv[0]);
    } else if (action == "hexpatch") {
        if (argc < 4)
            usage(argv[0]);
        return hexpatch(argv[2], argv[3], argv[4]);
    } else {
        usage(argv[0]);
    }

    return 0;
}
