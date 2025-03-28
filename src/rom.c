/* umac ROM-patching code
 *
 * Copyright 2024 Matt Evans
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <inttypes.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "machw.h"
#include "rom.h"

#ifdef DEBUG
#define RDBG(...)       printf(__VA_ARGS__)
#else
#define RDBG(...)       do {} while(0)
#endif

#define RERR(...)       fprintf(stderr, __VA_ARGS__)

#define ROM_PLUSv3_VERSION      0x4d1f8172
#define ROM_PLUSv3_SONYDRV      0x17d30

#define M68K_INST_NOP           0x4e71

////////////////////////////////////////////////////////////////////////////////
// Replacement drivers to thwack over the ROM

static const uint8_t sony_driver[] = {
#include "sonydrv.h"
};


////////////////////////////////////////////////////////////////////////////////


static int      rom_patch1(uint8_t *rom_base, int disp_width, int disp_height, int mem_size);
#if !defined(UMAC_STANDALONE_PATCHER)
int      rom_patch(uint8_t *rom_base) {
        return rom_patch1(rom_base, DISP_WIDTH, DISP_HEIGHT, UMAC_MEMSIZE * 1024);
}
#endif

#undef DISP_WIDTH
#undef DISP_HEIGHT
#undef UMAC_MEMSIZE

static uint32_t rom_get_version(uint8_t *rom_base)
{
#if BYTE_ORDER == LITTLE_ENDIAN
        return __builtin_bswap32(*(uint32_t *)rom_base);
#else
        return *(uint32_t *)rom_base);
#endif
}

/* Not perf-critical, so open-coding to support BE _and_ unaligned access */
#define ROM_WR32(offset, data) do {                                     \
                rom_base[(offset)+0] = ((data) >> 24) & 0xff;           \
                rom_base[(offset)+1] = ((data) >> 16) & 0xff;           \
                rom_base[(offset)+2] = ((data) >> 8) & 0xff;            \
                rom_base[(offset)+3] = (data) & 0xff;                   \
        } while (0)
#define ROM_WR16(offset, data) do {                             \
                rom_base[(offset)+0] = ((data) >> 8) & 0xff;    \
                rom_base[(offset)+1] = (data) & 0xff;           \
        } while (0)
#define ROM_WR8(offset, data) do {                              \
                rom_base[(offset)+0] = (data) & 0xff;           \
        } while (0)


