/* harness.c — host test executable for the C protocol primitives
 * (jielib + upgrade). Each mode reads bytes from stdin and writes the
 * result to stdout, for bit-to-bit comparison with the Python reference.
 *
 * Usage: harness <mode> [args...]
 *   pack7          stdin: raw bytes -> stdout: 7-bit packets
 *   unpack7        stdin: 7-bit packets -> stdout: raw bytes
 *   response ADDR FL LEN   stdin: LEN data bytes -> stdout: SysEx F0..F7
 *   req-parse      stdin: SysEx F0..F7 -> stdout: "OK addr=.. len=.. fl=.." | "BAD"
 *   id-parse       stdin: SysEx F0..F7 -> stdout: text | "BAD"
 *   mengli         stdin: raw bytes -> stdout: encrypted
 *   cdb OP SUB N   stdin: N args bytes -> stdout: 16 CDB bytes
 *   chipkey        stdin: 34-byte blob -> stdout: "KEY=0x.." | "BAD"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fwsc.h"
#include "jielib.h"
#include "upgrade.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int read_all(uint8_t **out, size_t *out_len)
{
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        return -1;
    }
    for (;;) {
        if (n == cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
        }
        const size_t r = fread(buf + n, 1, cap - n, stdin);
        n += r;
        if (r == 0) {
            break;
        }
    }
    *out = buf;
    *out_len = n;
    return 0;
}

static void write_bytes(const uint8_t *b, size_t n)
{
    fwrite(b, 1, n, stdout);
}

static unsigned long arg_ul(const char *s)
{
    return strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* binary mode on input AND output: the protocol handles raw bytes
     * (otherwise an incoming 0x1A is read as EOF, and an outgoing 0x0A becomes CRLF) */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        fprintf(stderr, "usage: %s <mode> [args]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    uint8_t *in = NULL;
    size_t in_len = 0;

    if (strcmp(mode, "pack7") == 0 || strcmp(mode, "unpack7") == 0 ||
        strcmp(mode, "mengli") == 0) {
        if (read_all(&in, &in_len) != 0) {
            return 1;
        }
        uint8_t out[8192];
        size_t n = 0;
        if (strcmp(mode, "pack7") == 0) {
            n = fw_up_pack7(in, in_len, out);
        } else if (strcmp(mode, "unpack7") == 0) {
            n = fw_up_unpack7(in, in_len, out);
        } else {
            memcpy(out, in, in_len);
            fw_mengli(out, in_len);
            n = in_len;
        }
        write_bytes(out, n);
        free(in);
        return 0;
    }

    if (strcmp(mode, "response") == 0) {
        if (argc < 5) {
            return 2;
        }
        const uint32_t addr = (uint32_t)arg_ul(argv[2]);
        const uint8_t fl = (uint8_t)arg_ul(argv[3]);
        const size_t len = (size_t)arg_ul(argv[4]);
        if (read_all(&in, &in_len) != 0 || in_len != len) {
            return 1;
        }
        uint8_t out[8192];
        const size_t n = fw_up_response(addr, in, len, fl, out, sizeof(out));
        write_bytes(out, n);
        free(in);
        return n ? 0 : 1;
    }

    if (strcmp(mode, "req-parse") == 0 || strcmp(mode, "id-parse") == 0) {
        if (read_all(&in, &in_len) != 0) {
            return 1;
        }
        if (strcmp(mode, "req-parse") == 0) {
            uint32_t addr = 0;
            size_t dlen = 0;
            uint8_t fl = 0;
            if (fw_up_parse_request(in, in_len, &addr, &dlen, &fl)) {
                printf("OK addr=0x%08X len=%u fl=%u\n", addr, (unsigned)dlen, fl);
                return 0;
            }
            printf("BAD\n");
            return 1;
        }
        char text[32];
        if (fw_up_parse_identity(in, in_len, text, sizeof(text))) {
            printf("%s\n", text);
            return 0;
        }
        printf("BAD\n");
        return 1;
    }

    if (strcmp(mode, "cdb") == 0) {
        if (argc < 5) {
            return 2;
        }
        const uint8_t op = (uint8_t)arg_ul(argv[2]);
        const uint8_t sub = (uint8_t)arg_ul(argv[3]);
        const size_t nargs = (size_t)arg_ul(argv[4]);
        if (read_all(&in, &in_len) != 0 || in_len != nargs) {
            return 1;
        }
        uint8_t out[16];
        fw_cdb(op, sub, in, nargs, out);
        write_bytes(out, 16);
        free(in);
        return 0;
    }

    if (strcmp(mode, "chipkey") == 0) {
        if (read_all(&in, &in_len) != 0 || in_len != 34) {
            fprintf(stderr, "chipkey: expected 34 bytes\n");
            return 1;
        }
        uint16_t key = 0;
        if (fw_chip_key(in, &key) != 0) {
            printf("BAD\n");
            return 1;
        }
        printf("KEY=0x%04X\n", key);
        free(in);
        return 0;
    }

    if (strcmp(mode, "product") == 0) {
        if (read_all(&in, &in_len) != 0) {
            return 1;
        }
        char p[FW_MAX_PRODUCT + 1];
        if (fwsc_product_of(in, in_len, p, sizeof(p)) != 0) {
            printf("BAD\n");
            return 1;
        }
        printf("%s\n", p);
        free(in);
        return 0;
    }

    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
