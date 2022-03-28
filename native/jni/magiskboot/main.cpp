#include <string_view>
#include <mincrypt/sha.h>
#include <utils.hpp>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <errno.h>

#include "magiskboot.hpp"
#include "compress.hpp"

using namespace std;

static void print_formats() {
    for (int fmt = GZIP; fmt < LZOP; ++fmt) {
        fprintf(stderr, "%s ", fmt2name[(format_t) fmt]);
    }
}

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
    ramdisk.cpio from their original formats.
    If '-h' is provided, it will dump header info to 'header',
    which will be parsed when repacking.
    Return values:
    0:valid    1:error    2:chromeos

  repack [-n] <origbootimg> [outbootimg]
    Repack boot image components from current directory
    to [outbootimg], or new-boot.img if not specified.
    If '-n' is provided, it will not attempt to recompress ramdisk.cpio,
    otherwise it will compress ramdisk.cpio and kernel with the same format
    as in <origbootimg> if the file provided is not already compressed.
    If env variable PATCHVBMETAFLAG is set to true, all disable flags will
    be set in the vbmeta header.

  hexpatch <file> <hexpattern1> <hexpattern2>
    Search <hexpattern1> in <file>, and replace with <hexpattern2>

  cpio <incpio> [commands...]
    Do cpio commands to <incpio> (modifications are done in-place)
    Each command is a single argument, add quotes for each command
    Supported commands:
      exists ENTRY
        Return 0 if ENTRY exists, else return 1
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
        Test the current cpio's status
        Return value is 0 or bitwise or-ed of following values:
        0x1:Magisk    0x2:unsupported    0x4:Sony
      patch
        Apply ramdisk patches
        Configure with env variables: KEEPVERITY KEEPFORCEENCRYPT
      backup ORIG
        Create ramdisk backups from ORIG
      restore
        Restore ramdisk from ramdisk backup stored within incpio
      sha1
        Print stock boot SHA1 if previously backed up in ramdisk

  dtb <input> <action> [args...]
    Do dtb related actions to <input>
    Supported actions:
      print [-f]
        Print all contents of dtb for debugging
        Specify [-f] to only print fstab nodes
      patch
        Search for fstab and remove verity/avb
        Modifications are done directly to the file in-place
        Configure with env variables: KEEPVERITY

  split <input>
    Split image.*-dtb into kernel + kernel_dtb

  sha1 <file>
    Print the SHA1 checksum for <file>

  cleanup
    Cleanup the current working directory

  compress[=format] <infile> [outfile]
    Compress <infile> with [format] (default: gzip), optionally to [outfile]
    <infile>/[outfile] can be '-' to be STDIN/STDOUT
    Supported formats: )EOF", arg0);

    print_formats();

    fprintf(stderr, R"EOF(

  decompress <infile> [outfile]
    Detect format and decompress <infile>, optionally to [outfile]
    <infile>/[outfile] can be '-' to be STDIN/STDOUT
    Supported formats: )EOF");

    print_formats();

    fprintf(stderr, "\n\n");
    exit(1);
}

static __inline__ int  unix_open(const char*  path, int options,...)
{
    if ((options & O_CREAT) == 0)
    {
        return  TEMP_FAILURE_RETRY( open(path, options) );
    }
    else
    {
        int      mode;
        va_list  args;
        va_start( args, options );
        mode = va_arg( args, int );
        va_end( args );
        return TEMP_FAILURE_RETRY( open( path, options, mode ) );
    }
}


/* Returns the device used to mount a directory in /proc/mounts */
static char *find_mount(const char *dir)
{
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
        SHA_hash(m.buf, m.sz, sha1);
        for (uint8_t i : sha1)
            printf("%02x", i);
        printf("\n");
    } else if (argc > 2 && action == "split") {
        return split_image_dtb(argv[2]);
    } else if (argc > 2 && action == "unpack") {
        int idx = 2;
        bool nodecomp = false;
        bool hdr = false;
        for (;;) {
            if (idx >= argc)
                usage(argv[0]);
            if (argv[idx][0] != '-')
                break;
            for (char *flag = &argv[idx][1]; *flag; ++flag) {
                if (*flag == 'n')
                    nodecomp = true;
                else if (*flag == 'h')
                    hdr = true;
                else
                    usage(argv[0]);
            }
            ++idx;
        }
        return unpack(argv[idx], nodecomp, hdr);
    } else if (argc > 2 && action == "repack") {
        if (argv[2] == "-n"sv) {
            if (argc == 3)
                usage(argv[0]);
            repack(argv[3], argv[4] ? argv[4] : NEW_BOOT, true);
        } else {
            repack(argv[2], argv[3] ? argv[3] : NEW_BOOT);
        }
    } else if (argc > 2 && action == "decompress") {
        decompress(argv[2], argv[3]);
    } else if (argc > 2 && str_starts(action, "compress")) {
        compress(action[8] == '=' ? &action[9] : "gzip", argv[2], argv[3]);
    } else if (argc > 4 && action == "hexpatch") {
        return hexpatch(argv[2], argv[3], argv[4]);
    } else if (argc > 2 && action == "cpio"sv) {
        if (cpio_commands(argc - 2, argv + 2))
            usage(argv[0]);
    } else if (argc > 3 && action == "dtb") {
        if (dtb_commands(argc - 2, argv + 2))
            usage(argv[0]);
    } else {
        usage(argv[0]);
    }

    return 0;
}
