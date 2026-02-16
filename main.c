#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libpad.h>
#include <stdbool.h>
#include "cJSON.h"
#include "sqlite3.h"

// We don't provide anything SQLite related for simplicity's sake
// This does mean that we can't actually write SQLite files
// But we are using only memory SQLite dbs, so it's ok :)
// If we ACTUALLY wanted to, we would need to provide our own SQLite3 VFS and set -DSQLITE_OS_OTHER=1

static char padBuf[256] __attribute__((aligned(64)));

/* Dead-simple BMP loader — reads entire file to memory, converts to CT32 */
static int load_bmp_rgba(GSGLOBAL *gsGlobal, GSTEXTURE *Texture, const char *path,
                         char *dbg, int dbgLen)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        snprintf(dbg, dbgLen, "BMP: fopen failed");
        return -1;
    }

    /* Read entire file */
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    u8 *file = malloc(fileSize);
    if (!file) { fclose(fp); snprintf(dbg, dbgLen, "BMP: malloc failed"); return -1; }

    size_t got = fread(file, 1, fileSize, fp);
    fclose(fp);

    snprintf(dbg, dbgLen, "BMP: read %d/%ld bytes", (int)got, fileSize);
    if ((long)got != fileSize) { free(file); return -1; }

    /* Parse BMP header (BITMAPFILEHEADER + BITMAPINFOHEADER) */
    if (file[0] != 'B' || file[1] != 'M') {
        snprintf(dbg, dbgLen, "BMP: bad magic %02X %02X", file[0], file[1]);
        free(file);
        return -1;
    }

    u32 dataOffset = *(u32 *)&file[10];
    s32 w = *(s32 *)&file[18];
    s32 h = *(s32 *)&file[22];
    u16 bpp = *(u16 *)&file[28];

    /* We only handle 24-bit and 32-bit uncompressed */
    if (bpp != 24 && bpp != 32) {
        snprintf(dbg, dbgLen, "BMP: unsupported bpp=%d", bpp);
        free(file);
        return -1;
    }

    int flipY = (h > 0); /* positive height = bottom-up */
    if (h < 0) h = -h;

    snprintf(dbg, dbgLen, "BMP OK %dx%d bpp=%d off=%u fsz=%ld",
             w, h, bpp, dataOffset, fileSize);

    /* Build CT32 texture */
    memset(Texture, 0, sizeof(*Texture));
    Texture->Width  = w;
    Texture->Height = h;
    Texture->PSM    = GS_PSM_CT32;
    Texture->Filter = GS_FILTER_NEAREST;
    Texture->Mem    = memalign(128, w * h * 4);

    int srcBytesPerPixel = bpp / 8;
    int srcRowBytes = ((w * srcBytesPerPixel + 3) & ~3); /* BMP rows are 4-byte aligned */
    u8 *dst = (u8 *)Texture->Mem;

    for (int row = 0; row < h; row++) {
        int srcRow = flipY ? (h - 1 - row) : row;
        u8 *src = file + dataOffset + srcRow * srcRowBytes;
        for (int col = 0; col < w; col++) {
            u8 b = src[col * srcBytesPerPixel + 0];
            u8 g = src[col * srcBytesPerPixel + 1];
            u8 r = src[col * srcBytesPerPixel + 2];
            dst[(row * w + col) * 4 + 0] = r;
            dst[(row * w + col) * 4 + 1] = g;
            dst[(row * w + col) * 4 + 2] = b;
            dst[(row * w + col) * 4 + 3] = 0x00; /* fully opaque on GS */
        }
    }
    free(file);

    FlushCache(0);

    Texture->Vram = gsKit_vram_alloc(gsGlobal,
        gsKit_texture_size(Texture->Width, Texture->Height, Texture->PSM),
        GSKIT_ALLOC_USERBUFFER);
    if (Texture->Vram == GSKIT_ALLOC_ERROR) {
        snprintf(dbg, dbgLen, "BMP: VRAM alloc failed");
        free(Texture->Mem);
        Texture->Mem = NULL;
        return -1;
    }
    gsKit_texture_upload(gsGlobal, Texture);
    free(Texture->Mem);
    Texture->Mem = NULL;
    return 0;
}

