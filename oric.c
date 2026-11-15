/*
 * ORIC ATMOS  -  émulateur Win32 pur (GDI + waveOut + shell32), aucune lib externe.
 *
 * Compilation (Direct3D 11 est charge dynamiquement : rien de plus a lier) :
 *   MinGW :  gcc -O2 -s -mwindows oric.c -o oric.exe -lgdi32 -lwinmm -lshell32 -luser32
 *   MSVC  :  cl /O2 oric.c /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib winmm.lib shell32.lib
 *
 * Fichiers à fournir à côté de l'exe (ou dans .\roms\) :
 *   basic11b.rom  (ROM Atmos 16384 octets)      microdis.rom (ROM Microdisc, 8192 octets, pour les disquettes)
 *
 * Touches :
 *   F1 selecteur de fichiers   F2 effet CRT          F3 rembobiner la K7   F4 filtre suivant
 *   F5  turbo CPU (cadence IRQ/video/son d'origine)   F10 turbo total (tout accelere)
 *   F6  acceleration K7 + disquette (chargement instantane / turbo pendant les acces)
 *   F7  pause    F8 (ou Impr. ecran) capture d'ecran PNG    Inser enregistre un snapshot    Debut recharge le dernier
 *   clic droit : menu des reglages
 *   F11 soft reset (NMI)       F12 hard reset        Alt+Entree plein ecran
 *   F9  statistiques de diagnostic dans la barre de titre (MHz, images/s, IRQ/s, timer VIA)
 *   (les scancodes de ces touches sont modifiables dans settings.ini, section [Keys])
 *
 * Test du coeur sous Linux :  gcc -DORIC_TEST -O1 -Wall oric.c -lm -o oric_test && ./oric_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi.h>
#else
#include <strings.h>
#define MAX_PATH 260
#define _stricmp strcasecmp
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;

/* ====================================================================== */
/*  Reglages (settings.ini)                                                */
/* ====================================================================== */
static char exe_dir[MAX_PATH], ini_path[MAX_PATH];

#define NFILTERS 7
static const char *filter_names[NFILTERS] = {
    "Nearest (pixels nets)", "Bilinear", "Bicubic (Catmull-Rom)", "Scale2x", "Scale3x", "Eagle",
    "Lissage directionnel (maison)"
};

static struct {
    char rom[MAX_PATH];        /* ROM BASIC par defaut (vide = recherche auto)   */
    char mdrom[MAX_PATH];      /* ROM Microdisc                                   */
    char browse[MAX_PATH];     /* dernier dossier du selecteur                    */
    char lastfile[MAX_PATH];   /* dernier fichier choisi dans ce dossier           */
    int  start_browser;        /* 1 = demarrer sur le selecteur                   */
    int  fullscreen;
    int  volume;               /* 0..100                                          */
    int  loadsnd;              /* son de chargement K7/disquette                  */
    int  loadvol;              /* 0..100 (volume du son de chargement, faible)    */
    int  azerty;               /* 1 = lettres selon legendes AZERTY               */
    int  writable_disks;
    int  hook;                 /* adresse du vecteur lecture octet K7, 0 = auto   */
    int  k_hard, k_soft, k_browser, k_turbo, k_rewind, k_stats, k_scan;   /* scancodes */
    int  scanlines, scan_strength;   /* effet CRT (F2) et sa force 0..100 */
    int  crt_glow, crt_flicker;      /* halo 0..100, scintillement 0..100 */
    int  filter, k_aa;               /* filtre de mise a l'echelle (F4 = suivant) */
    int  fast_tape, fast_disk, k_fast;   /* acceleration K7 / disquette (F6 : les deux) */
    int  k_pause, k_shot, k_snap, k_snapload;   /* pause, capture d'ecran, snapshot (enregistrer / charger le dernier) */
    char shot_dir[MAX_PATH], snap_dir[MAX_PATH];  /* dossiers captures / snapshots */
    int  renderer;                   /* 0 = auto (GPU si possible), 1 = Direct3D 11, 2 = GDI (CPU) */
    int  cpu_turbo, cpu_mult, k_cputurbo;   /* turbo CPU a cadence IRQ d'origine (F5), facteur */
} cfg;

static void ui_error(const char *msg);

/* ---- journal de diagnostic (settings.ini : [Debug] log=1  ->  oric.log a cote de l'exe) ---- */
static int dbg_on, dbg_lines;
static FILE *dbg_f;
static u16 PC;
static void dbg_log(const char *fmt, ...)
{
    va_list ap;
    if (!dbg_on || dbg_lines > 20000) return;
    if (!dbg_f) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s/oric.log", exe_dir);
        dbg_f = fopen(path, "w");
        if (!dbg_f) { dbg_on = 0; return; }
    }
    dbg_lines++;
    va_start(ap, fmt);
    vfprintf(dbg_f, fmt, ap);
    va_end(ap);
    fputc('\n', dbg_f);
    fflush(dbg_f);
}

#ifdef _WIN32
static int ini_int(const char *sec, const char *key, int def)
{
    char b[64], d[32];
    snprintf(d, sizeof d, "%d", def);
    GetPrivateProfileStringA(sec, key, d, b, sizeof b, ini_path);
    return (int)strtol(b, NULL, 0);
}
static void ini_str(const char *sec, const char *key, const char *def, char *out, int n)
{
    GetPrivateProfileStringA(sec, key, def, out, n, ini_path);
}
static void ini_put(const char *sec, const char *key, const char *val)
{
    WritePrivateProfileStringA(sec, key, val, ini_path);
}
#else
static int ini_int(const char *s, const char *k, int d) { (void)s; (void)k; return d; }
static void ini_str(const char *s, const char *k, const char *d, char *o, int n) { (void)s; (void)k; snprintf(o, n, "%s", d); }
static void ini_put(const char *s, const char *k, const char *v) { (void)s; (void)k; (void)v; }
#endif

static void ini_puti(const char *sec, const char *key, int v, int hex)
{
    char b[32];
    snprintf(b, sizeof b, hex ? "0x%02X" : "%d", v);
    ini_put(sec, key, b);
}

static void settings_save(void)
{
    ini_put("Oric", "rom", cfg.rom);
    ini_put("Oric", "microdisc_rom", cfg.mdrom);
    ini_put("Oric", "browse_dir", cfg.browse);
    ini_put("Oric", "browse_file", cfg.lastfile);
    ini_puti("Oric", "start_in_browser", cfg.start_browser, 0);
    ini_puti("Oric", "fullscreen", cfg.fullscreen, 0);
    ini_puti("Oric", "writable_disks", cfg.writable_disks, 0);
    ini_puti("Oric", "cpu_turbo", cfg.cpu_turbo, 0);
    ini_puti("Oric", "fast_tape", cfg.fast_tape, 0);
    ini_puti("Oric", "fast_disk", cfg.fast_disk, 0);
    ini_puti("Keys", "fast_load", cfg.k_fast, 1);
    ini_puti("Keys", "pause", cfg.k_pause, 1);
    ini_puti("Keys", "screenshot", cfg.k_shot, 1);
    ini_puti("Keys", "snapshot_save", cfg.k_snap, 1);
    ini_puti("Keys", "snapshot_load_last", cfg.k_snapload, 1);
    ini_put("Paths", "screenshots", cfg.shot_dir);
    ini_put("Paths", "snapshots", cfg.snap_dir);
    ini_puti("Debug", "log", dbg_on, 0);
    ini_puti("Oric", "cpu_turbo_factor", cfg.cpu_mult, 0);
    ini_puti("Keys", "cpu_turbo", cfg.k_cputurbo, 1);
    ini_puti("Oric", "tape_hook_addr", cfg.hook, 0);
    ini_puti("Sound", "volume", cfg.volume, 0);
    ini_puti("Sound", "loading_sound", cfg.loadsnd, 0);
    ini_puti("Sound", "loading_volume", cfg.loadvol, 0);
    ini_put("Keys", "layout", cfg.azerty ? "azerty" : "qwerty");
    ini_puti("Keys", "hard_reset", cfg.k_hard, 1);
    ini_puti("Keys", "soft_reset_nmi", cfg.k_soft, 1);
    ini_puti("Keys", "browser", cfg.k_browser, 1);
    ini_puti("Keys", "turbo", cfg.k_turbo, 1);
    ini_puti("Keys", "tape_rewind", cfg.k_rewind, 1);
    ini_puti("Keys", "stats", cfg.k_stats, 1);
    ini_puti("Keys", "scanlines", cfg.k_scan, 1);
    ini_puti("Video", "scanlines", cfg.scanlines, 0);
    ini_puti("Video", "scanline_strength", cfg.scan_strength, 0);
    ini_put("Video", "renderer", cfg.renderer == 1 ? "d3d11" : cfg.renderer == 2 ? "gdi" : "auto");
    ini_puti("Video", "crt_glow", cfg.crt_glow, 0);
    ini_puti("Video", "filter", cfg.filter, 0);
    ini_puti("Keys", "filter", cfg.k_aa, 1);
    ini_puti("Video", "crt_flicker", cfg.crt_flicker, 0);
}

static void settings_load(void)
{
    char t[32];
    ini_str("Oric", "rom", "", cfg.rom, MAX_PATH);
    ini_str("Oric", "microdisc_rom", "", cfg.mdrom, MAX_PATH);
    ini_str("Oric", "browse_dir", exe_dir, cfg.browse, MAX_PATH);
    ini_str("Oric", "browse_file", "", cfg.lastfile, MAX_PATH);
    cfg.start_browser  = ini_int("Oric", "start_in_browser", 1);
    cfg.fullscreen     = ini_int("Oric", "fullscreen", 0);
    cfg.writable_disks = ini_int("Oric", "writable_disks", 1);
    cfg.cpu_turbo      = ini_int("Oric", "cpu_turbo", 0);
    cfg.fast_tape      = ini_int("Oric", "fast_tape", 1);
    cfg.fast_disk      = ini_int("Oric", "fast_disk", 1);
    cfg.k_fast         = ini_int("Keys", "fast_load", 0x40);        /* F6  */
    cfg.k_pause        = ini_int("Keys", "pause", 0x41);            /* F7  */
    cfg.k_shot         = ini_int("Keys", "screenshot", 0x42);       /* F8  (Impr. ecran fonctionne aussi) */
    cfg.k_snap         = ini_int("Keys", "snapshot_save", 0xE052);  /* Inser */
    cfg.k_snapload     = ini_int("Keys", "snapshot_load_last", 0xE047);   /* Debut */
    ini_str("Paths", "screenshots", "", cfg.shot_dir, MAX_PATH);
    {
        char def[MAX_PATH];
        snprintf(def, sizeof def, "%s/snapshots", exe_dir);
        ini_str("Paths", "snapshots", def, cfg.snap_dir, MAX_PATH);
    }
    dbg_on             = ini_int("Debug", "log", 0);
    cfg.cpu_mult       = ini_int("Oric", "cpu_turbo_factor", 4);
    if (cfg.cpu_mult < 2) cfg.cpu_mult = 2;
    if (cfg.cpu_mult > 64) cfg.cpu_mult = 64;
    cfg.k_cputurbo     = ini_int("Keys", "cpu_turbo", 0x3F);        /* F5  */
    cfg.hook           = ini_int("Oric", "tape_hook_addr", 0);
    cfg.volume         = ini_int("Sound", "volume", 80);
    cfg.loadsnd        = ini_int("Sound", "loading_sound", 1);
    cfg.loadvol        = ini_int("Sound", "loading_volume", 25);
    ini_str("Keys", "layout", "qwerty", t, sizeof t);
    cfg.azerty         = (_stricmp(t, "azerty") == 0);
    cfg.k_hard         = ini_int("Keys", "hard_reset", 0x58);      /* F12 */
    cfg.k_soft         = ini_int("Keys", "soft_reset_nmi", 0x57);  /* F11 */
    cfg.k_browser      = ini_int("Keys", "browser", 0x3B);         /* F1  */
    cfg.k_turbo        = ini_int("Keys", "turbo", 0x44);           /* F10 */
    cfg.k_rewind       = ini_int("Keys", "tape_rewind", 0x3D);     /* F3  */
    cfg.k_scan         = ini_int("Keys", "scanlines", 0x3C);       /* F2  */
    cfg.scanlines      = ini_int("Video", "scanlines", 0);
    cfg.scan_strength  = ini_int("Video", "scanline_strength", 55);
    ini_str("Video", "renderer", "auto", t, sizeof t);
    cfg.renderer       = (_stricmp(t, "gdi") == 0) ? 2 : (_stricmp(t, "d3d11") == 0) ? 1 : 0;
    cfg.crt_glow       = ini_int("Video", "crt_glow", 40);
    cfg.filter         = ini_int("Video", "filter", -1);
    if (cfg.filter < 0) cfg.filter = ini_int("Video", "antialias", 0) ? 1 : 0;     /* ancien reglage */
    if (cfg.filter >= NFILTERS) cfg.filter = 0;
    cfg.k_aa           = ini_int("Keys", "filter", ini_int("Keys", "antialias", 0x3E));   /* F4  */
    cfg.crt_flicker    = ini_int("Video", "crt_flicker", 35);
    if (cfg.crt_glow < 0) cfg.crt_glow = 0;
    if (cfg.crt_glow > 100) cfg.crt_glow = 100;
    if (cfg.crt_flicker < 0) cfg.crt_flicker = 0;
    if (cfg.crt_flicker > 100) cfg.crt_flicker = 100;
    if (cfg.scan_strength < 0) cfg.scan_strength = 0;
    if (cfg.scan_strength > 100) cfg.scan_strength = 100;
    if (cfg.k_rewind == cfg.k_scan) cfg.k_rewind = 0x3D;   /* ancien reglage : F2 = rembobiner */
    cfg.k_stats        = ini_int("Keys", "stats", 0x43);           /* F9  */
    if (cfg.volume < 0) cfg.volume = 0;
    if (cfg.volume > 100) cfg.volume = 100;
    if (cfg.loadvol < 0) cfg.loadvol = 0;
    if (cfg.loadvol > 100) cfg.loadvol = 100;
    settings_save();   /* (re)ecrit le fichier avec toutes les cles */
}