static int     rom_patch_plusv3(uint8_t *rom_base, int disp_width, int disp_height, int ram_size)
{
        /* Inspired by patches in BasiliskII!
         */

        /* Disable checksum check by bodging out the comparison, an "eor.l d3, d1",
         * into a simple eor.l d1,d1:
         */
        ROM_WR16(0xd92, 0xb381 /* eor.l d1, d1 */);     // Checksum compares 'same' kthx

        /* Replace .Sony driver: */
        memcpy(rom_base + ROM_PLUSv3_SONYDRV, sony_driver, sizeof(sony_driver));
        /* Register the FaultyRegion for the Sony driver: */
        ROM_WR32(ROM_PLUSv3_SONYDRV + sizeof(sony_driver) - 4, PV_SONY_ADDR);

        /* To do:
         *
         * - No IWM init
         * - new Sound?
         */
        if (ram_size > 128*1024 && ram_size < 512*1024) {
                /* Hack to change memtop: try out a 256K Mac :) */
                for (int i = 0x376; i < 0x37e; i += 2)
                        ROM_WR16(i, M68K_INST_NOP);
                ROM_WR16(0x376, 0x2a7c); // moveal #ram_size, A5
                ROM_WR16(0x378, ram_size >> 16);
                ROM_WR16(0x37a, ram_size & 0xffff);
                /* That overrides the probed memory size, but
                 * P_ChecksumRomAndTestMemory returns a failure code for
                 * things that aren't 128/512.  Skip that:
                 */
                ROM_WR16(0x132, 0x6000); // Bra (was BEQ)
                /* FIXME: We should also remove the memory probe routine, by
                 * allowing the ROM checksum to fail (it returns failure, then
                 * we carry on).  This avoids wild RAM addrs being accessed.
                 */

                /* Fix up the sound buffer as used by BootBeep */
                ROM_WR32(0x292, ram_size - 768);
        }

        if(disp_width != 512 || disp_height != 342) {
                int screen_size = (disp_width*disp_height/8);
                int screen_distance_from_top = screen_size + 0x380;
                int screen_base = 0x400000-screen_distance_from_top;
printf("screen size=%d screen_base=%x\n", screen_size, screen_base);
#define SBCOORD(x, y)                   (screen_base + ((disp_width/8)*(y)) + ((x)/8))

                /* Changing video res:
                 *
                 * Original 512*342 framebuffer is 0x5580 bytes; the screen
                 * buffer lands underneath sound/other buffers at top of mem,
                 * i,e, 0x3fa700 = 0x400000-0x5580-0x380.  So any new buffer
                 * will be placed (and read out from for the GUI) at
                 * MEM_TOP-0x380-screen_size.
                 *
                 * For VGA, size is 0x9600 bytes (0x2580 words)
                 */

                /* We need some space, low down, to create jump-out-and-patch
                 * routines where a patch is too large to put inline.  The
                 * TestSoftware check at 0x42 isn't used:
                 */
                ROM_WR16(0x42, 0x6000);                 /* bra */
                ROM_WR16(0x44, 0x62-0x44);              /* offset */
                /* Now 0x46-0x57 can be used */
                unsigned int patch_0 = 0x46;
                ROM_WR16(patch_0 + 0, 0x9bfc);          /* suba.l #imm32, A5 */
                ROM_WR32(patch_0 + 2, screen_distance_from_top);
                ROM_WR16(patch_0 + 6, 0x6000);          /* bra */
                ROM_WR16(patch_0 + 8, 0x3a4 - (patch_0 + 8));   /* Return to 3a4 */

                // Additional patches needed if DISP_WIDTH is 1024 or above

                unsigned int patch_2 = 0x32;
                unsigned int patch_1 = patch_0 + 10;
                if ((disp_width / 8) >= 128) {
                        ROM_WR16(patch_1 + 0, 0x3a3c);           /* move.l ..., D5 */
                        ROM_WR16(patch_1 + 2, disp_width / 8);   /*        ^^^ */
                        ROM_WR16(patch_1 + 4, 0xc2c5);           /* mulu D5, D1 */
                        ROM_WR16(patch_1 + 6, 0x4e75);           /* rts */
                        if (patch_1 + 8 > 0x58) {
                            RERR("patch_1 extends too far (0x%x > 0x58)\n", patch_1 + 8);
                            return -1;
                        }

                        // is this the illegal instruction handler entry? if it is, it
                        // eventually falls through to 'check if test software exists',
                        // below.... @sc's annotated disassembly suggests "never called by the mac plus"
                        // but it looks to me like 0x2e is in the vector table at 0x16...
                        // patch it to jump down to after the test software check too.
                        ROM_WR16(0x2e, 0x6000);                 /* bra */
                        ROM_WR16(0x30, 0x62-0x30);              /* offset */

                        ROM_WR16(patch_2 + 0, 0x303c);           /* move.l ..., D0 */
                        ROM_WR16(patch_2 + 2, disp_width / 8);   /*        ^^^ */
                        ROM_WR16(patch_2 + 4, 0x41f8);           /* Lea.L     (CrsrSave), A0 */
                        ROM_WR16(patch_2 + 6, 0x088c);           /*            ^^^^^^^^ */
                        ROM_WR16(patch_2 + 8, 0x4e75);           /* rts */
                        if (patch_2 + 10 > 0x41) {
                                RERR("patch_2 extends too far (0x%x > 0x41)\n", patch_2);
                                return -1;
                        }
                }

                /* Magic screen-related locations in Mac Plus ROM 4d1f8172:
                 *
                 * 8c : screen base addr (usually 3fa700, now 3f6680)
                 * 148 : screen base addr again
                 * 164 : u32 screen address of crash Mac/critErr hex numbers
                 * 188 : u16 bytes per row (critErr)
                 * 194 : u16 bytes per row (critErr)
                 * 19c : u16 (bytes per row * 6)-1 (critErr)
                 * 1a4 : u32 screen address of critErr twiddly pattern
                 * 1ee : u16 screen sie in words minus one
                 * 3a2 : u16 screen size in bytes (BUT can't patch immediate)
                 * 474 : u16 bytes per row
                 * 494 : u16 screen y
                 * 498 : u16 screen x
                 * a0e : y
                 * a10 : x
                 * ee2 : u16 bytes per row minus 4 (tPutIcon)
                 * ef2 : u16 bytes per row (tPutIcon)
                 * 7e0 : u32 screen address of disk icon (240, 145)
                 * 7f2 : u32 screen address of disk icon's symbol (248, 160)
                 * f0c : u32 screen address of Mac icon (240, 145)
                 * f18 : u32 screen address of Mac icon's face (248, 151)
                 * f36 : u16 bytes per row minus 2 (mPutSymbol)
                 * 1cd1 : hidecursor's bytes per line
                 * 1d48 : xres minus 32 (for cursor rect clipping)
                 * 1d4e : xres minus 32
                 * 1d74 : y
                 * 1d93 : bytes per line (showcursor)
                 * 1e68 : y
                 * 1e6e : x
                 * 1e82 : y
                 */
                ROM_WR32(0x8a, screen_base);
                ROM_WR32(0x146, screen_base);
                ROM_WR32(0x164, SBCOORD(disp_width/2 - (48/2), disp_height/2 + 8));
                ROM_WR16(0x188, disp_width/8);
                ROM_WR16(0x194, disp_width/8);
                ROM_WR16(0x19c, (6*disp_width/8)-1);
                ROM_WR32(0x1a4, SBCOORD(disp_width/2 - 8, disp_height/2 + 8 + 8));
                ROM_WR16(0x1ee, (screen_size/4)-1);

                ROM_WR32(0xf0c, SBCOORD(disp_width/2 - 16, disp_height/2 - 26));
                ROM_WR32(0xf18, SBCOORD(disp_width/2 - 8, disp_height/2 - 20));
                ROM_WR32(0x7e0, SBCOORD(disp_width/2 - 16, disp_height/2 - 26));
                ROM_WR32(0x7f2, SBCOORD(disp_width/2 - 8, disp_height/2 - 11));

                /* Patch "SubA #$5900, A5" to subtract 0x9880.
                 * However... can't just patch the int16 immediate, as that's
                 * sign-extended (and we end up with a subtract-negative,
                 * i.e. an add).  There isn't space here to turn it into sub.l
                 * so add some rigamarole to branch to some bytes stolen at
                 * patch_0 up above.
                 */
                ROM_WR16(0x3a0, 0x6000);                /* bra */
                ROM_WR16(0x3a2, patch_0 - 0x3a2);       /* ...to patch0, returns at 0x3a4 */

                ROM_WR16(0x474, disp_width/8);
                ROM_WR16(0x494, disp_height);
                ROM_WR16(0x498, disp_width);
                ROM_WR16(0xa0e, disp_height);           /* copybits? */
                ROM_WR16(0xa10, disp_width);
                ROM_WR16(0xee2, (disp_width/8)-4);      /* tPutIcon bytes per row, minus 4 */
                ROM_WR16(0xef2, disp_width/8);          /* tPutIcon bytes per row */
                ROM_WR16(0xf36, (disp_width/8)-2);      /* tPutIcon bytes per row, minus 2 */

                // getting the stride of the framebuffer for hidecursor
                if ((disp_width / 8) >= 128) {
                        ROM_WR16(0x1ccc, 0x4eba);               /* (hidecursor) jsr */
                        ROM_WR16(0x1cce, patch_2 - 0x1cce);     /* .. to patch2, returns at 1cd0 */
                        ROM_WR16(0x1cd0, 0x4e71);               /* nop */
                } else {
                        ROM_WR8(0x1cd1, disp_width/8);         /* hidecursor */
                }

                ROM_WR16(0x1d48, disp_width-32);        /* 1d46+2 was originally 512-32 rite? */
                ROM_WR16(0x1d4e, disp_width-32);        /* 1d4c+2 is 480, same */
                ROM_WR16(0x1d6e, disp_height-16);       /* showcursor (YESS fixed Y crash bug!) */
                ROM_WR16(0x1d74, disp_height);          /* showcursor */
                ROM_WR8(0x1d93, disp_width/8);          /* showcursor */
                ROM_WR16(0x1e68, disp_height);          /* mScrnSize */
                // getting the stride of the framebuffer for showcursor
                if ((disp_width / 8) >= 128) {
                        ROM_WR16(0x1d92, 0x4eba);               /* jsr */
                        ROM_WR16(0x1d94, patch_1 - 0x1d94);     /* .. to patch1, returns at 1d96 */
                } else {
                        ROM_WR8(0x1d93, disp_width/8);          /* showcursor */
                }
                ROM_WR16(0x1e6e, disp_width);           /* mScrnSize */
                ROM_WR16(0x1e82, disp_height);          /* tScrnBitMap */
        }

        /* FIXME: Welcome To Macintosh is drawn at the wrong position. Find where that's done. */
        return 0;
}