typedef struct {
    float x;
    float y;
    float gravity;
    float horizontal_speed;
    bool on_ground;
} Player;

// This function is like this
// char position[16];
// snprintf(position, 16, "x: %f", player.x);
//
// gsKit_fontm_print_scaled(gsGlobal, fontm, 10.0f, 32.0f, 4, 0.6f, white, position);
//
// But I asked Claude to wrap it in a function to be easier for us to use :3
// thx claude xoxo
void draw_text(GSGLOBAL *gsGlobal, GSFONTM *fontm, float x, float y, int z, float scale, u64 color, const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    gsKit_fontm_print_scaled(gsGlobal, fontm, x, y, z, scale, color, buffer);
}

// According to Claude:
// - 0 (port) — The physical controller port. PS2 has two ports: 0 (player 1) and 1 (player 2).
// - 0 (slot) — The slot within that port. This is 0 for a standard controller plugged directly in. Slots >0 are used with the multitap adapter
// (up to 4 controllers per port).
// ---
// We need to wait for the controller to be ACTUALLY ready for us to be configured
void wait_controller_ready(int port, int slot) {
    int padState = -1;

    while (true) {
        padState = padGetState(port, slot);

        if (padState == PAD_STATE_STABLE || padState == PAD_STATE_FINDCTP1)
            break;
    }
}