static u8 *file_read(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    u8 *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    b = (u8 *)malloc(n ? (size_t)n : 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static const char *base_name(const char *p)
{
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}
static int has_ext(const char *name, const char *ext)
{
    const char *d = strrchr(name, '.');
    return d && _stricmp(d, ext) == 0;
}

/* ====================================================================== */
/*  Memoire / pagination                                                  */
/* ====================================================================== */
static u8 ram[65536];
static u8 bram[65536];                 /* image memoire du selecteur (police + ecran texte) */
static u8 *vram = ram;                 /* ce que l'ULA affiche                             */
static u8 rom_basic[16384];
static u8 rom_disk[16384];
static int rom_disk_size, have_basic, have_disk;

static int disk_on;                    /* interface Microdisc presente                      */
static u8  md_ctl;                     /* registre de controle $314                         */
static int ram_top = 0xBFFF;           /* au-dessus : ROM                                   */
static const u8 *paged_rom = rom_basic;
static int rom_mode;                   /* 0 BASIC, 1 RAM overlay, 2 ROM Microdisc           */

/* Pagination Microdisc (deduite du code reel de dos.rom, qui relaie les IRQ vers la ROM BASIC en ne
   basculant que le bit 1 : la ROM BASIC doit alors etre visible partout en $C000-$FFFF) :
     b1 = 1 : ROM BASIC en $C000-$FFFF                                  (quel que soit b7)
     b1 = 0 : b7 = 0 -> EPROM Microdisc en $E000-$FFFF, RAM de recouvrement en $C000-$DFFF
              b7 = 1 -> RAM de recouvrement partout en $C000-$FFFF
   Les ecritures vont toujours dans la RAM (qui existe sous la ROM, comme sur le vrai Oric).        */
static void paging_update(void)
{
    if (!disk_on || !have_disk) rom_mode = 0;
    else if (md_ctl & 0x02)     rom_mode = 0;           /* BASIC ROM */
    else if (!(md_ctl & 0x80))  rom_mode = 2;           /* EPROM + RAM */
    else                        rom_mode = 1;           /* RAM partout */
    if (rom_mode == 0)      { ram_top = 0xBFFF; paged_rom = rom_basic; }
    else if (rom_mode == 1) { ram_top = 0xFFFF; }
    else                    { ram_top = 0xFFFF - rom_disk_size; paged_rom = rom_disk; }
}

/* clavier injecte via $02DF (comme CLK) */
static char tq[128];
static int  tq_len, tq_pos, tq_delay;
static void type_str(const char *s, int delay)
{
    if (tq_pos >= tq_len) { tq_len = tq_pos = 0; tq_delay = delay; }
    while (*s && tq_len < (int)sizeof tq) tq[tq_len++] = *s++;
}

/* ====================================================================== */
/*  AY-3-8912                                                             */
/* ====================================================================== */
static struct {
    u8  r[16];
    int addr, bc1, bdir;
    int tcnt[3], tout[3], ncnt, nout, ecnt, sub;
    u32 lfsr;
    int env_val, env_dir, env_hold, env_att;
} ay;

static const int ay_vol[16] = { 0, 96, 144, 204, 296, 433, 593, 958, 1184, 1853, 2469, 3149, 3993, 4811, 5937, 7000 };

static void env_reset(void)
{
    ay.env_att = (ay.r[13] & 4) != 0;
    ay.env_val = ay.env_att ? 0 : 15;
    ay.env_dir = ay.env_att ? 1 : -1;
    ay.env_hold = 0;
    ay.ecnt = 0;
}
static void env_step(void)
{
    int nv, sh = ay.r[13] & 15;
    if (ay.env_hold) return;
    nv = ay.env_val + ay.env_dir;
    if (nv >= 0 && nv <= 15) { ay.env_val = nv; return; }
    if (!(sh & 8)) { ay.env_val = 0; ay.env_hold = 1; return; }
    if (sh & 1) {                                   /* hold */
        ay.env_val = (ay.env_att ^ ((sh >> 1) & 1)) ? 15 : 0;
        ay.env_hold = 1;
    } else if (sh & 2) {                            /* alternate */
        ay.env_dir = -ay.env_dir;
        ay.env_val = (ay.env_dir > 0) ? 1 : 14;
    } else {                                        /* repeat */
        ay.env_val = ay.env_att ? 0 : 15;
    }
}
static void ay_write(int reg, u8 v)
{
    reg &= 15;
    if (reg == 1 || reg == 3 || reg == 5 || reg == 13) v &= 0x0F;
    if (reg == 6 || (reg >= 8 && reg <= 10)) v &= 0x1F;
    ay.r[reg] = v;
    if (reg == 13) env_reset();
}
static u8 ay_read(int reg) { return ay.r[reg & 15]; }
static void ay_lines(int bc1, int bdir, u8 bus)
{
    if (bc1 == ay.bc1 && bdir == ay.bdir) return;
    ay.bc1 = bc1; ay.bdir = bdir;
    if (bdir && bc1) ay.addr = bus & 15;
    else if (bdir && !bc1) ay_write(ay.addr, bus);
}
static void ay_reset(void)
{
    memset(&ay, 0, sizeof ay);
    ay.lfsr = 1;
    ay.r[7] = 0xFF;
}
/* un pas = 8 cycles CPU */
static int ay_tick(void)
{
    int c, mix = 0, np, ep;
    u8 mx = ay.r[7];
    for (c = 0; c < 3; c++) {
        int tp = ay.r[c * 2] | (ay.r[c * 2 + 1] << 8);
        if (tp < 1) tp = 1;
        if (++ay.tcnt[c] >= tp) { ay.tcnt[c] = 0; ay.tout[c] ^= 1; }
    }
    ay.sub ^= 1;
    if (!ay.sub) {
        np = ay.r[6]; if (np < 1) np = 1;
        if (++ay.ncnt >= np) {
            ay.ncnt = 0;
            ay.lfsr = (ay.lfsr >> 1) | (((ay.lfsr ^ (ay.lfsr >> 3)) & 1u) << 16);
            ay.nout = (int)(ay.lfsr & 1u);
        }
        ep = ay.r[11] | (ay.r[12] << 8); if (ep < 1) ep = 1;
        if (++ay.ecnt >= ep) { ay.ecnt = 0; env_step(); }
    }
    for (c = 0; c < 3; c++) {
        int t = ay.tout[c] | ((mx >> c) & 1);
        int n = ay.nout | ((mx >> (3 + c)) & 1);
        if (t & n) {
            int a = ay.r[8 + c];
            mix += ay_vol[(a & 16) ? ay.env_val : (a & 15)];
        }
    }
    return mix;
}

/* ====================================================================== */
/*  Clavier (matrice Oric)                                                */
/* ====================================================================== */
static int rom_is_azerty;
static u8 keyrows[8];
#define K(r, m) (((r) << 8) | (m))
enum {
    KY_3 = K(0,0x80), KY_X = K(0,0x40), KY_1 = K(0,0x20), KY_V = K(0,0x08), KY_5 = K(0,0x04), KY_N = K(0,0x02), KY_7 = K(0,0x01),
    KY_D = K(1,0x80), KY_Q = K(1,0x40), KY_ESC = K(1,0x20), KY_F = K(1,0x08), KY_R = K(1,0x04), KY_T = K(1,0x02), KY_J = K(1,0x01),
    KY_C = K(2,0x80), KY_2 = K(2,0x40), KY_Z = K(2,0x20), KY_CTRL = K(2,0x10), KY_4 = K(2,0x08), KY_B = K(2,0x04), KY_6 = K(2,0x02), KY_M = K(2,0x01),
    KY_QUOTE = K(3,0x80), KY_BSL = K(3,0x40), KY_MINUS = K(3,0x08), KY_SEMI = K(3,0x04), KY_9 = K(3,0x02), KY_K = K(3,0x01),
    KY_RIGHT = K(4,0x80), KY_DOWN = K(4,0x40), KY_LEFT = K(4,0x20), KY_LSHIFT = K(4,0x10), KY_UP = K(4,0x08), KY_DOT = K(4,0x04), KY_COMMA = K(4,0x02), KY_SPACE = K(4,0x01),
    KY_LBR = K(5,0x80), KY_RBR = K(5,0x40), KY_DEL = K(5,0x20), KY_FUNCT = K(5,0x10), KY_P = K(5,0x08), KY_O = K(5,0x04), KY_I = K(5,0x02), KY_U = K(5,0x01),
    KY_W = K(6,0x80), KY_S = K(6,0x40), KY_A = K(6,0x20), KY_E = K(6,0x08), KY_G = K(6,0x04), KY_H = K(6,0x02), KY_Y = K(6,0x01),
    KY_EQ = K(7,0x80), KY_RET = K(7,0x20), KY_RSHIFT = K(7,0x10), KY_SLASH = K(7,0x08), KY_0 = K(7,0x04), KY_L = K(7,0x02), KY_8 = K(7,0x01)
};

/* scancode (set 1, 0xE0xx pour les touches etendues) -> touche Oric (0 = aucune) */
static int sc_to_oric(int sc)
{
    if (cfg.azerty && !rom_is_azerty) {   /* lettres selon les legendes AZERTY (inutile avec une ROM AZERTY) */
        switch (sc) {
        case 0x10: return KY_A;  case 0x1E: return KY_Q;
        case 0x11: return KY_Z;  case 0x2C: return KY_W;
        case 0x27: return KY_M;  case 0x32: return KY_COMMA;
        case 0x33: return KY_SEMI;
        }
    }
    switch (sc) {
    case 0x01: return KY_ESC;
    case 0x02: return KY_1; case 0x03: return KY_2; case 0x04: return KY_3; case 0x05: return KY_4;
    case 0x06: return KY_5; case 0x07: return KY_6; case 0x08: return KY_7; case 0x09: return KY_8;
    case 0x0A: return KY_9; case 0x0B: return KY_0; case 0x0C: return KY_MINUS; case 0x0D: return KY_EQ;
    case 0x0E: return KY_DEL;
    case 0x10: return KY_Q; case 0x11: return KY_W; case 0x12: return KY_E; case 0x13: return KY_R;
    case 0x14: return KY_T; case 0x15: return KY_Y; case 0x16: return KY_U; case 0x17: return KY_I;
    case 0x18: return KY_O; case 0x19: return KY_P; case 0x1A: return KY_LBR; case 0x1B: return KY_RBR;
    case 0x1C: case 0xE01C: return KY_RET;
    case 0x1D: case 0xE01D: return KY_CTRL;
    case 0x1E: return KY_A; case 0x1F: return KY_S; case 0x20: return KY_D; case 0x21: return KY_F;
    case 0x22: return KY_G; case 0x23: return KY_H; case 0x24: return KY_J; case 0x25: return KY_K;
    case 0x26: return KY_L; case 0x27: return KY_SEMI; case 0x28: return KY_QUOTE;
    case 0x2A: return KY_LSHIFT; case 0x2B: return KY_BSL;
    case 0x2C: return KY_Z; case 0x2D: return KY_X; case 0x2E: return KY_C; case 0x2F: return KY_V;
    case 0x30: return KY_B; case 0x31: return KY_N; case 0x32: return KY_M;
    case 0x33: return KY_COMMA; case 0x34: return KY_DOT; case 0x35: case 0xE035: return KY_SLASH;
    case 0x36: return KY_RSHIFT;
    case 0x38: case 0xE038: return KY_FUNCT;
    case 0x39: return KY_SPACE;
    case 0xE048: return KY_UP; case 0xE04B: return KY_LEFT; case 0xE04D: return KY_RIGHT; case 0xE050: return KY_DOWN;
    case 0xE053: return KY_DEL;
    }
    return 0;
}
static void key_set(int key, int down)
{
    if (!key) return;
    if (down) keyrows[key >> 8] |= (u8)(key & 255);
    else      keyrows[key >> 8] &= (u8)~(key & 255);
}

/* ====================================================================== */
/*  VIA 6522                                                              */
/* ====================================================================== */
static struct {
    u8 orb, ora, ddrb, ddra, acr, pcr, ifr, ier, sr;
    int t1c, t2c, t1_armed, t2_armed, cb1, irq;
    u16 t1l, t2l;
} via;
static int tape_motor, vsync_lvl, kbd_row;
static int tape_in;                  /* niveau du signal K7 genere */
static void gen_start(void);
static void gen_stop(void);
static void via_update_irq(void) { via.irq = (via.ifr & via.ier & 0x7F) != 0; }
static void via_reset(void)
{
    memset(&via, 0, sizeof via);
    via.t1c = via.t2c = 0xFFFF; via.t1l = via.t2l = 0xFFFF;
    via.cb1 = 1;
    tape_motor = 0; kbd_row = 0;
}
static void via_set_cb1(int lvl)
{
    if (lvl == via.cb1) return;
    via.cb1 = lvl;
    if (lvl == ((via.pcr >> 4) & 1)) { via.ifr |= 0x10; via_update_irq(); }
}
static void cb1_update(void) { via_set_cb1(tape_motor ? !tape_in : !vsync_lvl); }
static void via_portb_out(void)
{
    u8 pb = (u8)((via.orb & via.ddrb) | ~via.ddrb);
    int m;
    kbd_row = pb & 7;
    m = (via.ddrb & 0x40) && (via.orb & 0x40);          /* PB6 pilotee par la VIA = moteur K7 */
    if (m != tape_motor) {
        tape_motor = m;
        if (m) gen_start(); else gen_stop();
        cb1_update();
    }
}
static void via_pcr_apply(void)
{
    int ca2 = ((via.pcr & 0x0E) == 0x0E);
    int cb2 = ((via.pcr & 0xE0) == 0xE0);
    ay_lines(ca2, cb2, (u8)((via.ora & via.ddra) | ~via.ddra));   /* CA2 = BC1, CB2 = BDIR */
}
static u8 porta_in(void)
{
    u8 bus = 0xFF;
    if (ay.bc1 && !ay.bdir) bus = ay_read(ay.addr);
    return (u8)((via.ora & via.ddra) | (bus & ~via.ddra));
}
static u8 via_read(int r)
{
    u8 v;
    switch (r & 15) {
    case 0:
        via.ifr &= ~0x18; via_update_irq();
        v = (keyrows[kbd_row] & (u8)~ay.r[14]) ? 0x08 : 0;      /* PB3 = sens clavier */
        return (u8)((via.orb & via.ddrb) | (v & ~via.ddrb));
    case 1: via.ifr &= ~0x03; via_update_irq(); return porta_in();
    case 15: return porta_in();
    case 2: return via.ddrb;
    case 3: return via.ddra;
    case 4: via.ifr &= ~0x40; via_update_irq(); return (u8)(via.t1c & 255);
    case 5: return (u8)((via.t1c >> 8) & 255);
    case 6: return (u8)(via.t1l & 255);
    case 7: return (u8)(via.t1l >> 8);
    case 8: via.ifr &= ~0x20; via_update_irq(); return (u8)(via.t2c & 255);
    case 9: return (u8)((via.t2c >> 8) & 255);
    case 10: via.ifr &= ~0x04; via_update_irq(); return via.sr;
    case 11: return via.acr;
    case 12: return via.pcr;
    case 13: return (u8)((via.ifr & 0x7F) | (via.irq ? 0x80 : 0));
    default: return (u8)(via.ier | 0x80);
    }
}
static void via_write(int r, u8 v)
{
    switch (r & 15) {
    case 0: via.orb = v; via.ifr &= ~0x18; via_update_irq(); via_portb_out(); break;
    case 1: via.ifr &= ~0x03; via_update_irq(); via.ora = v; break;
    case 15: via.ora = v; break;
    case 2: via.ddrb = v; via_portb_out(); break;
    case 3: via.ddra = v; break;
    case 4: case 6: via.t1l = (u16)((via.t1l & 0xFF00) | v); break;
    case 5: via.t1l = (u16)((via.t1l & 0x00FF) | (v << 8)); via.t1c = via.t1l; via.ifr &= ~0x40; via.t1_armed = 1; via_update_irq(); break;
    case 7: via.t1l = (u16)((via.t1l & 0x00FF) | (v << 8)); via.ifr &= ~0x40; via_update_irq(); break;
    case 8: via.t2l = (u16)((via.t2l & 0xFF00) | v); break;
    case 9: via.t2l = (u16)((via.t2l & 0x00FF) | (v << 8)); via.t2c = via.t2l; via.ifr &= ~0x20; via.t2_armed = 1; via_update_irq(); break;
    case 10: via.sr = v; break;
    case 11: via.acr = v; break;
    case 12: via.pcr = v; via_pcr_apply(); break;
    case 13: via.ifr &= (u8)~(v & 0x7F); via_update_irq(); break;
    case 14: if (v & 0x80) via.ier |= v & 0x7F; else via.ier &= (u8)~(v & 0x7F); via_update_irq(); break;
    }
}
static void via_tick(int n)
{
    via.t1c -= n;
    while (via.t1c < 0) {
        if (via.t1_armed) { via.ifr |= 0x40; if (!(via.acr & 0x40)) via.t1_armed = 0; }
        via.t1c += (via.acr & 0x40) ? via.t1l + 2 : 0x10000;
    }
    if (!(via.acr & 0x20)) {
        via.t2c -= n;
        while (via.t2c < 0) {
            if (via.t2_armed) { via.ifr |= 0x20; via.t2_armed = 0; }
            via.t2c += 0x10000;
        }
    }
    via_update_irq();
}

/* ====================================================================== */
/*  Disquettes (.dsk MFM_DISK) + WD1793 / Microdisc                       */
/* ====================================================================== */
typedef struct {
    u8 *img; size_t size;
    int sides, tracks, loaded, writable;
    char path[MAX_PATH];
} Drive;
static Drive drv[4];

typedef struct { u8 t, s, n, sz; int idpos, datapos, deleted; } SecInfo;

static u16 crc_upd(u16 crc, u8 b)
{
    int i;
    crc ^= (u16)(b << 8);
    for (i = 0; i < 8; i++) crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
    return crc;
}
static u8 *trk_ptr(Drive *d, int track, int side)
{
    if (!d->loaded || track < 0 || track >= d->tracks || side < 0 || side >= d->sides) return NULL;
    return d->img + 256 + (size_t)6400 * (size_t)(d->tracks * side + track);
}
static int scan_track(u8 *tp, SecInfo *out, int max)
{
    int i, j, cnt = 0;
    for (i = 0; i < 6380 && cnt < max; i++) {
        if (tp[i] == 0xA1 && tp[i + 1] == 0xA1 && tp[i + 2] == 0xA1 && tp[i + 3] == 0xFE) {
            SecInfo si;
            si.t = tp[i + 4]; si.s = tp[i + 5]; si.n = tp[i + 6]; si.sz = tp[i + 7];
            si.idpos = i; si.datapos = -1; si.deleted = 0;
            for (j = i + 10; j < i + 90 && j + 4 < 6400; j++) {
                if (tp[j] == 0xA1 && tp[j + 1] == 0xA1 && tp[j + 2] == 0xA1 &&
                    (tp[j + 3] == 0xFB || tp[j + 3] == 0xF8 || tp[j + 3] == 0xF9 || tp[j + 3] == 0xFA)) {
                    si.datapos = j + 4; si.deleted = (tp[j + 3] != 0xFB);
                    break;
                }
            }
            if (si.datapos >= 0 && si.datapos + (128 << (si.sz & 3)) + 2 <= 6400) out[cnt++] = si;
            i += 7;
        }
    }
    return cnt;
}
static void disk_flush(Drive *d, size_t off, size_t len)
{
    FILE *f;
    if (!d->writable || off + len > d->size) return;
    f = fopen(d->path, "r+b");
    if (!f) return;
    fseek(f, (long)off, SEEK_SET);
    fwrite(d->img + off, 1, len, f);
    fclose(f);
}
static void drive_eject(int i)
{
    free(drv[i].img);
    memset(&drv[i], 0, sizeof drv[i]);
}
static int drive_load(int i, const char *path)
{
    size_t sz;
    u8 *b = file_read(path, &sz);
    int sides, tracks, geom;
    if (!b) { ui_error("Impossible de lire la disquette."); return 0; }
    if (sz < 256 || memcmp(b, "MFM_DISK", 8) != 0) {
        free(b);
        ui_error("Format de disquette non supporte (image Oric MFM_DISK requise).");
        return 0;
    }
    sides  = (int)(b[8]  | (b[9]  << 8) | (b[10] << 16) | (b[11] << 24));
    tracks = (int)(b[12] | (b[13] << 8) | (b[14] << 16) | (b[15] << 24));
    geom   = (int)(b[16] | (b[17] << 8) | (b[18] << 16) | (b[19] << 24));
    (void)geom;
    if (sides < 1 || sides > 2 || tracks < 1) { free(b); ui_error("En-tete de disquette invalide."); return 0; }
    if (sz < 256 + (size_t)6400 * sides * tracks) {
        tracks = (int)((sz - 256) / ((size_t)6400 * sides));
        if (tracks < 1) { free(b); ui_error("Image de disquette tronquee."); return 0; }
    }
    drive_eject(i);
    drv[i].img = b; drv[i].size = sz; drv[i].sides = sides; drv[i].tracks = tracks; drv[i].loaded = 1;
    snprintf(drv[i].path, MAX_PATH, "%s", path);
#ifdef _WIN32
    drv[i].writable = cfg.writable_disks && !(GetFileAttributesA(path) & FILE_ATTRIBUTE_READONLY);
#else
    drv[i].writable = cfg.writable_disks;
#endif
    return 1;
}

/* --- WD1793 --- */
enum { F_IDLE, F_T1, F_SEARCH, F_RDATA, F_RDONE, F_WDATA, F_WT, F_FIN };
#define F_INF 0x3FFFFFFF
static struct {
    u8 cmd, trk, sec, dat, err;
    int type, busy, drq, intrq, state, wait;
    int drive, side, dirc, head[4];
    u8 *xbuf; int xlen, xpos;
    u8 xtmp[6500];
    SecInfo si; u8 *tp;
    int multi, lost, t1kind, t1steps, verify, upd, stepcyc, ra_idx, wt_pos, wt_prevf5;
    u16 wt_crc;
    long long time;
} fdc;

static int disk_act;           /* activite (son)  */
static long fdc_ncmd, fdc_nbytes;
static long fdc_hot;               /* > 0 : acces disque recent (turbo disquette) */
static u8 fdc_lastcmd, fdc_laststatus;
static double click_env;
static void fdc_reset(void)
{
    memset(&fdc, 0, sizeof fdc);
    fdc.type = 1; fdc.dirc = 1;
    disk_act = 0; click_env = 0;
}
static void fdc_finish(void)
{
    fdc_laststatus = (u8)(fdc.err | (fdc.lost ? 4 : 0));
    dbg_log("FDC fin cmd %02X  erreurs=%02X  octets transferes=%ld  piste physique %d", fdc.cmd, fdc_laststatus, fdc_nbytes, fdc.head[fdc.drive]);
    fdc.busy = 0; fdc.drq = 0; fdc.intrq = 1; fdc.state = F_IDLE;
}
static int index_pulse(void) { return (fdc.time % 200000) < 4000; }
static u8 fdc_status(void)
{
    Drive *d = &drv[fdc.drive];
    u8 s;
    if (fdc.type == 1) {
        s = fdc.err & 0x18;
        if (index_pulse()) s |= 2;
        if (fdc.head[fdc.drive] == 0) s |= 4;
        if (d->loaded) s |= 0x20;
        if (d->loaded && !d->writable) s |= 0x40;
    } else {
        s = fdc.err & 0x7C;
        if (fdc.drq) s |= 2;
    }
    if (fdc.busy) s |= 1;
    if (!d->loaded) s |= 0x80;
    return s;
}
static void fdc_sector_op(void)    /* demarre la recherche de secteur */
{
    fdc.state = F_SEARCH; fdc.wait = 4000;
}
static void fdc_cmd(u8 c)
{
    Drive *d = &drv[fdc.drive];
    int k = c >> 4;
    static const int steps[4] = { 3000, 6000, 10000, 15000 };
    fdc.cmd = c; fdc.intrq = 0;
    fdc_ncmd++; fdc_lastcmd = c;
    dbg_log("FDC cmd %02X (TR=%d SR=%d DR=%d) lecteur %d face %d tete %d  PC=%04X", c, fdc.trk, fdc.sec, fdc.dat,
            fdc.drive, fdc.side, fdc.head[fdc.drive], PC);
    if (k == 0xD) {                                     /* Force Interrupt */
        fdc.state = F_IDLE; fdc.busy = 0; fdc.drq = 0; fdc.type = 1;
        if (c & 8) fdc.intrq = 1;
        return;
    }
    if (fdc.busy) return;
    fdc.busy = 1; fdc.err = 0; fdc.lost = 0;
    if (k < 8) {                                        /* Type I */
        fdc.type = 1; fdc.drq = 0;
        fdc.upd = c & 0x10; fdc.verify = c & 4;
        fdc.stepcyc = steps[c & 3];
        fdc.t1steps = 0;
        if (k == 0) fdc.t1kind = 0;
        else if (k == 1) fdc.t1kind = 1;
        else {
            fdc.t1kind = 2; fdc.t1steps = 1;
            if (k >= 4 && k <= 5) fdc.dirc = 1;
            if (k >= 6) fdc.dirc = -1;
        }
        fdc.state = F_T1; fdc.wait = 1500;
        return;
    }
    fdc.type = 2; fdc.drq = 0; fdc.xpos = 0;
    fdc.multi = (c & 0x10) != 0;
    if (k == 8 || k == 9 || k == 0xA || k == 0xB) { fdc_sector_op(); return; }
    if (k == 0xC) {                                     /* Read Address */
        fdc.state = F_SEARCH; fdc.wait = 3000; return;
    }
    if (k == 0xE) { fdc_sector_op(); return; }          /* Read Track  */
    if (k == 0xF) {                                     /* Write Track */
        if (!d->writable || !trk_ptr(d, fdc.head[fdc.drive], fdc.side)) { fdc.err |= 0x40; fdc.state = F_FIN; fdc.wait = 100; return; }
        fdc.state = F_WT; fdc.drq = 1; fdc.wait = F_INF; fdc.wt_pos = 0; fdc.wt_prevf5 = 0;
        return;
    }
    fdc_finish();
}
static void fdc_search_done(void)
{
    Drive *d = &drv[fdc.drive];
    u8 *tp = trk_ptr(d, fdc.head[fdc.drive], fdc.side);
    SecInfo list[48];
    int n = tp ? scan_track(tp, list, 48) : 0, i, k = -1, len;
    int op = fdc.cmd >> 4;
    fdc.tp = tp;
    if (op == 0xC) {                                    /* Read Address */
        u16 crc = 0xFFFF;
        const u8 pre[4] = { 0xA1, 0xA1, 0xA1, 0xFE };
        if (!n) { fdc.err |= 0x10; fdc.state = F_FIN; fdc.wait = 150000; return; }
        i = fdc.ra_idx++ % n;
        fdc.xtmp[0] = list[i].t; fdc.xtmp[1] = list[i].s; fdc.xtmp[2] = list[i].n; fdc.xtmp[3] = list[i].sz;
        for (k = 0; k < 4; k++) crc = crc_upd(crc, pre[k]);
        for (k = 0; k < 4; k++) crc = crc_upd(crc, fdc.xtmp[k]);
        fdc.xtmp[4] = (u8)(crc >> 8); fdc.xtmp[5] = (u8)crc;
        fdc.sec = fdc.xtmp[0];
        fdc.xbuf = fdc.xtmp; fdc.xlen = 6; fdc.xpos = 0;
        fdc.state = F_RDATA; fdc.wait = 32;
        return;
    }
    if (op == 0xE) {                                    /* Read Track */
        if (!tp) { fdc.err |= 0x10; fdc.state = F_FIN; fdc.wait = 150000; return; }
        fdc.xbuf = tp; fdc.xlen = 6250; fdc.xpos = 0;
        fdc.state = F_RDATA; fdc.wait = 32;
        return;
    }
    for (i = 0; i < n; i++) if (list[i].t == fdc.trk && list[i].n == fdc.sec) { k = i; break; }
    if (k < 0) {
        char ids[200]; int q, o = 0;
        for (q = 0; q < n && o < 180; q++) o += snprintf(ids + o, sizeof ids - (size_t)o, " %d/%d", list[q].t, list[q].n);
        dbg_log("FDC secteur introuvable : demande piste %d secteur %d (tete en %d, face %d) ; IDs presents :%s",
                fdc.trk, fdc.sec, fdc.head[fdc.drive], fdc.side, n ? ids : " aucun");
        fdc.err |= 0x10; fdc.state = F_FIN; fdc.wait = 150000; return;
    }
    fdc.si = list[k];
    len = 128 << (fdc.si.sz & 3);
    if (op == 8 || op == 9) {
        fdc.xbuf = tp + fdc.si.datapos; fdc.xlen = len; fdc.xpos = 0;
        if (fdc.si.deleted) fdc.err |= 0x20;
        fdc.state = F_RDATA; fdc.wait = 32;
    } else {
        if (!d->writable) { fdc.err |= 0x40; fdc.state = F_FIN; fdc.wait = 100; return; }
        fdc.xlen = len; fdc.xpos = 0;
        fdc.state = F_WDATA; fdc.drq = 1; fdc.wait = F_INF;
    }
}
static void fdc_commit_sector(void)
{
    Drive *d = &drv[fdc.drive];
    u8 *dp = fdc.tp + fdc.si.datapos;
    u16 crc = 0xFFFF;
    int i;
    u8 mark = (fdc.cmd & 1) ? 0xF8 : 0xFB;
    memcpy(dp, fdc.xtmp, fdc.xlen);
    dp[-1] = mark;
    for (i = 0; i < 3; i++) crc = crc_upd(crc, 0xA1);
    crc = crc_upd(crc, mark);
    for (i = 0; i < fdc.xlen; i++) crc = crc_upd(crc, dp[i]);
    dp[fdc.xlen] = (u8)(crc >> 8); dp[fdc.xlen + 1] = (u8)crc;
    disk_flush(d, (size_t)(dp - 1 - d->img), (size_t)fdc.xlen + 3);
}
static void fdc_commit_track(void)
{
    Drive *d = &drv[fdc.drive];
    u8 *tp = trk_ptr(d, fdc.head[fdc.drive], fdc.side);
    int n = fdc.wt_pos > 6400 ? 6400 : fdc.wt_pos;
    if (!tp) return;
    memcpy(tp, fdc.xtmp, (size_t)n);
    if (n < 6400) memset(tp + n, 0x4E, (size_t)(6400 - n));
    disk_flush(d, (size_t)(tp - d->img), 6400);
}
static void fdc_write_data(u8 v)
{
    fdc.dat = v;
    if (fdc.state == F_WDATA && fdc.drq) {
        fdc.xtmp[fdc.xpos++] = v; fdc.drq = 0; fdc.wait = 32; disk_act = 20000;
    } else if (fdc.state == F_WT && fdc.drq) {
        int p = fdc.wt_pos;
        if (p < 6395) {
            if (v == 0xF5) {
                if (!fdc.wt_prevf5) fdc.wt_crc = 0xFFFF;
                fdc.xtmp[p++] = 0xA1; fdc.wt_crc = crc_upd(fdc.wt_crc, 0xA1);
            } else if (v == 0xF6) {
                fdc.xtmp[p++] = 0xC2;
            } else if (v == 0xF7) {
                fdc.xtmp[p++] = (u8)(fdc.wt_crc >> 8); fdc.xtmp[p++] = (u8)fdc.wt_crc;
            } else {
                fdc.xtmp[p++] = v; fdc.wt_crc = crc_upd(fdc.wt_crc, v);
            }
        }
        fdc.wt_prevf5 = (v == 0xF5);
        fdc.wt_pos = p; fdc.drq = 0; fdc.wait = 32; disk_act = 20000;
    }
}
static void fdc_t1_event(void)
{
    Drive *d = &drv[fdc.drive];
    int *h = &fdc.head[fdc.drive];
    int dir, maxt = 86;
    if (fdc.t1kind == 0) {
        if (*h > 0) { (*h)--; click_env = 1.0; disk_act = 20000; fdc.wait = fdc.stepcyc; return; }
        fdc.trk = 0;
    } else if (fdc.t1kind == 1) {
        if (fdc.trk != fdc.dat) {
            dir = fdc.dat > fdc.trk ? 1 : -1;
            fdc.dirc = dir;
            *h += dir; if (*h < 0) *h = 0; if (*h > maxt) *h = maxt;
            fdc.trk = (u8)(fdc.trk + dir);
            click_env = 1.0; disk_act = 20000;
            fdc.wait = fdc.stepcyc; return;
        }
    } else {
        if (fdc.t1steps > 0) {
            fdc.t1steps--;
            *h += fdc.dirc; if (*h < 0) *h = 0; if (*h > maxt) *h = maxt;
            if (fdc.upd) fdc.trk = (u8)(fdc.trk + fdc.dirc);
            click_env = 1.0; disk_act = 20000;
            fdc.wait = fdc.stepcyc; return;
        }
    }
    if (fdc.verify) {                                   /* verification de l'ID */
        u8 *tp = trk_ptr(d, *h, fdc.side);
        SecInfo list[48];
        int n = tp ? scan_track(tp, list, 48) : 0, i, ok = 0;
        for (i = 0; i < n; i++) if (list[i].t == fdc.trk) ok = 1;
        if (!ok && d->loaded) fdc.err |= 0x10;
    }
    fdc_finish();
}
static void fdc_event(void)
{
    switch (fdc.state) {
    case F_T1: fdc_t1_event(); break;
    case F_SEARCH: fdc_search_done(); break;
    case F_RDATA:
        if (fdc.drq) fdc.lost = 1;
        fdc.dat = fdc.xbuf[fdc.xpos++]; fdc.drq = 1; disk_act = 20000; fdc_nbytes++;
        fdc.state = (fdc.xpos >= fdc.xlen) ? F_RDONE : F_RDATA;
        fdc.wait = 32;
        break;
    case F_RDONE:
        if (fdc.lost) fdc.err |= 4;
        if (fdc.multi && (fdc.cmd >> 4) <= 9) { fdc.sec++; fdc.state = F_SEARCH; fdc.wait = 2000; }
        else fdc_finish();
        break;
    case F_WDATA:
        if (fdc.xpos < fdc.xlen) { fdc.drq = 1; fdc.wait = F_INF; }
        else { fdc_commit_sector(); fdc_finish(); }
        break;
    case F_WT:
        if (fdc.wt_pos >= 6250) { fdc_commit_track(); fdc_finish(); }
        else { fdc.drq = 1; fdc.wait = F_INF; }
        break;
    case F_FIN: fdc_finish(); break;
    default: fdc.state = F_IDLE; break;
    }
}
static void fdc_run(int n)
{
    fdc.time += n;
    if (fdc.state != F_IDLE) fdc_hot = 300000;     /* ~0,3 s emulee de maintien apres le dernier acces */
    else if (fdc_hot > 0) fdc_hot -= n;
    if (fdc.state == F_IDLE) return;
    fdc.wait -= n;
    while (fdc.state != F_IDLE && fdc.wait <= 0) fdc_event();
}
static u8 fdc_read(int r)
{
    switch (r & 3) {
    case 0: fdc.intrq = 0; return fdc_status();
    case 1: return fdc.trk;
    case 2: return fdc.sec;
    default: fdc.drq = 0; return fdc.dat;
    }
}
static void fdc_write(int r, u8 v)
{
    switch (r & 3) {
    case 0: fdc_cmd(v); break;
    case 1: fdc.trk = v; break;
    case 2: fdc.sec = v; break;
    default: fdc_write_data(v); break;
    }
}
static int md_irq(void) { return disk_on && (md_ctl & 1) && fdc.intrq; }
static void md_control(u8 v)
{
    dbg_log("Microdisc controle $314 <- %02X  (EPROM %s, BASIC %s, IRQ %s, lecteur %d, face %d, %s)  PC=%04X", v,
            (v & 0x02) ? "(masquee)" : ((v & 0x80) ? "off" : "ON"), (v & 0x02) ? "ROM" : "RAM", (v & 1) ? "on" : "off", (v >> 5) & 3, (v >> 4) & 1,
            (v & 8) ? "simple densite" : "double densite", PC);
    md_ctl = v;
    fdc.drive = (v >> 5) & 3;
    fdc.side = (v >> 4) & 1;
    paging_update();
}

/* ====================================================================== */
/*  K7 (.tap) : acceleration du vecteur de lecture d'octet de la ROM       */
/* ====================================================================== */
static u8 *tape; static size_t tape_len, tape_pos;
static int tape_act; static u8 tape_last;
static int pending_run;
static long hook_frame;
static char tape_name[MAX_PATH];

/* K7 : .tap, ou K7 decoupee en .tap + .ta1 + .ta2 ... (numeros 0..999) : on concatene les morceaux dans l'ordre. */
static int tape_parts;
/* -1 pour ".tap", N pour ".taN" (N >= 0), -2 si ce n'est pas une K7 */
static int tape_part_no(const char *name)
{
    const char *d = strrchr(name, '.');
    int i, v = 0;
    if (!d) return -2;
    if (_stricmp(d, ".tap") == 0) return -1;
    if ((d[1] != 't' && d[1] != 'T') || (d[2] != 'a' && d[2] != 'A') || !d[3]) return -2;
    for (i = 3; d[i]; i++) { if (d[i] < '0' || d[i] > '9' || i > 5) return -2; v = v * 10 + (d[i] - '0'); }
    return v;
}
static int tape_load(const char *path)
{
    size_t n, i, total = 0;
    u8 *all = NULL;
    char cur[MAX_PATH];
    const char *dot = strrchr(path, '.'), *sep1 = strrchr(path, '\\'), *sep2 = strrchr(path, '/');
    int baselen, part, next;
    if (sep2 > sep1) sep1 = sep2;
    baselen = (dot && (!sep1 || dot > sep1)) ? (int)(dot - path) : (int)strlen(path);
    next = tape_part_no(path) + 1;                       /* .tap -> 0 + 1 = .ta1 ; .taN -> N + 1 */
    if (next < 1) next = 1;
    tape_parts = 0;
    for (part = 0; part < 1000; part++) {
        u8 *b, *nb;
        if (part == 0) snprintf(cur, sizeof cur, "%s", path);
        else snprintf(cur, sizeof cur, "%.*s.ta%d", baselen, path, next++);
        b = file_read(cur, &n);
        if (!b) { if (part == 0) { ui_error("Impossible de lire la K7 (.tap)."); return 0; } break; }
        nb = (u8 *)realloc(all, total + n);
        if (!nb) { free(b); free(all); return 0; }
        all = nb; memcpy(all + total, b, n); total += n; free(b);
        tape_parts++;
    }
    if (total < 8) { free(all); ui_error("K7 trop courte ou invalide."); return 0; }
    free(tape);
    tape = all; tape_len = total; tape_pos = 0; n = total;
    snprintf(tape_name, MAX_PATH, "%s", path);
    /* header : 16 16 16 24 | 00 00 | type | autorun | ...  -> faut-il envoyer RUN ? */
    pending_run = 0;
    i = 0;
    while (i < n && tape[i] == 0x16) i++;
    if (i >= 3 && i + 4 < n && tape[i] == 0x24) {
        u8 type = tape[i + 3], autorun = tape[i + 4];
        if (type == 0x00 && autorun == 0) pending_run = 1;
    }
    return 1;
}

/* --- generateur du signal K7 (codage rapide, comme MAME : echantillons a 4800 Hz) ---
   bit = 1 echantillon HAUT puis (1 si bit=1, 2 si bit=0) echantillons BAS.
   octet = depart(0) + 8 bits LSB first + parite impaire + 4 arrets(1).
   Sert (a) au mode "temps reel" (acceleration K7 desactivee) et (b) aux programmes qui lisent eux-memes
   la K7 sans passer par la ROM (leur lecture est alors accelere par le turbo automatique).        */
#define GEN_LEADER 512            /* octets 0x16 avant la synchro (~4 s, comme MAME) : un chargeur peut mettre du temps a ecouter */
#define GEN_GAP 40                /* pause entre l'en-tete et les donnees : 40 trames de bits 1 (~560 bits, ~230 ms) */
#define SAMPLE_CYC (1000000.0 / 4800.0)
static int gen_state, gen_phase, gen_bit, gen_leader, gen_curbit, gen_gapbytes, gen_isgap, gen_gap_used;
static size_t gen_pos, gen_gap_at;
static u8 gen_byte;
static double gen_t;
static long gen_idle;

static void tape_set_level(int l) { tape_in = l; cb1_update(); }
static int gen_bit_value(void)
{
    int i = gen_bit, ones, k;
    if (gen_isgap) return 1;
    if (i == 0) return 0;
    if (i <= 8) return (gen_byte >> (i - 1)) & 1;
    if (i == 9) { for (ones = 0, k = 0; k < 8; k++) ones += (gen_byte >> k) & 1; return (1 + ones) & 1; }
    return 1;
}
static void gen_load_byte(void)
{
    if (gen_leader > 0) { gen_leader--; gen_byte = 0x16; gen_isgap = 0; }
    else if (gen_gapbytes > 0) { gen_gapbytes--; gen_isgap = 1; gen_byte = 0xFF; }
    else if (gen_pos < tape_len) {
        if (gen_pos == gen_gap_at && !gen_gap_used) {            /* fin de l'en-tete : la ROM a besoin de temps pour afficher "Loading" */
            gen_gap_used = 1; gen_gapbytes = GEN_GAP - 1; gen_isgap = 1; gen_byte = 0xFF;
        } else { gen_isgap = 0; gen_byte = tape[gen_pos++]; }
    }
    else { gen_state = 0; tape_set_level(0); }
}
static void gen_halt(void)                       /* arrete le signal sans toucher a la position de la bande */
{
    gen_state = 0; gen_idle = 0;
    if (tape_in) tape_set_level(0);
}
static void gen_start(void)                      /* moteur allume : le signal part de la position courante */
{
    size_t p = tape_pos;
    gen_state = 0; gen_leader = 0; gen_idle = 0;
    gen_gap_at = (size_t)-1; gen_gap_used = 0; gen_gapbytes = 0; gen_isgap = 0;
    if (tape_in) tape_set_level(0);
    if (!tape || p >= tape_len) return;
    if (tape[p] == 0x16) {                       /* debut de fichier : leader + synchro */
        size_t q = p;
        while (q < tape_len && tape[q] == 0x16) q++;
        if (q < tape_len && tape[q] == 0x24 && q > p) {
            size_t z = q + 10;                   /* apres 0x24 : 9 octets d'en-tete puis le nom termine par 0 */
            while (z < tape_len && tape[z] != 0) z++;
            gen_gap_at = z + 1;
            p = q; gen_leader = GEN_LEADER;
        }
    }
    gen_pos = p; gen_state = 1; gen_t = 20000;   /* petit silence avant le leader */
}
static void gen_stop(void)                       /* moteur coupe : la bande reste la ou le signal s'est arrete */
{
    if (gen_state && gen_pos > tape_pos) {
        size_t q, r;
        tape_pos = gen_pos;
        /* arret au milieu d'un leader (suite de 0x16 suivie de 0x24) : on revient au debut de la serie,
           pour que le prochain demarrage rejoue un leader complet et la synchro */
        q = tape_pos; r = tape_pos;
        while (q > 0 && tape[q - 1] == 0x16) q--;
        while (r < tape_len && tape[r] == 0x16) r++;
        if (r < tape_len && tape[r] == 0x24 && r > q) tape_pos = q;
    }
    gen_halt();
}
static void tape_tick(int cy)
{
    if (!tape_motor) return;
    if (!gen_state) {
        /* acceleration K7 : les lectures de la ROM sont interceptees ; si plus aucun appel n'arrive alors que le
           moteur tourne, un programme lit la K7 lui-meme -> on lui envoie le signal */
        if (cfg.fast_tape && tape && tape_pos < tape_len) { gen_idle += cy; if (gen_idle > 30000) gen_start(); }
        return;
    }
    gen_t -= cy;
    while (gen_t <= 0 && gen_state) {
        if (gen_state == 1) { gen_state = 2; gen_phase = 2; gen_bit = 13; }
        if (gen_phase == 0) {
            tape_set_level(1); gen_phase = 1; gen_t += SAMPLE_CYC;
        } else if (gen_phase == 1) {
            tape_set_level(0); gen_phase = 2; gen_t += SAMPLE_CYC * (gen_curbit ? 1 : 2);
        } else {
            gen_bit++;
            if (gen_bit > 13) { gen_load_byte(); gen_bit = 0; if (!gen_state) break; }
            gen_curbit = gen_bit_value(); gen_phase = 0;
        }
    }
}

/* ====================================================================== */
/*  Bus + CPU 6502                                                        */
/* ====================================================================== */
static u8 io_read(u16 a)
{
    if (a < 0x310 || !disk_on) return via_read(a & 15);
    if (a < 0x314) return fdc_read(a & 3);
    if (a < 0x318) return fdc.intrq ? 0x7F : 0xFF;
    if (a < 0x31C) return fdc.drq ? 0x7F : 0xFF;
    return 0xFF;
}
static void io_write(u16 a, u8 v)
{
    if (a < 0x310 || !disk_on) { via_write(a & 15, v); return; }
    if (a < 0x314) fdc_write(a & 3, v);
    else if (a < 0x318) md_control(v);
}
static u8 rd(u16 a)
{
    if ((int)a > ram_top) return paged_rom[a - ram_top - 1];
    if ((a & 0xFF00) == 0x0300) return io_read(a);
    if (a == 0x02DF && tq_pos < tq_len && tq_delay <= 0) {
        u8 v = (u8)tq[tq_pos++] | 0x80;
        if (tq_pos >= tq_len) tq_pos = tq_len = 0;
        return v;
    }
    return ram[a];
}
static void wr(u16 a, u8 v)
{
    if ((a & 0xFF00) == 0x0300) { io_write(a, v); return; }   /* sinon : toujours la RAM, meme sous une ROM */
    ram[a] = v;
}
static u16 rd16(u16 a) { return (u16)(rd(a) | (rd((u16)(a + 1)) << 8)); }

#define FC 0x01
#define FZ 0x02
#define FI 0x04
#define FD 0x08
#define FB 0x10
#define FU 0x20
#define FV 0x40
#define FN 0x80
static u8 A, X, Y, S, P;
static u16 PC;
static int nmi_pending, cyc;
static long stat_cyc, stat_irq, stat_nmi, stat_frames;

#define SETNZ(v) (P = (u8)((P & ~(FN | FZ)) | ((v) & 0x80) | ((v) ? 0 : FZ)))
static void push(u8 v) { ram[0x100 + S] = v; S--; }
static u8 pull(void) { S++; return ram[0x100 + S]; }

static u16 ea(int mode, int wrt)
{
    u16 a, b, z;
    switch (mode) {
    case 0: a = PC++; cyc = 2; break;
    case 1: a = rd(PC++); cyc = 3; break;
    case 2: a = (u8)(rd(PC++) + X); cyc = 4; break;
    case 3: a = (u8)(rd(PC++) + Y); cyc = 4; break;
    case 4: a = rd16(PC); PC += 2; cyc = 4; break;
    case 5: b = rd16(PC); PC += 2; a = (u16)(b + X); cyc = (wrt || ((a ^ b) & 0xFF00)) ? 5 : 4; break;
    case 6: b = rd16(PC); PC += 2; a = (u16)(b + Y); cyc = (wrt || ((a ^ b) & 0xFF00)) ? 5 : 4; break;
    case 7: z = (u8)(rd(PC++) + X); a = (u16)(rd(z) | (rd((u8)(z + 1)) << 8)); cyc = 6; break;
    default: z = rd(PC++); b = (u16)(rd(z) | (rd((u8)(z + 1)) << 8)); a = (u16)(b + Y); cyc = (wrt || ((a ^ b) & 0xFF00)) ? 6 : 5; break;
    }
    return a;
}
static void op_adc(u8 v)
{
    unsigned c = P & 1;
    if (P & FD) {
        unsigned al = (A & 15) + (v & 15) + c, a, bin = A + v + c;
        if (al >= 10) al = ((al + 6) & 15) + 16;
        a = (A & 0xF0) + (v & 0xF0) + al;
        P &= (u8)~(FC | FZ | FN | FV);
        if (!(bin & 255)) P |= FZ;
        if (a & 0x80) P |= FN;
        if (~(A ^ v) & (A ^ a) & 0x80) P |= FV;
        if (a >= 0xA0) a += 0x60;
        if (a >= 0x100) P |= FC;
        A = (u8)a;
    } else {
        unsigned r = A + v + c;
        P &= (u8)~(FC | FV);
        if (r > 255) P |= FC;
        if (~(A ^ v) & (A ^ r) & 0x80) P |= FV;
        A = (u8)r; SETNZ(A);
    }
}
static void op_sbc(u8 v)
{
    unsigned c = P & 1;
    if (P & FD) {
        int al = (A & 15) - (v & 15) + (int)c - 1, a;
        unsigned r = A + (u8)~v + c;
        if (al < 0) al = ((al - 6) & 15) - 16;
        a = (A & 0xF0) - (v & 0xF0) + al;
        if (a < 0) a -= 0x60;
        P &= (u8)~(FC | FZ | FN | FV);
        if (r > 255) P |= FC;
        if ((A ^ v) & (A ^ r) & 0x80) P |= FV;
        if (r & 0x80) P |= FN;
        if (!(r & 255)) P |= FZ;
        A = (u8)a;
    } else {
        unsigned r = A + (u8)~v + c;
        P &= (u8)~(FC | FV);
        if (r > 255) P |= FC;
        if ((A ^ v) & (A ^ r) & 0x80) P |= FV;
        A = (u8)r; SETNZ(A);
    }
}
static void op_cmp(u8 r, u8 v)
{
    u8 t = (u8)(r - v);
    P = (u8)((P & ~FC) | (r >= v ? FC : 0));
    SETNZ(t);
}
static u8 op_asl(u8 v) { P = (u8)((P & ~FC) | (v >> 7)); v <<= 1; SETNZ(v); return v; }
static u8 op_lsr(u8 v) { P = (u8)((P & ~FC) | (v & 1)); v >>= 1; SETNZ(v); return v; }
static u8 op_rol(u8 v) { u8 c = P & 1; P = (u8)((P & ~FC) | (v >> 7)); v = (u8)((v << 1) | c); SETNZ(v); return v; }
static u8 op_ror(u8 v) { u8 c = (u8)((P & 1) << 7); P = (u8)((P & ~FC) | (v & 1)); v = (u8)((v >> 1) | c); SETNZ(v); return v; }

static int irq_active(void) { return via.irq || md_irq(); }

/* Adresses des routines K7 de la ROM (valeurs d'Oricutron) :
   synchro (a sauter) et lecture d'un octet (octet -> A, $2F ; RTS final a l'adresse *_end) */
static int hook_rb = 0xE6C9, hook_rb_end = 0xE6FB, hook_sync = 0xE735, hook_sync_end = 0xE759, hook_atmos = 1;

static int tape_hook(void)
{
    u8 b = tape[tape_pos++];
    gen_halt();
    A = b;
    ram[0x2F] = b;
    if (hook_atmos) { ram[0x2B1] = 0; P |= FC; } else P &= (u8)~FC;
    P = (u8)((P & ~(FN | FZ)) | (b & 0x80) | (b ? 0 : FZ));
    PC = (u16)hook_rb_end;                              /* execute le vrai RTS de la ROM */
    tape_act = 40000; tape_last = b;
    return 6;
}
/* saute la fonction de synchro. Selon le code appelant, la bande est placee :
   - juste AVANT le 0x24 si l'appelant boucle ensuite sur "JSR lecture_octet / CMP #$24 / BNE"
     (c'est le cas de CLOAD et des loaders qui recopient son code),
   - juste APRES le 0x24 sinon.                                                              */
static int tape_sync_hook(void)
{
    size_t p = tape_pos;
    gen_halt();
    int found = 0, loop_style;
    {
        u16 ret = (u16)((ram[0x100 + (u8)(S + 1)] | (ram[0x100 + (u8)(S + 2)] << 8)) + 1);
        loop_style = rd(ret) == 0x20 && rd((u16)(ret + 1)) == (hook_rb & 255) && rd((u16)(ret + 2)) == ((hook_rb >> 8) & 255) &&
                     rd((u16)(ret + 3)) == 0xC9 && rd((u16)(ret + 4)) == 0x24;
    }
    while (p < tape_len && !found) {
        if (tape[p] == 0x16) {
            size_t q = p;
            while (q < tape_len && tape[q] == 0x16) q++;
            if (q < tape_len && tape[q] == 0x24) { tape_pos = loop_style ? q : q + 1; found = 1; }
            p = q;
        } else p++;
    }
    if (!found) tape_pos = tape_len;                    /* fin de bande */
    PC = (u16)hook_sync_end;
    tape_act = 40000; tape_last = 0x16;
    return 6;
}

static int cpu_step(void)
{
    u8 op, v;
    u16 a;
    int aaa, bbb, cc, m;
    if (nmi_pending) {
        nmi_pending = 0; stat_nmi++;
        push((u8)(PC >> 8)); push((u8)PC); push((u8)((P & ~FB) | FU)); P |= FI;
        PC = rd16(0xFFFA);
        return 7;
    }
    if (!(P & FI) && irq_active()) {
        stat_irq++;
        push((u8)(PC >> 8)); push((u8)PC); push((u8)((P & ~FB) | FU)); P |= FI;
        PC = rd16(0xFFFE);
        return 7;
    }
    if (cfg.fast_tape && rom_mode == 0 && tape && tape_pos < tape_len) {
        if (PC == hook_rb) return tape_hook();
        if (PC == hook_sync) return tape_sync_hook();
    }

    op = rd(PC++);
    aaa = op >> 5; bbb = (op >> 2) & 7; cc = op & 3;
    switch (cc) {
    case 1: {
        static const u8 modes[8] = { 7, 1, 0, 4, 8, 2, 6, 5 };
        if (op == 0x89) { PC++; return 2; }
        m = modes[bbb];
        if (aaa == 4) { a = ea(m, 1); wr(a, A); return cyc; }
        a = ea(m, 0); v = rd(a);
        switch (aaa) {
        case 0: A |= v; SETNZ(A); break;
        case 1: A &= v; SETNZ(A); break;
        case 2: A ^= v; SETNZ(A); break;
        case 3: op_adc(v); break;
        case 5: A = v; SETNZ(A); break;
        case 6: op_cmp(A, v); break;
        default: op_sbc(v); break;
        }
        return cyc;
    }
    case 2:
        if (bbb == 2) {
            switch (aaa) {
            case 0: A = op_asl(A); break;
            case 1: A = op_rol(A); break;
            case 2: A = op_lsr(A); break;
            case 3: A = op_ror(A); break;
            case 4: A = X; SETNZ(A); break;
            case 5: X = A; SETNZ(X); break;
            case 6: X--; SETNZ(X); break;
            default: break;
            }
            return 2;
        }
        if (bbb == 6) {
            if (aaa == 4) S = X;
            else if (aaa == 5) { X = S; SETNZ(X); }
            return 2;
        }
        if (bbb == 4) return 2;
        if (bbb == 0 && aaa != 5) { PC++; return 2; }
        switch (bbb) {
        case 0: m = 0; break;
        case 1: m = 1; break;
        case 3: m = 4; break;
        case 5: m = (aaa == 4 || aaa == 5) ? 3 : 2; break;
        default: m = (aaa == 5) ? 6 : 5; break;
        }
        if (aaa == 4) { a = ea(m, 1); if (bbb != 7) wr(a, X); return cyc; }
        if (aaa == 5) { a = ea(m, 0); X = rd(a); SETNZ(X); return cyc; }
        a = ea(m, 1); v = rd(a);
        switch (aaa) {
        case 0: v = op_asl(v); break;
        case 1: v = op_rol(v); break;
        case 2: v = op_lsr(v); break;
        case 3: v = op_ror(v); break;
        case 6: v--; SETNZ(v); break;
        default: v++; SETNZ(v); break;
        }
        wr(a, v);
        return cyc + 2;
    case 0:
        switch (bbb) {
        case 4: {
            static const u8 mask[4] = { FN, FV, FC, FZ };
            s8 off = (s8)rd(PC++);
            int c = 2;
            if (((P & mask[aaa >> 1]) != 0) == (aaa & 1)) {
                u16 t = (u16)(PC + off);
                c++;
                if ((t ^ PC) & 0xFF00) c++;
                PC = t;
            }
            return c;
        }
        case 2:
            switch (aaa) {
            case 0: push((u8)(P | FB | FU)); return 3;
            case 1: P = (u8)((pull() & ~FB) | FU); return 4;
            case 2: push(A); return 3;
            case 3: A = pull(); SETNZ(A); return 4;
            case 4: Y--; SETNZ(Y); return 2;
            case 5: Y = A; SETNZ(Y); return 2;
            case 6: Y++; SETNZ(Y); return 2;
            default: X++; SETNZ(X); return 2;
            }
        case 6:
            switch (aaa) {
            case 0: P &= (u8)~FC; break;
            case 1: P |= FC; break;
            case 2: P &= (u8)~FI; break;
            case 3: P |= FI; break;
            case 4: A = Y; SETNZ(A); break;
            case 5: P &= (u8)~FV; break;
            case 6: P &= (u8)~FD; break;
            default: P |= FD; break;
            }
            return 2;
        case 0:
            switch (aaa) {
            case 0: PC++; push((u8)(PC >> 8)); push((u8)PC); push((u8)(P | FB | FU)); P |= FI; PC = rd16(0xFFFE); return 7;
            case 1: a = rd16(PC); { u16 t = (u16)(PC + 1); push((u8)(t >> 8)); push((u8)t); } PC = a; return 6;
            case 2: P = (u8)((pull() & ~FB) | FU); PC = pull(); PC |= (u16)(pull() << 8); return 6;
            case 3: PC = pull(); PC |= (u16)(pull() << 8); PC++; return 6;
            case 4: PC++; return 2;
            default:
                a = ea(0, 0); v = rd(a);
                if (aaa == 5) { Y = v; SETNZ(Y); }
                else if (aaa == 6) op_cmp(Y, v);
                else op_cmp(X, v);
                return 2;
            }
        default:
            m = (bbb == 1) ? 1 : (bbb == 3) ? 4 : (bbb == 5) ? 2 : 5;
            if (bbb == 3 && aaa == 2) { PC = rd16(PC); return 3; }
            if (bbb == 3 && aaa == 3) {
                u16 p = rd16(PC);
                PC = (u16)(rd(p) | (rd((u16)((p & 0xFF00) | ((p + 1) & 0xFF))) << 8));
                return 5;
            }
            if (aaa == 4) { a = ea(m, 1); if (bbb != 7) wr(a, Y); return cyc; }
            a = ea(m, 0);
            if (aaa == 5) { Y = rd(a); SETNZ(Y); return cyc; }
            if (aaa == 1 && (bbb == 1 || bbb == 3)) {
                v = rd(a);
                P = (u8)((P & ~(FN | FV | FZ)) | (v & 0xC0) | ((A & v) ? 0 : FZ));
                return cyc;
            }
            if ((aaa == 6 || aaa == 7) && (bbb == 1 || bbb == 3)) { v = rd(a); op_cmp(aaa == 6 ? Y : X, v); return cyc; }
            return cyc;                         /* NOP illegaux */
        }
    default:
        return 2;                               /* cc == 3 : opcodes illegaux -> NOP */
    }
}

static void cpu_reset(void)
{
    A = X = Y = 0; S = 0xFD; P = FI | FU; nmi_pending = 0;
    PC = rd16(0xFFFC);
}

/* ====================================================================== */
/*  ULA / video                                                            */
/* ====================================================================== */
#define VW 240
#define VH 224
static u32 fb[VH][VW];
static const u32 pal[8] = { 0x000000, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF };
static int hires_mode, next_sixty, frame_lines = 312, vline, vcyc, frame_done;
static long frame_counter;

static void render_line(int y)
{
    u32 *dst = fb[y];
    int ink = 7, paper = 0, alt = 0, dbl = 0, blink = 0, col, i;
    int hi = hires_mode && y < 200;
    int sub = y & 7;
    u16 base, cs = hires_mode ? 0x9800 : 0xB400;
    int new_hires = hires_mode, new_sixty = next_sixty;

    if (hi) base = (u16)(0xA000 + y * 40);
    else if (hires_mode) base = (u16)(0xBF68 + ((y - 200) >> 3) * 40);
    else base = (u16)(0xBB80 + (y >> 3) * 40);

    for (col = 0; col < 40; col++) {
        u8 b = vram[(u16)(base + col)];
        int c = b & 0x7F, inv = (b & 0x80) != 0, px;
        if ((c & 0x60) == 0) {
            switch (c & 0x18) {
            case 0x00: ink = c & 7; break;
            case 0x08: alt = c & 1; dbl = (c >> 1) & 1; blink = (c >> 2) & 1; break;
            case 0x10: paper = c & 7; break;
            default:   new_hires = (c & 4) != 0; new_sixty = !(c & 2); break;
            }
            px = 0;
        } else if (hi) {
            px = c & 0x3F;
        } else {
            int gr = sub;
            if (dbl) gr = (sub >> 1) + (((y >> 3) & 1) ? 4 : 0);   /* double hauteur : moitie haute sur les lignes texte paires, basse sur les impaires */
            px = vram[(u16)(cs + (alt ? 0x400 : 0) + c * 8 + gr)] & 0x3F;
            if (blink && (frame_counter & 32)) px = 0;
        }
        for (i = 0; i < 6; i++) {
            int bit = (px >> (5 - i)) & 1;
            int clr = bit ? ink : paper;
            if (inv) clr ^= 7;
            dst[col * 6 + i] = pal[clr];
        }
    }
    hires_mode = new_hires; next_sixty = new_sixty;
}
static void video_line(void)
{
    int vs = (frame_lines == 312) ? 256 : 240;
    if (vline < VH) render_line(vline);
    vline++;
    if (vline == vs) { vsync_lvl = 1; cb1_update(); }
    else if (vline == vs + 3) { vsync_lvl = 0; cb1_update(); }
    if (vline >= frame_lines) {
        vline = 0; frame_counter++; stat_frames++; frame_done = 1;
        frame_lines = next_sixty ? 264 : 312;
    }
}
static void video_run(int cy)
{
    vcyc += cy;
    while (vcyc >= 64) { vcyc -= 64; video_line(); }
}

/* ====================================================================== */
/*  Audio : generation d'echantillons (AY + son de chargement faible)     */
/* ====================================================================== */
#define SR 44100
#define RING 16384
static short ring[RING];
static volatile int ring_r, ring_w;
static int audio_muted;
static double out_phase, cps_base = 1000000.0 / SR;
static int mix_sum, mix_n, ay_acc;
static double dc_x, dc_y;
static double tph, bitclk; static int bitidx, nval, nctr;
static u32 snoise = 12345;

static int ring_fill(void) { return (ring_w - ring_r) & (RING - 1); }

static int load_sound(double cps)
{
    double amp = 6000.0 * cfg.loadvol / 100.0;
    int s = 0;
    if (!cfg.loadsnd) return 0;
    if (tape_act > 0) {
        int bit = (tape_last >> bitidx) & 1;
        tape_act -= (int)cps;
        tph += (bit ? 2400.0 : 1200.0) / SR; if (tph >= 1.0) tph -= 1.0;
        s += (tph < 0.5) ? (int)amp : -(int)amp;
        bitclk += 1.0;
        if (bitclk >= 18.3) { bitclk -= 18.3; bitidx = (bitidx + 1) & 7; }
    }
    if (gen_state == 2) s += tape_in ? (int)amp : -(int)amp;      /* signal K7 audible, comme sur un magnetophone */
    if (disk_act > 0) {
        disk_act -= (int)cps;
        if ((nctr++ & 7) == 0) { snoise = snoise * 1664525u + 1013904223u; nval = (snoise & 0x10000) ? 1 : -1; }
        s += (int)(nval * amp * 0.5);
    }
    if (click_env > 0.01) {
        snoise = snoise * 1664525u + 1013904223u;
        s += (int)(((snoise & 0x10000) ? 1 : -1) * click_env * amp * 1.5);
        click_env *= 0.996;
    }
    return s;
}
static void audio_push(int v)
{
    int w = ring_w, nx = (w + 1) & (RING - 1);
    if (nx == ring_r) return;
    ring[w] = (short)v;
    ring_w = nx;
}
static void audio_run(int cy)
{
    double cps = cps_base * (1.0 + 0.02 * ((double)ring_fill() - 2048.0) / 8192.0);
    ay_acc += cy;
    while (ay_acc >= 8) {
        ay_acc -= 8;
        mix_sum += ay_tick(); mix_n++;
        out_phase += 8.0;
        while (out_phase >= cps) {
            double x, y;
            int v;
            out_phase -= cps;
            x = (double)mix_sum / (mix_n ? mix_n : 1);
            mix_sum = 0; mix_n = 0;
            y = x - dc_x + 0.997 * dc_y; dc_x = x; dc_y = y;
            v = (int)(y * cfg.volume / 100.0) + (int)(load_sound(cps) * cfg.volume / 100.0);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            audio_push(v);
        }
    }
}

/* ====================================================================== */
/*  Machine : pas, frame, reset                                           */
/* ====================================================================== */
static int machine_running;
static int periph_acc;      /* reste de la division cycles CPU -> cycles peripheriques (turbo CPU) */

/* Turbo CPU : le processeur execute 'mult' fois plus de cycles dans le meme temps, alors que la VIA
   (timers/IRQ), l'ULA (images, VSYNC), l'AY et le FDC avancent au rythme normal de 1 MHz.        */
static void emu_step(void)
{
    int cy = cpu_step(), pcy, mult = cfg.cpu_turbo ? cfg.cpu_mult : 1;
    stat_cyc += cy;
    if (mult > 1) { periph_acc += cy; pcy = periph_acc / mult; periph_acc -= pcy * mult; }
    else pcy = cy;
    if (pcy > 0) {
        via_tick(pcy);
        fdc_run(pcy);
        tape_tick(pcy);
        video_run(pcy);
        if (!audio_muted) audio_run(pcy);
    }
    if (tq_delay > 0) tq_delay -= pcy;
    if (tape_act > 0 && audio_muted) tape_act -= pcy;
}
static void housekeeping(void)
{
    if (tape && tape_pos > 0) hook_frame = hook_frame ? hook_frame : frame_counter;
    if (pending_run && tape && tape_pos >= tape_len && tq_pos >= tq_len && tape_act <= 0) {
        type_str("RUN\r", 1);
        pending_run = 0;
    }
}
static void emu_frame(void)
{
    frame_done = 0;
    while (!frame_done) emu_step();
    housekeeping();
}
static void video_only_frame(void)
{
    frame_done = 0;
    while (!frame_done) video_run(64);
}

/* Contenu de la RAM a la mise sous tension : sur un vrai Oric il est aleatoire. Certains logiciels en dependent
   (le secteur de boot Sedoric additionne la RAM de recouvrement $C980-$FFFF et ne charge que 4 secteurs
   au lieu de 60 si la somme est nulle). Motif pseudo-aleatoire reproductible ; la zone ecran ($A000-$BFFF)
   reste a zero pour ne pas afficher de pixels parasites avant l'initialisation de la ROM.                 */
static void ram_power_on(void)
{
    u32 x = 0x1337C0DEu;
    int i;
    for (i = 0; i < 65536; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        ram[i] = (i >= 0xA000 && i < 0xC000) ? 0 : (u8)(x >> 11);
    }
}
static void machine_hard_reset(void)
{
    ram_power_on();
    md_ctl = 0;
    paging_update();
    via_reset(); ay_reset(); fdc_reset();
    memset(keyrows, 0, sizeof keyrows);
    hires_mode = 0; next_sixty = 0; frame_lines = 312; vline = 0; vcyc = 0;
    vsync_lvl = 0; tq_len = tq_pos = 0; tq_delay = 0;
    tape_act = 0; periph_acc = 0; ay_acc = 0; gen_state = 0; tape_in = 0; fdc_hot = 0; mix_sum = mix_n = 0; out_phase = 0;
    ring_r = ring_w = 0;
    cpu_reset();
}
static void machine_nmi(void) { nmi_pending = 1; }

enum { MODE_NOROM, MODE_RUN, MODE_BROWSER };
static int mode = MODE_NOROM;
static int boot_turbo;           /* frames a executer a vitesse maximale (demarrage) */
static int paused;               /* emulation en pause */

/* ====================================================================== */
/*  CRC32 / Adler32 / ecriture PNG (sans bibliotheque : deflate "fixed Huffman" + repetitions) */
/* ====================================================================== */
static u32 crc_table[256];
static u32 crc32_buf(u32 crc, const u8 *b, size_t n)
{
    size_t i;
    if (!crc_table[1]) {
        u32 k, c; int j;
        for (k = 0; k < 256; k++) { c = k; for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[k] = c; }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) crc = crc_table[(crc ^ b[i]) & 255] ^ (crc >> 8);
    return ~crc;
}
typedef struct { u8 *buf; size_t len, cap; u32 bits; int nb; } BitW;
static void bw_byte(BitW *w, u8 v)
{
    if (w->len >= w->cap) { w->cap = w->cap ? w->cap * 2 : 65536; w->buf = (u8 *)realloc(w->buf, w->cap); }
    w->buf[w->len++] = v;
}
static void bw_bits(BitW *w, u32 v, int n)                 /* bits de poids faible d'abord */
{
    w->bits |= v << w->nb; w->nb += n;
    while (w->nb >= 8) { bw_byte(w, (u8)w->bits); w->bits >>= 8; w->nb -= 8; }
}
static void bw_huff(BitW *w, u32 code, int n)              /* code de Huffman : bits de poids fort d'abord */
{
    u32 r = 0; int i;
    for (i = 0; i < n; i++) { r = (r << 1) | (code & 1); code >>= 1; }
    bw_bits(w, r, n);
}
static void bw_lit(BitW *w, int v)
{
    if (v < 144) bw_huff(w, 0x30 + (u32)v, 8);
    else if (v < 256) bw_huff(w, 0x190 + (u32)(v - 144), 9);
    else if (v < 280) bw_huff(w, (u32)(v - 256), 7);
    else bw_huff(w, 0xC0 + (u32)(v - 280), 8);
}
static void bw_match(BitW *w, int len, int dist)
{
    static const int lb[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const int le[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const int db[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const int de[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    int c = 28, d = 29;
    while (lb[c] > len) c--;
    bw_lit(w, 257 + c);
    if (le[c]) bw_bits(w, (u32)(len - lb[c]), le[c]);
    while (db[d] > dist) d--;
    bw_huff(w, (u32)d, 5);
    if (de[d]) bw_bits(w, (u32)(dist - db[d]), de[d]);
}
/* compresse 'raw' (n octets) : repetitions a distance 1 (zones unies) ou 'row' (lignes identiques) */
static void deflate_fixed(BitW *w, const u8 *raw, size_t n, int row)
{
    size_t i = 0;
    bw_bits(w, 1, 1); bw_bits(w, 1, 2);                      /* dernier bloc, Huffman fixe */
    while (i < n) {
        int best = 0, bd = 0, l;
        if (i >= 1) { l = 0; while (i + (size_t)l < n && l < 258 && raw[i + l] == raw[i + l - 1]) l++; if (l > best) { best = l; bd = 1; } }
        if (row > 1 && i >= (size_t)row) { l = 0; while (i + (size_t)l < n && l < 258 && raw[i + l] == raw[i + l - row]) l++; if (l > best) { best = l; bd = row; } }
        if (best >= 4) { bw_match(w, best, bd); i += (size_t)best; }
        else { bw_lit(w, raw[i]); i++; }
    }
    bw_lit(w, 256);
    if (w->nb) bw_bits(w, 0, 8 - w->nb);
}
static void put32(FILE *f, u32 v) { fputc((int)(v >> 24), f); fputc((int)(v >> 16) & 255, f); fputc((int)(v >> 8) & 255, f); fputc((int)v & 255, f); }
static void png_chunk(FILE *f, const char *type, const u8 *data, size_t n)
{
    u32 c = crc32_buf(0, (const u8 *)type, 4);
    put32(f, (u32)n); fwrite(type, 1, 4, f);
    if (n) { fwrite(data, 1, n, f); c = crc32_buf(c, data, n); }
    put32(f, c);
}
/* enregistre l'image Oric (240x224) agrandie 'scale' fois (pixels nets) en PNG */
static int png_save(const char *path, int scale)
{
    int W = VW * scale, H = VH * scale, x, y, k;
    size_t rowb = (size_t)W * 3 + 1, i, a = 1, b = 0;
    u8 *raw = (u8 *)malloc(rowb * (size_t)H), ihdr[13];
    BitW w;
    FILE *f;
    if (!raw) return 0;
    for (y = 0; y < H; y++) {                                /* lignes filtrees "Sub" (zero dans les zones unies) */
        u8 *r = raw + rowb * (size_t)y, prev[3] = { 0, 0, 0 };
        r[0] = 1;
        for (x = 0; x < W; x++) {
            u32 p = fb[y / scale][x / scale];
            u8 c[3];
            c[0] = (u8)(p >> 16); c[1] = (u8)(p >> 8); c[2] = (u8)p;
            for (k = 0; k < 3; k++) { r[1 + x * 3 + k] = (u8)(c[k] - prev[k]); prev[k] = c[k]; }
        }
    }
    for (i = 0; i < rowb * (size_t)H; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    memset(&w, 0, sizeof w);
    bw_byte(&w, 0x78); bw_byte(&w, 0x01);                    /* en-tete zlib */
    deflate_fixed(&w, raw, rowb * (size_t)H, (int)rowb);
    bw_byte(&w, (u8)(b >> 8)); bw_byte(&w, (u8)b); bw_byte(&w, (u8)(a >> 8)); bw_byte(&w, (u8)a);   /* Adler32 */
    f = fopen(path, "wb");
    if (!f) { free(raw); free(w.buf); return 0; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    ihdr[0] = (u8)(W >> 24); ihdr[1] = (u8)(W >> 16); ihdr[2] = (u8)(W >> 8); ihdr[3] = (u8)W;
    ihdr[4] = (u8)(H >> 24); ihdr[5] = (u8)(H >> 16); ihdr[6] = (u8)(H >> 8); ihdr[7] = (u8)H;
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", w.buf, w.len);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(w.buf);
    return 1;
}

/* ====================================================================== */
/*  Snapshots : etat complet de la machine (RAM, CPU, VIA, AY, FDC, ULA, medias)  */
/* ====================================================================== */
#define SNAP_VER 1
typedef struct {
    char magic[8];
    u32 ver, sz_via, sz_ay, sz_fdc, sz_misc, rom_crc, drom_crc;
    char game[64], tape[MAX_PATH], disk[4][MAX_PATH];
} SnapHdr;
typedef struct {
    u8 a, x, y, s, p; u16 pc;
    int nmi_pending, disk_on, md_ctl, tape_motor, kbd_row, vsync_lvl, hires_mode, next_sixty, frame_lines, vline, vcyc, pending_run;
    long frame_counter;
    u32 tape_pos;
} SnapMisc;

/* nom du "jeu" courant (disquette, sinon K7, sinon BASIC), nettoye pour servir de nom de fichier */
static void game_name(char *out, int n)
{
    const char *src = "BASIC", *dot;
    char tmp[MAX_PATH];
    int i, o = 0;
    if (disk_on && drv[0].loaded) src = base_name(drv[0].path);
    else if (tape && tape_len && tape_name[0]) src = base_name(tape_name);
    snprintf(tmp, sizeof tmp, "%s", src);
    dot = strrchr(tmp, '.');
    if (dot && dot != tmp) tmp[dot - tmp] = 0;
    for (i = 0; tmp[i] && o < n - 1; i++) {
        char c = tmp[i];
        out[o++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    out[o] = 0;
    if (!o) snprintf(out, (size_t)n, "BASIC");
}
static int snap_save(const char *path)
{
    SnapHdr h;
    SnapMisc m;
    FILE *f;
    int i;
    u8 *sx = fdc.xbuf, *st = fdc.tp;
    int ss = fdc.state, sb = fdc.busy, sd = fdc.drq;
    memset(&h, 0, sizeof h); memset(&m, 0, sizeof m);
    memcpy(h.magic, "ORICSNAP", 8);
    h.ver = SNAP_VER; h.sz_via = (u32)sizeof via; h.sz_ay = (u32)sizeof ay; h.sz_fdc = (u32)sizeof fdc; h.sz_misc = (u32)sizeof m;
    h.rom_crc = crc32_buf(0, rom_basic, sizeof rom_basic);
    h.drom_crc = have_disk ? crc32_buf(0, rom_disk, (size_t)rom_disk_size) : 0;
    game_name(h.game, sizeof h.game);
    if (tape && tape_len) snprintf(h.tape, MAX_PATH, "%s", tape_name);
    for (i = 0; i < 4; i++) if (drv[i].loaded) snprintf(h.disk[i], MAX_PATH, "%s", drv[i].path);
    m.a = A; m.x = X; m.y = Y; m.s = S; m.p = P; m.pc = PC;
    m.nmi_pending = nmi_pending; m.disk_on = disk_on; m.md_ctl = md_ctl; m.tape_motor = tape_motor; m.kbd_row = kbd_row;
    m.vsync_lvl = vsync_lvl; m.hires_mode = hires_mode; m.next_sixty = next_sixty; m.frame_lines = frame_lines;
    m.vline = vline; m.vcyc = vcyc; m.pending_run = pending_run; m.frame_counter = frame_counter; m.tape_pos = (u32)tape_pos;
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(&h, sizeof h, 1, f);
    fwrite(&m, sizeof m, 1, f);
    fwrite(ram, 1, sizeof ram, f);
    fwrite(&via, sizeof via, 1, f);
    fwrite(&ay, sizeof ay, 1, f);
    fdc.xbuf = NULL; fdc.tp = NULL; fdc.state = F_IDLE; fdc.busy = 0; fdc.drq = 0;     /* un transfert en cours est abandonne */
    fwrite(&fdc, sizeof fdc, 1, f);
    fdc.xbuf = sx; fdc.tp = st; fdc.state = ss; fdc.busy = sb; fdc.drq = sd;
    fclose(f);
    return 1;
}
static int snap_load(const char *path)
{
    SnapHdr h;
    SnapMisc m;
    FILE *f = fopen(path, "rb");
    u8 *rb = NULL, *vb = NULL, *ab = NULL, *fb2 = NULL;
    int ok = 0, i;
    if (!f) { ui_error("Snapshot illisible."); return 0; }
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "ORICSNAP", 8) != 0 || h.ver != SNAP_VER ||
        h.sz_via != sizeof via || h.sz_ay != sizeof ay || h.sz_fdc != sizeof fdc || h.sz_misc != sizeof m) {
        fclose(f); ui_error("Snapshot invalide ou cree par une autre version de l'emulateur."); return 0;
    }
    if (h.rom_crc != crc32_buf(0, rom_basic, sizeof rom_basic) ||
        (have_disk && h.drom_crc && h.drom_crc != crc32_buf(0, rom_disk, (size_t)rom_disk_size))) {
        fclose(f); ui_error("Ce snapshot a ete cree avec d'autres ROM (BASIC ou disque) : changez de ROM puis recommencez."); return 0;
    }
    rb = (u8 *)malloc(sizeof ram); vb = (u8 *)malloc(sizeof via); ab = (u8 *)malloc(sizeof ay); fb2 = (u8 *)malloc(sizeof fdc);
    if (rb && vb && ab && fb2 && fread(&m, sizeof m, 1, f) == 1 && fread(rb, 1, sizeof ram, f) == sizeof ram &&
        fread(vb, sizeof via, 1, f) == 1 && fread(ab, sizeof ay, 1, f) == 1 && fread(fb2, sizeof fdc, 1, f) == 1) ok = 1;
    fclose(f);
    if (!ok) { free(rb); free(vb); free(ab); free(fb2); ui_error("Snapshot tronque."); return 0; }

    /* medias : on recharge la K7 / les disquettes du snapshot si elles ne sont pas deja en place */
    if (h.tape[0] && (!tape || _stricmp(tape_name, h.tape) != 0)) tape_load(h.tape);
    for (i = 0; i < 4; i++)
        if (h.disk[i][0] && (!drv[i].loaded || _stricmp(drv[i].path, h.disk[i]) != 0)) drive_load(i, h.disk[i]);

    memcpy(ram, rb, sizeof ram); memcpy(&via, vb, sizeof via); memcpy(&ay, ab, sizeof ay); memcpy(&fdc, fb2, sizeof fdc);
    free(rb); free(vb); free(ab); free(fb2);
    fdc.xbuf = NULL; fdc.tp = NULL; fdc.state = F_IDLE; fdc.busy = 0; fdc.drq = 0;
    A = m.a; X = m.x; Y = m.y; S = m.s; P = m.p; PC = m.pc;
    nmi_pending = m.nmi_pending; disk_on = m.disk_on && have_disk && drv[0].loaded; md_ctl = m.md_ctl;
    tape_motor = m.tape_motor; kbd_row = m.kbd_row; vsync_lvl = m.vsync_lvl;
    hires_mode = m.hires_mode; next_sixty = m.next_sixty; frame_lines = m.frame_lines; vline = m.vline; vcyc = m.vcyc;
    pending_run = m.pending_run; frame_counter = m.frame_counter;
    if (tape && m.tape_pos <= tape_len) tape_pos = m.tape_pos;
    paging_update();
    via_update_irq();
    memset(keyrows, 0, sizeof keyrows);
    gen_state = 0; tape_in = 0; tq_len = tq_pos = 0; tq_delay = 0; tape_act = 0; fdc_hot = 0; frame_done = 0;
    periph_acc = 0; ay_acc = 0; mix_sum = mix_n = 0; out_phase = 0; ring_r = ring_w = 0;
    machine_running = 1; vram = ram; mode = MODE_RUN; boot_turbo = 0;
    return 1;
}

/* detection de la version de BASIC pour le vecteur K7 */
static void detect_hook(void)
{
    int i, v11 = 1;
    for (i = 0; i < 16384 - 12; i++)
        if (memcmp(rom_basic + i, "BASIC V1.", 9) == 0) { if (rom_basic[i + 9] == '0') v11 = 0; break; }
    if (v11) { hook_rb = 0xE6C9; hook_rb_end = 0xE6FB; hook_sync = 0xE735; hook_sync_end = 0xE759; hook_atmos = 1; }
    else     { hook_rb = 0xE630; hook_rb_end = 0xE65B; hook_sync = 0xE696; hook_sync_end = 0xE6B9; hook_atmos = 0; }
    if (cfg.hook) hook_rb = cfg.hook;
}

/* ====================================================================== */
/*  Chargement des ROM                                                    */
/* ====================================================================== */
static int find_in_dirs(const char *name, char *out)
{
    static const char *sub[] = { "", "roms\\", "rom\\", "ROMS\\", "ROM\\" };
    int i;
    for (i = 0; i < 5; i++) {
        FILE *f;
        snprintf(out, MAX_PATH, "%s\\%s%s", exe_dir, sub[i], name);
        f = fopen(out, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}
/* rom_is_azerty : la ROM AZERTY traduit elle-meme les touches -> pas de remappage logiciel */
static int rom_load_basic(const char *path)
{
    size_t n;
    u8 *b = file_read(path, &n);
    if (!b || n < 8192) { free(b); return 0; }
    memset(rom_basic, 0xFF, sizeof rom_basic);
    if (n >= 16384) memcpy(rom_basic, b + (n - 16384), 16384);
    else memcpy(rom_basic + (16384 - n), b, n);
    free(b);
    have_basic = 1;
    {   /* nom contenant "azerty" */
        char low[MAX_PATH];
        size_t k;
        snprintf(low, sizeof low, "%s", base_name(path));
        for (k = 0; low[k]; k++) if (low[k] >= 'A' && low[k] <= 'Z') low[k] = (char)(low[k] + 32);
        rom_is_azerty = strstr(low, "azerty") != NULL;
    }
    detect_hook();
    return 1;
}
static int rom_load_disk(const char *path)       /* ROM Microdisc (<= 16 Ko, en pratique 8 Ko) */
{
    size_t n;
    u8 *b = file_read(path, &n);
    if (!b || n < 512 || n > 65536) { free(b); return 0; }
    if (n > 8192) { memcpy(rom_disk, b + (n - 8192), 8192); rom_disk_size = 8192; }   /* EPROM Microdisc : 8 Ko en $E000-$FFFF */
    else { memcpy(rom_disk, b, n); rom_disk_size = (int)n; }
    dbg_log("ROM disque chargee : %s (%u octets dans le fichier, %d utilises)", path, (unsigned)n, rom_disk_size);
    have_disk = 1;
    free(b);
    return 1;
}
/* classe une ROM choisie par l'utilisateur : 1 = BASIC (16 Ko), 2 = Microdisc (< 16 Ko), 0 = invalide */
static int rom_assign(const char *path)
{
    size_t n;
    u8 *b = file_read(path, &n);
    free(b);
    if (!b) return 0;
    if (n >= 16384) return rom_load_basic(path) ? 1 : 0;
    return rom_load_disk(path) ? 2 : 0;
}
static void roms_init(void)
{
    char p[MAX_PATH];
    /* recherche dans le dossier de l'exe puis dans roms\ ; par defaut : atmos.rom et dos.rom */
    static const char *names[] = { "atmos.rom", "basic11b.rom", "basic11.rom", "oric1.rom", NULL };
    static const char *dn[] = { "dos.rom", "microdis.rom", "microdisc.rom", NULL };
    int i;
    have_basic = 0; have_disk = 0; rom_disk_size = 0;
    if (cfg.rom[0] && rom_load_basic(cfg.rom)) { /* ROM choisie precedemment */ }
    else {
        cfg.rom[0] = 0;
        for (i = 0; names[i] && !have_basic; i++)
            if (find_in_dirs(names[i], p) && rom_load_basic(p)) snprintf(cfg.rom, MAX_PATH, "%s", p);
    }
    if (!(cfg.mdrom[0] && rom_load_disk(cfg.mdrom))) {
        cfg.mdrom[0] = 0;
        for (i = 0; dn[i] && !have_disk; i++)
            if (find_in_dirs(dn[i], p) && rom_load_disk(p)) snprintf(cfg.mdrom, MAX_PATH, "%s", p);
    }
}
/* demarre une machine "nue" pour recuperer la police (jeu de caracteres copie par la ROM) */
static int capture_font(void)
{
    int i;
    disk_on = 0; md_ctl = 0;
    machine_hard_reset();
    audio_muted = 1;
    for (i = 0; i < 200; i++) emu_frame();
    audio_muted = 0;
    memcpy(bram, ram, sizeof bram);
    return bram[0xB400 + 'A' * 8 + 1] != 0;
}

/* ====================================================================== */
/*  Selecteur de fichiers (rendu par l'ULA de l'Oric)                      */
/* ====================================================================== */

typedef struct { char name[MAX_PATH]; int dir, kind; } Ent;   /* kind 1 rom, 2 dsk, 3 tap */
static Ent *ents; static int nent, nalloc, bsel, btop;
static char bdir[MAX_PATH], bmsg[64], blast[MAX_PATH];   /* blast : nom a selectionner apres un scan */
static int saved_hires;
#define LIST_ROWS 21

static void ent_add(const char *name, int dir, int kind)
{
    if (nent >= nalloc) {
        nalloc = nalloc ? nalloc * 2 : 256;
        ents = (Ent *)realloc(ents, sizeof(Ent) * (size_t)nalloc);
    }
    snprintf(ents[nent].name, MAX_PATH, "%s", name);
    ents[nent].dir = dir; ents[nent].kind = kind;
    nent++;
}
static int ent_cmp(const void *a, const void *b)
{
    const Ent *x = (const Ent *)a, *y = (const Ent *)b;
    if (strcmp(x->name, "..") == 0) return -1;
    if (strcmp(y->name, "..") == 0) return 1;
    if (x->dir != y->dir) return y->dir - x->dir;
    return _stricmp(x->name, y->name);
}
static void browser_restore_sel(void)
{
    int i;
    bsel = 0; btop = 0;
    if (blast[0])
        for (i = 0; i < nent; i++)
            if (_stricmp(ents[i].name, blast) == 0) { bsel = i; break; }
    btop = bsel - LIST_ROWS / 2;
    if (btop > nent - LIST_ROWS) btop = nent - LIST_ROWS;
    if (btop < 0) btop = 0;
}
static void browser_scan(void)
{
    nent = 0; bsel = 0; btop = 0;
#ifdef _WIN32
    if (!bdir[0]) {
        DWORD m = GetLogicalDrives();
        int i;
        for (i = 0; i < 26; i++)
            if (m & (1u << i)) { char d[4] = { (char)('A' + i), ':', '\\', 0 }; ent_add(d, 1, 0); }
        browser_restore_sel();
        return;
    }
    {
        char pat[MAX_PATH + 4];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof pat, "%s*", bdir);
        ent_add("..", 1, 0);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ent_add(fd.cFileName, 1, 0);
                else if (has_ext(fd.cFileName, ".rom")) ent_add(fd.cFileName, 0, 1);
                else if (has_ext(fd.cFileName, ".dsk")) ent_add(fd.cFileName, 0, 2);
                else if (has_ext(fd.cFileName, ".tap")) ent_add(fd.cFileName, 0, 3);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    qsort(ents, (size_t)nent, sizeof(Ent), ent_cmp);
    browser_restore_sel();
#endif
}
static void bput(int row, int ink, int paper, const char *s)
{
    u8 *p = &bram[0xBB80 + row * 40];
    int i, end = 0;
    p[0] = (u8)(16 + paper); p[1] = (u8)ink;
    for (i = 0; i < 38; i++) {
        char ch = ' ';
        if (!end && s[i]) { ch = s[i]; if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = '?'; }
        else end = 1;
        p[2 + i] = (u8)ch;
    }
}
static void browser_draw(void)
{
    char line[MAX_PATH + 16];
    int r, i;
    const char *rn = cfg.rom[0] ? base_name(cfg.rom) : "(aucune)";
    memset(&bram[0xBB80], 0x20, 1120);
    bput(0, 7, 4, " ORIC ATMOS  -  SELECTEUR DE FICHIERS");
    if (!bdir[0]) snprintf(line, sizeof line, "(lecteurs)");
    else {
        size_t n = strlen(bdir);
        if (n > 37) snprintf(line, sizeof line, "...%s", bdir + n - 34);
        else snprintf(line, sizeof line, "%s", bdir);
    }
    bput(1, 6, 0, line);
    bput(2, 4, 0, "--------------------------------------");
    for (r = 0; r < LIST_ROWS; r++) {
        i = btop + r;
        if (i >= nent) { bput(3 + r, 7, 0, ""); continue; }
        {
            Ent *e = &ents[i];
            int ink = e->dir ? 3 : (e->kind == 1 ? 2 : e->kind == 2 ? 6 : 5);
            const char *tag = e->dir ? "" : (e->kind == 1 ? "ROM" : e->kind == 2 ? "DSK" : "TAP");
            char nm[MAX_PATH + 4], full[MAX_PATH * 2];
            int isdef = 0;
            if (e->dir) snprintf(nm, sizeof nm, "[%s]", e->name); else snprintf(nm, sizeof nm, "%s", e->name);
            if (e->kind == 1) {
                snprintf(full, sizeof full, "%s%s", bdir, e->name);
                isdef = (_stricmp(full, cfg.rom) == 0);
            }
            snprintf(line, sizeof line, "%c%-31.31s %3s", isdef ? '*' : ' ', nm, tag);
            if (i == bsel) bput(3 + r, 0, 7, line); else bput(3 + r, ink, 0, line);
        }
    }
    bput(24, 4, 0, "--------------------------------------");
    snprintf(line, sizeof line, "ROM: %s", rn);
    bput(25, 2, 0, line);
    bput(26, 7, 0, "ENTREE:choisir  RETOUR:dossier parent");
    bput(27, 3, 0, bmsg[0] ? bmsg : "ECHAP:BASIC seul  F1:selecteur");
}
static void boot_machine(int use_disk, int autotape);
static void update_title(void);

static void browser_enter(void)
{
    if (mode != MODE_BROWSER) { saved_hires = hires_mode; }
    hires_mode = 0; vram = bram; mode = MODE_BROWSER;
    bmsg[0] = 0;
    if (bdir[0] && nent > 0 && bsel < nent)
        snprintf(blast, MAX_PATH, "%s", ents[bsel].name);     /* reprend la ligne surlignee a la fermeture */
    if (!bdir[0]) {
        snprintf(blast, MAX_PATH, "%s", cfg.lastfile);
        snprintf(bdir, MAX_PATH, "%s", cfg.browse[0] ? cfg.browse : exe_dir);
        { size_t n = strlen(bdir); if (n && bdir[n - 1] != '\\') { bdir[n] = '\\'; bdir[n + 1] = 0; } }
    }
    browser_scan();
    browser_draw();
    update_title();
}
static void browser_leave_resume(void)
{
    vram = ram; hires_mode = saved_hires; mode = MODE_RUN; update_title();
}
static void browser_parent(void)
{
    size_t n = strlen(bdir);
    if (!n) return;
    if (n == 3 && bdir[1] == ':') { snprintf(blast, MAX_PATH, "%s", bdir); bdir[0] = 0; return; }
    bdir[n - 1] = 0;
    {
        char *p = strrchr(bdir, '\\');
        snprintf(blast, MAX_PATH, "%s", p ? p + 1 : bdir);      /* nom du dossier quitte */
        if (p) p[1] = 0; else bdir[0] = 0;
    }
}
static void browser_select(void)
{
    Ent *e;
    char path[MAX_PATH * 2];
    if (!nent) return;
    e = &ents[bsel];
    bmsg[0] = 0;
    if (e->dir) {
        blast[0] = 0;
        if (!strcmp(e->name, "..")) browser_parent();
        else if (!bdir[0]) snprintf(bdir, MAX_PATH, "%s", e->name);
        else { size_t n = strlen(bdir); snprintf(bdir + n, MAX_PATH - n, "%s\\", e->name); }
        snprintf(cfg.browse, MAX_PATH, "%s", bdir);
        browser_scan(); browser_draw();
        return;
    }
    snprintf(path, sizeof path, "%s%s", bdir, e->name);
    snprintf(cfg.browse, MAX_PATH, "%s", bdir);
    snprintf(cfg.lastfile, MAX_PATH, "%s", e->name);
    snprintf(blast, MAX_PATH, "%s", e->name);
    if (e->kind == 1) {                                  /* ROM -> defaut pour les prochains boot */
        int kind = rom_assign(path);
        if (kind == 1) {
            snprintf(cfg.rom, MAX_PATH, "%s", path);
            settings_save();
            capture_font();
            vram = bram; hires_mode = 0;
            machine_running = 0;
            snprintf(bmsg, sizeof bmsg, "ROM par defaut: %.30s", e->name);
        } else if (kind == 2) {
            snprintf(cfg.mdrom, MAX_PATH, "%s", path);
            settings_save();
            snprintf(bmsg, sizeof bmsg, "ROM disque: %.32s", e->name);
        } else snprintf(bmsg, sizeof bmsg, "ROM invalide");
        browser_draw();
    } else if (e->kind == 2) {                           /* disquette -> boot Microdisc */
        if (!have_disk) { ui_error("ROM Microdisc (microdis.rom) introuvable."); return; }
        if (drive_load(0, path)) { settings_save(); boot_machine(1, 0); }
    } else {                                             /* K7 -> boot BASIC + CLOAD"" */
        if (tape_load(path)) { settings_save(); boot_machine(0, 1); }
    }
}
static void browser_jump(char ch)
{
    int i, n = nent;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    for (i = 1; i <= n; i++) {
        int k = (bsel + i) % n;
        char c0 = ents[k].name[0];
        if (c0 >= 'a' && c0 <= 'z') c0 = (char)(c0 - 32);
        if (c0 == ch) { bsel = k; break; }
    }
    if (bsel < btop) btop = bsel;
    if (bsel >= btop + LIST_ROWS) btop = bsel - LIST_ROWS + 1;
    browser_draw();
}
static void browser_key(int sc)
{
    int n = nent;
    switch (sc) {
    case 0x48: case 0xE048: if (bsel > 0) bsel--; break;
    case 0x50: case 0xE050: if (bsel < n - 1) bsel++; break;
    case 0x49: case 0xE049: bsel -= LIST_ROWS - 1; if (bsel < 0) bsel = 0; break;
    case 0x51: case 0xE051: bsel += LIST_ROWS - 1; if (bsel > n - 1) bsel = n - 1; break;
    case 0xE047: bsel = 0; break;
    case 0xE04F: bsel = n - 1; break;
    case 0x1C: case 0xE01C: browser_select(); return;
    case 0x0E: case 0xE04B: browser_parent(); browser_scan(); browser_draw(); return;
    case 0x01:
        if (machine_running) browser_leave_resume();
        else boot_machine(0, 0);
        return;
    default: return;
    }
    if (bsel < btop) btop = bsel;
    if (bsel >= btop + LIST_ROWS) btop = bsel - LIST_ROWS + 1;
    browser_draw();
}

/* ====================================================================== */
/*  Demarrage d'une configuration                                         */
/* ====================================================================== */
static void boot_machine(int use_disk, int autotape)
{
    paused = 0;
    disk_on = use_disk && have_disk && drv[0].loaded;
    dbg_log("--- DEMARRAGE : disque=%d (ROM disque %s, %d octets, lecteur 0 %s) BASIC=%s", disk_on, have_disk ? "chargee" : "ABSENTE", rom_disk_size,
            drv[0].loaded ? drv[0].path : "vide", cfg.rom);
    fdc_ncmd = fdc_nbytes = 0;
    machine_hard_reset();
    vram = ram; mode = MODE_RUN; machine_running = 1;
    boot_turbo = 0;
    hook_frame = 0;
    if (autotape && tape_len) {
        tape_pos = 0;
        type_str("CLOAD\"\"\r", 2400000);
        boot_turbo = 125;
    } else if (tape) {
        tape_pos = 0;
    }
    update_title();
}

/* ====================================================================== */
/*  Partie Win32                                                          */
/* ====================================================================== */
#ifdef _WIN32

static HWND hwnd;
static BITMAPINFO bmi;
static u32 *scaled, *hbuf, *inter;
static int use_gpu;                  /* rendu Direct3D 11 actif */
static void gpu_resize(void);
static int sx = 1, sy = 1, dest_x, dest_y, fullscreen, turbo;
static WINDOWPLACEMENT wp_saved;
static LONG_PTR style_saved;
static int show_stats;
static char note[96];                /* message temporaire dans la barre de titre */
static DWORD note_until;
static char stats_str[320];

static void ui_error(const char *msg) { MessageBoxA(hwnd, msg, "Oric", MB_OK | MB_ICONERROR); }

/* --- audio waveOut --- */
#define NBUF 6
#define ABUFS 512
static HWAVEOUT hwo;
static WAVEHDR whdr[NBUF];
static short wbuf[NBUF][ABUFS];
static int audio_ok;

static void audio_fill(int i)
{
    int k, avail = ring_fill();
    short last = 0;
    for (k = 0; k < ABUFS; k++) {
        if (k < avail) { last = ring[ring_r]; ring_r = (ring_r + 1) & (RING - 1); wbuf[i][k] = last; }
        else wbuf[i][k] = (short)(last / 2);
    }
}
static void audio_init(void)
{
    WAVEFORMATEX wf;
    int i;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = SR;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = SR * 2;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
    for (i = 0; i < NBUF; i++) {
        memset(&whdr[i], 0, sizeof whdr[i]);
        whdr[i].lpData = (LPSTR)wbuf[i]; whdr[i].dwBufferLength = ABUFS * 2;
        waveOutPrepareHeader(hwo, &whdr[i], sizeof(WAVEHDR));
        memset(wbuf[i], 0, sizeof wbuf[i]);
        waveOutWrite(hwo, &whdr[i], sizeof(WAVEHDR));
    }
    audio_ok = 1;
}
static void audio_pump(void)
{
    int i;
    if (!audio_ok) return;
    for (i = 0; i < NBUF; i++)
        if (whdr[i].dwFlags & WHDR_DONE) {
            audio_fill(i);
            whdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(hwo, &whdr[i], sizeof(WAVEHDR));
        }
}

/* --- affichage : mise a l'echelle entiere, pixels carres 1:1, pixel perfect --- */
/* Paliers de taille : x1, x2, x3 ... Pixels carres (1:1) : chaque pixel Oric occupe
   toujours exactement st x st pixels ecran.                                          */
#define MAX_STEP 20
#define WIN_STYLE (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX)
static void step_dims(int st, int *ox, int *oy) { *ox = st; *oy = st; }
static int step_fit(int W, int H)           /* plus grand palier qui tient dans W x H (au moins 1) */
{
    int st, best = 1, a, b;
    for (st = 1; st <= MAX_STEP; st++) {
        step_dims(st, &a, &b);
        if (VW * a <= W && VH * b <= H) best = st;
    }
    return best;
}
static void frame_extra(int *fw, int *fh)
{
    RECT r;
    r.left = r.top = r.right = r.bottom = 0;
    AdjustWindowRect(&r, WIN_STYLE, FALSE);
    *fw = r.right - r.left; *fh = r.bottom - r.top;
}
static void layout_update(void)
{
    RECT rc;
    int W, H, st;
    if (!hwnd) return;
    GetClientRect(hwnd, &rc);
    W = rc.right; H = rc.bottom;
    st = step_fit(W, H);
    step_dims(st, &sx, &sy);
    dest_x = (W - VW * sx) / 2; dest_y = (H - VH * sy) / 2;
    if (dest_x < 0) dest_x = 0;
    if (dest_y < 0) dest_y = 0;
    if (use_gpu) {                                  /* GPU : pas de tampons CPU, juste la taille du swap chain */
        free(scaled); free(hbuf); scaled = NULL; hbuf = NULL;
        gpu_resize();
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    free(scaled); free(hbuf);
    scaled = (u32 *)malloc((size_t)(VW * sx) * (size_t)(VH * sy) * 4);
    hbuf = (u32 *)malloc((size_t)(VW * sx) * (size_t)(VH * 3) * 4);
    if (!inter) inter = (u32 *)malloc((size_t)(VW * 3) * (size_t)(VH * 3) * 4);
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = VW * sx; bmi.bmiHeader.biHeight = -(VH * sy);
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    InvalidateRect(hwnd, NULL, TRUE);
}
/* ====================================================================== */
/*  Filtres de mise a l'echelle (F4) + effet CRT (F2), entierement logiciels */
/* ====================================================================== */
static u32 mulc(u32 p, u32 m)                 /* p * m/256 sur les 3 canaux (m <= 256) */
{
    return (((p & 0xFF00FF) * m >> 8) & 0xFF00FF) | (((p & 0x00FF00) * m >> 8) & 0x00FF00);
}
static u32 mixc(u32 a, u32 b, u32 w)         /* (a*(256-w) + b*w) / 256, arrondi : a == b donne exactement a */
{
    u32 iw = 256 - w;
    u32 rb = (((a & 0xFF00FF) * iw + (b & 0xFF00FF) * w + 0x800080) >> 8) & 0xFF00FF;
    u32 g  = (((a & 0x00FF00) * iw + (b & 0x00FF00) * w + 0x008000) >> 8) & 0x00FF00;
    return rb | g;
}
static u32 mix4(u32 a, u32 b, u32 c, u32 d, const int *w)    /* combinaison a 4 points (poids /256, peut etre negatif) */
{
    int sh, res = 0;
    for (sh = 16; sh >= 0; sh -= 8) {
        int v = (int)((a >> sh) & 255) * w[0] + (int)((b >> sh) & 255) * w[1] +
                (int)((c >> sh) & 255) * w[2] + (int)((d >> sh) & 255) * w[3] + 128;
        v >>= 8;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        res |= v << sh;
    }
    return (u32)res;
}
static u32 pxc(int x, int y)                  /* pixel Oric avec bords repliques */
{
    if (x < 0) x = 0; else if (x >= VW) x = VW - 1;
    if (y < 0) y = 0; else if (y >= VH) y = VH - 1;
    return fb[y][x];
}

/* --- 0. Nearest : duplication pure --- */
static void filt_nearest(void)
{
    int y, x, k;
    size_t stride = (size_t)(VW * sx);
    for (y = 0; y < VH; y++) {
        u32 *row = scaled + (size_t)y * sy * stride;
        for (x = 0; x < VW; x++) { u32 p = fb[y][x]; for (k = 0; k < sx; k++) row[x * sx + k] = p; }
        for (k = 1; k < sy; k++) memcpy(row + (size_t)k * stride, row, stride * 4);
    }
}

/* --- 1. Bilinear --- */
static int aa_w(int n, int j)                /* poids du voisin pour la sous-position j parmi n */
{
    double d = 0.5 - (j + 0.5) / n;
    if (d < 0) d = -d;
    return (int)(256.0 * d);
}
static void hsmooth(u32 *dst, const u32 *src)
{
    int x, j, aw[MAX_STEP + 1];
    for (j = 0; j < sx; j++) aw[j] = aa_w(sx, j);
    for (x = 0; x < VW; x++) {
        u32 c = src[x], l = src[x > 0 ? x - 1 : x], r = src[x < VW - 1 ? x + 1 : x];
        for (j = 0; j < sx; j++) {
            u32 n = (j < sx / 2) ? l : r;
            dst[x * sx + j] = aw[j] ? mixc(c, n, (u32)aw[j]) : c;
        }
    }
}
static void filt_bilinear(void)
{
    int y, x, k, av[MAX_STEP + 1];
    size_t stride = (size_t)(VW * sx);
    for (k = 0; k < sy; k++) av[k] = aa_w(sy, k);
    for (y = 0; y < VH; y++) hsmooth(hbuf + (size_t)y * stride, fb[y]);
    for (y = 0; y < VH; y++)
        for (k = 0; k < sy; k++) {
            u32 *dst = scaled + ((size_t)y * sy + k) * stride;
            const u32 *c = hbuf + (size_t)y * stride;
            const u32 *n = hbuf + (size_t)((k < sy / 2) ? (y > 0 ? y - 1 : y) : (y < VH - 1 ? y + 1 : y)) * stride;
            if (!av[k]) memcpy(dst, c, stride * 4);
            else for (x = 0; x < (int)stride; x++) dst[x] = mixc(c[x], n[x], (u32)av[k]);
        }
}

/* --- 2. Bicubic (Catmull-Rom), separable --- */
static void cubic_tables(int n, int *boff, int w[][4])
{
    int j;
    for (j = 0; j < n; j++) {
        double p = (j + 0.5) / n - 0.5, t, t2, t3, c[4];
        int i, sum = 0;
        if (p < 0) { boff[j] = -1; t = p + 1.0; } else { boff[j] = 0; t = p; }
        t2 = t * t; t3 = t2 * t;
        c[0] = -0.5 * t3 + t2 - 0.5 * t;
        c[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
        c[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
        c[3] = 0.5 * t3 - 0.5 * t2;
        for (i = 0; i < 4; i++) { w[j][i] = (int)floor(c[i] * 256.0 + 0.5); sum += w[j][i]; }
        w[j][1] += 256 - sum;                      /* somme exacte = 256 */
    }
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void filt_bicubic(void)
{
    int y, x, j, k, bo[MAX_STEP + 1], bw[MAX_STEP + 1][4];
    size_t stride = (size_t)(VW * sx);
    if (stride * VH * sy > 4000000u || sx != sy) { filt_bilinear(); return; }
    cubic_tables(sx, bo, bw);
    for (y = 0; y < VH; y++) {                     /* passe horizontale */
        u32 *dst = hbuf + (size_t)y * stride;
        for (x = 0; x < VW; x++)
            for (j = 0; j < sx; j++) {
                int b = x + bo[j];
                dst[x * sx + j] = mix4(fb[y][clampi(b - 1, 0, VW - 1)], fb[y][clampi(b, 0, VW - 1)],
                                       fb[y][clampi(b + 1, 0, VW - 1)], fb[y][clampi(b + 2, 0, VW - 1)], bw[j]);
            }
    }
    for (y = 0; y < VH; y++)                       /* passe verticale */
        for (k = 0; k < sy; k++) {
            u32 *dst = scaled + ((size_t)y * sy + k) * stride;
            int b = y + bo[k];
            const u32 *r0 = hbuf + (size_t)clampi(b - 1, 0, VH - 1) * stride, *r1 = hbuf + (size_t)clampi(b, 0, VH - 1) * stride;
            const u32 *r2 = hbuf + (size_t)clampi(b + 1, 0, VH - 1) * stride, *r3 = hbuf + (size_t)clampi(b + 2, 0, VH - 1) * stride;
            for (x = 0; x < (int)stride; x++) dst[x] = mix4(r0[x], r1[x], r2[x], r3[x], bw[k]);
        }
}

/* --- 3..6 : filtres a facteur fixe produisant une image intermediaire f x --- */
static void filt_scale2x(void)               /* Scale2x (AdvMAME2x) */
{
    int x, y, W2 = VW * 2;
    for (y = 0; y < VH; y++)
        for (x = 0; x < VW; x++) {
            u32 B = pxc(x, y - 1), D = pxc(x - 1, y), E = fb[y][x], F = pxc(x + 1, y), H = pxc(x, y + 1);
            u32 e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F) { e0 = (D == B) ? D : E; e1 = (B == F) ? F : E; e2 = (D == H) ? D : E; e3 = (H == F) ? F : E; }
            inter[(size_t)(2 * y) * W2 + 2 * x] = e0;     inter[(size_t)(2 * y) * W2 + 2 * x + 1] = e1;
            inter[(size_t)(2 * y + 1) * W2 + 2 * x] = e2; inter[(size_t)(2 * y + 1) * W2 + 2 * x + 1] = e3;
        }
}
static void filt_scale3x(void)               /* Scale3x (AdvMAME3x) */
{
    int x, y, i, W3 = VW * 3;
    for (y = 0; y < VH; y++)
        for (x = 0; x < VW; x++) {
            u32 A = pxc(x - 1, y - 1), B = pxc(x, y - 1), C = pxc(x + 1, y - 1);
            u32 D = pxc(x - 1, y),     E = fb[y][x],      F = pxc(x + 1, y);
            u32 G = pxc(x - 1, y + 1), H = pxc(x, y + 1), I = pxc(x + 1, y + 1);
            u32 e[9];
            for (i = 0; i < 9; i++) e[i] = E;
            if (B != H && D != F) {
                e[0] = (D == B && B != F && D != H) ? D : E;
                e[1] = ((D == B && B != F && D != H && E != C) || (B == F && B != D && F != H && E != A)) ? B : E;
                e[2] = (B == F && B != D && F != H) ? F : E;
                e[3] = ((D == B && B != F && D != H && E != G) || (D == H && D != B && H != F && E != A)) ? D : E;
                e[5] = ((B == F && B != D && F != H && E != I) || (H == F && D != H && B != F && E != C)) ? F : E;
                e[6] = (D == H && D != B && H != F) ? D : E;
                e[7] = ((D == H && D != B && H != F && E != I) || (H == F && D != H && B != F && E != G)) ? H : E;
                e[8] = (H == F && D != H && B != F) ? F : E;
            }
            for (i = 0; i < 9; i++) inter[(size_t)(3 * y + i / 3) * W3 + 3 * x + i % 3] = e[i];
        }
}
static void filt_eagle(void)                 /* Eagle 2x */
{
    int x, y, W2 = VW * 2;
    for (y = 0; y < VH; y++)
        for (x = 0; x < VW; x++) {
            u32 A = pxc(x - 1, y - 1), B = pxc(x, y - 1), C = pxc(x + 1, y - 1);
            u32 D = pxc(x - 1, y),     E = fb[y][x],      F = pxc(x + 1, y);
            u32 G = pxc(x - 1, y + 1), H = pxc(x, y + 1), I = pxc(x + 1, y + 1);
            inter[(size_t)(2 * y) * W2 + 2 * x] = (D == A && A == B) ? A : E;
            inter[(size_t)(2 * y) * W2 + 2 * x + 1] = (B == C && C == F) ? C : E;
            inter[(size_t)(2 * y + 1) * W2 + 2 * x] = (D == G && G == H) ? G : E;
            inter[(size_t)(2 * y + 1) * W2 + 2 * x + 1] = (F == I && I == H) ? I : E;
        }
}
/* Lissage directionnel (algorithme maison, ni HQx ni xBR) : memes diagonales detectees que Scale2x,
   mais le sous-pixel de coin est un melange (50 % ou 75 %) au lieu d'un remplacement net -> escaliers adoucis. */
static u32 smooth_corner(u32 E, u32 s1, u32 s2, u32 d, int guard)
{
    if (!guard || s1 != s2 || s1 == E) return E;
    return mixc(E, s1, d == s1 ? 192 : 128);
}
static void filt_smooth2x(void)
{
    int x, y, W2 = VW * 2;
    for (y = 0; y < VH; y++)
        for (x = 0; x < VW; x++) {
            u32 A = pxc(x - 1, y - 1), B = pxc(x, y - 1), C = pxc(x + 1, y - 1);
            u32 D = pxc(x - 1, y),     E = fb[y][x],      F = pxc(x + 1, y);
            u32 G = pxc(x - 1, y + 1), H = pxc(x, y + 1), I = pxc(x + 1, y + 1);
            int guard = (B != H && D != F);
            inter[(size_t)(2 * y) * W2 + 2 * x] = smooth_corner(E, D, B, A, guard);
            inter[(size_t)(2 * y) * W2 + 2 * x + 1] = smooth_corner(E, F, B, C, guard);
            inter[(size_t)(2 * y + 1) * W2 + 2 * x] = smooth_corner(E, D, H, G, guard);
            inter[(size_t)(2 * y + 1) * W2 + 2 * x + 1] = smooth_corner(E, F, H, I, guard);
        }
}
/* amene l'image intermediaire (facteur f) a l'echelle ecran : repetition si st est multiple de f, sinon bilinear */
static void inter_to_scaled(int f)
{
    int W2 = VW * f, H2 = VH * f, X, Y, y2, x, k, kk = sx / f;
    size_t stride = (size_t)(VW * sx);
    static int xi[VW * MAX_STEP + 8], xw[VW * MAX_STEP + 8];
    if (sx % f == 0) {
        for (y2 = 0; y2 < H2; y2++) {
            u32 *row = scaled + (size_t)y2 * kk * stride;
            for (x = 0; x < W2; x++) { u32 p = inter[(size_t)y2 * W2 + x]; for (k = 0; k < kk; k++) row[x * kk + k] = p; }
            for (k = 1; k < kk; k++) memcpy(row + (size_t)k * stride, row, stride * 4);
        }
        return;
    }
    for (X = 0; X < (int)stride; X++) {                         /* table horizontale */
        double u = ((double)X + 0.5) * W2 / (double)stride - 0.5;
        int x0 = (int)floor(u);
        xw[X] = (int)(256.0 * (u - x0));
        xi[X] = x0;
    }
    for (y2 = 0; y2 < H2; y2++) {                               /* rangees horizontales intermediaires */
        u32 *dst = hbuf + (size_t)y2 * stride;
        const u32 *src = inter + (size_t)y2 * W2;
        for (X = 0; X < (int)stride; X++) {
            u32 a = src[clampi(xi[X], 0, W2 - 1)], b = src[clampi(xi[X] + 1, 0, W2 - 1)];
            dst[X] = xw[X] ? mixc(a, b, (u32)xw[X]) : a;
        }
    }
    for (Y = 0; Y < VH * sy; Y++) {
        double v = ((double)Y + 0.5) * H2 / (double)(VH * sy) - 0.5;
        int y0 = (int)floor(v), wv = (int)(256.0 * (v - y0));
        const u32 *r0 = hbuf + (size_t)clampi(y0, 0, H2 - 1) * stride, *r1 = hbuf + (size_t)clampi(y0 + 1, 0, H2 - 1) * stride;
        u32 *dst = scaled + (size_t)Y * stride;
        if (!wv) memcpy(dst, r0, stride * 4);
        else for (X = 0; X < (int)stride; X++) dst[X] = mixc(r0[X], r1[X], (u32)wv);
    }
}

/* --- effet CRT (F2) : post-traitement de l'image a l'echelle ecran ---
   - faisceau : chaque ligne Oric est une bande lumineuse (profil gaussien sur les lignes ecran) ;
   - luminescence : les pixels clairs elargissent le faisceau et debordent un peu sur leurs voisins ;
   - scintillement : legere variation de luminosite a chaque image + faible barre de ronflement mobile. */
static u32 flick_seed = 1;
static u32 glowc(u32 c, u32 l, u32 r, int g)
{
    int sh;
    u32 res = 0;
    for (sh = 16; sh >= 0; sh -= 8) {
        int cc = (int)((c >> sh) & 255), avg = (int)((((l >> sh) & 255) + ((r >> sh) & 255)) >> 1), d = avg - cc;
        if (d > 0) cc += d * g >> 8;
        res |= (u32)cc << sh;
    }
    return res;
}
static void crt_post(void)
{
    int Y, X, k, wk[MAX_STEP + 1], bl, fm, rm = 256, r, hglow, g;
    size_t stride = (size_t)(VW * sx);
    double s = cfg.scan_strength / 100.0, a = cfg.crt_flicker / 100.0;
    static u32 rowbuf[VW * MAX_STEP + 8];

    bl = (int)(256.0 * cfg.crt_glow / 100.0 * 0.8);
    g = (int)(0.60 * cfg.crt_glow / 100.0 * 256.0);
    r = sx / 2 > 0 ? sx / 2 : 1;
    for (k = 0; k < sy; k++) {
        if (sy == 1) wk[k] = 256;
        else if (sy == 2) wk[k] = (k == 0) ? 256 : (int)(256.0 * (1.0 - s * 0.85));
        else {
            double t = (k + 0.5) / sy, d = (t - 0.5) / 0.32;
            wk[k] = (int)(256.0 * (1.0 - s * (1.0 - exp(-d * d))));
        }
    }
    hglow = g > 0 && ((size_t)stride * VH * sy <= 4000000u);
    flick_seed = flick_seed * 1664525u + 1013904223u;
    fm = 256 - (int)(256.0 * a * 0.04 * (double)((flick_seed >> 16) & 255) / 255.0);

    for (Y = 0; Y < VH * sy; Y++) {
        int y = Y / sy, w;
        u32 *row = scaled + (size_t)Y * stride;
        k = Y % sy;
        if (k == 0) {
            double hum = 0.5 + 0.5 * sin(6.2831853 * y / (double)VH + frame_counter * 0.09);
            rm = (int)(fm * (1.0 - 0.03 * a * hum));
        }
        w = wk[k];
        if (sy == 1 && (y & 1)) w = (int)(256.0 * (1.0 - s * 0.6));
        {   /* assombrissement du faisceau ; les pixels identiques consecutifs reutilisent le resultat precedent */
            u32 lastp = 0xFFFFFFFFu, lastout = 0;
            for (X = 0; X < (int)stride; X++) {
                u32 p = row[X];
                if (p != lastp) {
                    int lum = (int)((((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255)) / 3);
                    int wp = w + ((((256 - w) * bl) >> 8) * lum >> 8);
                    lastp = p; lastout = mulc(p, (u32)(wp * rm >> 8));
                }
                row[X] = lastout;
            }
        }
        if (hglow) {                                   /* halo : seulement la ou un voisin est plus clair */
            memcpy(rowbuf, row, stride * 4);
            for (X = 0; X < (int)stride; X++) {
                u32 c = rowbuf[X], l = rowbuf[X - r < 0 ? 0 : X - r], rr = rowbuf[X + r >= (int)stride ? (int)stride - 1 : X + r];
                if (l != c || rr != c) row[X] = glowc(c, l, rr, g);
            }
        }
    }
}

static void build_scaled(void)
{
    int st = sx;
    if (!scaled) return;
    switch (cfg.filter) {
    case 1: filt_bilinear(); break;
    case 2: filt_bicubic(); break;
    case 3: if (st >= 2) { filt_scale2x(); inter_to_scaled(2); } else filt_nearest(); break;
    case 4: if (st >= 3) { filt_scale3x(); inter_to_scaled(3); } else filt_nearest(); break;
    case 5: if (st >= 2) { filt_eagle(); inter_to_scaled(2); } else filt_nearest(); break;
    case 6: if (st >= 2) { filt_smooth2x(); inter_to_scaled(2); } else filt_nearest(); break;
    default: filt_nearest(); break;
    }
    if (cfg.scanlines) crt_post();
}
/* ====================================================================== */
/*  Rendu GPU : Direct3D 11 (charge dynamiquement d3d11.dll + d3dcompiler_47.dll,       */
/*  aucune bibliotheque a lier). Tous les filtres et l'effet CRT tournent dans un pixel   */
/*  shader : le CPU n'envoie plus qu'une texture de 240x224 par image.                    */
/*  Si le GPU / les DLL manquent ou si le shader ne compile pas : retour automatique GDI.  */
/* ====================================================================== */
typedef HRESULT (WINAPI *PFN_CDASC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                    const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                    ID3D11DeviceContext **);
typedef HRESULT (WINAPI *PFN_D3DCOMPILE_)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                          UINT, UINT, ID3DBlob **, ID3DBlob **);
static const GUID g_IID_Tex2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID g_IID_Factory = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };

static const char hlsl_vs[] =
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs(uint id : SV_VertexID)\n"
    "{\n"
    "    VSOut o;\n"
    "    float2 t = float2((id << 1) & 2, id & 2);\n"
    "    o.uv = t;\n"
    "    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

static const char hlsl_ps[] =
    "// Oric : filtres de mise a l'echelle + effet CRT (pixel shader, SM 4.0)\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState lin : register(s0);\n"
    "cbuffer C : register(b0)\n"
    "{\n"
    "    float4 c0;   // x: filtre, y: echelle entiere, z: CRT actif, w: facteur de scintillement de l'image\n"
    "    float4 c1;   // x: force des scanlines, y: luminescence, z: scintillement, w: compteur d'images\n"
    "};\n"
    "static const float2 TSZ = float2(240.0, 224.0);\n"
    "\n"
    "float3 T(int2 i)\n"
    "{\n"
    "    i = clamp(i, int2(0, 0), int2(239, 223));\n"
    "    return tex.Load(int3(i, 0)).rgb;\n"
    "}\n"
    "bool eq(float3 a, float3 b) { return all(a == b); }\n"
    "\n"
    "float3 sc2x(int2 ip, int2 sub)\n"
    "{\n"
    "    float3 B = T(ip + int2(0, -1)), D = T(ip + int2(-1, 0)), E = T(ip), F = T(ip + int2(1, 0)), H = T(ip + int2(0, 1));\n"
    "    if (eq(B, H) || eq(D, F)) return E;\n"
    "    if (sub.y == 0) { if (sub.x == 0) return eq(D, B) ? D : E; return eq(B, F) ? F : E; }\n"
    "    if (sub.x == 0) return eq(D, H) ? D : E;\n"
    "    return eq(H, F) ? F : E;\n"
    "}\n"
    "float3 sc3x(int2 ip, int2 sub)\n"
    "{\n"
    "    float3 A = T(ip + int2(-1, -1)), B = T(ip + int2(0, -1)), C = T(ip + int2(1, -1));\n"
    "    float3 D = T(ip + int2(-1, 0)),  E = T(ip),               F = T(ip + int2(1, 0));\n"
    "    float3 G = T(ip + int2(-1, 1)),  H = T(ip + int2(0, 1)),  I = T(ip + int2(1, 1));\n"
    "    if (eq(B, H) || eq(D, F)) return E;\n"
    "    bool DB = eq(D, B), BF = eq(B, F), DH = eq(D, H), HF = eq(H, F);\n"
    "    int i = sub.y * 3 + sub.x;\n"
    "    if (i == 0) return (DB && !BF && !DH) ? D : E;\n"
    "    if (i == 1) return ((DB && !BF && !DH && !eq(E, C)) || (BF && !DB && !HF && !eq(E, A))) ? B : E;\n"
    "    if (i == 2) return (BF && !DB && !HF) ? F : E;\n"
    "    if (i == 3) return ((DB && !BF && !DH && !eq(E, G)) || (DH && !DB && !HF && !eq(E, A))) ? D : E;\n"
    "    if (i == 5) return ((BF && !DB && !HF && !eq(E, I)) || (HF && !DH && !BF && !eq(E, C))) ? F : E;\n"
    "    if (i == 6) return (DH && !DB && !HF) ? D : E;\n"
    "    if (i == 7) return ((DH && !DB && !HF && !eq(E, I)) || (HF && !DH && !BF && !eq(E, G))) ? H : E;\n"
    "    if (i == 8) return (HF && !DH && !BF) ? F : E;\n"
    "    return E;\n"
    "}\n"
    "float3 eagle(int2 ip, int2 sub)\n"
    "{\n"
    "    float3 A = T(ip + int2(-1, -1)), B = T(ip + int2(0, -1)), C = T(ip + int2(1, -1));\n"
    "    float3 D = T(ip + int2(-1, 0)),  E = T(ip),               F = T(ip + int2(1, 0));\n"
    "    float3 G = T(ip + int2(-1, 1)),  H = T(ip + int2(0, 1)),  I = T(ip + int2(1, 1));\n"
    "    if (sub.y == 0) { if (sub.x == 0) return (eq(D, A) && eq(A, B)) ? A : E; return (eq(B, C) && eq(C, F)) ? C : E; }\n"
    "    if (sub.x == 0) return (eq(D, G) && eq(G, H)) ? G : E;\n"
    "    return (eq(F, I) && eq(I, H)) ? I : E;\n"
    "}\n"
    "float3 smo(int2 ip, int2 sub)               // lissage directionnel (algorithme maison)\n"
    "{\n"
    "    float3 A = T(ip + int2(-1, -1)), B = T(ip + int2(0, -1)), C = T(ip + int2(1, -1));\n"
    "    float3 D = T(ip + int2(-1, 0)),  E = T(ip),               F = T(ip + int2(1, 0));\n"
    "    float3 G = T(ip + int2(-1, 1)),  H = T(ip + int2(0, 1)),  I = T(ip + int2(1, 1));\n"
    "    float3 s1 = (sub.x == 0) ? D : F;\n"
    "    float3 s2 = (sub.y == 0) ? B : H;\n"
    "    float3 d = (sub.y == 0) ? ((sub.x == 0) ? A : C) : ((sub.x == 0) ? G : I);\n"
    "    if (eq(B, H) || eq(D, F) || !eq(s1, s2) || eq(s1, E)) return E;\n"
    "    return lerp(E, s1, eq(d, s1) ? 0.75 : 0.5);\n"
    "}\n"
    "float3 IC(int2 iq, int ft)                  // pixel de l'image intermediaire (facteur 2 ou 3)\n"
    "{\n"
    "    int f = (ft == 4) ? 3 : 2;\n"
    "    int2 ip = int2(floor(float2(iq) / (float)f));\n"
    "    int2 sub = iq - ip * f;\n"
    "    if (ft == 3) return sc2x(ip, sub);\n"
    "    if (ft == 4) return sc3x(ip, sub);\n"
    "    if (ft == 5) return eagle(ip, sub);\n"
    "    return smo(ip, sub);\n"
    "}\n"
    "float4 cr(float t)                          // poids Catmull-Rom\n"
    "{\n"
    "    float t2 = t * t, t3 = t2 * t;\n"
    "    return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);\n"
    "}\n"
    "\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    float2 p = uv * TSZ;                    // coordonnees continues en pixels Oric\n"
    "    int ft = (int)(c0.x + 0.5);\n"
    "    float st = c0.y;\n"
    "    float3 col;\n"
    "    if (ft == 1) {\n"
    "        col = tex.SampleLevel(lin, uv, 0).rgb;\n"
    "    } else if (ft == 2) {\n"
    "        float2 q = p - 0.5;\n"
    "        float2 fq = floor(q);\n"
    "        float2 t = q - fq;\n"
    "        int2 i0 = int2(fq);\n"
    "        float4 wx = cr(t.x), wy = cr(t.y);\n"
    "        col = float3(0, 0, 0);\n"
    "        [unroll] for (int j = 0; j < 4; j++) {\n"
    "            float3 rowc = float3(0, 0, 0);\n"
    "            [unroll] for (int i = 0; i < 4; i++) rowc += T(i0 + int2(i - 1, j - 1)) * wx[i];\n"
    "            col += saturate(rowc) * wy[j];         // ecretage apres la passe horizontale (comme le rendu CPU)\n"
    "        }\n"
    "        col = saturate(col);\n"
    "    } else if (ft >= 3 && ft <= 6) {\n"
    "        float f = (ft == 4) ? 3.0 : 2.0;\n"
    "        if (st < f) {\n"
    "            col = T(int2(floor(p)));\n"
    "        } else if (fmod(st, f) < 0.5) {\n"
    "            col = IC(int2(floor(p * f)), ft);   // echelle multiple du facteur : pixels nets\n"
    "        } else {                                // sinon : reechantillonnage bilineaire de l'image intermediaire\n"
    "            float2 q = p * f - 0.5;\n"
    "            float2 fq = floor(q);\n"
    "            float2 t = q - fq;\n"
    "            int2 ib = int2(fq);\n"
    "            float3 c00 = IC(ib, ft), c10 = IC(ib + int2(1, 0), ft);\n"
    "            float3 c01 = IC(ib + int2(0, 1), ft), c11 = IC(ib + int2(1, 1), ft);\n"
    "            col = lerp(lerp(c00, c10, t.x), lerp(c01, c11, t.x), t.y);\n"
    "        }\n"
    "    } else {\n"
    "        col = T(int2(floor(p)));\n"
    "    }\n"
    "\n"
    "    if (c0.z > 0.5) {                       // effet CRT\n"
    "        float s = c1.x, glow = c1.y, a = c1.z, frame = c1.w;\n"
    "        int sy = (int)(st + 0.5);\n"
    "        float yy = floor(p.y), fy = p.y - yy, w;\n"
    "        if (sy == 1) w = (fmod(yy, 2.0) >= 1.0) ? 1.0 - s * 0.6 : 1.0;\n"
    "        else if (sy == 2) w = (fy < 0.5) ? 1.0 : 1.0 - s * 0.85;\n"
    "        else { float d = (fy - 0.5) / 0.32; w = 1.0 - s * (1.0 - exp(-d * d)); }\n"
    "        float lum = (col.r + col.g + col.b) / 3.0;\n"
    "        float wp = w + (1.0 - w) * (glow * 0.8) * lum;          // les pixels clairs elargissent le faisceau\n"
    "        float hum = 0.5 + 0.5 * sin(6.2831853 * yy / 224.0 + frame * 0.09);\n"
    "        float rm = c0.w * (1.0 - 0.03 * a * hum);               // scintillement + barre de ronflement\n"
    "        col = col * (wp * rm);\n"
    "        if (glow > 0.0) {                                       // halo horizontal sur les bords\n"
    "            float r = max(1.0, floor(st * 0.5)) / st;\n"
    "            float3 Lc = T(int2(floor(p - float2(r, 0.0)))) * (w * rm);\n"
    "            float3 Rc = T(int2(floor(p + float2(r, 0.0)))) * (w * rm);\n"
    "            col += max((Lc + Rc) * 0.5 - col, float3(0, 0, 0)) * (0.6 * glow);\n"
    "        }\n"
    "    }\n"
    "    return float4(saturate(col), 1.0);\n"
    "}\n";

typedef struct { float c0[4], c1[4]; } GpuCB;

static ID3D11Device *d3d_dev;
static ID3D11DeviceContext *d3d_ctx;
static IDXGISwapChain *d3d_sc;
static ID3D11RenderTargetView *d3d_rtv;
static ID3D11VertexShader *d3d_vs;
static ID3D11PixelShader *d3d_ps;
static ID3D11Texture2D *d3d_tex;
static ID3D11ShaderResourceView *d3d_srv;
static ID3D11SamplerState *d3d_smp;
static ID3D11Buffer *d3d_cb;

static void gpu_shutdown(void)
{
    if (d3d_ctx) { ID3D11DeviceContext_ClearState(d3d_ctx); ID3D11DeviceContext_Flush(d3d_ctx); }
    if (d3d_cb)  { ID3D11Buffer_Release(d3d_cb); d3d_cb = NULL; }
    if (d3d_smp) { ID3D11SamplerState_Release(d3d_smp); d3d_smp = NULL; }
    if (d3d_srv) { ID3D11ShaderResourceView_Release(d3d_srv); d3d_srv = NULL; }
    if (d3d_tex) { ID3D11Texture2D_Release(d3d_tex); d3d_tex = NULL; }
    if (d3d_ps)  { ID3D11PixelShader_Release(d3d_ps); d3d_ps = NULL; }
    if (d3d_vs)  { ID3D11VertexShader_Release(d3d_vs); d3d_vs = NULL; }
    if (d3d_rtv) { ID3D11RenderTargetView_Release(d3d_rtv); d3d_rtv = NULL; }
    if (d3d_sc)  { IDXGISwapChain_Release(d3d_sc); d3d_sc = NULL; }
    if (d3d_ctx) { ID3D11DeviceContext_Release(d3d_ctx); d3d_ctx = NULL; }
    if (d3d_dev) { ID3D11Device_Release(d3d_dev); d3d_dev = NULL; }
}
static void gpu_make_rtv(void)
{
    ID3D11Texture2D *bb = NULL;
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(d3d_sc, 0, &g_IID_Tex2D, (void **)&bb)) && bb) {
        ID3D11Device_CreateRenderTargetView(d3d_dev, (ID3D11Resource *)bb, NULL, &d3d_rtv);
        ID3D11Texture2D_Release(bb);
    }
}
static void gpu_resize(void)
{
    RECT rc;
    if (!d3d_sc) return;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;                 /* fenetre reduite */
    ID3D11DeviceContext_OMSetRenderTargets(d3d_ctx, 0, NULL, NULL);
    if (d3d_rtv) { ID3D11RenderTargetView_Release(d3d_rtv); d3d_rtv = NULL; }
    IDXGISwapChain_ResizeBuffers(d3d_sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    gpu_make_rtv();
}
static int gpu_compile(PFN_D3DCOMPILE_ compile, const char *src, const char *entry, const char *target, ID3DBlob **out)
{
    ID3DBlob *err = NULL;
    HRESULT hr = compile(src, strlen(src), "oric.hlsl", NULL, NULL, entry, target, 1 << 15 /* OPTIMIZATION_LEVEL3 */, 0, out, &err);
    if (FAILED(hr)) {
        char msg[2048];
        snprintf(msg, sizeof msg, "Compilation du shader (%s) impossible :\n%s", target,
                 err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(pas de message)");
        ui_error(msg);
        if (err) ID3D10Blob_Release(err);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);
    return 1;
}
static int gpu_init(void)
{
    HMODULE m11, mc;
    PFN_CDASC create;
    PFN_D3DCOMPILE_ compile;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SAMPLER_DESC smd;
    D3D11_BUFFER_DESC bd;
    ID3DBlob *vsb = NULL, *psb = NULL;
    IDXGIFactory *fac = NULL;
    HRESULT hr;

    m11 = LoadLibraryA("d3d11.dll");
    if (!m11) return 0;
    create = (PFN_CDASC)GetProcAddress(m11, "D3D11CreateDeviceAndSwapChain");
    mc = LoadLibraryA("d3dcompiler_47.dll");
    if (!mc) mc = LoadLibraryA("d3dcompiler_46.dll");
    if (!mc) mc = LoadLibraryA("d3dcompiler_43.dll");
    if (!create || !mc) return 0;
    compile = (PFN_D3DCOMPILE_)GetProcAddress(mc, "D3DCompile");
    if (!compile) return 0;

    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &d3d_sc, &d3d_dev, &fl, &d3d_ctx);
    if (FAILED(hr) || !d3d_sc || !d3d_dev || !d3d_ctx) { gpu_shutdown(); return 0; }

    /* Alt+Entree est gere par l'emulateur (plein ecran fenetre sans bordure), pas par DXGI */
    if (SUCCEEDED(IDXGISwapChain_GetParent(d3d_sc, &g_IID_Factory, (void **)&fac)) && fac) {
        IDXGIFactory_MakeWindowAssociation(fac, hwnd, 3 /* NO_WINDOW_CHANGES | NO_ALT_ENTER */);
        IDXGIFactory_Release(fac);
    }
    gpu_make_rtv();

    if (!gpu_compile(compile, hlsl_vs, "vs", "vs_4_0", &vsb) || !gpu_compile(compile, hlsl_ps, "main", "ps_4_0", &psb)) {
        if (vsb) ID3D10Blob_Release(vsb);
        gpu_shutdown();
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(d3d_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &d3d_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(d3d_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &d3d_ps);
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb);
    if (FAILED(hr)) { gpu_shutdown(); return 0; }

    memset(&td, 0, sizeof td);                         /* texture de l'image Oric (240x224, mise a jour a chaque image) */
    td.Width = VW; td.Height = VH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateTexture2D(d3d_dev, &td, NULL, &d3d_tex);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(d3d_dev, (ID3D11Resource *)d3d_tex, NULL, &d3d_srv);
    if (FAILED(hr)) { gpu_shutdown(); return 0; }

    memset(&smd, 0, sizeof smd);
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = ID3D11Device_CreateSamplerState(d3d_dev, &smd, &d3d_smp);
    if (FAILED(hr)) { gpu_shutdown(); return 0; }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(GpuCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(d3d_dev, &bd, NULL, &d3d_cb);
    if (FAILED(hr)) { gpu_shutdown(); return 0; }
    return 1;
}
static int gpu_present(void)                      /* 0 = peripherique perdu (bascule en GDI) */
{
    D3D11_MAPPED_SUBRESOURCE ms;
    D3D11_VIEWPORT vp;
    GpuCB cb;
    float black[4] = { 0.f, 0.f, 0.f, 1.f };
    ID3D11ShaderResourceView *srv = d3d_srv;
    ID3D11SamplerState *smp = d3d_smp;
    ID3D11Buffer *cbuf = d3d_cb;
    HRESULT hr;
    int y;
    float a = cfg.crt_flicker / 100.0f;
    if (!d3d_sc) return 0;
    if (!d3d_rtv) return 1;                       /* fenetre reduite : rien a dessiner */

    hr = ID3D11DeviceContext_Map(d3d_ctx, (ID3D11Resource *)d3d_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr)) return 0;
    for (y = 0; y < VH; y++) memcpy((char *)ms.pData + (size_t)y * ms.RowPitch, fb[y], VW * 4);
    ID3D11DeviceContext_Unmap(d3d_ctx, (ID3D11Resource *)d3d_tex, 0);

    flick_seed = flick_seed * 1664525u + 1013904223u;
    cb.c0[0] = (float)cfg.filter;
    cb.c0[1] = (float)sx;
    cb.c0[2] = cfg.scanlines ? 1.0f : 0.0f;
    cb.c0[3] = 1.0f - a * 0.04f * (float)((flick_seed >> 16) & 255) / 255.0f;
    cb.c1[0] = cfg.scan_strength / 100.0f;
    cb.c1[1] = cfg.crt_glow / 100.0f;
    cb.c1[2] = a;
    cb.c1[3] = (float)(frame_counter % 100000);
    hr = ID3D11DeviceContext_Map(d3d_ctx, (ID3D11Resource *)d3d_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr)) return 0;
    memcpy(ms.pData, &cb, sizeof cb);
    ID3D11DeviceContext_Unmap(d3d_ctx, (ID3D11Resource *)d3d_cb, 0);

    ID3D11DeviceContext_ClearRenderTargetView(d3d_ctx, d3d_rtv, black);
    vp.TopLeftX = (float)dest_x; vp.TopLeftY = (float)dest_y;
    vp.Width = (float)(VW * sx); vp.Height = (float)(VH * sy);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(d3d_ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(d3d_ctx, 1, &d3d_rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(d3d_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(d3d_ctx, d3d_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d3d_ctx, d3d_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d3d_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(d3d_ctx, 0, 1, &smp);
    ID3D11DeviceContext_PSSetConstantBuffers(d3d_ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_Draw(d3d_ctx, 3, 0);
    hr = IDXGISwapChain_Present(d3d_sc, 0, 0);
    return !(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET);
}
/* choisit le moteur de rendu selon les reglages (auto : GPU si disponible) */
static void renderer_apply(void)
{
    int want = (cfg.renderer != 2) && mode != MODE_NOROM;
    if (want && !use_gpu) { if (gpu_init()) use_gpu = 1; }
    else if (!want && use_gpu) { gpu_shutdown(); use_gpu = 0; }
    layout_update();
    update_title();
}

static void present_dc(HDC dc)
{
    if (mode == MODE_NOROM || !scaled) return;
    StretchDIBits(dc, dest_x, dest_y, VW * sx, VH * sy, 0, 0, VW * sx, VH * sy, scaled, &bmi, DIB_RGB_COLORS, SRCCOPY);
}
static void present(void)
{
    HDC dc;
    if (use_gpu) {
        if (gpu_present()) return;
        gpu_shutdown(); use_gpu = 0;              /* peripherique perdu : on bascule sur le rendu GDI */
        layout_update(); update_title();
    }
    build_scaled();
    dc = GetDC(hwnd);
    present_dc(dc);
    ReleaseDC(hwnd, dc);
}

static void update_title(void)
{
    char t[MAX_PATH + 160];
    const char *what = "BASIC";
    if (mode == MODE_BROWSER) what = "selecteur";
    else if (disk_on) what = base_name(drv[0].path);
    else if (tape && tape_len && tq_len) {
        static char wb[MAX_PATH + 32];
        snprintf(wb, sizeof wb, "%s", base_name(tape_name));
        if (tape_parts > 1) snprintf(wb + strlen(wb), sizeof wb - strlen(wb), " (%d parties)", tape_parts);
        what = wb;
    }
    char tg[200];
    tg[0] = 0;
    if (turbo) snprintf(tg, sizeof tg, " (TURBO)");
    if (cfg.cpu_turbo) snprintf(tg + strlen(tg), sizeof tg - strlen(tg), " (CPU x%d)", cfg.cpu_mult);
    if (paused) snprintf(tg + strlen(tg), sizeof tg - strlen(tg), " (PAUSE)");
    if (note[0]) snprintf(tg + strlen(tg), sizeof tg - strlen(tg), " - %s", note);
    if (show_stats)
        snprintf(t, sizeof t, "Oric Atmos - %s%s  %s", what, tg, stats_str);
    else
        snprintf(t, sizeof t, "Oric Atmos - %s%s  [F1 selecteur | F11 NMI | F12 reset | Alt+Entree plein ecran | filtre: %s | rendu: %s]",
                 what, tg, filter_names[cfg.filter], use_gpu ? "D3D11" : "GDI");
    if (hwnd) SetWindowTextA(hwnd, t);
}

static void toggle_fullscreen(void)
{
    if (!fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        wp_saved.length = sizeof wp_saved;
        GetWindowPlacement(hwnd, &wp_saved);
        style_saved = GetWindowLongPtrA(hwnd, GWL_STYLE);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrA(hwnd, GWL_STYLE, style_saved & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = 1;
    } else {
        SetWindowLongPtrA(hwnd, GWL_STYLE, style_saved);
        SetWindowPlacement(hwnd, &wp_saved);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = 0;
    }
    cfg.fullscreen = fullscreen;
    settings_save();
    layout_update();
}

static void handle_files(char names[][MAX_PATH], int n)
{
    int i, did = 0, d = 0, tap = -1, rom = -1;
    for (i = 0; i < n; i++) {
        if (tape_part_no(names[i]) >= -1 && (tap < 0 || tape_part_no(names[i]) < tape_part_no(names[tap]))) tap = i;
        else if (has_ext(names[i], ".rom") && rom < 0) rom = i;
    }
    if (tap >= 0 && have_basic) {
        if (tape_load(names[tap])) { boot_machine(0, 1); did = 1; }
    }
    if (!did) {
        for (i = 0; i < n && d < 4; i++)
            if (has_ext(names[i], ".dsk")) { if (!d) { for (d = 0; d < 4; d++) drive_eject(d); d = 0; } if (drive_load(d, names[i])) d++; }
        if (d > 0) {
            if (!have_disk) ui_error("ROM Microdisc (microdis.rom) introuvable.");
            else { boot_machine(1, 0); did = 1; }
        }
    }
    if (!did && rom >= 0) {
        int kind = rom_assign(names[rom]);
        if (kind == 1) {
            snprintf(cfg.rom, MAX_PATH, "%s", names[rom]);
            settings_save();
            capture_font();
            if (mode == MODE_NOROM) { mode = MODE_RUN; renderer_apply(); }
            boot_machine(0, 0);
            did = 1;
        } else if (kind == 2) {
            snprintf(cfg.mdrom, MAX_PATH, "%s", names[rom]);
            did = 1;
        } else ui_error("ROM invalide.");
    }
    if (did) settings_save();
}

/* ---------- notes (barre de titre), captures d'ecran, snapshots ---------- */
static void show_note(const char *msg)
{
    snprintf(note, sizeof note, "%s", msg);
    note_until = GetTickCount() + 3000;
    update_title();
}
typedef void (WINAPI *PFN_COTASKMEMFREE)(LPVOID);
static int choose_folder(const char *title, char *out)           /* boite de dialogue "choisir un dossier" */
{
    BROWSEINFOA bi;
    char disp[MAX_PATH];
    LPITEMIDLIST pidl;
    int ok;
    HMODULE ole;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = hwnd; bi.pszDisplayName = disp; bi.lpszTitle = title; bi.ulFlags = BIF_RETURNONLYFSDIRS;
    pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return 0;
    ok = SHGetPathFromIDListA(pidl, out) ? 1 : 0;
    ole = LoadLibraryA("ole32.dll");
    if (ole) { PFN_COTASKMEMFREE fr = (PFN_COTASKMEMFREE)GetProcAddress(ole, "CoTaskMemFree"); if (fr) fr(pidl); }
    return ok && out[0];
}
static void ensure_dir(const char *path)
{
    char tmp[MAX_PATH];
    size_t i;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (i = 1; tmp[i]; i++)
        if (tmp[i] == '\\' || tmp[i] == '/') { char c = tmp[i]; tmp[i] = 0; CreateDirectoryA(tmp, NULL); tmp[i] = c; }
    CreateDirectoryA(tmp, NULL);
}
static void stamp_now(char *out, int n)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, (size_t)n, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}
static void do_screenshot(void)
{
    char game[64], ts[32], path[MAX_PATH * 2];
    if (!cfg.shot_dir[0]) {                                       /* pas de dossier configure : on demande */
        char chosen[MAX_PATH];
        if (!choose_folder("Dossier des captures d'ecran", chosen)) return;
        snprintf(cfg.shot_dir, MAX_PATH, "%s", chosen);
        settings_save();
    }
    ensure_dir(cfg.shot_dir);
    game_name(game, sizeof game); stamp_now(ts, sizeof ts);
    snprintf(path, sizeof path, "%s\\%s_%s.png", cfg.shot_dir, game, ts);
    if (png_save(path, (sx >= 1 && sx <= 10) ? sx : 1)) show_note("Capture d'ecran enregistree");
    else { cfg.shot_dir[0] = 0; settings_save(); ui_error("Impossible d'ecrire la capture : dossier invalide. Il sera redemande."); }
}
#define MAX_SNAPS 10
static char snap_list[MAX_SNAPS][MAX_PATH];
static int snap_count;
static int snap_cmp(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }   /* plus recent d'abord */
static void snap_scan(void)                                      /* derniers snapshots du jeu courant */
{
    static char all[256][MAX_PATH];
    char game[64], pat[MAX_PATH * 2];
    int n = 0, i;
    size_t want;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snap_count = 0;
    game_name(game, sizeof game);
    want = strlen(game) + 1 + 15 + 4;                            /* <jeu>_AAAAMMJJ_HHMMSS.osn */
    snprintf(pat, sizeof pat, "%s\\%s_*.osn", cfg.snap_dir, game);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (n < 256 && strlen(fd.cFileName) == want) snprintf(all[n++], MAX_PATH, "%s", fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(all, (size_t)n, MAX_PATH, snap_cmp);
    for (i = 0; i < n && i < MAX_SNAPS; i++) snprintf(snap_list[snap_count++], MAX_PATH, "%s\\%s", cfg.snap_dir, all[i]);
}
static void do_snapshot_save(void)
{
    char game[64], ts[32], path[MAX_PATH * 2];
    if (mode != MODE_RUN) { show_note("Snapshot : lancez d'abord un programme"); return; }
    ensure_dir(cfg.snap_dir);
    game_name(game, sizeof game); stamp_now(ts, sizeof ts);
    snprintf(path, sizeof path, "%s\\%s_%s.osn", cfg.snap_dir, game, ts);
    if (snap_save(path)) show_note("Snapshot enregistre");
    else ui_error("Impossible d'ecrire le snapshot (dossier invalide ?).");
}
static void do_snapshot_load(const char *path)
{
    if (snap_load(path)) { memset(keyrows, 0, sizeof keyrows); show_note("Snapshot charge"); }
}
static void do_snapshot_load_last(void)
{
    snap_scan();
    if (snap_count) do_snapshot_load(snap_list[0]);
    else show_note("Aucun snapshot pour ce jeu");
}

/* ---------- menu clic droit ---------- */
enum { IDM_BROWSER = 100, IDM_HARD, IDM_NMI, IDM_FULL, IDM_CRT, IDM_GLOW, IDM_FLICK, IDM_TURBO,
       IDM_LOADSND, IDM_AZERTY, IDM_REWIND, IDM_STATS, IDM_QUIT, IDM_CPUTURBO, IDM_GPU, IDM_LOG, IDM_FASTTAPE, IDM_FASTDISK, IDM_PAUSE, IDM_SHOT, IDM_SNAPSAVE, IDM_SNAPLAST,
       IDM_VOL = 200, IDM_MULT = 250, IDM_SIZE = 300, IDM_FILTER = 400, IDM_ROM = 500, IDM_SNAP = 600 };
#define MAX_ROMS 24
static char rom_list[MAX_ROMS][MAX_PATH];
static int rom_count;

/* ROM BASIC disponibles dans roms\\ (la ROM Microdisc est exclue) */
static void rom_scan(void)
{
    char pat[MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    rom_count = 0;
    snprintf(pat, sizeof pat, "%s\\roms\\*.rom", exe_dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(path, sizeof path, "%s\\roms\\%s", exe_dir, fd.cFileName);
        if (_stricmp(path, cfg.mdrom) == 0) continue;
        if (rom_count < MAX_ROMS) snprintf(rom_list[rom_count++], MAX_PATH, "%s", path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static void switch_rom(const char *path)
{
    int had_disk = disk_on;
    if (rom_assign(path) != 1) { ui_error("ROM BASIC invalide (16384 octets attendus)."); return; }
    snprintf(cfg.rom, MAX_PATH, "%s", path);
    settings_save();
    capture_font();
    if (mode == MODE_BROWSER) { vram = bram; hires_mode = 0; machine_running = 0; browser_draw(); }
    else boot_machine(had_disk, 0);
}
static const int mult_list[4] = { 2, 4, 8, 16 };

static void set_window_step(int st)
{
    int fw, fh, a, b;
    if (fullscreen) toggle_fullscreen();
    frame_extra(&fw, &fh);
    step_dims(st, &a, &b);
    SetWindowPos(hwnd, NULL, 0, 0, VW * a + fw, VH * b + fh, SWP_NOMOVE | SWP_NOZORDER);
}
static void run_command(int id)
{
    switch (id) {
    case IDM_BROWSER:
        if (mode == MODE_BROWSER) { if (machine_running) browser_leave_resume(); }
        else if (mode == MODE_RUN) { memset(keyrows, 0, sizeof keyrows); browser_enter(); }
        break;
    case IDM_HARD: if (mode != MODE_NOROM) boot_machine(disk_on, 0); break;
    case IDM_NMI: if (mode == MODE_RUN) machine_nmi(); break;
    case IDM_FULL: toggle_fullscreen(); break;
    case IDM_CRT: cfg.scanlines = !cfg.scanlines; break;
    case IDM_GLOW: cfg.crt_glow = cfg.crt_glow ? 0 : 40; break;
    case IDM_FLICK: cfg.crt_flicker = cfg.crt_flicker ? 0 : 35; break;
    case IDM_TURBO: turbo = !turbo; update_title(); break;
    case IDM_CPUTURBO: cfg.cpu_turbo = !cfg.cpu_turbo; update_title(); break;
    case IDM_PAUSE: if (mode == MODE_RUN) { paused = !paused; update_title(); } break;
    case IDM_SHOT: do_screenshot(); break;
    case IDM_SNAPSAVE: do_snapshot_save(); break;
    case IDM_SNAPLAST: do_snapshot_load_last(); break;
    case IDM_FASTTAPE: cfg.fast_tape = !cfg.fast_tape; break;
    case IDM_FASTDISK: cfg.fast_disk = !cfg.fast_disk; break;
    case IDM_LOG: dbg_on = !dbg_on; if (dbg_on) dbg_log("journal active"); break;
    case IDM_GPU:
        cfg.renderer = use_gpu ? 2 : 1;
        renderer_apply();
        if (cfg.renderer == 1 && !use_gpu) { cfg.renderer = 2; ui_error("Direct3D 11 indisponible : rendu GDI conserve."); }
        break;
    case IDM_LOADSND: cfg.loadsnd = !cfg.loadsnd; break;
    case IDM_AZERTY: cfg.azerty = !cfg.azerty; break;
    case IDM_REWIND: tape_pos = 0; break;
    case IDM_STATS: show_stats = !show_stats; stat_cyc = stat_irq = stat_nmi = stat_frames = 0; update_title(); break;
    case IDM_QUIT: DestroyWindow(hwnd); return;
    default:
        if (id >= IDM_VOL && id < IDM_VOL + 5) cfg.volume = (id - IDM_VOL) * 25;
        else if (id >= IDM_ROM && id < IDM_ROM + rom_count) { switch_rom(rom_list[id - IDM_ROM]); return; }
        else if (id >= IDM_SNAP && id < IDM_SNAP + snap_count) { do_snapshot_load(snap_list[id - IDM_SNAP]); return; }
        else if (id >= IDM_FILTER && id < IDM_FILTER + NFILTERS) { cfg.filter = id - IDM_FILTER; update_title(); }
        else if (id >= IDM_MULT && id < IDM_MULT + 4) { cfg.cpu_mult = mult_list[id - IDM_MULT]; update_title(); }
        else if (id >= IDM_SIZE && id <= IDM_SIZE + MAX_STEP) set_window_step(id - IDM_SIZE);
        break;
    }
    settings_save();
}
static void show_context_menu(int x, int y)
{
    HMENU m = CreatePopupMenu(), sz = CreatePopupMenu(), vol = CreatePopupMenu(), mu = CreatePopupMenu(), fl = CreatePopupMenu(), rm = CreatePopupMenu(), sn = CreatePopupMenu();
    MONITORINFO mi;
    char t[64];
    int st, i, fw, fh, maxw, maxh, id, a, b;
#define CHK(c) ((c) ? MF_CHECKED : 0)
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    frame_extra(&fw, &fh);
    maxw = (mi.rcWork.right - mi.rcWork.left) - fw; maxh = (mi.rcWork.bottom - mi.rcWork.top) - fh;
    for (st = 1; st <= MAX_STEP; st++) {
        step_dims(st, &a, &b);
        if (st > 1 && (VW * a > maxw || VH * b > maxh)) break;
        snprintf(t, sizeof t, "x%d   (%dx%d)", st, VW * a, VH * b);
        AppendMenuA(sz, MF_STRING | CHK(!fullscreen && sx == a), IDM_SIZE + st, t);
    }
    for (i = 0; i < 5; i++) {
        snprintf(t, sizeof t, "%d %%", i * 25);
        AppendMenuA(vol, MF_STRING | CHK((cfg.volume + 12) / 25 == i), IDM_VOL + i, t);
    }
    rom_scan();
    for (i = 0; i < rom_count; i++)
        AppendMenuA(rm, MF_STRING | CHK(_stricmp(rom_list[i], cfg.rom) == 0), IDM_ROM + i, base_name(rom_list[i]));
    if (!rom_count) AppendMenuA(rm, MF_STRING | MF_GRAYED, 0, "(aucune ROM dans roms\\)");
    AppendMenuA(m, MF_STRING, IDM_BROWSER, "Selecteur de fichiers\tF1");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)rm, "ROM (redemarre la machine)");
    AppendMenuA(m, MF_STRING, IDM_HARD, "Hard reset\tF12");
    AppendMenuA(m, MF_STRING, IDM_NMI, "Soft reset (NMI)\tF11");
    AppendMenuA(m, MF_STRING, IDM_REWIND, "Rembobiner la K7\tF3");
    snap_scan();
    AppendMenuA(sn, MF_STRING, IDM_SNAPSAVE, "Enregistrer un snapshot\tInser");
    AppendMenuA(sn, MF_STRING | (snap_count ? 0 : MF_GRAYED), IDM_SNAPLAST, "Recharger le dernier\tDebut");
    AppendMenuA(sn, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < snap_count; i++) {
        const char *nm = base_name(snap_list[i]);
        size_t gl = strlen(nm) - 4 - 15;                          /* debut de AAAAMMJJ_HHMMSS */
        snprintf(t, sizeof t, "%.4s-%.2s-%.2s   %.2s:%.2s:%.2s", nm + gl, nm + gl + 4, nm + gl + 6, nm + gl + 9, nm + gl + 11, nm + gl + 13);
        AppendMenuA(sn, MF_STRING, IDM_SNAP + i, t);
    }
    if (!snap_count) AppendMenuA(sn, MF_STRING | MF_GRAYED, 0, "(aucun snapshot pour ce jeu)");
    AppendMenuA(m, MF_STRING | CHK(paused), IDM_PAUSE, "Pause\tF7");
    AppendMenuA(m, MF_STRING, IDM_SHOT, "Capture d'ecran (PNG)\tF8");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sn, "Snapshots (derniers du jeu)");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | CHK(fullscreen), IDM_FULL, "Plein ecran\tAlt+Entree");
    AppendMenuA(m, MF_POPUP | (fullscreen ? MF_GRAYED : 0), (UINT_PTR)sz, "Taille de la fenetre");
    AppendMenuA(m, MF_STRING | CHK(cfg.scanlines), IDM_CRT, "Effet CRT (scanlines)\tF2");
    AppendMenuA(m, MF_STRING | CHK(cfg.crt_glow), IDM_GLOW, "   Luminescence");
    AppendMenuA(m, MF_STRING | CHK(cfg.crt_flicker), IDM_FLICK, "   Scintillement");
    for (i = 0; i < NFILTERS; i++) AppendMenuA(fl, MF_STRING | CHK(cfg.filter == i), IDM_FILTER + i, filter_names[i]);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)fl, "Filtre de mise a l'echelle\tF4");
    AppendMenuA(m, MF_STRING | CHK(use_gpu), IDM_GPU, "Rendu GPU (Direct3D 11)");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)vol, "Volume");
    AppendMenuA(m, MF_STRING | CHK(cfg.loadsnd), IDM_LOADSND, "Son de chargement K7/disquette");
    AppendMenuA(m, MF_STRING | CHK(cfg.azerty), IDM_AZERTY, "Clavier AZERTY (legendes)");
    for (i = 0; i < 4; i++) {
        snprintf(t, sizeof t, "x%d", mult_list[i]);
        AppendMenuA(mu, MF_STRING | CHK(cfg.cpu_mult == mult_list[i]), IDM_MULT + i, t);
    }
    AppendMenuA(m, MF_STRING | CHK(cfg.cpu_turbo), IDM_CPUTURBO, "Turbo CPU (cadence IRQ d'origine)\tF5");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)mu, "   Facteur du turbo CPU");
    AppendMenuA(m, MF_STRING | CHK(cfg.fast_tape), IDM_FASTTAPE, "Acceleration K7 (chargement instantane)\tF6");
    AppendMenuA(m, MF_STRING | CHK(cfg.fast_disk), IDM_FASTDISK, "Acceleration disquette (turbo pendant les acces)\tF6");
    AppendMenuA(m, MF_STRING | CHK(turbo), IDM_TURBO, "Turbo total (tout accelere)\tF10");
    AppendMenuA(m, MF_STRING | CHK(show_stats), IDM_STATS, "Statistiques (barre de titre)\tF9");
    AppendMenuA(m, MF_STRING | CHK(dbg_on), IDM_LOG, "Journal de diagnostic (oric.log)");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, IDM_QUIT, "Quitter");
    SetForegroundWindow(hwnd);
    id = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    PostMessageA(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (id) run_command(id);
#undef CHK
}

static void on_key(WPARAM vk, LPARAM l, int down)
{
    int sc = (int)((l >> 16) & 0xFF);
    int repeat = down && (l & (1 << 30)) != 0;   /* bit 30 est toujours 1 sur un relachement */
    if (l & (1 << 24)) sc |= 0xE000;
    if (mode == MODE_NOROM) return;
    if (!down && (vk == VK_SNAPSHOT || sc == 0xE037)) { do_screenshot(); return; }   /* Impr. ecran : seul le relachement est signale */
    if (down && !repeat && sc == cfg.k_shot) { do_screenshot(); return; }
    if (down && (sc == 0x1C || sc == 0xE01C) && (GetKeyState(VK_MENU) & 0x8000)) {
        if (!repeat) toggle_fullscreen();
        return;
    }
    if (mode == MODE_BROWSER) {
        if (down) {
            if (sc == cfg.k_browser && !repeat) {
                if (machine_running) browser_leave_resume();
            } else browser_key(sc);
        }
        return;
    }
    if (down && !repeat) {
        if (sc == cfg.k_browser) { memset(keyrows, 0, sizeof keyrows); browser_enter(); return; }
        if (sc == cfg.k_hard)    { boot_machine(disk_on, 0); return; }
        if (sc == cfg.k_soft)    { machine_nmi(); return; }
        if (sc == cfg.k_turbo)   { turbo = !turbo; update_title(); return; }
        if (sc == cfg.k_rewind)  { tape_pos = 0; return; }
        if (sc == cfg.k_pause)   { paused = !paused; update_title(); return; }
        if (sc == cfg.k_snap)    { do_snapshot_save(); return; }
        if (sc == cfg.k_snapload) { do_snapshot_load_last(); return; }
        if (sc == cfg.k_fast)    { int on = !(cfg.fast_tape && cfg.fast_disk); cfg.fast_tape = cfg.fast_disk = on; settings_save(); return; }
        if (sc == cfg.k_scan)    { cfg.scanlines = !cfg.scanlines; settings_save(); return; }
        if (sc == cfg.k_aa)      { cfg.filter = (cfg.filter + 1) % NFILTERS; settings_save(); update_title(); return; }
        if (sc == cfg.k_cputurbo) { cfg.cpu_turbo = !cfg.cpu_turbo; settings_save(); update_title(); return; }
        if (sc == cfg.k_stats)   { show_stats = !show_stats; stat_cyc = stat_irq = stat_nmi = stat_frames = 0; update_title(); return; }
    }
    if (sc == cfg.k_browser || sc == cfg.k_hard || sc == cfg.k_soft || sc == cfg.k_turbo || sc == cfg.k_rewind || sc == cfg.k_stats || sc == cfg.k_scan || sc == cfg.k_aa || sc == cfg.k_cputurbo || sc == cfg.k_fast ||
        sc == cfg.k_pause || sc == cfg.k_shot || sc == cfg.k_snap || sc == cfg.k_snapload) return;
    if (repeat) return;
    key_set(sc_to_oric(sc), down);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE: layout_update(); return 0;
    case WM_GETMINMAXINFO:
        if (!fullscreen) {
            MINMAXINFO *mm = (MINMAXINFO *)l;
            int fw, fh, a, b;
            frame_extra(&fw, &fh);
            step_dims(1, &a, &b);
            mm->ptMinTrackSize.x = VW * a + fw;
            mm->ptMinTrackSize.y = VH * b + fh;
        }
        return 0;
    case WM_SIZING: {                       /* redimensionnement par paliers : pas de bordure noire */
        RECT *r = (RECT *)l;
        MONITORINFO mi;
        int fw, fh, cw, ch, maxw, maxh, st, best = 1, bestd = 1 << 30, a, b, W, H;
        frame_extra(&fw, &fh);
        cw = (r->right - r->left) - fw; ch = (r->bottom - r->top) - fh;
        mi.cbSize = sizeof mi;
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        maxw = (mi.rcWork.right - mi.rcWork.left) - fw; maxh = (mi.rcWork.bottom - mi.rcWork.top) - fh;
        for (st = 1; st <= MAX_STEP; st++) {
            int d;
            step_dims(st, &a, &b);
            if (st > 1 && (VW * a > maxw || VH * b > maxh)) break;
            d = abs(VW * a - cw) + abs(VH * b - ch);
            if (d < bestd) { bestd = d; best = st; }
        }
        step_dims(best, &a, &b);
        W = VW * a + fw; H = VH * b + fh;
        switch (w) {
        case WMSZ_LEFT: case WMSZ_TOPLEFT: case WMSZ_BOTTOMLEFT: r->left = r->right - W; break;
        default: r->right = r->left + W; break;
        }
        switch (w) {
        case WMSZ_TOP: case WMSZ_TOPLEFT: case WMSZ_TOPRIGHT: r->top = r->bottom - H; break;
        default: r->bottom = r->top + H; break;
        }
        return TRUE;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(h, &ps);
        if (use_gpu && mode != MODE_NOROM) { EndPaint(h, &ps); gpu_present(); return 0; }
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (mode == MODE_NOROM) {
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            TextOutA(dc, 20, 20, "ROM Atmos introuvable (atmos.rom, 16384 octets).", 48);
            TextOutA(dc, 20, 44, "Placez-la a cote de oric.exe (ou dans roms\\) ou glissez un fichier .rom ici.", 76);
        } else present_dc(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (m == WM_SYSKEYDOWN && w == VK_F4) break;
        on_key(w, l, 1); return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        on_key(w, l, 0); return 0;
    case WM_CHAR:
        if (mode == MODE_BROWSER && w > 32 && w < 127) browser_jump((char)w);
        return 0;
    case WM_KILLFOCUS: memset(keyrows, 0, sizeof keyrows); return 0;
    case WM_CONTEXTMENU: {
        int px = (int)(short)LOWORD(l), py = (int)(short)((l >> 16) & 0xFFFF);
        if (l == -1) { POINT pt; GetCursorPos(&pt); px = pt.x; py = pt.y; }
        show_context_menu(px, py);
        return 0;
    }
    case WM_SETCURSOR:
        if (fullscreen && LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)w;
        static char names[8][MAX_PATH];
        UINT n = DragQueryFileA(hd, 0xFFFFFFFF, NULL, 0), i;
        int c = 0;
        for (i = 0; i < n && c < 8; i++) { DragQueryFileA(hd, i, names[c], MAX_PATH); c++; }
        DragFinish(hd);
        handle_files(names, c);
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cl, int ns)
{
    WNDCLASSA wc;
    MSG msg;
    LARGE_INTEGER fq;
    LONGLONG freq, next_t, stat_t0 = 0;
    char *p;
    int i, cw, ch, wx, wy;
    RECT wa, rc;
    (void)hp; (void)cl; (void)ns;

    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    p = strrchr(exe_dir, '\\'); if (p) *p = 0;
    snprintf(ini_path, MAX_PATH, "%s\\settings.ini", exe_dir);
    settings_load();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "OricAtmosEmu"; wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    {   /* plus grand palier qui tient dans la zone de travail */
        int fw, fh, a, b;
        frame_extra(&fw, &fh);
        step_dims(step_fit((wa.right - wa.left) - fw, (wa.bottom - wa.top) - fh), &a, &b);
        cw = VW * a; ch = VH * b;
    }
    rc.left = 0; rc.top = 0; rc.right = cw; rc.bottom = ch;
    AdjustWindowRect(&rc, WIN_STYLE, FALSE);
    wx = wa.left + ((wa.right - wa.left) - (rc.right - rc.left)) / 2;
    wy = wa.top + ((wa.bottom - wa.top) - (rc.bottom - rc.top)) / 2;
    hwnd = CreateWindowExA(0, "OricAtmosEmu", "Oric Atmos", WIN_STYLE | WS_VISIBLE,
                           wx, wy, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hi, NULL);
    DragAcceptFiles(hwnd, TRUE);
    timeBeginPeriod(1);
    audio_init();
    QueryPerformanceFrequency(&fq); freq = fq.QuadPart;
    layout_update();

    roms_init();
    if (!have_basic) { mode = MODE_NOROM; InvalidateRect(hwnd, NULL, TRUE); }
    else {
        if (!capture_font()) ui_error("La ROM semble invalide (jeu de caracteres introuvable).");
        mode = MODE_RUN;
        renderer_apply();
        if (cfg.fullscreen) toggle_fullscreen();
        {   /* fichiers passes en argument (glisse sur l'exe, "ouvrir avec") */
            static char names[8][MAX_PATH];
            int c = 0;
            for (i = 1; i < __argc && c < 8; i++) { snprintf(names[c], MAX_PATH, "%s", __argv[i]); c++; }
            if (c) handle_files(names, c);
            else if (cfg.start_browser) { vram = bram; browser_enter(); }
            else boot_machine(0, 0);
        }
    }

    next_t = qpc();
    for (;;) {
        LONGLONG now, frame_ticks;
        int n;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        audio_pump();
        if (mode == MODE_NOROM) { Sleep(20); continue; }
        now = qpc();
        if (note[0] && GetTickCount() > note_until) { note[0] = 0; update_title(); }
        if (show_stats && now - stat_t0 >= freq) {
            double dt = (double)(now - stat_t0) / (double)freq;
            snprintf(stats_str, sizeof stats_str,
                     "| %.2f MHz | %.0f img/s | IRQ %.0f/s | NMI %ld | T1L=%04X ACR=%02X IER=%02X | PC=%04X mem=%d %s",
                     stat_cyc / dt / 1e6, stat_frames / dt, stat_irq / dt, stat_nmi,
                     via.t1l, via.acr, via.ier, PC, rom_mode, "");
            if (disk_on)
                snprintf(stats_str + strlen(stats_str), sizeof stats_str - strlen(stats_str),
                         "| DISQUE ctl=%02X cmds=%ld dernier=%02X err=%02X octets=%ld piste=%d",
                         md_ctl, fdc_ncmd, fdc_lastcmd, fdc_laststatus, fdc_nbytes, fdc.head[fdc.drive]);
            update_title();
            stat_cyc = stat_irq = stat_frames = 0; stat_t0 = now;
        }
        frame_ticks = freq * frame_lines * 64 / 1000000;
        if (now < next_t) {
            if (next_t - now > freq / 1000) Sleep(1);
            continue;
        }
        n = (mode == MODE_RUN && (turbo || boot_turbo > 0 ||
                                  (cfg.fast_tape && gen_state && tape_motor) ||          /* lecteur K7 maison : le signal passe a pleine vitesse */
                                  (cfg.fast_disk && disk_on && fdc_hot > 0))) ? 8 : 1;   /* acces disquette */
        if (mode == MODE_RUN && paused) {
            /* pause : rien n'est execute, l'image est simplement re-presentee */
        } else if (mode == MODE_RUN) {
            audio_muted = (n > 1);
            for (i = 0; i < n; i++) { emu_frame(); if (boot_turbo > 0) boot_turbo--; }
        } else {
            video_only_frame();
        }
        present();
        next_t += frame_ticks;
        if (n > 1 || now - next_t > freq / 8) next_t = now + frame_ticks;
    }
done:
    settings_save();
    gpu_shutdown();
    if (audio_ok) { waveOutReset(hwo); waveOutClose(hwo); }
    timeEndPeriod(1);
    return 0;
}

#else  /* ---- pas Windows : uniquement pour tester le coeur ---- */

static void ui_error(const char *msg) { fprintf(stderr, "ERREUR: %s\n", msg); }
static void update_title(void) {}

#ifdef ORIC_TEST
/* petit test du CPU / VIA / AY / video sur Linux */
static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("ECHEC: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)
int main(void)
{
    static const u8 prog[] = {
        0xA2, 0x0A,             /* LDX #10        */
        0xA9, 0x00,             /* LDA #0         */
        0x18,                   /* loop: CLC      */
        0x69, 0x03,             /* ADC #3         */
        0xCA,                   /* DEX            */
        0xD0, 0xFA,             /* BNE loop       */
        0x8D, 0x00, 0x02,       /* STA $0200      */
        0xF8,                   /* SED            */
        0xA9, 0x19, 0x18, 0x69, 0x28,   /* LDA #$19 CLC ADC #$28 */
        0x8D, 0x01, 0x02,       /* STA $0201      */
        0x38, 0xA9, 0x50, 0xE9, 0x13,   /* SEC LDA #$50 SBC #$13 */
        0x8D, 0x02, 0x02,       /* STA $0202      */
        0xD8,                   /* CLD            */
        0x20, 0x40, 0x04,       /* JSR $0440      */
        0x8D, 0x03, 0x02,       /* STA $0203      */
        0xA9, 0x80, 0x85, 0x10, 0x06, 0x10, 0xA5, 0x10,  /* ASL zp : 0x80<<1 = 0, C=1 */
        0x8D, 0x04, 0x02,
        0xA2, 0x01, 0xBD, 0x00, 0x05, 0x8D, 0x05, 0x02,  /* LDX #1 ; LDA $0500,X ; STA $0205 */
        0x4C, 0x7F, 0x04        /* JMP * (fin) */
    };
    int i;
    memset(ram, 0, sizeof ram);
    memset(rom_basic, 0, sizeof rom_basic);
    rom_basic[0x3FFC] = 0x00; rom_basic[0x3FFD] = 0x04;
    memcpy(ram + 0x400, prog, sizeof prog);
    ram[0x440] = 0xA9; ram[0x441] = 0x77; ram[0x442] = 0x60;      /* LDA #$77 ; RTS */
    ram[0x501] = 0x5A;
    ram[0x47F] = 0x4C; ram[0x480] = 0x7F; ram[0x481] = 0x04;
    memcpy(ram + 0x400 + sizeof prog - 3, (const u8[]){ 0x4C, 0x7F, 0x04 }, 3);
    disk_on = 0; paging_update();
    via_reset(); ay_reset(); fdc_reset();
    cpu_reset();
    for (i = 0; i < 400; i++) cpu_step();
    CHECK(ram[0x200] == 30, "boucle ADC/DEX/BNE = 30");
    CHECK(ram[0x201] == 0x47, "ADC decimal 19+28 = 47");
    CHECK(ram[0x202] == 0x37, "SBC decimal 50-13 = 37");
    CHECK(ram[0x203] == 0x77, "JSR/RTS");
    CHECK(ram[0x204] == 0x00, "ASL zp");
    CHECK(ram[0x205] == 0x5A, "LDA abs,X");

    /* VIA : timer 1 en mode libre, IRQ */
    via_reset();
    via_write(14, 0xC0);        /* IER: T1 */
    via_write(11, 0x40);        /* ACR: free run */
    via_write(4, 0x10); via_write(5, 0x00);
    via_tick(100);
    CHECK(via.irq, "VIA T1 declenche l'IRQ");
    via_read(4);
    CHECK(!via.irq, "lecture T1C-L acquitte l'IRQ");

    /* AY : ecriture de registres via les lignes de controle de la VIA */
    via_reset(); ay_reset();
    via_write(3, 0xFF); via_write(2, 0xFF);
    via_write(1, 0x08); via_write(12, 0xFF);   /* latch adresse 8 */
    via_write(12, 0xDD);
    via_write(1, 0x0F); via_write(12, 0xFD);   /* ecriture donnee = 15 */
    via_write(12, 0xDD);
    CHECK(ay.r[8] == 0x0F, "ecriture AY reg 8 via VIA");

    /* clavier : touche A, ligne 6, masque 0x20 */
    via_reset(); ay_reset();
    key_set(KY_A, 1);
    ay.r[14] = (u8)~0x20;
    via_write(2, 0x07); via_write(0, 6);
    CHECK((via_read(0) & 8) != 0, "matrice clavier : A detectee (ligne 6)");
    via_write(0, 5);
    CHECK((via_read(0) & 8) == 0, "matrice clavier : rien sur ligne 5");

    /* video : ecran texte avec attribut */
    memset(bram, 0, sizeof bram);
    vram = ram; hires_mode = 0;
    memset(ram, 0x20, sizeof ram);
    ram[0xBB80] = 0x02;                         /* ink vert */
    ram[0xBB81] = 'A';
    for (i = 0; i < 8; i++) ram[0xB400 + 'A' * 8 + i] = 0x3F;
    render_line(0);
    CHECK(fb[0][6] == pal[2] || fb[0][6] == pal[0], "rendu ULA ligne 0");
    CHECK(fb[0][6] == pal[2], "attribut ink applique au caractere suivant");

    /* double hauteur : moitie haute sur les lignes texte paires, basse sur les impaires (quelle que soit la 1re ligne) */
    {
        int row, g, okp = 1, okq = 1;
        memset(ram, 0x20, sizeof ram);
        for (g = 0; g < 8; g++) ram[0xB400 + 'A' * 8 + g] = (u8)(0x01 << (g % 6) | 0x20);   /* glyphe : chaque ligne differente */
        vram = ram; hires_mode = 0;
        /* lignes 1 et 2 en double hauteur (commence sur une ligne IMPAIRE) : 'A' attribut 0x0A = double hauteur */
        for (row = 1; row <= 2; row++) { ram[0xBB80 + row * 40] = 0x0A; ram[0xBB80 + row * 40 + 1] = 'A'; }
        for (row = 0; row < 4; row++) {
            for (g = 0; g < 8; g++) {
                int y = row * 8 + g, px, expect_g;
                render_line(y);
                px = 0; { int b; for (b = 0; b < 6; b++) if (fb[y][6 + b] == pal[7]) px |= 1 << (5 - b); }
                if (row == 1 || row == 2) {
                    expect_g = (g >> 1) + ((row & 1) ? 4 : 0);
                    if (px != (ram[0xB400 + 'A' * 8 + expect_g] & 0x3F)) { if (row == 1) okp = 0; else okq = 0; }
                }
            }
        }
        CHECK(okp, "double hauteur : ligne texte impaire (1) = moitie BASSE du caractere");
        CHECK(okq, "double hauteur : ligne texte paire (2) = moitie HAUTE du caractere");
    }
    cfg.fast_tape = 1;
    /* hooks K7 : synchro sautee puis lecture d'octets (octet dans A et $2F, RTS de la ROM) */
    {
        static u8 t[] = { 0x16, 0x16, 0x16, 0x24, 0x00, 0x00, 0x80, 0xC7 };
        tape = t; tape_len = sizeof t; tape_pos = 0; rom_mode = 0;
        hook_sync = 0x0700; hook_sync_end = 0x0710; hook_rb = 0x0600; hook_rb_end = 0x0620; hook_atmos = 1;
        S = 0xFD; push(0x08); push(0x00);                         /* retour en 0x0801, pas de boucle */
        PC = 0x0700; cpu_step();
        CHECK(PC == 0x0710 && tape_pos == 4, "hook synchro : bande placee apres 0x24");
        {   /* appelant du type loader : JSR lecture ; CMP #$24 ; BNE */
            static const u8 pat[5] = { 0x20, 0x00, 0x06, 0xC9, 0x24 };
            memcpy(ram + 0x0801, pat, 5);
            tape_pos = 0; S = 0xFD; push(0x08); push(0x00);
            PC = 0x0700; cpu_step();
            CHECK(PC == 0x0710 && tape_pos == 3 && tape[tape_pos] == 0x24, "hook synchro : bande placee avant 0x24 (loader avec boucle)");
        }
        tape_pos = 4;
        PC = 0x0600; cpu_step();
        CHECK(A == 0x00 && ram[0x2F] == 0 && (P & FZ) && (P & FC) && PC == 0x0620 && tape_pos == 5, "hook lecture octet (A, $2F, Z, C)");
        PC = 0x0600; cpu_step(); PC = 0x0600; cpu_step();
        CHECK(A == 0x80 && ram[0x2F] == 0x80 && (P & FN) && ram[0x2B1] == 0, "hook lecture octet 0x80");
        tape = NULL;
    }
    /* turbo CPU : les timers VIA restent a 1 MHz, le CPU execute mult fois plus de cycles */
    {
        int mult, results[2] = { 0, 0 };
        for (mult = 1; mult <= 4; mult += 3) {
            long total = 0;
            memset(ram, 0, sizeof ram);
            ram[0x500] = 0xE8; ram[0x501] = 0x4C; ram[0x502] = 0x00; ram[0x503] = 0x05;   /* INX ; JMP $0500 */
            via_reset(); ay_reset(); fdc_reset(); disk_on = 0; paging_update();
            cpu_reset(); PC = 0x500; P |= FI; periph_acc = 0; vcyc = 0; vline = 0; tq_len = tq_pos = 0;
            cfg.cpu_turbo = (mult > 1); cfg.cpu_mult = mult;
            via_write(11, 0x40); via_write(4, 0xE8); via_write(5, 0x03);   /* T1 libre = 1000 cycles */
            audio_muted = 1;
            while (!(via.ifr & 0x40) && total < 100000) { int before = (int)stat_cyc; emu_step(); total += (long)stat_cyc - before; }
            results[mult > 1] = (int)total;
        }
        CHECK(results[0] > 900 && results[0] < 1100, "turbo CPU off : le timer expire apres ~1000 cycles CPU");
        CHECK(results[1] > 3800 && results[1] < 4200, "turbo CPU x4 : ~4000 cycles CPU pour le meme delai VIA (IRQ inchangees)");
        cfg.cpu_turbo = 0; audio_muted = 0;
    }
    /* recherche des ROM dans roms\\ : atmos.rom par defaut, dos.rom pour le Microdisc */
    {
        snprintf(exe_dir, MAX_PATH, "/tmp/romtest");
        cfg.rom[0] = 0; cfg.mdrom[0] = 0; cfg.hook = 0;
        roms_init();
        CHECK(have_basic && strstr(cfg.rom, "atmos.rom") && !strstr(cfg.rom, "azerty"), "ROM par defaut : roms\\atmos.rom");
        CHECK(have_disk && strstr(cfg.mdrom, "dos.rom") && rom_disk_size == 8192, "ROM Microdisc : roms\\dos.rom (8192 octets)");
        CHECK(hook_rb == 0xE6C9 && hook_atmos && !rom_is_azerty, "Atmos BASIC 1.1 : hook K7 en $E6C9");
        CHECK(rom_assign("/tmp/romtest\\roms\\oric1.rom") == 1 && hook_rb == 0xE630 && !hook_atmos, "oric1.rom : BASIC 1.0, hook K7 en $E630");
        CHECK(rom_assign("/tmp/romtest\\roms\\atmos_azerty.rom") == 1 && rom_is_azerty, "atmos_azerty.rom reconnue (pas de remappage logiciel)");
        CHECK(rom_assign("/tmp/romtest\\roms\\dos.rom") == 2, "dos.rom classee comme ROM disque et non comme BASIC");
        cfg.azerty = 1;
        CHECK(sc_to_oric(0x10) == KY_Q, "ROM AZERTY : clavier positionnel (pas de permutation A/Q)");
        rom_is_azerty = 0;
        CHECK(sc_to_oric(0x10) == KY_A, "ROM QWERTY + option AZERTY : touche etiquetee A -> A");
        cfg.azerty = 0;
    }
    /* snapshot : enregistrement puis restauration de l'etat (RAM, CPU, VIA, AY, ULA, K7) ; refus si ROM differente */
    {
        u8 saved_ram_a, saved_ram_b;
        u16 pc0;
        (void)system("mkdir -p /tmp/snaps");
        memset(ram, 0, sizeof ram);
        memset(rom_basic, 0x11, sizeof rom_basic); disk_on = 0; paging_update();
        via_reset(); ay_reset(); fdc_reset();
        ram[0x1234] = 0xAB; ram[0xC123] = 0xCD;
        A = 0x42; X = 0x43; Y = 0x44; S = 0xEE; P = 0xA5; PC = 0x6789; hires_mode = 1; vline = 77; frame_counter = 4242;
        via.t1l = 0x1357; via.ier = 0xC0; ay.r[8] = 9; md_ctl = 0x04;
        { static u8 t[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }; tape = t; tape_len = 10; tape_pos = 6; snprintf(tape_name, MAX_PATH, "/tmp/snaps/x.tap"); }
        CHECK(snap_save("/tmp/snaps/a.osn"), "snapshot : ecriture");
        ram[0x1234] = 0; ram[0xC123] = 0; A = X = Y = 0; PC = 0; hires_mode = 0; vline = 0; frame_counter = 0; via.t1l = 0; ay.r[8] = 0; tape_pos = 0;
        saved_ram_a = 0; saved_ram_b = 0; pc0 = 0;
        CHECK(snap_load("/tmp/snaps/a.osn"), "snapshot : lecture");
        saved_ram_a = ram[0x1234]; saved_ram_b = ram[0xC123]; pc0 = PC;
        CHECK(saved_ram_a == 0xAB && saved_ram_b == 0xCD, "snapshot : RAM restauree (y compris sous la ROM)");
        CHECK(A == 0x42 && X == 0x43 && Y == 0x44 && S == 0xEE && P == 0xA5 && pc0 == 0x6789, "snapshot : registres du 6502 restaures");
        CHECK(via.t1l == 0x1357 && via.ier == 0xC0 && ay.r[8] == 9 && hires_mode == 1 && vline == 77 && frame_counter == 4242, "snapshot : VIA, AY et ULA restaures");
        CHECK(tape_pos == 6, "snapshot : position de la K7 restauree");
        memset(rom_basic, 0x22, sizeof rom_basic);                    /* autre ROM BASIC -> refus */
        CHECK(!snap_load("/tmp/snaps/a.osn"), "snapshot : refuse avec une autre ROM BASIC");
        memset(rom_basic, 0x11, sizeof rom_basic);
        tape = NULL; tape_len = 0; tape_name[0] = 0;
    }
    /* capture PNG : signature, dimensions et fin de fichier */
    {
        FILE *f;
        u8 hdr[33];
        int i;
        for (i = 0; i < VW * VH; i++) ((u32 *)fb)[i] = (i & 1) ? 0xFF0000 : 0x0000FF;
        CHECK(png_save("/tmp/snaps/a.png", 2), "capture PNG : ecriture");
        f = fopen("/tmp/snaps/a.png", "rb");
        CHECK(f && fread(hdr, 1, 33, f) == 33 && memcmp(hdr, "\x89PNG\r\n\x1a\n", 8) == 0 && hdr[18] == 0x01 && hdr[19] == 0xE0 && hdr[22] == 0x01 && hdr[23] == 0xC0,
              "capture PNG : signature valide, 480x448");
        if (f) fclose(f);
    }
    /* K7 decoupee : .tap + .ta1 + .ta2 ... concatenes dans l'ordre */
    {
        FILE *f;
        static const char *nm[4] = { "/tmp/split/jeu.tap", "/tmp/split/jeu.ta1", "/tmp/split/jeu.ta2", "/tmp/split/jeu.ta4" };
        u8 a[20], b[10], c[5], d[4];
        int i;
        for (i = 0; i < 20; i++) a[i] = (u8)(0x10 + i);
        for (i = 0; i < 10; i++) b[i] = (u8)(0x40 + i);
        for (i = 0; i < 5; i++) c[i] = (u8)(0x60 + i);
        for (i = 0; i < 4; i++) d[i] = 0xEE;
        (void)system("mkdir -p /tmp/split");
        f = fopen(nm[0], "wb"); fwrite(a, 1, 20, f); fclose(f);
        f = fopen(nm[1], "wb"); fwrite(b, 1, 10, f); fclose(f);
        f = fopen(nm[2], "wb"); fwrite(c, 1, 5, f); fclose(f);
        f = fopen(nm[3], "wb"); fwrite(d, 1, 4, f); fclose(f);          /* .ta4 : la suite est interrompue en .ta3 manquant */
        CHECK(tape_load(nm[0]) && tape_parts == 3 && tape_len == 35, "K7 decoupee : .tap + .ta1 + .ta2 concatenes (35 octets, 3 parties)");
        CHECK(tape[0] == 0x10 && tape[19] == 0x23 && tape[20] == 0x40 && tape[30] == 0x60 && tape[34] == 0x64, "K7 decoupee : ordre et contenu corrects");
        CHECK(tape_len == 35, "K7 decoupee : s'arrete au premier numero manquant (.ta3 absent, .ta4 ignore)");
        CHECK(tape_load(nm[1]) && tape_parts == 2 && tape_len == 15, "ouvrir directement .ta1 : .ta1 + .ta2");
        CHECK(tape_part_no("a.TAP") == -1 && tape_part_no("a.ta12") == 12 && tape_part_no("a.tar") == -2 && tape_part_no("a.dsk") == -2 && tape_part_no("a.ta") == -2,
              "reconnaissance des extensions .tap / .taN");
        free(tape); tape = NULL; tape_len = 0; tape_parts = 0;
    }
    /* acceleration K7 : signal genere (mode temps reel), garde-fou, redemarrage pour les chargeurs maison */
    {
        static u8 t[] = { 0x16, 0x16, 0x16, 0x24, 0x00, 0x00, 0x80, 0xC7, 0x12, 0x34, 0x10, 0x00, 0x00 };
        static int bits[12000];
        int nb = 0, last_in = 0, prev_edge = -1, k, framepos = -1, found24 = 0, nleader = 0;
        double now = 0, last_rise = 0;
        via_reset(); cfg.fast_tape = 0;
        tape = t; tape_len = sizeof t; tape_pos = 0;
        via_write(2, 0x40); via_write(0, 0x40);                           /* PB6 en sortie, moteur ON -> gen_start */
        CHECK(tape_motor == 1 && gen_state == 1 && gen_pos == 3, "moteur K7 allume : le signal demarre sur la synchro");
        for (k = 0; k < 9000000 && gen_state && nb < 11990; k += 10) {
            tape_tick(10); now += 10;
            if (tape_in && !last_in) {
                if (prev_edge >= 0) { double per = (now - last_rise) / SAMPLE_CYC; bits[nb++] = per < 2.5 ? 1 : 0; }
                prev_edge = 1; last_rise = now;
            }
            last_in = tape_in;
        }
        for (k = 20; k < nb - 14 && framepos < 0; k++)
            if (bits[k] == 0 && bits[k-1] == 1 && bits[k-2] == 1 && bits[k-3] == 1) framepos = k;
        while (framepos >= 0 && framepos + 14 <= nb) {
            int v = 0, b;
            for (b = 0; b < 8; b++) v |= bits[framepos + 1 + b] << b;
            if (v == 0x16) nleader++;
            if (v == 0x24) { found24 = 1; break; }
            framepos += 14;
        }
        CHECK(nleader >= 400 && found24, "signal K7 temps reel : leader 0x16 (>= 400 octets) puis synchro 0x24 decodes");
        CHECK(tape_pos == 0, "la position de la bande n'est pas modifiee tant que le signal joue");
        tape_pos = 0; gen_pos = 6; gen_state = 2;
        via_write(0, 0x00);                                               /* moteur OFF */
        CHECK(!tape_motor && gen_state == 0 && tape_pos == 6, "moteur coupe : la bande reste ou le signal s'est arrete");
        /* acceleration K7 desactivee : la ROM n'est PAS interceptee */
        tape_pos = 0; hook_rb = 0x0600; hook_rb_end = 0x0620; rom_mode = 0; PC = 0x0600; ram[0x600] = 0xEA;
        cpu_step();
        CHECK(PC == 0x0601 && tape_pos == 0, "acceleration K7 off : pas de hook (la ROM lit le signal)");
        /* acceleration K7 activee : un lecteur maison (aucun appel au hook) recoit le signal apres 30000 cycles */
        cfg.fast_tape = 1; tape_pos = 4; gen_state = 0; gen_idle = 0; via_write(0, 0x40);
        tape_motor = 1; gen_state = 0;
        tape_tick(40000);
        CHECK(gen_state != 0, "acceleration K7 on : signal envoye a un lecteur K7 maison");
        gen_halt(); tape_motor = 0; tape = NULL;
    }
    /* acceleration disquette : maintien d'activite */
    {
        fdc_reset(); fdc_hot = 0;
        fdc.state = F_FIN; fdc.wait = 100000; fdc_run(10);
        CHECK(fdc_hot > 0, "acces disque en cours -> marque d'activite (turbo disquette)");
        fdc.state = F_IDLE; fdc_run(400000);
        CHECK(fdc_hot <= 0, "activite disque terminee -> la marque retombe");
    }
    /* FDC : image MFM_DISK minimale avec un secteur */
    {
        static u8 img[256 + 6400];
        Drive *d = &drv[0];
        u8 *tp = img + 256;
        u16 crc;
        int k;
        memset(img, 0x4E, sizeof img);
        memcpy(img, "MFM_DISK", 8);
        memset(img + 8, 0, 12);
        img[8] = 1; img[12] = 1; img[16] = 1;
        k = 100;
        tp[k] = tp[k + 1] = tp[k + 2] = 0xA1; tp[k + 3] = 0xFE;
        tp[k + 4] = 0; tp[k + 5] = 0; tp[k + 6] = 1; tp[k + 7] = 1; tp[k + 8] = 0; tp[k + 9] = 0;
        k += 40;
        tp[k] = tp[k + 1] = tp[k + 2] = 0xA1; tp[k + 3] = 0xFB;
        for (i = 0; i < 256; i++) tp[k + 4 + i] = (u8)i;
        (void)crc;
        d->img = img; d->size = sizeof img; d->sides = 1; d->tracks = 1; d->loaded = 1; d->writable = 0;
        disk_on = 1; have_disk = 1; fdc_reset();
        fdc.drive = 0; fdc.side = 0;
        fdc.trk = 0; fdc.sec = 1;
        fdc_cmd(0x80);
        {
            int got = 0, okdata = 1, guard = 0;
            while (got < 256 && guard++ < 200000) {
                fdc_run(1);
                if (fdc.drq) { u8 v = fdc_read(3); if (v != (u8)got) okdata = 0; got++; }
            }
            CHECK(got == 256 && okdata, "FDC : lecture de secteur 256 octets");
            fdc_run(200);
            CHECK(!fdc.busy && fdc.intrq, "FDC : commande terminee + INTRQ");
        }
        disk_on = 0;
    }
    /* AY : un son simple */
    ay_reset();
    ay.r[0] = 10; ay.r[7] = 0xFE; ay.r[8] = 15;
    {
        int mx = 0, mn = 99999;
        for (i = 0; i < 2000; i++) { int v = ay_tick(); if (v > mx) mx = v; if (v < mn) mn = v; }
        CHECK(mx > 0 && mn == 0, "AY : onde carree sur la voie A");
    }
    printf(fails ? "\n%d echec(s)\n" : "\ntous les tests passent\n", fails);
    return fails != 0;
}
#else
int main(void) { puts("Ce programme est destine a Windows (voir l'en-tete)."); return 0; }
#endif

#endif