static int      rom_patch1(uint8_t *rom_base, int disp_width, int disp_height, int mem_size)
{
        uint32_t v = rom_get_version(rom_base);
        int r = -1;
        /* See https://docs.google.com/spreadsheets/d/1wB2HnysPp63fezUzfgpk0JX_b7bXvmAg6-Dk7QDyKPY/edit#gid=840977089
         */
        switch(v) {
        case ROM_PLUSv3_VERSION:
                r = rom_patch_plusv3(rom_base, disp_width, disp_height, mem_size);
                break;

        default:
                RERR("Unknown ROM version %08x, no patching", v);
        }

        return r;
}

#if defined(UMAC_STANDALONE_PATCHER)
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char **argv) {
        uint8_t *rom_base;
        char *rom_filename = "4D1F8172 - MacPlus v3.ROM";
        char *rom_dump_filename = NULL; // Raw bytes
        char *rom_header_filename = NULL; // C header
        int ch;
        int disp_width = 512;
        int disp_height = 342;
        int ram_size = 128;

        while ((ch = getopt(argc, argv, "vh:w:m:r:o:W:")) != -1) {
                switch (ch) {
                case 'v':
                        disp_width = 640;
                        disp_height = 480;
                        break;
                case 'w':
                        disp_width = atoi(optarg);
                        break;
                case 'h':
                        disp_height = atoi(optarg);
                        break;
                case 'm':
                        ram_size = atoi(optarg);
                        break;
                case 'r':
                        rom_filename = strdup(optarg);
                        break;
                case 'W':
                        rom_dump_filename = strdup(optarg);
                        break;
                case 'o':
                        rom_header_filename = strdup(optarg);
                        break;
                case '?':
                        abort();
                }
        }
        if (!rom_dump_filename && !rom_header_filename) {
                printf("Must specify either a -W (binary) or -o (C header) output file");
                abort();
        }
        printf("Opening ROM '%s'\n", rom_filename);
        int ofd = open(rom_filename, O_RDONLY);
        if (ofd < 0) {
                perror("ROM");
                return 1;
        }

        struct stat sb;
        fstat(ofd, &sb);
        off_t _rom_size = sb.st_size;
        rom_base = mmap(0, _rom_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, ofd, 0);
        if (rom_base == MAP_FAILED) {
                printf("Can't mmap ROM!\n");
                return 1;
        }
        if (rom_patch1(rom_base, disp_width, disp_height, ram_size*1024)) {
                printf("Failed to patch ROM\n");
                return 1;
        }
        printf("Patched ROM for screen size %dx%d\n", disp_width, disp_height);
        if (rom_dump_filename) {
                int rfd = open(rom_dump_filename, O_CREAT | O_TRUNC | O_RDWR, 0655);
                if (rfd < 0) {
                        perror("ROM dump");
                        return 1;
                }
                ssize_t written = write(rfd, rom_base, _rom_size);
                if (written < 0) {
                        perror("ROM dump write");
                        return 1;
                }
                if (written < _rom_size) {
                        printf("*** WARNING: Short write to %s!\n",
                               rom_dump_filename);
                } else {
                        printf("Dumped ROM to %s\n", rom_dump_filename);
                }
                close(rfd);
        }
        if (rom_header_filename) {
            FILE *ofd = fopen(rom_header_filename, "w");
            if (!ofd) { perror("fopen"); abort(); }
            for(off_t i=0; i<_rom_size; i++) {
                fprintf(ofd, "%d,", rom_base[i]);
                if(i % 16 == 15) fprintf(ofd, "\n");
            }
            fprintf(ofd, "\n");
            printf("Dumped ROM to %s as header\n", rom_header_filename);
            fclose(ofd);
        }
}
#endif