int main(void)
{
    struct padButtonStatus buttons;
    unsigned char ljoy_h, ljoy_v;
    float speed = 3.0f;
    int ret;
    GSTEXTURE tex;
    char debugMsg[256];
    printf("Welcome to the next level! :3\n");

    /* Initialize IOP modules */
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);

    /* Initialize gsKit */
    GSGLOBAL *gsGlobal = gsKit_init_global();

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(gsGlobal);

    /* Init ROM font */
    GSFONTM *fontm = gsKit_init_fontm();
    gsKit_fontm_upload(gsGlobal, fontm);

    u64 white = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00);
    u64 green = GS_SETREG_RGBAQ(0x00, 0xFF, 0x00, 0x80, 0x00);
    u64 red   = GS_SETREG_RGBAQ(0xFF, 0x00, 0x00, 0x80, 0x00);

    /* Load BMP texture */
    debugMsg[0] = '\0';
    int texRet = load_bmp_rgba(gsGlobal, &tex, "host:loritta.bmp", debugMsg, sizeof(debugMsg));
    int texLoaded = texRet >= 0;

    u64 bg = texLoaded
        ? GS_SETREG_RGBAQ(0x00, 0x20, 0x00, 0x80, 0x00)
        : GS_SETREG_RGBAQ(0x40, 0x00, 0x00, 0x80, 0x00);
    float size = 64.0f;

    Player player = {
        .x = (gsGlobal->Width  - size) / 2.0f,
        .y = (gsGlobal->Height - size) / 2.0f,
        .gravity = 0.0f
    };

    printf("Initial width and height %d %d and sprite size %f", gsGlobal->Width, gsGlobal->Height, size);

    float height = gsGlobal->Height;

    /* Initialize pad */
    padInit(0);
    padPortOpen(0, 0, padBuf);

    wait_controller_ready(0, 0);

    // Now we need to set the controller's mode
    // According to Claude:
    // - 0, 0 — port 0, slot 0
    // - 1 — enable analog mode (DualShock sticks active)
    // - 3 — lock the mode so the player can't toggle it with the Analog button
    padSetMainMode(0, 0, 1, 3);
    wait_controller_ready(0, 0);

    /* ---- SQLite in-memory database ---- */
    sqlite3 *db = NULL;
    char sqliteMsg[256] = "SQLite: not started";
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        snprintf(sqliteMsg, sizeof(sqliteMsg), "SQLite open err: %s", sqlite3_errmsg(db));
    } else {
        /* Create a table, insert a row, and query it back */
        sqlite3_exec(db,
            "CREATE TABLE greetings(id INTEGER PRIMARY KEY, msg TEXT);"
            "CREATE TABLE positions(id INTEGER PRIMARY KEY, x REAL, y REAL);"
            "INSERT INTO greetings(msg) VALUES('Hello from SQLite on PS2!');"
            "INSERT INTO greetings(msg) VALUES('Loritta is so cute! :3');",
            NULL, NULL, NULL);

        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db, "SELECT msg FROM greetings WHERE id = 1", -1, &stmt, NULL);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            snprintf(sqliteMsg, sizeof(sqliteMsg), "SQLite: %s", sqlite3_column_text(stmt, 0));
        } else {
            snprintf(sqliteMsg, sizeof(sqliteMsg), "SQLite query err: %s", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
    }

    char hewwoMsg[256] = "???";
    cJSON *json = cJSON_Parse("{\"hewwo\":\"Loritta is so cute! :3\"}");
    cJSON *hewwo = cJSON_GetObjectItem(json, "hewwo");

    // This was Claude's original code
    // if (cJSON_IsString(hewwo) && hewwo->valuestring) {
    //     snprintf(hewwoMsg, sizeof(hewwoMsg), "%s", hewwo->valuestring);
    // }

    // Because we don't need all of that, I kept it like this:
    snprintf(hewwoMsg, sizeof(hewwoMsg), "%s", hewwo->valuestring);

    cJSON_Delete(json);

    while (true) {
        int padState = padGetState(0, 0);
        if (padState == PAD_STATE_STABLE || padState == PAD_STATE_FINDCTP1) {
            ret = padRead(0, 0, &buttons);
            if (ret != 0) {
                ljoy_h = buttons.ljoy_h;
                ljoy_v = buttons.ljoy_v;

                /* if (ljoy_h < 118 || ljoy_h > 138)
                    x += (ljoy_h - 128) / 128.0f * speed;
                if (ljoy_v < 118 || ljoy_v > 138)
                    y += (ljoy_v - 128) / 128.0f * speed; */

                // Jumping
                // Originally we used the analog stick like this:
                // if (ljoy_v < 118 || ljoy_v > 138) {
                //     if (player.gravity == 0.0f && player.on_ground) {
                //         player.gravity -= 128.0f;
                //     }
                // }
                // But now we use the X button :)
                // The gamepad uses pressure buttons, but that doesn't work if we don't enter pressure mode
                // So Claude recommended us to use the "btns" bitmask, which works outside of pressure mode
                u32 paddata = 0xFFFF ^ buttons.btns;
                if (paddata & PAD_CROSS) {
                    if (player.gravity == 0.0f && player.on_ground) {
                        player.gravity -= 128.0f;
                    }
                }

                // In PCSX2:
                // ljoy_h == 0 is full left
                // ljoy_h == 255 is full right
                // In a real console I think that you would check it in a range to not force the player to put EXACTLY at full left/right
                if (ljoy_h == 0) {
                    player.horizontal_speed -= 4.0f;
                }

                if (ljoy_h == 255) {
                    player.horizontal_speed += 4.0f;
                }

                printf("My ljoy_h is %d\n", ljoy_h);

                /* if (x < 0) x = 0;
                if (y < 0) y = 0;
                if (x + size > gsGlobal->Width)  x = gsGlobal->Width  - size;
                if (y + size > gsGlobal->Height) y = gsGlobal->Height - size; */
            }
        }

        if (player.horizontal_speed >= 8.0f)
            player.horizontal_speed = 8.0f;
        if (player.horizontal_speed <= -8.0f)
            player.horizontal_speed = -8.0f;

        if (player.y + size >= height && player.gravity >= 0.0f) {
            player.gravity = 0.0f;
            player.y = gsGlobal->Height - size;
            player.on_ground = true;
        } else {
            player.gravity += 4.0f;

            player.y += (player.gravity * 0.1);
            player.on_ground = false;

            if (player.y + size >= height && player.gravity >= 0.0f) {
                player.gravity = 0.0f;
                player.y = gsGlobal->Height - size;
                player.on_ground = true;
            }
        }

        player.x += player.horizontal_speed;

        // Friction
        // TODO: We need to add a epsilon to make the player be "essentially standing still" on very low speeds
        player.horizontal_speed = player.horizontal_speed * 0.9f;

        printf("My y is %f and my gravity is %f and the gsGlobalHeight is %f and the horizontal speed is %f\n", player.y, player.gravity, height, player.horizontal_speed);

        // Store it in the database
        // The -1's here mean "read until null"
        // These are PreparedStatements (woo)
        {
            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(db, "INSERT INTO positions(x, y) VALUES (?, ?);", -1, &stmt, NULL);
            sqlite3_bind_double(stmt, 1, player.x);  // binds to first ?
            sqlite3_bind_double(stmt, 2, player.y);  // binds to second ?
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Get count
        int count = 0;
        double maxX = 0;
        double maxY = 0;
        double minX = 0;
        double minY = 0;
        {
            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(db, "SELECT COUNT(*), MAX(x), MAX(y), MIN(x), MIN(y) FROM positions;", -1, &stmt, NULL);
            sqlite3_step(stmt); // Used to retrieve the next row of data]
            count = sqlite3_column_int(stmt, 0);
            maxX = sqlite3_column_double(stmt, 1);
            maxY = sqlite3_column_double(stmt, 2);
            minX = sqlite3_column_double(stmt, 3);
            minY = sqlite3_column_double(stmt, 4);
            sqlite3_finalize(stmt);
        }

        gsKit_clear(gsGlobal, bg);

        gsKit_prim_sprite(gsGlobal, player.x, player.y, player.x + size, player.y + size, 1, red);

        if (texLoaded) {
            gsKit_prim_sprite_texture(gsGlobal, &tex,
                player.x, player.y, 0.0f, 0.0f,
                player.x + size, player.y + size, tex.Width, tex.Height,
                2, white);
        }

        gsKit_prim_line(gsGlobal, player.x,        player.y,        player.x + size, player.y,        3, green);
        gsKit_prim_line(gsGlobal, player.x + size, player.y,        player.x + size, player.y + size, 3, green);
        gsKit_prim_line(gsGlobal, player.x + size, player.y + size, player.x,        player.y + size, 3, green);
        gsKit_prim_line(gsGlobal, player.x,        player.y + size, player.x,        player.y,        3, green);

        gsKit_fontm_print_scaled(gsGlobal, fontm, 10.0f, 10.0f, 4, 0.6f, white, debugMsg);

        draw_text(gsGlobal, fontm, 10.0f, 32.0f, 4, 0.6f, white, "x: %f", player.x);
        draw_text(gsGlobal, fontm, 10.0f, 48.0f, 4, 0.6f, white, "y: %f", player.y);
        draw_text(gsGlobal, fontm, 10.0f, 64.0f, 4, 0.6f, white, "gravity: %f", player.gravity);
        draw_text(gsGlobal, fontm, 10.0f, 80.0f, 4, 0.6f, white, "horizontal speed: %f", player.horizontal_speed);
        draw_text(gsGlobal, fontm, 10.0f, 96.0f, 4, 0.6f, white, "on ground? %d", player.on_ground);
        draw_text(gsGlobal, fontm, 10.0f, 112.0f, 4, 0.6f, white, "hewwo: %s", hewwoMsg);
        draw_text(gsGlobal, fontm, 10.0f, 128.0f, 4, 0.6f, white, "%s", sqliteMsg);
        draw_text(gsGlobal, fontm, 10.0f, 144.0f, 4, 0.6f, white, "Position Counts: %d", count);
        draw_text(gsGlobal, fontm, 10.0f, 160.0f, 4, 0.6f, white, "minX: %d, minY: %d", (int) minX, (int) minY);
        draw_text(gsGlobal, fontm, 10.0f, 176.0f, 4, 0.6f, white, "maxX: %d, maxY: %d", (int) maxX, (int) maxY);

        /* PS2 system stats */
        struct mallinfo mi = mallinfo();
        draw_text(gsGlobal, fontm, 10.0f, 204.0f, 4, 0.6f, white, "--- PS2 Stats ---");
        draw_text(gsGlobal, fontm, 10.0f, 220.0f, 4, 0.6f, white, "VRAM used: %d KB / 4096 KB", gsGlobal->CurrentPointer / 1024);
        draw_text(gsGlobal, fontm, 10.0f, 236.0f, 4, 0.6f, white, "EE RAM heap used: %d KB", mi.uordblks / 1024);
        draw_text(gsGlobal, fontm, 10.0f, 252.0f, 4, 0.6f, white, "Resolution: %dx%d", gsGlobal->Width, gsGlobal->Height);

        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }

    return 0;
}
